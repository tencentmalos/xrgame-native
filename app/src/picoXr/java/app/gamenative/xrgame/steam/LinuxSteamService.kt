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
        val status = MutableStateFlow("Official Steam is stopped")
        val displayReady = MutableStateFlow(false)
        val xServerState = MutableStateFlow<XServer?>(null)
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
            status.value = getString(R.string.steam_client_preparing)
            displayReady.value = false
            runtime.prepare()
            if (!File(runtime.client, "steampsp-bootstrap.json").isFile) {
                status.value = getString(R.string.steam_client_downloading)
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
            gameHandoff = SteamGameHandoff(this).also { it.prepare() }
            val metrics = android.util.DisplayMetrics()
            @Suppress("DEPRECATION")
            getSystemService(android.hardware.display.DisplayManager::class.java)
                .getDisplay(android.view.Display.DEFAULT_DISPLAY)?.getRealMetrics(metrics)
            val (width, height) = SteamClientDisplay.resolution(metrics.widthPixels, metrics.heightPixels)
            withContext(Dispatchers.Main) {
                val server = XServer(ScreenInfo(width, height), true, false, 24)
                // DesktopHelper recognizes Wine's WM_HINTS window group. Xephyr
                // has no Wine window group, so give its sole desktop window
                // keyboard focus explicitly without a Wine WinHandler.
                server.windowManager.addOnWindowModificationListener(object : WindowManager.OnWindowModificationListener {
                    override fun onMapWindow(window: Window) {
                        if (window.parent == server.windowManager.rootWindow && window.isInputOutput) {
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
                // This server displays a software-rendered desktop client. Our
                // game's SYNC implements Present fences, not the desktop counter
                // protocol; advertising it makes XSyncInitialize abort Steam.
                server.extensions.remove(SyncExtension.MAJOR_OPCODE.toInt())
                server.extensions.remove(DRI3Extension.MAJOR_OPCODE.toInt())
                server.extensions.remove(PresentExtension.MAJOR_OPCODE.toInt())
                connector = XServerComponent(server, UnixSocketConfig.createSocket(runtime.root.path, UnixSocketConfig.XSERVER_PATH)).also { it.start() }
                xServerState.value = server
            }
            var restarts = 0
            do {
                status.value = getString(R.string.steam_client_opening)
                val locales = Resources.getSystem().configuration.locales
                val language = SteamClientLanguage.select((0 until locales.size()).map { locales[it] })
                val builder = runtime.process(listOf("/usr/bin/dbus-run-session", "--",
                    "/usr/bin/python3", "/usr/local/lib/steampsp/launch-client.py", "${runtime.client.path}/steamrtarm64/steam",
                    "-gamepadui", "-language", language, "-clientbeta", "steamdeck_publicbeta", "-no-cef-sandbox", "-cef-ozone-platform=x11", "-cef-disable-gpu"), "client-$restarts.log")
                builder.environment()["LD_LIBRARY_PATH"] = "${runtime.client.path}/steamrtarm64:${runtime.client.path}/steamrtarm64/steam-runtime/usr/lib/aarch64-linux-gnu"
                val readyFile = File(runtime.directory, "display-ready")
                readyFile.delete()
                builder.environment()["XRGAME_STEAM_READY_FILE"] = readyFile.path
                builder.environment()["XRGAME_STEAM_SCREEN"] = "${width}x${height}"
                val process = builder.start()
                child = process
                running = true
                status.value = getString(R.string.steam_client_running)
                var nextLibraryScan = 0L
                while (currentCoroutineContext().isActive && process.isAlive) {
                    if (!displayReady.value && readyFile.isFile) displayReady.value = true
                    gameHandoff?.poll()
                    val now = android.os.SystemClock.elapsedRealtime()
                    if (now >= nextLibraryScan) {
                        library.reconcile()
                        nextLibraryScan = now + 10000
                    }
                    delay(500)
                }
                running = false
                child = null
                val exit = process.exitValue()
                if (exit != 42) { status.value = getString(R.string.steam_client_exited, exit); break }
                // Valve's client uses 42 after updating, as steam.sh does. Bound update loops.
                check(++restarts <= 3) { "Steam update restarted repeatedly; inspect client logs" }
            } while (currentCoroutineContext().isActive)
        } catch (e: CancellationException) { throw e }
        catch (e: Exception) { status.value = e.message ?: "Official Steam could not start" }
        finally {
            // Cancellation must not interrupt cleanup when returning from the Main dispatcher.
            // Keep the lease until Steam/PRoot has finished flushing its manifests.
            withContext(NonCancellable + Dispatchers.IO) {
                gameHandoff?.close(); gameHandoff = null
                running = false
                displayReady.value = false
                var childStopped = child == null
                try {
                    child?.let { process ->
                        process.destroy()
                        if (!process.waitFor(3, TimeUnit.SECONDS)) {
                            process.destroyForcibly()
                            check(process.waitFor(3, TimeUnit.SECONDS))
                        }
                        childStopped = true
                    }
                    child = null
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

    override fun onDestroy() {
        job?.cancel()
        child?.destroy() // runClient's finally awaits teardown before releasing the library.
        connector?.stop(); connector = null; xServerState.value = null; running = false
        scope.cancel()
        super.onDestroy()
    }
}
