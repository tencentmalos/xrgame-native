package app.gamenative.xrgame

import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.Process
import android.os.SystemClock
import android.system.Os
import android.system.OsConstants
import androidx.annotation.Keep
import app.gamenative.BuildConfig
import app.gamenative.MainActivity
import app.gamenative.PluviaApp
import app.gamenative.PrefManager
import app.gamenative.service.ActiveGameRegistry
import app.gamenative.service.SteamService
import app.gamenative.utils.ContainerUtils
import app.gamenative.ui.screen.xr.ImmersiveXrActivity
import app.gamenative.ui.screen.xr.XrDebugInput
import app.gamenative.ui.screen.xr.XrNative
import app.gamenative.ui.screen.xr.windows.WindowsVrGripCorrection
import app.gamenative.ui.screen.xr.windows.WindowsVrTuning
import app.gamenative.ui.screen.xr.windows.WindowsVrUpscale
import com.winlator.xserver.extensions.PresentExtension
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileDescriptor
import java.io.PrintWriter
import java.security.MessageDigest
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.ThreadPoolExecutor
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException

/** Debug APK only. Android's DUMP permission protects start/bind; dumpsys itself is shell gated. */
@Keep
class DebugBusService : Service() {
    companion object {
        init { System.loadLibrary("xrgame_debugbus") }
        private val ARGUMENT_COMMANDS = setOf("present", "api_capture", "vr_tuning", "vr_upscale", "vr_grip",
            "launch", "input", "quickmenu", "vr_hud", "vr_oc", "vr_keyboard", "guest_patch", "guest_debugger", "linux_steam")
        private val CAPTURE_COMMANDS = setOf("capture_source", "capture_screenshot", "capture_video", "capture_config")
        private const val LAUNCH_GAME = "app.gamenative.LAUNCH_GAME"
        @Volatile private var lastLaunch: Int? = null
        @Volatile private var linuxSteamCheck = -1
        @Volatile private var guestDebugger: java.lang.Process? = null
        @Volatile private var guestDebuggerTarget = 0
        @Volatile private var guestDebuggerExe = ""
        @Volatile private var guestDebuggerStartedMs = 0L
    }
    private val worker = ThreadPoolExecutor(1, 1, 0, TimeUnit.MILLISECONDS, ArrayBlockingQueue(1)) {
        Thread(it, "XRGame:DebugBus").apply { isDaemon = true }
    }
    private val startedNs = SystemClock.elapsedRealtimeNanos()
    private external fun execute(request: String, args: Array<String>): String
    override fun onBind(intent: Intent?): IBinder? = null
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int) = START_NOT_STICKY
    override fun onDestroy() { worker.shutdownNow(); super.onDestroy() }

    override fun dump(fd: FileDescriptor?, writer: PrintWriter, args: Array<out String>?) {
        val input = args.orEmpty()
        if (input.size > 5 || input.any { it.length > 64 }) {
            writer.println("{\"error\":\"request_limit\"}")
            return
        }
        val future = try {
            worker.submit<String> { execute(input.firstOrNull() ?: "help", input.drop(1).toTypedArray()) }
        } catch (_: java.util.concurrent.RejectedExecutionException) {
            writer.println("{\"error\":\"busy\"}")
            return
        }
        try {
            writer.print(future.get(2, TimeUnit.SECONDS))
        } catch (_: TimeoutException) {
            future.cancel(true)
            writer.println("{\"error\":\"timeout\"}")
        } catch (_: Exception) {
            future.cancel(true)
            writer.println("{\"error\":\"query_failed\"}")
        }
    }

    /** Called synchronously by Foundation handlers on the bounded query worker, never a GPU lock. */
    @Keep
    fun query(command: String, args: Array<String>): String = if (command in CAPTURE_COMMANDS) {
        capture(command, args)
    } else try {
        require(command in ARGUMENT_COMMANDS || args.isEmpty()) { "unexpected_arguments" }
        val result = when (command) {
            "status" -> JSONObject()
                .put("pid", Process.myPid()).put("uid", Process.myUid())
                .put("sdk", Build.VERSION.SDK_INT).put("targetSdk", applicationInfo.targetSdkVersion)
                .put("model", Build.MODEL).put("build", Build.FINGERPRINT)
                .put("bootId", readBounded(File("/proc/sys/kernel/random/boot_id"), 128)?.trim() ?: JSONObject.NULL)
                .put("serviceStartedNs", startedNs).put("version", BuildConfig.VERSION_NAME)
                .put("activeSteamAppId", ActiveGameRegistry.get()?.appId ?: JSONObject.NULL)
                .put("xServerPresent", PluviaApp.xServerView != null)
                .put("guestControl", false)
            "runtime" -> runtime()
            "linux_steam" -> linuxSteam(args)
            "processes" -> processes()
            "present" -> present(args)
            "api_capture" -> apiCapture(args)
            "vr_tuning" -> vrTuning(args)
            "vr_upscale" -> vrUpscale(args)
            "vr_grip" -> vrGrip(args)
            "launch" -> launch(args)
            "input" -> input(args)
            "quickmenu" -> quickMenu(args)
            "vr_hud" -> vrHud(args)
            "vr_keyboard" -> vrKeyboard(args)
            "game_state" -> gameState()
            "vr_oc" -> vrOpenComposite(args)
            "guest_patch" -> guestPatch(args)
            "guest_debugger" -> guestDebuggerCommand(args)
            else -> error("unknown_provider")
        }
        result.put("schema", 1).put("sampledAtBootNs", SystemClock.elapsedRealtimeNanos()).toString()
    } catch (e: IllegalArgumentException) {
        JSONObject().put("error", e.message ?: "invalid_arguments").toString()
    } catch (e: Exception) {
        // Never return exception messages containing paths, launch arguments or account data.
        JSONObject().put("error", "provider_failed").put("type", e.javaClass.simpleName).toString()
    }

    /**
     * The scrcpy capture SDK's commands for the immersive session, answered in its `key: value`
     * format (my_mcp_tools capturectl.py, scrcpy MCP capture_app_screenshot / record_app_video).
     * Files go to app-private files/captures; tools pull them with run-as.
     */
    private fun capture(command: String, args: Array<String>): String = try {
        XrNative.nativeCaptureCommand(command, args, File(filesDir, "captures").absolutePath)
    } catch (e: Throwable) {
        "status: failed\nerror: ${e.javaClass.simpleName}\n"
    }

    /** Bounded official-client diagnostics. Login remains in Valve's UI; no identifiers returned. */
    private fun linuxSteam(args: Array<String>): JSONObject {
        val action = args.firstOrNull() ?: "status"
        require(args.size <= 1 && action in listOf("status", "open", "start", "stop", "check", "trace", "library")) {
            "usage: linux_steam status|open|start|stop|check|trace|library"
        }
        when (action) {
            "open" -> startActivity(Intent(this, app.gamenative.xrgame.steam.LinuxSteamActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            "start" -> startForegroundService(Intent(this, app.gamenative.xrgame.steam.LinuxSteamService::class.java))
            "stop" -> stopService(Intent(this, app.gamenative.xrgame.steam.LinuxSteamService::class.java))
            "check" -> if (linuxSteamCheck != -2) {
                linuxSteamCheck = -2
                Thread({ linuxSteamCheck = try {
                    app.gamenative.xrgame.steam.LinuxSteamRuntime(this).checkSession()
                } catch (_: Exception) { 12 } }, "SteamSessionCheck").start()
            }
        }
        return JSONObject().put("running", app.gamenative.xrgame.steam.LinuxSteamService.running)
            .put("display", app.gamenative.xrgame.steam.LinuxSteamService.xServerState.value != null)
            .put("sessionCheck", linuxSteamCheck)
            .put("libraryOwned", app.gamenative.xrgame.XrGameOfficialSteam.ownsLibrary)
            .put("mode", if (app.gamenative.xrgame.XrGameOfficialSteam.active) "official" else "custom")
            .put("javaRunning", SteamService.isRunning).put("javaConnected", SteamService.isConnected)
            .put("javaLoggedIn", SteamService.isLoggedIn)
            .apply { if (action == "library") {
                val records = JSONArray()
                for (root in app.gamenative.xrgame.steam.LinuxSteamLibrary.roots(this@DebugBusService)) {
                    for (file in File(root, "steamapps").listFiles().orEmpty()) {
                        app.gamenative.xrgame.steam.SteamLibraryManifest.read(file)?.let { install ->
                            records.put(JSONObject().put("appId", install.appId).put("complete", install.complete)
                                .put("depots", install.depots.size).put("external", !root.path.startsWith(filesDir.path)))
                        }
                    }
                }
                put("installs", records)
                put("saveConflicts", JSONArray(app.gamenative.xrgame.steam.SteamSaveSharing.conflicts.toList()))
            } }
            .apply { if (action == "trace") put("x11Requests", app.gamenative.xrgame.steam.LinuxSteamService.x11Trace()) }
    }

    /** Windows VR pacing/latency experiments; no arguments reports the current values. */
    private fun vrTuning(args: Array<String>): JSONObject {
        WindowsVrTuning.apply(args.toList())?.let { throw IllegalArgumentException(it) }
        return JSONObject()
            .put("pacing", WindowsVrTuning.pacing.name.lowercase())
            .put("startTargetUs", WindowsVrTuning.frameStartTargetUs)
            .put("predict", WindowsVrTuning.extendedPrediction)
    }

    /** Windows VR reconstruction on the Vulkan composite; no arguments reports the current values. */
    private fun vrUpscale(args: Array<String>): JSONObject {
        WindowsVrUpscale.apply(args.toList())?.let { throw IllegalArgumentException(it) }
        val settings = WindowsVrUpscale.settings
        return JSONObject()
            .put("filter", settings.filter.key).put("sharp", settings.sharpness)
            .put("fov", settings.foveation.key).put("level", settings.level.key)
            .put("out", settings.outputPercent).put("eye", settings.eyeBuffer.key)
            .put("fg", settings.frameGen.key).put("flow", settings.flowPercent).put("debug", settings.debug)
    }

    /** Controller grip correction for Windows games; no arguments reports the current values. */
    private fun vrGrip(args: Array<String>): JSONObject {
        WindowsVrGripCorrection.apply(args.toList())?.let { throw IllegalArgumentException(it) }
        val settings = WindowsVrGripCorrection.settings
        return JSONObject()
            .put("device", WindowsVrGripCorrection.device)
            .put("pitch", settings.pitch.toDouble()).put("yaw", settings.yaw.toDouble())
            .put("roll", settings.roll.toDouble()).put("x", settings.xMm.toDouble())
            .put("y", settings.yMm.toDouble()).put("z", settings.zMm.toDouble())
    }

    /**
     * Starts a Steam game the same way as its Play button (MainActivity's LAUNCH_GAME intent);
     * `status` or no argument reports progress. `load=<save>` makes this one launch load a Source 2
     * save (`+load <save>`, e.g. `s0/autosave`) instead of stopping at the main menu; the container's
     * own arguments are not changed. Such a launch also presses the trigger when the game asks for it
     * after loading (`trigger=off` leaves it to a person). Android only lets the service start the
     * activity while SteamPSP has a visible window.
     */
    private fun launch(args: Array<String>): JSONObject {
        val usage = "usage: launch <steamAppId> [load=<save> [trigger=auto|off]]|status"
        val action = args.firstOrNull() ?: "status"
        require(args.size <= 3 && (action != "status" || args.size <= 1)) { usage }
        if (action != "status") {
            val appId = action.toIntOrNull()?.takeIf { it > 0 } ?: throw IllegalArgumentException(usage)
            val save = args.getOrNull(1)?.let {
                require(it.startsWith("load=") && XrGameDebugLaunch.isSaveName(it.removePrefix("load="))) { usage }
                it.removePrefix("load=")
            }
            val trigger = args.getOrNull(2)?.let {
                require(it == "trigger=auto" || it == "trigger=off") { usage }
                it == "trigger=auto"
            } ?: true
            require(ActiveGameRegistry.get() == null && ImmersiveXrActivity.current?.get() == null &&
                PluviaApp.xServerView == null) { "game_running" }
            if (save != null) XrGameDebugLaunch.loadSave(appId, save, trigger)
            startActivity(Intent(LAUNCH_GAME).setClass(this, MainActivity::class.java)
                .putExtra("app_id", appId).putExtra("game_source", "STEAM")
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP))
            lastLaunch = appId
        }
        val immersive = ImmersiveXrActivity.current?.get()
        return JSONObject().put("action", action).put("requested", lastLaunch ?: JSONObject.NULL)
            .put("activeSteamAppId", ActiveGameRegistry.get()?.appId ?: JSONObject.NULL)
            .put("xServerPresent", PluviaApp.xServerView != null)
            .put("immersive", immersive?.let { JSONObject(it.debugQuickMenu("status")) } ?: JSONObject.NULL)
            .put("pendingArguments", XrGameDebugLaunch.pendingArguments() ?: JSONObject.NULL)
            .put("autoTrigger", XrGameDebugLaunch.autoTrigger)
    }

    /** Synthetic controller input through the immersive session; see [XrDebugInput]. */
    private fun input(args: Array<String>): JSONObject {
        if (args.firstOrNull() != "status") {
            XrDebugInput.apply(args.toList())?.let { throw IllegalArgumentException(it) }
        } else {
            require(args.size == 1) { "unexpected_arguments" }
        }
        val state = XrDebugInput.last
        val axes = JSONObject()
        XrDebugInput.AXES.forEachIndexed { index, name ->
            if (state.axisMask and (1 shl index) != 0) axes.put(name, state.axes[index].toDouble())
        }
        return JSONObject().put("buttons", JSONArray(XrDebugInput.buttonNames(state.buttons)))
            .put("axes", axes).put("remainingMs", XrDebugInput.remainingMs())
            .put("session", ImmersiveXrActivity.current?.get() != null)
            .put("scope", "merged_into_immersive_controllers_while_session_runs")
    }

    /**
     * Opens, closes or navigates the immersive quick menu; `nav` sends the controller inputs it uses.
     * `exit` confirms the menu's Exit item, so the game exits with the exit cloud sync.
     */
    private fun quickMenu(args: Array<String>): JSONObject {
        val action = args.firstOrNull() ?: "status"
        val navigation = mapOf(
            // The quick menu reads ly > 0 as down (handleMenuNavigation).
            "up" to listOf("axis", "ly", "-1"), "down" to listOf("axis", "ly", "1"),
            "left" to listOf("axis", "lx", "-1"), "right" to listOf("axis", "lx", "1"),
            "ok" to listOf("btn", "a"), "back" to listOf("btn", "b"),
            "next" to listOf("btn", "rb"), "prev" to listOf("btn", "lb"),
        )
        require(
            (action in listOf("status", "open", "close", "toggle", "exit") && args.size <= 1) ||
                (action == "nav" && args.size == 2 && args[1] in navigation),
        ) { "usage: quickmenu [status|open|close|toggle|exit|nav up|down|left|right|ok|back|next|prev]" }
        val activity = ImmersiveXrActivity.current?.get() ?: return JSONObject().put("active", false)
        if (action == "nav") {
            XrDebugInput.apply(navigation.getValue(args[1]) + "150")?.let { throw IllegalArgumentException(it) }
        }
        return JSONObject(activity.debugQuickMenu(action)).put("active", true).put("action", action)
    }

    /** The XR keyboard: show/hide it in the running immersive session; status never includes typed text. */
    private fun vrKeyboard(args: Array<String>): JSONObject {
        val action = args.firstOrNull() ?: "status"
        require(args.size <= 1 && action in listOf("status", "show", "hide")) { "usage: vr_keyboard [status|show|hide]" }
        val activity = ImmersiveXrActivity.current?.get()
        if (action != "status") {
            requireNotNull(activity) { "no_immersive_session" }
            val latch = java.util.concurrent.CountDownLatch(1)
            activity.runOnUiThread {
                if (action == "show") activity.showXrKeyboard() else activity.hideXrKeyboard()
                latch.countDown()
            }
            latch.await(1500, java.util.concurrent.TimeUnit.MILLISECONDS)
        }
        return JSONObject(XrNative.nativeXrKeyboardStatus())
            .put("open", activity?.xrKeyboardOpen)
            .put("session", activity != null)
    }

    /** The immersive status layer: on/off flips its switch (the launch panel's status layer row). */
    private fun vrHud(args: Array<String>): JSONObject {
        val action = args.firstOrNull() ?: "status"
        require(args.size <= 1 && action in listOf("status", "on", "off")) { "usage: vr_hud [status|on|off]" }
        if (action != "status") {
            PrefManager.xrgameStatusLayer = action == "on"
            XrNative.nativeSetPerfHudVisible(action == "on")
        }
        return JSONObject(XrNative.nativePerfHudStatus()).put("statusLayer", PrefManager.xrgameStatusLayer)
            .put("session", ImmersiveXrActivity.current?.get() != null)
            .put("scope", "vulkan_composite_local_space_quad")
    }

    /**
     * Scene readiness of the running Steam game from its Source 2 `-condebug` console log; see
     * [XrGameConsoleState]. Reports state names and save/map targets only, never log lines. A log
     * last written before the running immersive session started belongs to an earlier process
     * (`current` false) and never counts as in scene.
     */
    private fun gameState(): JSONObject {
        val appId = ActiveGameRegistry.get()?.appId ?: lastLaunch
        val immersive = ImmersiveXrActivity.current?.get()
        val result = JSONObject().put("appId", appId ?: JSONObject.NULL)
            .put("session", immersive != null)
            .put("scope", "source2_condebug_console_log")
        val installDir = appId?.let { File(SteamService.getAppDirPath(it)) } ?: return result.put("log", JSONObject.NULL)
        val log = XrGameConsoleState.findLog(installDir) ?: return result.put("log", JSONObject.NULL)
        val state = XrGameConsoleState.read(log)
        val current = PluviaApp.xServerView != null &&
            (immersive == null || log.lastModified() >= immersive.createdAtMs)
        return result.put("log", log.relativeTo(installDir).path).put("bytes", log.length())
            .put("modifiedMs", log.lastModified()).put("current", current)
            .put("launchedAt", state.launchedAt ?: JSONObject.NULL)
            .put("hostState", state.hostState ?: JSONObject.NULL).put("target", state.target ?: JSONObject.NULL)
            .put("pendingState", state.pendingState ?: JSONObject.NULL)
            .put("pendingTarget", state.pendingTarget ?: JSONObject.NULL)
            .put("paused", state.paused).put("inScene", current && state.inScene)
            .put("lastEventAt", state.lastEventAt ?: JSONObject.NULL)
    }

    /**
     * OpenComposite's same-session A/B switch: `full_copy=1` creates the trigger file that makes it
     * copy the whole submitted texture again, `full_copy=0` removes it. The DLL checks it once a second.
     */
    private fun vrOpenComposite(args: Array<String>): JSONObject {
        require(args.size <= 1 && (args.isEmpty() || args[0] in listOf("full_copy=0", "full_copy=1"))) {
            "usage: vr_oc [full_copy=0|1]"
        }
        val appId = ActiveGameRegistry.get()?.appId ?: lastLaunch ?: error("no_active_game")
        val container = ContainerUtils.getContainer(this, "STEAM_$appId")
        val trigger = File(container.rootDir, ".wine/drive_c/gamenative-xr/opencomposite-full-copy")
        when (args.firstOrNull()) {
            "full_copy=1" -> check(trigger.parentFile?.isDirectory == true && (trigger.exists() || trigger.createNewFile()))
            "full_copy=0" -> trigger.delete()
        }
        return JSONObject().put("appId", appId).put("fullCopy", trigger.exists())
            .put("scope", "trigger_file_checked_by_opencomposite_once_a_second")
    }

    /**
     * Guest patches (XrGameGuestPatches): `status [appId]` lists a game's packages, selection and
     * the installer's status; `select <appId> [name,name]` sets the selection for the next launch
     * (no names clears it); `enable|disable [package|hook|package/hook]` switches hooks of the
     * running game (no name: all).
     */
    private fun guestPatch(args: Array<String>): JSONObject {
        val usage = "usage: guest_patch [status [appId]] | select <appId> [name,...] | enable|disable [target]"
        val action = args.firstOrNull() ?: "status"
        fun appId(index: Int): Int = args.getOrNull(index)?.let { requireNotNull(it.toIntOrNull()?.takeIf { id -> id > 0 }) { usage } }
            ?: ActiveGameRegistry.get()?.appId ?: lastLaunch ?: error("no_active_game")
        return when (action) {
            "status" -> {
                require(args.size <= 2) { usage }
                val appId = appId(1)
                XrGameGuestPatches.describe(ContainerUtils.getContainer(this, "STEAM_$appId"), appId)
            }
            "select" -> {
                require(args.size in 2..3) { usage }
                val appId = appId(1)
                val names = args.getOrNull(2)?.split(',')?.filter { it.isNotEmpty() }.orEmpty()
                val container = ContainerUtils.getContainer(this, "STEAM_$appId")
                container.envVars = XrGameGuestPatches.withSelection(container.envVars, names)
                container.saveData()
                XrGameGuestPatches.describe(container, appId).put("scope", "next_launch")
            }
            "enable", "disable" -> {
                require(args.size <= 2) { usage }
                val appId = ActiveGameRegistry.get()?.appId ?: error("no_active_game")
                val container = ContainerUtils.getContainer(this, "STEAM_$appId")
                val sequence = XrGameGuestPatches.control(container, appId, action == "enable", args.getOrNull(1))
                JSONObject().put("appId", appId).put("sequence", sequence)
                    .put("scope", "control_file_polled_by_the_installer_every_250_ms")
            }
            else -> throw IllegalArgumentException(usage)
        }
    }

    private fun runtime(): JSONObject {
        val catalog = assets.open(XrGameComponents.ASSET).use { it.readBytes() }
        val hash = MessageDigest.getInstance("SHA-256").digest(catalog).joinToString("") { "%02x".format(it) }
        val receipts = JSONArray()
        for (entry in XrGameComponents.load(this).items.values.flatten()) {
            val text = readBounded(File(filesDir, "xrgame/installed/${entry.id}.json"), 4096)
            val receipt = text?.let { runCatching { JSONObject(it) }.getOrNull() }
            receipts.put(JSONObject().put("id", entry.id).put("catalogSha256", entry.sha256)
                .put("receiptPresent", text != null)
                .put("receiptMatchesArchive", receipt?.optString("archiveSha256") == entry.sha256))
        }
        return JSONObject().put("catalogSha256", hash).put("components", receipts)
            .put("fullFileVerification", "not_run_in_debug_query")
            .put("defaults", JSONObject().put("wine", XrGameRuntimeVersions.WINE)
                .put("fex", XrGameRuntimeVersions.FEX).put("dxvk", XrGameRuntimeVersions.DXVK)
                .put("vkd3d", XrGameRuntimeVersions.VKD3D).put("turnip", XrGameRuntimeVersions.TURNIP))
    }

    private fun processes(): JSONObject {
        val rows = JSONArray()
        var skipped = 0
        var truncated = false
        val deadline = SystemClock.elapsedRealtime() + 1000
        for (dir in File("/proc").listFiles().orEmpty()) {
            if (dir.name.toIntOrNull() == null) continue
            if (Thread.currentThread().isInterrupted || SystemClock.elapsedRealtime() > deadline || rows.length() >= 128) {
                truncated = true; break
            }
            if (runCatching { Os.stat(dir.path).st_uid }.getOrNull() != Process.myUid()) continue
            val stat = readBounded(File(dir, "stat"), 4096)
            if (stat == null || !stat.contains(") ")) { skipped++; continue }
            val fields = stat.substringAfterLast(") ").trim().split(Regex("\\s+"))
            if (fields.size < 20) { skipped++; continue }
            rows.put(JSONObject().put("pid", dir.name.toInt())
                .put("comm", stat.substringAfter('(').substringBeforeLast(')'))
                .put("state", fields[0]).put("ppid", fields[1].toInt())
                .put("startTimeTicks", fields[19]))
        }
        return JSONObject().put("scope", "same_uid_proc_visible").put("processes", rows)
            .put("skipped", skipped).put("truncated", truncated).put("atomicSnapshot", false)
    }

    private fun present(args: Array<String>): JSONObject {
        require(args.isEmpty() || (args.size == 2 &&
            ((args[0] == "trace" && args[1].toIntOrNull() in 0..3600) ||
             (args[0] in listOf("async_copy", "copy_pipeline") && args[1] in listOf("0", "1"))))) {
            "usage: present [trace 0..3600 | async_copy 0|1 | copy_pipeline 0|1]"
        }
        val view = PluviaApp.xServerView
        val extension = view?.getxServer()?.getExtensionByName("Present") as? PresentExtension
        if (extension == null) return JSONObject().put("active", false)
        if (args.firstOrNull() == "trace") extension.setTraceFrames(args[1].toInt())
        if (args.firstOrNull() == "async_copy") extension.setAsyncCopy(args[1] == "1")
        if (args.firstOrNull() == "copy_pipeline") extension.setCopyPipeline(args[1] == "1")
        return JSONObject(extension.diagnosticSnapshot()).put("active", true)
            .put("renderer", view.renderer.javaClass.simpleName)
            .put("scope", "host_present_queue_not_guest_or_gpu_execution")
    }

    /** The trigger file starts a GFXR trimmed capture at the next frame; removing it stops early. */
    private fun apiCapture(args: Array<String>): JSONObject {
        val action = args.firstOrNull() ?: "status"
        require(action in listOf("status", "start", "stop") && args.size <= 2 &&
            args.getOrNull(1)?.matches(Regex("[A-Za-z0-9_]{1,64}")) != false) {
            "usage: api_capture [status|start|stop] [container]"
        }
        val root = File(com.winlator.xenvironment.ImageFs.find(this).rootDir, "xrgame-captures")
        if (action != "status") {
            val id = args.getOrNull(1) ?: ActiveGameRegistry.get()?.appId?.let { "STEAM_$it" }
                ?: error("no_active_game")
            val dir = File(root, id)
            require(File(dir, "capture.json").isFile) { "capture_not_configured" }
            val trigger = File(dir, "trigger")
            if (action == "start") check(trigger.exists() || trigger.createNewFile()) else trigger.delete()
        }
        val containers = JSONArray()
        for (dir in root.listFiles().orEmpty().filter { it.isDirectory }.sortedBy { it.name }.take(16)) {
            val sidecar = readBounded(File(dir, "capture.json"), 16384)?.let { runCatching { JSONObject(it) }.getOrNull() }
            val traces = JSONArray()
            for (file in dir.listFiles().orEmpty().filter { it.name.endsWith(".gfxr") }.sortedBy { it.name }.takeLast(32)) {
                traces.put(JSONObject().put("name", file.name).put("bytes", file.length()).put("modifiedMs", file.lastModified()))
            }
            containers.put(JSONObject().put("container", dir.name).put("trigger", File(dir, "trigger").exists())
                .put("mode", sidecar?.optString("mode") ?: JSONObject.NULL)
                .put("frames", sidecar?.optInt("frames") ?: JSONObject.NULL).put("traces", traces))
        }
        return JSONObject().put("action", action).put("containers", containers)
            .put("scope", "trigger_files_not_capture_completion")
    }

    /**
     * Starts the WineDbg that tools/xrgame/run-guest-debugger.py staged in the prefix, from this app
     * process so it runs in the game's SELinux domain. Started through run-as it runs as runas_app:
     * on Swan (enforcing) the wineserver's socket fds then never reach it (recv fails with
     * STATUS_TOO_MANY_OPENED_FILES) and the game may not signal it. The target is checked by PID and
     * start time, and WineDbg gets the target's environment. Output goes to
     * files/xrgame-debugger/winedbg.log, which the helper reads with run-as; replies carry no paths.
     */
    private fun guestDebuggerCommand(args: Array<String>): JSONObject {
        val usage = "usage: guest_debugger start target=<pid>,<startTicks> wine=<winePid> " +
            "dbg=<sha16>,<port>[,<flags f|c|s|d>] | status | stop"
        val action = args.firstOrNull() ?: "status"
        val running = guestDebugger?.takeIf { it.isAlive }
        val log = File(File(filesDir, "xrgame-debugger"), "winedbg.log")
        when (action) {
            "status" -> require(args.size <= 1) { usage }
            "stop" -> {
                // A fallback: WineDbg detaches by itself when its RSP client disconnects. It ignores
                // SIGTERM, the only signal Android's Process.destroy*() sends, so it gets a SIGKILL;
                // once it is gone the wineserver drops its debug object, so the debuggee continues
                // (kill on exit is off).
                require(args.size == 1) { usage }
                if (running != null) {
                    killChild(guestDebuggerExe)
                    running.waitFor(1, TimeUnit.SECONDS)
                }
            }
            "start" -> {
                require(args.size == 4) { usage }
                require(running == null) { "debugger_running" }
                val values = args.drop(1).associate { it.substringBefore('=', "") to it.substringAfter('=', "") }
                val target = values["target"]?.split(',')?.takeIf { it.size == 2 } ?: throw IllegalArgumentException(usage)
                val pid = target[0].toIntOrNull()?.takeIf { it > 0 } ?: throw IllegalArgumentException(usage)
                val startTicks = target[1].toLongOrNull() ?: throw IllegalArgumentException(usage)
                val winePid = values["wine"]?.toIntOrNull()?.takeIf { it > 0 } ?: throw IllegalArgumentException(usage)
                val debugger = values["dbg"]?.split(',')?.takeIf { it.size in 2..3 } ?: throw IllegalArgumentException(usage)
                val sha16 = debugger[0].takeIf { Regex("[0-9a-f]{16}").matches(it) } ?: throw IllegalArgumentException(usage)
                val port = debugger[1].toIntOrNull()?.takeIf { it in 1024..65535 } ?: throw IllegalArgumentException(usage)
                val flags = debugger.getOrElse(2) { "" }
                require(flags.all { it in "fcsd" }) { usage }
                require(runCatching { Os.stat("/proc/$pid").st_uid }.getOrNull() == Process.myUid()) { "target_not_found" }
                val stat = readBounded(File("/proc/$pid/stat"), 4096) ?: throw IllegalArgumentException("target_not_found")
                require(stat.substringAfterLast(") ").split(' ').getOrNull(19)?.toLongOrNull() == startTicks) {
                    "target_identity_mismatch"
                }
                val environ = File("/proc/$pid/environ").readBytes().also { require(it.size <= 1 shl 20) { "environment_too_large" } }
                // The game's whole environment: from the app domain the loader can only start through the
                // same no-exec path (WINELOADERNOEXEC, the exec shim in LD_PRELOAD) the game uses.
                val env = String(environ, Charsets.UTF_8).split('\u0000').filter { '=' in it }
                    .associate { it.substringBefore('=') to it.substringAfter('=') }.toMutableMap()
                // An fd the game inherited from its parent; WineDbg connects to the wineserver itself.
                env.remove("WINESERVERSOCKET")
                val prefix = File(env["WINEPREFIX"] ?: throw IllegalArgumentException("no_prefix"))
                val exe = File(prefix, "drive_c/xrgame-debugger/xrgame-winedbg-$sha16.exe")
                require(exe.isFile) { "debugger_not_staged" }
                val digest = MessageDigest.getInstance("SHA-256").let { md ->
                    exe.inputStream().use { input ->
                        val buffer = ByteArray(1 shl 16)
                        while (true) { val n = input.read(buffer); if (n < 0) break; md.update(buffer, 0, n) }
                    }
                    md.digest().joinToString("") { "%02x".format(it) }
                }
                require(digest.startsWith(sha16)) { "debugger_hash_mismatch" }
                // The game's PATH starts with the runtime's bin directory (Proton's), which holds the loader.
                val loader = env["PATH"]?.split(':')?.map { File(it, "wine") }?.firstOrNull { it.isFile }
                    ?: throw IllegalArgumentException("no_wine_loader")
                env["WINEDEBUG"] = if ('d' in flags) "err+all,+winedbg" else "-all"
                env["WINE_GDB_REMOTE_NTDLL_BREAKIN"] = "1"
                env["WINE_GDB_EXPECT_UNIX_PID"] = pid.toString()
                if ('c' in flags) env["WINE_GDB_CONTROL_TRACE"] = "1"
                if ('s' in flags) env["WINE_GDB_SOFTWARE_BREAKPOINTS"] = "1"
                if ('f' in flags) env["WINE_GDB_FEX_EXCEPTIONS"] = "1"
                log.parentFile?.mkdirs()
                guestDebugger = ProcessBuilder("/system/bin/linker64", loader.path,
                    "C:\\xrgame-debugger\\xrgame-winedbg-$sha16.exe", "--gdb", "--no-start", "--port", port.toString(),
                    winePid.toString())
                    .directory(filesDir).redirectErrorStream(true).redirectOutput(log)
                    .also { builder -> builder.environment().clear(); builder.environment().putAll(env) }
                    .start()
                guestDebuggerTarget = pid
                guestDebuggerExe = "xrgame-winedbg-$sha16.exe"
                guestDebuggerStartedMs = SystemClock.elapsedRealtime()
            }
            else -> throw IllegalArgumentException(usage)
        }
        val process = guestDebugger
        val output = readBounded(log, 1 shl 20).orEmpty()
        return JSONObject().put("action", action)
            .put("running", process?.isAlive == true)
            .put("exitCode", process?.takeIf { !it.isAlive }?.exitValue() ?: JSONObject.NULL)
            .put("targetPid", guestDebuggerTarget.takeIf { it > 0 } ?: JSONObject.NULL)
            .put("ageMs", if (guestDebuggerStartedMs > 0) SystemClock.elapsedRealtime() - guestDebuggerStartedMs else JSONObject.NULL)
            .put("listening", "target remote localhost" in output)
            .put("pidVerified", "Verified Wine pid" in output)
            .put("log", "files/xrgame-debugger/winedbg.log")
            .put("scope", "app_process_selinux_domain")
    }

    /** SIGKILLs this process's children whose command line names [exe] (the Wine loader rewrites argv). */
    private fun killChild(exe: String) {
        if (exe.isEmpty()) return
        val self = Process.myPid()
        File("/proc").listFiles { file -> file.name.all(Char::isDigit) }.orEmpty().forEach { dir ->
            val stat = readBounded(File(dir, "stat"), 4096) ?: return@forEach
            val parent = stat.substringAfterLast(") ").split(' ').getOrNull(1)?.toIntOrNull()
            val command = readBounded(File(dir, "cmdline"), 4096) ?: return@forEach
            if (parent == self && exe in command) runCatching { Os.kill(dir.name.toInt(), OsConstants.SIGKILL) }
        }
    }

    private fun readBounded(file: File, limit: Int): String? = runCatching {
        file.inputStream().use { input ->
            val data = ByteArray(limit + 1)
            var size = 0
            while (size < data.size) {
                val n = input.read(data, size, data.size - size)
                if (n < 0) break
                size += n
            }
            if (size > limit) null else String(data, 0, size, Charsets.UTF_8)
        }
    }.getOrNull()
}
