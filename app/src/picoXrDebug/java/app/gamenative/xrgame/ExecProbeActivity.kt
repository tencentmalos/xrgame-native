package app.gamenative.xrgame

import android.app.Activity
import android.os.Bundle
import android.widget.TextView
import org.json.JSONObject
import java.io.File
import java.util.UUID
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/** Fixed debug probe launched by adb shell; accepts no commands or filesystem paths. */
class ExecProbeActivity : Activity() {
    companion object { private val running = AtomicBoolean(false) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val status = TextView(this).also { it.text = "XRGame execution probe"; setContentView(it) }
        if (!running.compareAndSet(false, true)) {
            status.text = "Probe already running"
            return
        }
        val directory = File(filesDir, "xrgame-probes/${UUID.randomUUID()}")
        directory.mkdirs()
        val metadata = JSONObject()
            .put("schemaVersion", 1).put("targetSdk", applicationInfo.targetSdkVersion)
            .put("androidSdk", android.os.Build.VERSION.SDK_INT)
            .put("appPid", android.os.Process.myPid()).put("appUid", android.os.Process.myUid())
            .put("startedElapsedNs", android.os.SystemClock.elapsedRealtimeNanos())
        Thread({
            var child: Process? = null
            try {
                val executable = File(applicationInfo.nativeLibraryDir, "libxrgame_exec_probe.so")
                child = ProcessBuilder("/system/bin/linker64", executable.absolutePath, directory.absolutePath)
                    .redirectErrorStream(true).redirectOutput(File(directory, "probe.jsonl")).start()
                if (child.waitFor(45, TimeUnit.SECONDS)) {
                    metadata.put("exitCode", child.exitValue())
                } else {
                    child.destroyForcibly()
                    metadata.put("timeout", true)
                }
            } catch (error: Exception) {
                metadata.put("errorType", error.javaClass.simpleName)
            } finally {
                metadata.put("endedElapsedNs", android.os.SystemClock.elapsedRealtimeNanos())
                File(directory, "activity.json").writeText(metadata.toString(2))
                running.set(false)
                runOnUiThread { status.text = "Probe finished: ${directory.name}" }
            }
        }, "XRGameExecProbe").start()
    }
}
