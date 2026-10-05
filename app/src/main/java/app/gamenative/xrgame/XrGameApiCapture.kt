package app.gamenative.xrgame

import android.content.Context
import android.os.Build
import app.gamenative.BuildConfig
import com.winlator.container.Container
import com.winlator.core.envvars.EnvVars
import com.winlator.xenvironment.ImageFs
import org.json.JSONObject
import java.io.File
import java.security.MessageDigest
import java.time.OffsetDateTime

/**
 * GFXReconstruct capture for private validation (docs/specs/xrgame-native-api-replay-v1.md).
 *
 * D3D12 is recorded at the API level by GFXR's d3d12/dxgi proxies. Other APIs are recorded after DXVK's
 * translation by GFXR's Vulkan layer. The binaries ship only in debug APKs that staged `build/xrgame-gfxr`.
 * Settings are container environment variables, so the existing per-game editor can change them.
 */
object XrGameApiCapture {
    const val MODE_ENV = "XRGAME_API_CAPTURE"
    const val FRAMES_ENV = "XRGAME_API_CAPTURE_FRAMES"
    const val MEMORY_ENV = "XRGAME_API_CAPTURE_MEMORY"
    const val LAYER_NAME = "VK_LAYER_LUNARG_gfxreconstruct"
    const val DEFAULT_FRAMES = 3
    val MODES = listOf("off", "d3d12", "vulkan")
    private val MEMORY_MODES = setOf("page_guard", "unassisted", "assisted")
    private const val ASSETS = "xrgame-gfxr"

    fun mode(envVars: String): String = EnvVars(envVars).get(MODE_ENV).takeIf { it in MODES } ?: "off"

    fun withMode(envVars: String, mode: String): String = EnvVars(envVars).apply {
        require(mode in MODES) { "Unknown capture mode: $mode" }
        if (mode == "off") remove(MODE_ENV) else put(MODE_ENV, mode)
    }.toString()

    fun available(context: Context): Boolean =
        BuildConfig.XRGAME && runCatching { context.assets.list(ASSETS).orEmpty().contains("manifest.json") }.getOrDefault(false)

    /**
     * With page_guard, D3D12 writes went to GFXR's shadow allocations and never reached the VKD3D heaps under
     * Wine/FEX: MHR rendered black on the device and in the Windows replay. unassisted renders correctly
     * (docs/validation/api-replay-20261004.md). The Vulkan layer is untested and keeps GFXR's default.
     */
    fun defaultMemoryTracking(mode: String): String = if (mode == "d3d12") "unassisted" else "page_guard"

    /** Traces, trigger, log and sidecar live in app-private storage; Wine sees it as Z:\xrgame-captures. */
    fun captureDir(imageFs: ImageFs, containerId: String): File = File(imageFs.rootDir, "xrgame-captures/$containerId")

    /**
     * GFXR settings for one launch. Windows paths are used by the D3D12 proxies, Unix paths by the Vulkan layer,
     * which reads GFXRECON_* from the environment through the xrgame-wine-capture patch.
     */
    fun environment(mode: String, frames: Int, memory: String, unixDir: String, containerId: String): Map<String, String> {
        require(mode == "d3d12" || mode == "vulkan") { "Capture is off" }
        require(frames in 1..600) { "Capture frames must be 1..600" }
        require(memory in MEMORY_MODES) { "Unknown memory tracking mode: $memory" }
        require(containerId.matches(Regex("[A-Za-z0-9_]{1,64}"))) { "Invalid container id" }
        fun path(name: String) = if (mode == "d3d12") "Z:\\xrgame-captures\\$containerId\\$name" else "$unixDir/$name"
        val env = linkedMapOf(
            "GFXRECON_CAPTURE_FILE" to path("capture.gfxr"),
            "GFXRECON_CAPTURE_TRIGGER_FILE" to path("trigger"),
            "GFXRECON_CAPTURE_TRIGGER_FRAMES" to frames.toString(),
            "GFXRECON_CAPTURE_COMPRESSION_TYPE" to "ZSTD",
            "GFXRECON_MEMORY_TRACKING_MODE" to memory,
            "GFXRECON_LOG_FILE" to path("gfxrecon.log"),
            "GFXRECON_LOG_LEVEL" to "info",
        )
        if (mode == "d3d12") env["GFXRECON_DX_RUNTIME_DIR"] = "C:\\xrgame\\gfxr\\runtime"
        return env
    }

    /**
     * Run after XrGameRuntime.installGraphics, which restores system32 from the verified runtime on every launch.
     * Without capture this leaves nothing behind; with D3D12 capture the real runtimes move to C:\xrgame\gfxr\runtime.
     */
    fun install(context: Context, container: Container) {
        val mode = mode(container.envVars)
        if (mode == "off") return
        check(available(context)) { "This APK does not include the GFXReconstruct capture files" }
        val manifest = JSONObject(context.assets.open("$ASSETS/manifest.json").use { it.readBytes().decodeToString() })
        val files = manifest.getJSONObject("files")
        if (mode == "d3d12") {
            val system32 = File(container.rootDir, ".wine/drive_c/windows/system32")
            val runtime = File(container.rootDir, ".wine/drive_c/xrgame/gfxr/runtime")
            check(runtime.mkdirs() || runtime.isDirectory)
            // installGraphics has just verified these as the VKD3D and DXVK runtimes.
            File(system32, "d3d12.dll").copyTo(File(runtime, "d3d12_ms.dll"), overwrite = true)
            File(system32, "dxgi.dll").copyTo(File(runtime, "dxgi_ms.dll"), overwrite = true)
            for (name in listOf("d3d12.dll", "dxgi.dll", "d3d12_capture.dll")) {
                extract(context, "win-x64/$name", File(system32, name), files.getString("win-x64/$name"))
            }
        } else {
            val layer = File(context.filesDir, "xrgame-gfxr/libVkLayer_gfxreconstruct.so")
            val name = "android-arm64/libVkLayer_gfxreconstruct.so"
            extract(context, name, layer, files.getString(name))
            val imageFs = ImageFs.find(context)
            val json = File(imageFs.shareDir, "vulkan/explicit_layer.d/VkLayer_gfxreconstruct.json")
            check(json.parentFile!!.mkdirs() || json.parentFile!!.isDirectory)
            json.writeText(JSONObject().put("file_format_version", "1.2.0").put("layer", JSONObject()
                .put("name", LAYER_NAME).put("type", "GLOBAL").put("library_path", layer.absolutePath)
                .put("api_version", "1.4.0").put("implementation_version", "1")
                .put("description", "GFXReconstruct capture layer (xrgame private validation)")).toString(2))
        }
    }

    /** Run after the container environment has been merged into the launch environment. */
    fun configure(context: Context, container: Container, env: EnvVars) {
        val mode = mode(container.envVars)
        if (mode == "off" || !available(context)) return
        val containerEnv = EnvVars(container.envVars)
        val frames = containerEnv.get(FRAMES_ENV).toIntOrNull() ?: DEFAULT_FRAMES
        val memory = containerEnv.get(MEMORY_ENV).ifEmpty { defaultMemoryTracking(mode) }
        val imageFs = ImageFs.find(context)
        val dir = captureDir(imageFs, container.id)
        check(dir.mkdirs() || dir.isDirectory)
        // A trigger left by an earlier session would start capturing during startup.
        File(dir, "trigger").delete()
        val settings = environment(mode, frames, memory, dir.absolutePath, container.id)
        for ((key, value) in settings) env.put(key, value)
        if (mode == "vulkan") {
            val layers = env.get("VK_INSTANCE_LAYERS").split(':').filter { it.isNotEmpty() && it != LAYER_NAME }
            env.put("VK_INSTANCE_LAYERS", (layers + LAYER_NAME).joinToString(":"))
        }
        File(dir, "capture.json").writeText(sidecar(context, container, mode, frames, memory, settings).toString(2) + "\n")
    }

    private fun sidecar(context: Context, container: Container, mode: String, frames: Int, memory: String,
                        settings: Map<String, String>): JSONObject {
        val catalog = context.assets.open(XrGameComponents.ASSET).use { sha256(it.readBytes()) }
        val gfxr = JSONObject(context.assets.open("$ASSETS/manifest.json").use { it.readBytes().decodeToString() })
        return JSONObject().put("schema", "xrgame.api-replay.capture/1")
            .put("createdAt", OffsetDateTime.now().toString())
            .put("container", container.id).put("executable", container.executablePath)
            .put("mode", mode).put("frames", frames).put("memoryTracking", memory)
            .put("apk", JSONObject().put("versionName", BuildConfig.VERSION_NAME).put("catalogSha256", catalog))
            .put("defaults", JSONObject().put("wine", XrGameRuntimeVersions.WINE).put("fex", XrGameRuntimeVersions.FEX)
                .put("dxvk", XrGameRuntimeVersions.DXVK).put("vkd3d", XrGameRuntimeVersions.VKD3D)
                .put("turnip", XrGameRuntimeVersions.TURNIP))
            .put("gfxr", gfxr)
            .put("device", JSONObject().put("model", Build.MODEL).put("build", Build.FINGERPRINT)
                .put("bootId", runCatching { File("/proc/sys/kernel/random/boot_id").readText().trim() }.getOrNull()))
            .put("environment", JSONObject(settings as Map<*, *>))
    }

    private fun extract(context: Context, asset: String, target: File, sha256: String) {
        if (XrGameComponents.verify(target, sha256)) return
        check(target.parentFile!!.mkdirs() || target.parentFile!!.isDirectory)
        val partial = File.createTempFile(".xrgame-gfxr-", ".part", target.parentFile)
        try {
            context.assets.open("$ASSETS/$asset").use { input -> partial.outputStream().use { input.copyTo(it) } }
            check(XrGameComponents.verify(partial, sha256)) { "Bundled GFXReconstruct file is corrupt: $asset" }
            check(partial.renameTo(target) || (target.delete() && partial.renameTo(target))) { "Cannot install $asset" }
        } finally { partial.delete() }
    }

    private fun sha256(data: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(data).joinToString("") { "%02x".format(it) }
}
