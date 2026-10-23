// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.content.res.Resources
import app.gamenative.R
import com.winlator.xconnector.UnixSocketConfig
import com.winlator.xenvironment.components.XServerComponent
import com.winlator.xserver.ScreenInfo
import com.winlator.xserver.XServer
import com.winlator.xserver.Window
import com.winlator.xserver.WindowManager
import com.winlator.xserver.extensions.MITSHMExtension
import com.winlator.xserver.extensions.SyncExtension
import com.winlator.xserver.extensions.DRI3Extension
import com.winlator.xserver.extensions.PresentExtension
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import java.io.File
import java.util.concurrent.TimeUnit
import org.json.JSONArray
import dagger.hilt.android.AndroidEntryPoint
import javax.inject.Inject
import app.gamenative.db.PluviaDatabase
import app.gamenative.xrgame.XrGameOfficialSteam

/** Keeps the official desktop client alive while the existing game Activity is in front. */
@AndroidEntryPoint
class LinuxSteamService : Service() {
    @Inject lateinit var db: PluviaDatabase
    companion object {
        /** The idle status before any session; the transition page does not show it. */
        const val IDLE_STATUS = "Official Steam is stopped"
        val status = MutableStateFlow(IDLE_STATUS)
        /** Where startup is, for the transition page; [status] keeps the text. */
        val phase = MutableStateFlow(LinuxSteamPhase.STOPPED)
        val displayReady = MutableStateFlow(false)
        val xServerState = MutableStateFlow<XServer?>(null)
        /** Whether this session's X server takes CEF's GPU frames (see [LinuxSteamGpu]). */
        val gpuDisplay = MutableStateFlow(false)
        @Volatile var running = false
            private set
        private const val CHANNEL = "linux-steam-session"
        private const val NOTIFICATION = 41016
        private val requests = ArrayDeque<JSONArray>()
        fun x11Trace(): JSONArray = synchronized(requests) { JSONArray(requests.toList()) }
        private fun request(client: Int, sequence: Int, opcode: Int, data: Int, bytes: Int, complete: Boolean) = synchronized(requests) {
            if (requests.size == 64) requests.removeFirst()
            requests.addLast(JSONArray(listOf(client, sequence, opcode, data, bytes, complete)))
        }
    }
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var job: Job? = null
    @Volatile private var child: Process? = null
    /** [child] while it is the client itself, not the installer. */
    @Volatile private var clientProcess: Process? = null
    private var connector: XServerComponent? = null
    private var gameHandoff: SteamGameHandoff? = null

    override fun onBind(intent: Intent?): IBinder? = null
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == "stop") { stopSelf(); return START_NOT_STICKY }
        val notifications = getSystemService(NotificationManager::class.java)
        notifications.createNotificationChannel(NotificationChannel(CHANNEL, getString(R.string.steam_client_notification_channel), NotificationManager.IMPORTANCE_LOW))
        val open = PendingIntent.getActivity(this, 0, Intent(this, LinuxSteamActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        startForeground(NOTIFICATION, Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.stat_notify_sync).setContentTitle("SteamPSP · Official Steam")
            .setContentText(getString(R.string.steam_client_notification)).setContentIntent(open).setOngoing(true).build())
        if (job?.isActive != true) {
            // Select on the service's main thread. A delayed IO coroutine must not
            // switch ownership back after the user has already selected Custom.
            try { XrGameOfficialSteam.selectOfficial() }
            catch (e: IllegalStateException) {
                status.value = e.message ?: "Official Steam could not start"
                phase.value = LinuxSteamPhase.STOPPED
                stopSelf()
                return START_NOT_STICKY
            }
            job = scope.launch { runClient() }
        }
        return START_NOT_STICKY
    }

    private suspend fun runClient() {
        val runtime = LinuxSteamRuntime(this)
        var libraryClaimed = false
        try {
            XrGameOfficialSteam.claimLibrary()
            libraryClaimed = true
            app.gamenative.service.SteamService.suspendForOfficial()
            // Guest processes a killed PRoot detached in an earlier session of this app process.
            LinuxSteamProcesses.stop(null, runtime.home).let { if (it > 0) android.util.Log.w("LinuxSteam", "stopped stale client processes: $it") }
            status.value = getString(R.string.steam_client_preparing)
            phase.value = LinuxSteamPhase.PREPARING
            displayReady.value = false
            val gpu = LinuxSteamGpu.enabled(this)
            runtime.prepare(gpu)
            if (!File(runtime.client, "steampsp-bootstrap.json").isFile) {
                status.value = getString(R.string.steam_client_downloading)
                phase.value = LinuxSteamPhase.DOWNLOADING
                val installer = runtime.process(listOf("/usr/bin/python3", "-u", "/usr/local/lib/steampsp/install-client.py", runtime.client.path), "install.log").start()
                child = installer
                while (currentCoroutineContext().isActive && installer.isAlive) {
                    // Installer output contains component labels, never login data.
                    File(runtime.logs, "install.log").takeIf { it.isFile }?.useLines { lines ->
                        lines.filter { it.startsWith("Downloading ") }.lastOrNull()?.let {
                            status.value = getString(R.string.steam_client_download_component, it.removePrefix("Downloading "))
                        }
                    }
                    delay(1000)
                }
                check(installer.exitValue() == 0) { "Steam download failed; open again to retry verified downloads" }
                child = null
            }
            runtime.prepareClientLinks()
            // Valve updates its own legacycompat directory. Our checked PE facades
            // must live outside that tree or its update invalidates our source hashes.
            val compat = runtime.process(listOf("/usr/bin/python3", "-u", "/usr/local/lib/steampsp/install-client.py",
                "--compat-only", File(runtime.directory, "windows-abi").path), "compat.log").start()
            child = compat
            while (currentCoroutineContext().isActive && compat.isAlive) delay(250)
            check(compat.exitValue() == 0) { "Steam Windows interface preparation failed; open again to retry verified downloads." }
            child = null
            val library = LinuxSteamLibrary(this, db)
            library.prepare()
            SteamSaveSharing.prepareClient(this)
            // Before the first login there is no account to link saves for; see the client loop.
            var sharedAccount = app.gamenative.utils.SteamUtils.getSteam3AccountId()
            gameHandoff = SteamGameHandoff(this).also { it.prepare() }
            val metrics = android.util.DisplayMetrics()
            @Suppress("DEPRECATION")
            getSystemService(android.hardware.display.DisplayManager::class.java)
                .getDisplay(android.view.Display.DEFAULT_DISPLAY)?.getRealMetrics(metrics)
            val (width, height) = SteamClientDisplay.resolution(this@LinuxSteamService,
                app.gamenative.MainActivity.isHeadset(this@LinuxSteamService), metrics.widthPixels, metrics.heightPixels)
            withContext(Dispatchers.Main) {
                val server = XServer(ScreenInfo(width, height), true, false, 24)
                // DesktopHelper recognizes Wine's WM_HINTS window group. Xephyr
                // has no Wine window group, so give its sole desktop window
                // keyboard focus explicitly without a Wine WinHandler.
                server.windowManager.addOnWindowModificationListener(object : WindowManager.OnWindowModificationListener {
                    override fun onMapWindow(window: Window) {
                        // CEF's GPU frames arrive in override-redirect windows; Xephyr keeps focus.
                        if (window.parent == server.windowManager.rootWindow && window.isInputOutput &&
                            !window.attributes.isOverrideRedirect && !window.isInputPassthrough) {
                            server.lock(XServer.Lockable.WINDOW_MANAGER, XServer.Lockable.INPUT_DEVICE).use {
                                server.windowManager.setFocus(window, WindowManager.FocusRevertTo.POINTER_ROOT)
                            }
                        }
                    }
                })
                synchronized(requests) { requests.clear() }
                server.requestObserver = XServer.RequestObserver { client, sequence, opcode, data, bytes, complete ->
                    request(client, sequence, opcode, data, bytes, complete)
                }
                // Linux SysV SHM is absent on Android; this client uses ordinary X11 images.
                // This changes only its dedicated X server, never the game's AHB display.
                // XServer's SparseArray uses the signed Java byte as its key.
                server.extensions.remove(MITSHMExtension.MAJOR_OPCODE.toInt())
                // Our game's SYNC implements Present fences, not the desktop counter
                // protocol; advertising it makes XSyncInitialize abort Steam.
                server.extensions.remove(SyncExtension.MAJOR_OPCODE.toInt())
                // Only the GPU WSI uses DRI3/Present here, as games do; Xephyr does not.
                if (!gpu) {
                    server.extensions.remove(DRI3Extension.MAJOR_OPCODE.toInt())
                    server.extensions.remove(PresentExtension.MAJOR_OPCODE.toInt())
                }
                connector = XServerComponent(server, UnixSocketConfig.createSocket(runtime.root.path, UnixSocketConfig.XSERVER_PATH)).also { it.start() }
                gpuDisplay.value = gpu
                xServerState.value = server
            }
            var restarts = 0
            do {
                status.value = getString(R.string.steam_client_opening)
                phase.value = LinuxSteamPhase.OPENING
                val locales = Resources.getSystem().configuration.locales
                val language = SteamClientLanguage.select((0 until locales.size()).map { locales[it] })
                // launch-client.py owns the session bus too: no dbus-run-session process.
                val builder = runtime.process(listOf(
                    "/usr/bin/python3", "/usr/local/lib/steampsp/launch-client.py", "${runtime.client.path}/steamrtarm64/steam",
                    "-gamepadui", "-language", language, "-clientbeta", "steamdeck_publicbeta", "-no-cef-sandbox", "-cef-ozone-platform=x11") +
                    LinuxSteamGpu.cefArguments(gpu), "client-$restarts.log", gpu)
                builder.environment()["LD_LIBRARY_PATH"] = runtime.clientLibraryPath()
                if (gpu) builder.environment()["XRGAME_STEAM_GPU"] = "1"
                val readyFile = File(runtime.directory, "display-ready")
                readyFile.delete()
                builder.environment()["XRGAME_STEAM_READY_FILE"] = readyFile.path
                builder.environment()["XRGAME_STEAM_SCREEN"] = "${width}x${height}"
                val process = builder.start()
                child = process
                clientProcess = process
                running = true
                status.value = getString(R.string.steam_client_running)
                phase.value = LinuxSteamPhase.STARTING
                var nextLibraryScan = 0L
                while (currentCoroutineContext().isActive && process.isAlive) {
                    if (!displayReady.value && readyFile.isFile) displayReady.value = true
                    // Link the shared saves as soon as the first login records the account, before
                    // Steam AutoCloud resolves a game's save paths into a prefix of its own.
                    if (sharedAccount == null) app.gamenative.utils.SteamUtils.getSteam3AccountId()?.let {
                        sharedAccount = it
                        SteamSaveSharing.prepareClient(this@LinuxSteamService)
                        android.util.Log.i("LinuxSteam", "saves shared after login: conflicts=${SteamSaveSharing.conflicts.size}")
                    }
                    gameHandoff?.poll()
                    if (stopWhenIdle && !gameRunning()) {
                        if (steamPageOpen()) stopWhenIdle = false
                        else {
                            // onDestroy cancels this loop; runClient's finally stops the client tree.
                            android.util.Log.i("LinuxSteam", "no Steam page left: stopping the client")
                            stopWhenIdle = false
                            withContext(Dispatchers.Main) { stopSelf() }
                        }
                    }
                    val now = android.os.SystemClock.elapsedRealtime()
                    if (now >= nextLibraryScan) {
                        library.reconcile()
                        nextLibraryScan = now + 10000
                    }
                    delay(500)
                }
                running = false
                child = null
                clientProcess = null
                // A client that exited on its own, or a PRoot that Android killed, can leave
                // detached guest processes; the next start or a retry must not inherit them.
                LinuxSteamProcesses.stop(null, runtime.home).let { if (it > 0) android.util.Log.w("LinuxSteam", "left processes after client exit: $it") }
                val exit = process.exitValue()
                android.util.Log.i("LinuxSteam", "client exited: $exit")
                if (exit != 42) {
                    status.value = getString(R.string.steam_client_exited, exit)
                    phase.value = LinuxSteamPhase.STOPPED
                    break
                }
                // Valve's client uses 42 after updating, as steam.sh does. Bound update loops.
                check(++restarts <= 3) { "Steam update restarted repeatedly; inspect client logs" }
            } while (currentCoroutineContext().isActive)
        } catch (e: CancellationException) { throw e }
        catch (e: Exception) {
            status.value = e.message ?: "Official Steam could not start"
            phase.value = LinuxSteamPhase.STOPPED
        }
        finally {
            // Cancellation must not interrupt cleanup when returning from the Main dispatcher.
            // Keep the lease until Steam/PRoot has finished flushing its manifests.
            withContext(NonCancellable + Dispatchers.IO) {
                gameHandoff?.close(); gameHandoff = null
                running = false
                displayReady.value = false
                gpuDisplay.value = false
                phase.value = LinuxSteamPhase.STOPPED
                var childStopped = child == null
                try {
                    child?.let { process ->
                        // Let Valve's client save its state first, as its own Exit Steam does.
                        if (process === clientProcess && process.isAlive) {
                            val forwarded = runCatching { runtime.forwardShutdown(5) }.getOrDefault(-1)
                            val exited = process.waitFor(8, TimeUnit.SECONDS)
                            android.util.Log.i("LinuxSteam", "client shutdown: forward=$forwarded exited=$exited")
                        }
                        val signalled = LinuxSteamProcesses.stop(LinuxSteamProcesses.pid(process), runtime.home)
                        check(process.waitFor(3, TimeUnit.SECONDS))
                        android.util.Log.i("LinuxSteam", "client tree stopped: signalled=$signalled")
                        childStopped = true
                    }
                    child = null
                    clientProcess = null
                } finally {
                    try {
                        withContext(Dispatchers.Main) {
                            connector?.stop(); connector = null; xServerState.value = null
                            stopSelf()
                        }
                    } finally {
                        if (libraryClaimed && childStopped) XrGameOfficialSteam.releaseLibrary()
                    }
                }
            }
        }
    }

    /**
     * Closing the window (or swiping the task away) does not end this process, which keeps a
     * foreground service; the client would run on without any page. Stop it once no task shows
     * Steam, or, when the window closed during a game, as soon as that game ends.
     */
    override fun onTaskRemoved(rootIntent: Intent?) {
        super.onTaskRemoved(rootIntent)
        if (!steamPageOpen()) stopWhenIdle = true
    }

    @Volatile private var stopWhenIdle = false

    private fun steamPageOpen(): Boolean = runCatching {
        getSystemService(android.app.ActivityManager::class.java).appTasks.any { task ->
            val info = task.taskInfo
            listOfNotNull(info.baseIntent.component, info.baseActivity, info.topActivity).any {
                it.className == LinuxSteamActivity::class.java.name || it.className == SteamXrActivity::class.java.name ||
                    it.className == "app.gamenative.xrgame.LaunchActivity" || it.className == OfficialSteamGameActivity::class.java.name
            }
        }
    }.getOrDefault(true)

    private fun gameRunning() = app.gamenative.PluviaApp.xEnvironment != null || SteamGameHandoff.pending.value != null

    override fun onDestroy() {
        job?.cancel() // runClient's finally stops the client tree before releasing the library.
        connector?.stop(); connector = null; xServerState.value = null; running = false
        scope.cancel()
        super.onDestroy()
    }
}

/** Startup steps of the official client, in order. */
enum class LinuxSteamPhase { PREPARING, DOWNLOADING, OPENING, STARTING, STOPPED }
