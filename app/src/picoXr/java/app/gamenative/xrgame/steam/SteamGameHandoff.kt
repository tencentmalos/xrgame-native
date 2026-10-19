// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import android.content.Context
import app.gamenative.PluviaApp
import app.gamenative.service.ActiveGameRegistry
import app.gamenative.xrgame.XrGameOfficialSteam
import app.gamenative.utils.SteamUtils
import `in`.dragonbra.javasteam.types.KeyValue
import app.gamenative.xrgame.steam.SteamLibraryManifest.child
import app.gamenative.xrgame.steam.SteamLibraryManifest.put
import kotlinx.coroutines.flow.MutableStateFlow
import org.json.JSONObject
import java.io.File

/**
 * A private handoff of Steam's game launch: the app ID and the command Steam issued, inside the
 * game's install folder. The official client never spawns a second Wine prefix.
 */
class SteamGameHandoff(private val context: Context) {
    data class Request(val appId: Int, val file: File, val command: XrGameOfficialSteam.Command? = null,
                       @Volatile var dispatched: Boolean = false, var started: Boolean = false,
                       /** The Android game session (its X server or environment) has existed. */
                       var sessionSeen: Boolean = false,
                       /** When the session was last seen gone, 0 while it exists. */
                       var sessionGoneAt: Long = 0)
    companion object {
        private const val TOOL_ID = "steampsp-proton-arm64"
        /** game-launch.py MAX_REQUEST; larger requests carry no command there. */
        const val MAX_REQUEST = 16384
        private const val MAX_ARGUMENTS = 64
        val pending = MutableStateFlow<Request?>(null)
        val error = MutableStateFlow<String?>(null)

        /** The request's app ID and command; an unusable command is dropped, never the launch. */
        fun parse(text: String): Pair<Int, XrGameOfficialSteam.Command?>? {
            val body = runCatching { JSONObject(text) }.getOrNull() ?: return null
            val id = body.optLong("appId", 0).takeIf { it in 1 until 0x80000000 }?.toInt() ?: return null
            return id to body.optJSONObject("command")?.let(::command)
        }

        private fun command(value: JSONObject): XrGameOfficialSteam.Command? {
            fun relative(path: String) = path.length <= 1024 && !path.startsWith('/') && ':' !in path &&
                '\\' !in path && path.none { it.isISOControl() } && path.split('/').none { it == ".." }
            val executable = value.optString("executable")
            val work = value.optString("workingDirectory")
            val list = value.optJSONArray("arguments") ?: return null
            if (executable.isBlank() || !relative(executable) || !relative(work) || list.length() > MAX_ARGUMENTS) return null
            val arguments = (0 until list.length()).map { list.opt(it) as? String ?: return null }
            if (arguments.any { argument -> argument.any { it.isISOControl() } }) return null
            return XrGameOfficialSteam.Command(executable, app.gamenative.xrgame.XrGameCommandLine.join(arguments), work)
        }
    }
    private val runtime = LinuxSteamRuntime(context)
    private val directory = File(runtime.directory, "game-bridge")
    private var active: Request? = null

    fun prepare() {
        directory.mkdirs()
        directory.listFiles().orEmpty().filter { it.name.matches(Regex("[0-9a-f]{32}\\.(request|result|tmp)")) }.forEach { it.delete() }
        val tool = File(runtime.client, "compatibilitytools.d/steampsp").apply { mkdirs() }
        context.assets.open("linux-steam/game-launch.py").use { input -> File(tool, "launch.py").outputStream().use { input.copyTo(it) } }
        check(File(tool, "launch.py").setExecutable(true, true))
        SteamLibraryManifest.write(File(tool, "compatibilitytool.vdf"), KeyValue("compatibilitytools").apply {
            // Steam recognizes Proton's Windows AutoCloud roots by the tool identifier.
            // Keep the directory stable; only the registered identifier changes.
            child("compat_tools").child(TOOL_ID).apply {
                put("install_path", "."); put("display_name", "SteamPSP · existing game container")
                put("from_oslist", "windows"); put("to_oslist", "linux")
            }
        })
        SteamLibraryManifest.write(File(tool, "toolmanifest.vdf"), KeyValue("manifest").apply {
            put("version", "2"); put("commandline", "/launch.py %verb%"); put("use_sessions", "1")
            put("compatmanager_layer_name", "proton")
        })
        val config = File(runtime.client, "config/config.vdf")
        val kv = if (config.isFile) KeyValue.loadFromString(config.readText()) else KeyValue("InstallConfigStore")
        check(kv?.name == "InstallConfigStore")
        LinuxSteamLibrary.backup(config)
        val mapping = kv!!.child("Software").child("Valve").child("Steam").child("CompatToolMapping")
        mapping.child("0").apply { put("name", TOOL_ID); put("config", ""); put("priority", "75") }
        for (root in LinuxSteamLibrary.roots(context)) File(root, "steamapps").listFiles().orEmpty().forEach { file ->
            SteamLibraryManifest.read(file)?.let { install ->
                mapping.child(install.appId.toString()).apply { put("name", TOOL_ID); put("config", ""); put("priority", "250") }
            }
        }
        SteamLibraryManifest.write(config, kv)
    }

    private fun reply(request: Request, state: String) {
        val file = File(directory, request.file.nameWithoutExtension + ".result")
        val temp = File(directory, request.file.nameWithoutExtension + ".tmp")
        temp.writeText(JSONObject().put("state", state).toString())
        check(temp.renameTo(file))
    }

    fun poll() {
        val now = System.currentTimeMillis()
        active?.let { request ->
            val session = PluviaApp.xServerView != null || PluviaApp.xEnvironment != null
            if (session) { request.sessionSeen = true; request.sessionGoneAt = 0 }
            else if (request.sessionSeen && request.sessionGoneAt == 0L) request.sessionGoneAt = now
            // Activity handovers can clear the view for a moment; an ended session stays gone.
            val sessionEnded = request.sessionSeen && !session && now - request.sessionGoneAt >= 1500
            if (!XrGameOfficialSteam.isOfficialGame(request.appId)) {
                reply(request, "failed"); request.file.delete(); pending.value = null; active = null
            } else if (!request.file.isFile || now - request.file.lastModified() > 10000) {
                XrGameOfficialSteam.finishGame(request.appId); pending.value = null; active = null
            } else if (ActiveGameRegistry.get()?.appId == request.appId) {
                if (!request.started) { request.started = true; reply(request, "running") }
            } else if (sessionEnded) {
                // The session ended: by the game, the quick menu, a launch error or the user
                // leaving the page. Steam must not show the game as running any longer.
                reply(request, if (request.started) "finished" else "failed")
                XrGameOfficialSteam.finishGame(request.appId); pending.value = null; active = null
                request.file.delete()
            }
            return
        }
        val file = directory.listFiles().orEmpty().filter {
            it.name.matches(Regex("[0-9a-f]{32}\\.request")) && it.length() in 1..MAX_REQUEST.toLong() && now - it.lastModified() in 0..10000
        }.minByOrNull { it.lastModified() } ?: return
        val (id, command) = runCatching { parse(file.readText()) }.getOrNull() ?: run { file.delete(); return }
        val request = Request(id, file, command)
        val account = SteamUtils.getSteamId64()
        if (ActiveGameRegistry.get() != null || PluviaApp.xServerView != null ||
            LinuxSteamLibrary.install(context, id)?.complete != true || account == null ||
            runCatching { runtime.checkSession(account) }.getOrDefault(-1) != 0) {
            error.value = "Official game launch is not ready. Check the Steam login, installed game and shared save container."
            reply(request, "failed"); file.delete(); return
        }
        XrGameOfficialSteam.authorizeGame(id, command)
        active = request
        pending.value = request
    }

    fun close() {
        active?.let { runCatching { reply(it, "failed") }; XrGameOfficialSteam.finishGame(it.appId) }
        active = null; pending.value = null
    }
}
