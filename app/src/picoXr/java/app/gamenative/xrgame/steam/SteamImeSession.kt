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
    private val runtime = LinuxSteamRuntime(context.applicationContext)
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val operations = Channel<() -> Unit>(32)
    @Volatile private var process: Process? = null
    @Volatile private var closed = false

    init {
        scope.launch {
            for (operation in operations) {
                if (closed) break
                try { operation() } catch (_: Exception) {
                    if (!closed) withContext(Dispatchers.Main) { failed() }
                }
            }
        }
    }

    private fun enqueue(operation: () -> Unit) {
        if (!closed && operations.trySend(operation).isFailure) failed()
    }

    fun receiver(context: Context) = IMEInputReceiver(context, server,
        textHandler = { text -> enqueue {
            check(text.toByteArray(Charsets.UTF_8).size <= 16384)
            if (text.all { it.code in 32..126 || it == '\n' }) {
                text.forEach { IMEInputReceiver.typeCharacter(server, it) }
                return@enqueue
            }
            val child = runtime.process(listOf("/usr/bin/python3", "/usr/local/lib/steampsp/ime-text.py"), "ime-status.log")
                .redirectOutput(ProcessBuilder.Redirect.PIPE).redirectError(java.io.File("/dev/null")).start()
            process = child
            val timeout = scope.launch { delay(5000); child.destroy() }
            try {
                if (closed) return@enqueue
                child.outputStream.use { it.write(text.toByteArray(Charsets.UTF_8)) }
                check(child.inputStream.bufferedReader().readLine() == "ready")
                if (closed) return@enqueue
                server.injectKeyPress(XKeycode.KEY_CTRL_L, 0)
                IMEInputReceiver.pressKey(server, XKeycode.KEY_V)
                server.injectKeyRelease(XKeycode.KEY_CTRL_L)
                check(child.waitFor(5, TimeUnit.SECONDS) && child.exitValue() == 0)
            } finally { timeout.cancel(); child.destroy(); process = null }
        } },
        keyHandler = { key -> enqueue { IMEInputReceiver.pressKey(server, key) } },
        keyEventHandler = { event -> enqueue { server.keyboard.onVirtualKeyEvent(event) }; true },
        // The IME sees these before Activity.dispatchKeyEvent while it is open.
        dismissKeys = setOf(KeyEvent.KEYCODE_BACK, KeyEvent.KEYCODE_BUTTON_B,
            KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_F6))

    override fun close() {
        closed = true
        process?.destroy()
        operations.cancel()
        scope.cancel()
    }
}
