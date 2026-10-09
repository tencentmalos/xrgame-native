package app.gamenative.xrgame

import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.Process
import android.os.SystemClock
import android.system.Os
import androidx.annotation.Keep
import app.gamenative.BuildConfig
import app.gamenative.PluviaApp
import app.gamenative.service.ActiveGameRegistry
import app.gamenative.ui.screen.xr.windows.WindowsVrTuning
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
    companion object { init { System.loadLibrary("xrgame_debugbus") } }
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
    fun query(command: String, args: Array<String>): String = try {
        require(command in setOf("present", "api_capture", "vr_tuning") || args.isEmpty()) { "unexpected_arguments" }
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
            "processes" -> processes()
            "present" -> present(args)
            "api_capture" -> apiCapture(args)
            "vr_tuning" -> vrTuning(args)
            else -> error("unknown_provider")
        }
        result.put("schema", 1).put("sampledAtBootNs", SystemClock.elapsedRealtimeNanos()).toString()
    } catch (e: IllegalArgumentException) {
        JSONObject().put("error", e.message ?: "invalid_arguments").toString()
    } catch (e: Exception) {
        // Never return exception messages containing paths, launch arguments or account data.
        JSONObject().put("error", "provider_failed").put("type", e.javaClass.simpleName).toString()
    }

    /** Windows VR pacing/latency experiments; no arguments reports the current values. */
    private fun vrTuning(args: Array<String>): JSONObject {
        WindowsVrTuning.apply(args.toList())?.let { throw IllegalArgumentException(it) }
        return JSONObject()
            .put("pacing", WindowsVrTuning.pacing.name.lowercase())
            .put("startTargetUs", WindowsVrTuning.frameStartTargetUs)
            .put("predict", WindowsVrTuning.extendedPrediction)
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
