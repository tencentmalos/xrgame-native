// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import android.content.Context
import app.gamenative.utils.SteamUtils
import com.winlator.container.Container
import com.winlator.container.ContainerManager
import `in`.dragonbra.javasteam.types.KeyValue
import app.gamenative.xrgame.steam.SteamLibraryManifest.child
import app.gamenative.xrgame.steam.SteamLibraryManifest.put
import java.io.File
import java.nio.file.Files
import java.security.MessageDigest

/** Both clients use the existing saves. Only the selected client owns Cloud synchronization. */
object SteamSaveSharing {
    @Volatile var conflicts: Set<Int> = emptySet()
        private set
    fun prepareClient(context: Context) {
        conflicts = emptySet()
        val account = SteamUtils.getSteam3AccountId() ?: return
        val runtime = LinuxSteamRuntime(context)
        // Undo only the earlier sharing prototype's forced Cloud disable, once, preserving
        // unrelated settings and subsequent choices in Valve's UI.
        val shared = File(runtime.client, "userdata/$account/7/remote/sharedconfig.vdf")
        restoreOfficialCloudPreference(shared, File(runtime.directory, "migration/cloud/$account"))
        val failed = mutableSetOf<Int>()
        for (container in ContainerManager(context).containers) {
            val id = container.id.removePrefix("STEAM_").toIntOrNull() ?: continue
            if (container.id == "STEAM_$id" && LinuxSteamLibrary.install(context, id)?.complete == true) {
                // One conflicting save must not prevent the client/library from opening.
                // Launch repeats this check and reports the conflict for that game only.
                try { prepareContainer(context, container, id) } catch (_: Exception) { failed.add(id) }
            }
        }
        conflicts = failed
    }

    internal fun restoreOfficialCloudPreference(file: File, stateDirectory: File) {
        check(stateDirectory.isDirectory || stateDirectory.mkdirs())
        // App 7 syncs every file in remote/. Our migration state must live outside
        // that directory, otherwise Steam uploads the marker and backup as user config.
        fun privateState(name: String): File {
            val target = File(stateDirectory, name)
            val legacy = File(file.parentFile, name)
            if (legacy.isFile) {
                if (target.exists()) {
                    check(target.readBytes().contentEquals(legacy.readBytes())) { "Cloud migration backup differs; both copies were preserved." }
                    check(legacy.delete())
                } else check(legacy.renameTo(target))
            }
            return target
        }
        val backup = privateState("${file.name}.steampsp-before-sharing")
        val migrated = privateState("steampsp-cloud-owner-v2")
        if (!file.isFile || migrated.exists()) return
        if (backup.isFile) {
            val previous = KeyValue.loadFromString(backup.readText())
            val current = KeyValue.loadFromString(file.readText())
            check(current?.name == "UserRoamingConfigStore" && previous?.name == "UserRoamingConfigStore")
            val old = previous!!["Software"]["Valve"]["Steam"]["CloudEnabled"].value
            val steam = current!!.child("Software").child("Valve").child("Steam")
            if (steam["CloudEnabled"].value == "0" && old != "0") {
                steam.put("CloudEnabled", old?.takeIf { it == "1" } ?: "1")
                SteamLibraryManifest.write(file, current)
            }
        }
        migrated.writeText("1")
    }

    /** PRoot binds this link-only directory over steamapps/compatdata (FUSE cannot create symlinks). */
    fun compatData(context: Context) = File(LinuxSteamRuntime(context).directory, "compatdata").apply { mkdirs() }

    internal fun linkDirectory(alias: File, target: File) {
        check(target.isDirectory) { "Existing save container is not ready." }
        if (Files.isSymbolicLink(alias.toPath())) {
            check(alias.canonicalFile == target.canonicalFile) { "Save alias already points to another container." }
            return
        }
        check(!alias.exists() || (alias.isDirectory && alias.listFiles().isNullOrEmpty())) {
            "A separate Steam save prefix already exists; no data was replaced."
        }
        if (alias.exists()) check(alias.delete())
        check(alias.parentFile!!.isDirectory || alias.parentFile!!.mkdirs())
        Files.createSymbolicLink(alias.toPath(), target.canonicalFile.toPath())
    }

    fun prepareContainer(context: Context, container: Container, appId: Int) {
        val account = SteamUtils.getSteam3AccountId() ?: error("Sign in to Official Steam first.")
        val savedAccount = app.gamenative.PrefManager.steamUserAccountId.toLong() and 0xffffffffL
        check(savedAccount == 0L || account == savedAccount) {
            "The existing saves belong to another Custom Steam account. Use the same account before sharing this container."
        }
        val prefix = File(container.rootDir, ".wine")
        check(prefix.isDirectory) { "The existing game container is not ready." }
        val prefixAlias = File(compatData(context), "$appId/pfx")
        linkDirectory(prefixAlias, prefix)
        // Steam AutoCloud uses Proton's Windows user, while this Wine runtime uses xuser.
        linkDirectory(File(prefix, "drive_c/users/steamuser"), File(prefix, "drive_c/users/xuser"))
        val canonical = File(prefix, "drive_c/Program Files (x86)/Steam/userdata/$account/$appId/remote")
        val official = File(LinuxSteamRuntime(context).client, "userdata/$account/$appId/remote")
        linkRemote(official, canonical)
    }

    /** Never overwrite divergent saves. Preserve the previous tree beside the link. */
    internal fun linkRemote(official: File, canonical: File) {
        if (Files.isSymbolicLink(official.toPath())) {
            check(official.canonicalFile == canonical.canonicalFile) { "Existing Steam save link points to another container." }
            return
        }
        val old = hashes(official)
        val target = hashes(canonical)
        check(old.isEmpty() || target.isEmpty() || old == target) {
            "Official and Custom Steam contain different saves. Both copies were preserved; resolve the save conflict before launching."
        }
        check(canonical.isDirectory || canonical.mkdirs())
        if (old.isNotEmpty() && target.isEmpty()) {
            // Only an empty destination can receive the official copy, and verify before linking.
            official.copyRecursively(canonical, overwrite = false)
            check(hashes(canonical) == old) { "Save copy verification failed; original saves were preserved." }
        }
        check(official.parentFile!!.isDirectory || official.parentFile!!.mkdirs())
        val backup = File(official.parentFile, "remote.steampsp-before-sharing")
        if (official.exists()) {
            check(!backup.exists()) { "A previous save backup needs review; no saves were replaced." }
            check(official.renameTo(backup))
        }
        try { Files.createSymbolicLink(official.toPath(), canonical.canonicalFile.toPath()) }
        catch (error: Exception) { if (backup.exists()) backup.renameTo(official); throw error }
    }

    private fun hashes(root: File): Map<String, String> {
        if (!root.exists()) return emptyMap()
        check(root.isDirectory)
        return root.walkTopDown().onEnter {
            check(!Files.isSymbolicLink(it.toPath())) { "Save tree contains an unexpected link." }; true
        }.filter { it.isFile }.associate { file ->
            check(!Files.isSymbolicLink(file.toPath())) { "Save tree contains an unexpected link." }
            val hash = MessageDigest.getInstance("SHA-256")
            file.inputStream().use { input ->
                val bytes = ByteArray(65536)
                while (true) { val n = input.read(bytes); if (n < 0) break; hash.update(bytes, 0, n) }
            }
            file.relativeTo(root).path to hash.digest().joinToString("") { "%02x".format(it) }
        }
    }
}
