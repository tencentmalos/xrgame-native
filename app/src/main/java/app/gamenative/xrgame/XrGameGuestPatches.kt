package app.gamenative.xrgame

import android.content.Context
import com.winlator.container.Container
import com.winlator.core.envvars.EnvVars
import java.io.File
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.MessageDigest
import org.json.JSONArray
import org.json.JSONObject
import timber.log.Timber

/**
 * Guest patches: per-game packages of x86-64 code (schema `xrgame.guest-patch.v1`, built by
 * `tools/xrgame/guest-patch/build.py`) that the injected installer `xrgame_guest_patch64.dll`
 * applies to a game module before its code runs, without changing game files. Ported from
 * shadPS4's guest function patches; see `docs/specs/xrgame-guest-patch-v1.md`.
 *
 * Packages live in `<install root>/guest_patches/<Steam app id>/<name>.json`. A game's
 * selection is the container variable [SELECTION_ENV], file stems in install order. At launch
 * [prepare] copies the selected packages into the prefix and returns the installer for the
 * Steam client loader to inject; the installer verifies each module's SHA-256 and preimages
 * itself and skips a package that does not match. Packages run with the game's privileges:
 * only use ones you reviewed.
 */
object XrGameGuestPatches {
    const val SELECTION_ENV = "XRGAME_GUEST_PATCHES"
    const val INSTALLER = "xrgame_guest_patch64.dll"
    const val MAX_PACKAGES = 8
    private const val SCHEMA = "xrgame.guest-patch.v1"
    private const val MAX_PACKAGE_BYTES = 4L * 1024 * 1024
    private val SYMBOL = Regex("[A-Za-z_][A-Za-z0-9_]{0,95}")
    private val DIGEST = Regex("[0-9a-f]{64}")

    /** A module range a package changes, or calls without changing (a function binding). */
    data class Range(val begin: Long, val end: Long, val changes: Boolean, val what: String)

    data class Package(
        val name: String,
        val id: String,
        val displayName: String,
        val description: String,
        val module: String,
        val moduleSha256: String,
        val ranges: List<Range>,
        val conflicts: List<String> = emptyList(),
    )

    data class Listing(val packages: List<Package>, val skipped: List<String>)

    /** What [prepare] staged: the installer for the loader to inject and its environment. */
    data class Prepared(val installer: File, val environment: Map<String, String>, val packages: List<String>)

    fun directory(appId: Int): File? = XrGameStorage.installRoot?.let { File(it, "guest_patches/$appId") }

    /** A selection entry is a bare file stem: no separators or relative components. */
    fun validName(name: String): Boolean =
        name.isNotEmpty() && name.length <= 96 && name != "." && name != ".." &&
            name.none { it == '/' || it == '\\' || it == ':' || it == ',' || it.isISOControl() }

    fun selection(envVars: String): List<String> =
        EnvVars(envVars).get(SELECTION_ENV).split(',').map { it.trim() }.filter { it.isNotEmpty() }

    fun withSelection(envVars: String, names: List<String>): String = EnvVars(envVars).apply {
        require(names.size <= MAX_PACKAGES && names.distinct().size == names.size && names.all(::validName)) {
            "Invalid guest patch selection"
        }
        if (names.isEmpty()) remove(SELECTION_ENV) else put(SELECTION_ENV, names.joinToString(","))
    }.toString()

    /** Packages for [appId] in name order, with conflicts between them (as shadPS4 ListPackages). */
    fun list(appId: Int, directory: File? = directory(appId)): Listing {
        val packages = mutableListOf<Package>()
        val skipped = mutableListOf<String>()
        val files = directory?.listFiles { file -> file.isFile && file.name.endsWith(".json") }.orEmpty()
        for (file in files.sortedBy { it.name }) {
            val name = file.name.removeSuffix(".json")
            try {
                require(validName(name)) { "invalid file name" }
                require(file.length() <= MAX_PACKAGE_BYTES) { "package exceeds 4 MiB" }
                packages += parse(name, JSONObject(file.readText()), appId)
            } catch (e: Exception) {
                skipped += "${file.name}: ${e.message}"
            }
        }
        val withConflicts = packages.map { package_ ->
            package_.copy(conflicts = packages.filter { other ->
                other !== package_ && other.module.equals(package_.module, ignoreCase = true) &&
                    other.moduleSha256 == package_.moduleSha256 && overlaps(package_.ranges, other.ranges)
            }.map { it.name })
        }
        return Listing(withConflicts, skipped)
    }

    internal fun parse(name: String, json: JSONObject, appId: Int): Package {
        require(json.getString("schema") == SCHEMA && json.getString("abi") == "x86_64-win64" &&
            json.getInt("sdk_version") == 1) { "unsupported package format" }
        val id = json.getString("id")
        require(SYMBOL.matches(id)) { "invalid package id" }
        require(json.getString("title") == appId.toString()) { "package is for app ${json.getString("title")}" }
        val module = json.getString("module")
        require(module.isNotEmpty() && module.length <= 128 && module.none { it in "\\/:" }) { "invalid module" }
        val sha = json.getString("module_sha256")
        require(DIGEST.matches(sha)) { "invalid module SHA-256" }
        val ranges = mutableListOf<Range>()
        fun items(key: String): List<JSONObject> = json.optJSONArray(key)?.let { array ->
            (0 until array.length()).map { array.getJSONObject(it) }
        }.orEmpty()
        for (hook in items("hooks")) {
            val offset = hook.getLong("offset")
            val size = hook.getString("expected").length / 2L
            val kind = if (hook.optString("mode", "typed") == "typed") "hook" else "site"
            ranges += Range(offset, offset + size, true, "$kind ${hook.getString("name")}")
        }
        for (patch in items("patches")) {
            val offset = patch.getLong("offset")
            ranges += Range(offset, offset + patch.getString("expected").length / 2L, true, "code patch ${patch.getString("name")}")
        }
        for (binding in items("bindings")) {
            if (binding.getString("kind") != "function") continue
            val offset = binding.getLong("offset")
            ranges += Range(offset, offset + binding.getString("expected").length / 2L, false,
                "function binding ${binding.getString("name")}")
        }
        require(ranges.any { it.changes }) { "package changes nothing" }
        return Package(name, id, json.optString("name").ifEmpty { id }, json.optString("description"), module, sha, ranges)
    }

    /** The first overlap where either side changes the bytes (shadPS4 FindCodeOverlap). */
    fun overlaps(a: List<Range>, b: List<Range>): Boolean =
        a.any { x -> b.any { y -> (x.changes || y.changes) && x.begin < y.end && y.begin < x.end } }

    /**
     * Stages the selected packages for this launch, or returns null when none is selected (or
     * the launch cannot inject them). Missing, invalid and conflicting packages are left out
     * and logged; the installer still checks every remaining one against the loaded module.
     */
    fun prepare(context: Context, container: Container, appId: Int, followSelfRestart: Boolean): Prepared? {
        val selected = selection(container.envVars).filter(::validName).distinct().take(MAX_PACKAGES)
        val state = File(container.rootDir, ".wine/drive_c/xrgame/guest_patch/$appId")
        File(state, "packages").deleteRecursively()
        if (selected.isEmpty()) return null
        if (followSelfRestart) {
            // The Wine bootstrap of self-restarting games loads exactly one DLL.
            Timber.w("Guest patches %s for %d not installed: this game restarts itself", selected, appId)
            return null
        }
        val listing = list(appId)
        listing.skipped.forEach { Timber.w("Guest patch package skipped: %s", it) }
        val chosen = mutableListOf<Package>()
        for (name in selected) {
            val found = listing.packages.firstOrNull { it.name == name }
            when {
                found == null -> Timber.w("Guest patch %s for %d is missing or invalid", name, appId)
                chosen.any { it.name in found.conflicts } -> Timber.w("Guest patch %s conflicts with an earlier one", name)
                else -> chosen += found
            }
        }
        if (chosen.isEmpty()) return null
        val packages = File(state, "packages").apply { mkdirs() }
        val source = requireNotNull(directory(appId))
        chosen.forEachIndexed { index, package_ ->
            copyAtomic(File(source, "${package_.name}.json"), File(packages, "%02d-%s.json".format(index + 1, package_.name)))
        }
        for (stale in listOf("status.txt", "status.tmp", "control.txt")) File(state, stale).delete()
        val log = File(state, "guest-patch.log")
        if (log.isFile) Files.move(log.toPath(), File(state, "guest-patch.previous.log").toPath(), StandardCopyOption.REPLACE_EXISTING)
        val installer = File(state, INSTALLER)
        stageInstaller(context, installer)
        Timber.i("Guest patches for %d: %s", appId, chosen.map { it.name })
        return Prepared(installer, mapOf(
            "XRGAME_GUEST_PATCH_DIR" to "C:\\xrgame\\guest_patch\\$appId",
            "XRGAME_GUEST_PATCH_APP" to appId.toString(),
        ), chosen.map { it.name })
    }

    /** The installer from the APK, checked against the payload manifest built with it. */
    private fun stageInstaller(context: Context, destination: File) {
        val bytes = context.assets.open(INSTALLER).use { it.readBytes() }
        val manifest = context.assets.open("xrgame-payload.json").use { JSONObject(it.reader().readText()) }
        val expected = manifest.getJSONObject("sha256").getString(INSTALLER)
        check(sha256(bytes) == expected) { "Guest patch installer does not match the payload manifest" }
        if (destination.isFile && sha256(destination.readBytes()) == expected) return
        val partial = File(destination.parentFile, ".$INSTALLER.part")
        partial.writeBytes(bytes)
        Files.move(partial.toPath(), destination.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
    }

    private fun copyAtomic(source: File, destination: File) {
        val partial = File(destination.parentFile, ".${destination.name}.part")
        source.copyTo(partial, overwrite = true)
        Files.move(partial.toPath(), destination.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
    }

    private fun sha256(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }

    /** The installer's state directory of a game's container (status, control, log). */
    fun stateDirectory(container: Container, appId: Int): File =
        File(container.rootDir, ".wine/drive_c/xrgame/guest_patch/$appId")

    /** Queues `enable|disable [package|hook|package/hook]` for the running installer. */
    fun control(container: Container, appId: Int, enable: Boolean, target: String?): Long {
        require(target == null || (target.isNotEmpty() && target.length <= 190 && target.none { it.isWhitespace() || it.isISOControl() })) {
            "invalid guest patch target"
        }
        val file = File(stateDirectory(container, appId), "control.txt")
        val last = if (file.isFile) file.readLines().mapNotNull { it.substringBefore(' ').toLongOrNull() }.maxOrNull() ?: 0 else 0
        val sequence = last + 1
        file.appendText("$sequence ${if (enable) "enable" else "disable"}${target?.let { " $it" }.orEmpty()}\n")
        return sequence
    }

    /** Summary for DebugBus: the selection and listing of a game, and the installer's status. */
    fun describe(container: Container, appId: Int): JSONObject {
        val listing = list(appId)
        val state = stateDirectory(container, appId)
        val status = File(state, "status.txt").takeIf { it.isFile }?.readText()
        val packages = JSONArray()
        listing.packages.forEach { p ->
            packages.put(JSONObject().put("name", p.name).put("id", p.id).put("displayName", p.displayName)
                .put("module", p.module).put("moduleSha256", p.moduleSha256).put("conflicts", JSONArray(p.conflicts)))
        }
        return JSONObject()
            .put("appId", appId)
            .put("directory", directory(appId)?.path)
            .put("selection", JSONArray(selection(container.envVars)))
            .put("packages", packages)
            .put("skipped", JSONArray(listing.skipped))
            .put("status", status?.let(::statusJson))
    }

    /** The installer's `key: value` status lines, with repeated keys as arrays in order. */
    internal fun statusJson(text: String): JSONObject {
        val result = JSONObject()
        val packages = JSONArray()
        var current = result
        for (line in text.lineSequence()) {
            val key = line.substringBefore(": ", "").takeIf { it.isNotEmpty() } ?: continue
            val value = line.substringAfter(": ")
            if (key == "package") {
                current = JSONObject()
                packages.put(current)
            }
            if (key in listOf("hook", "site", "code_patch", "counter", "log_tag")) {
                (current.optJSONArray(key) ?: JSONArray().also { current.put(key, it) }).put(value)
            } else {
                current.put(key, value)
            }
        }
        if (packages.length() > 0) result.put("packages", packages)
        return result
    }
}
