// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import android.content.Context
import android.view.KeyEvent
import app.gamenative.externaldisplay.IMEInputReceiver
import com.winlator.xserver.XServer
import com.winlator.xserver.XKeycode
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.Channel
import java.util.concurrent.TimeUnit

/** Serialize text commits and editing keys; a paused Activity cannot paste later. */
class SteamImeSession(context: Context, private val server: XServer, private val failed: () -> Unit) : AutoCloseable {
    companion object {
        /**
         * Whether a key the IME passes on reaches Steam, as one press and release on its down event.
         * Pico's IME forwards only the down event of keys it does not handle (arrows, for one),
         * which would otherwise leave the key held in the guest.
         */
        fun forwardsKey(event: KeyEvent) = event.action == KeyEvent.ACTION_DOWN
    }

    private val runtime = LinuxSteamRuntime(context.applicationContext)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    // A streamed composition sends one Backspace per replaced character, often behind a paste.
    private val operations = Channel<() -> Unit>(256)
    @Volatile private var process: Process? = null
    @Volatile private var closed = false

    init {
        scope.launch {
            for (operation in operations) {
                if (closed) break
                try { operation() } catch (e: Exception) {
                    android.util.Log.w("SteamIme", "input operation failed: ${e.javaClass.simpleName}")
                    if (!closed) withContext(Dispatchers.Main) { failed() }
                }
            }
        }
    }

    private fun enqueue(operation: () -> Unit) {
        if (!closed && operations.trySend(operation).isFailure) failed()
    }

    /**
     * Text into the focused Steam field: printable ASCII as key presses, anything else (Chinese,
     * say) through the client's clipboard and Ctrl+V. Queued behind earlier input; any thread.
     */
    fun commit(text: String) = enqueue {
        check(text.toByteArray(Charsets.UTF_8).size <= 16384)
        if (text.all { it.code in 32..126 || it == '\n' }) {
            val typed = text.count { IMEInputReceiver.typeCharacter(server, it) }
            android.util.Log.i("SteamIme", "typed $typed of ${text.length} characters as keys")
            return@enqueue
        }
        android.util.Log.i("SteamIme", "pasting ${text.length} characters")
        val child = runtime.process(listOf("/usr/bin/python3", "/usr/local/lib/steampsp/ime-text.py"), "ime-status.log")
            .redirectOutput(ProcessBuilder.Redirect.PIPE).redirectError(java.io.File("/dev/null")).start()
        process = child
        val timeout = scope.launch { delay(5000); LinuxSteamProcesses.stopTree(child) }
        try {
            if (closed) return@enqueue
            child.outputStream.use { it.write(text.toByteArray(Charsets.UTF_8)) }
            check(child.inputStream.bufferedReader().readLine() == "ready")
            if (closed) return@enqueue
            server.injectKeyPress(XKeycode.KEY_CTRL_L, 0)
            IMEInputReceiver.pressKey(server, XKeycode.KEY_V)
            server.injectKeyRelease(XKeycode.KEY_CTRL_L)
            check(child.waitFor(5, TimeUnit.SECONDS) && child.exitValue() == 0)
        } finally { timeout.cancel(); LinuxSteamProcesses.stopTree(child); process = null }
    }

    /** One press and release of [key], queued behind earlier input. Any thread. */
    fun press(key: XKeycode) = enqueue { IMEInputReceiver.pressKey(server, key) }

    fun receiver(context: Context) = IMEInputReceiver(context, server,
        textHandler = { text -> commit(text) },
        keyHandler = { key -> press(key) },
        keyEventHandler = { event ->
            if (forwardsKey(event)) enqueue {
                server.keyboard.onVirtualKeyEvent(event)
                server.keyboard.onVirtualKeyEvent(KeyEvent.changeAction(event, KeyEvent.ACTION_UP))
            }
            true
        },
        // The IME sees these before Activity.dispatchKeyEvent while it is open.
        dismissKeys = setOf(KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_BUTTON_B,
            KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_F6),
        // Pico's IME composes every letter and commits only at the end; show the letters as they come.
        streamComposition = true)

    override fun close() {
        closed = true
        process?.let(LinuxSteamProcesses::stopTree)
        operations.cancel()
        scope.cancel()
    }
}
