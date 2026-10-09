package app.gamenative.ui.screen.xr.windows

import app.gamenative.ui.screen.xr.XrNative
import com.winlator.container.Container
import com.winlator.core.envvars.EnvVars

/**
 * Composite backend and eye-image reconstruction for Windows VR games.
 *
 * With the Vulkan composite (the app-side Turnip through XR_KHR_vulkan_enable2) the game's eye
 * images are reconstructed to the runtime's recommended size with FSR1 or Qualcomm SGSR1, and the
 * reconstruction pass can be foveated around a fixed centre or the tracked gaze (ETFR). The game's
 * own render scale, poses and FOV are unchanged. The per-game Graphics settings store the defaults
 * in the container's environment variables, like the other XRGame presentation options; DebugBus
 * `vr_upscale` changes them while a game runs.
 */
object WindowsVrUpscale {
    const val ENV_BACKEND = "XRGAME_XR_COMPOSITE"
    const val ENV_FILTER = "XRGAME_XR_UPSCALE"
    const val ENV_SHARPNESS = "XRGAME_XR_SHARPNESS"
    const val ENV_FOVEATION = "XRGAME_XR_FOVEATION"
    const val ENV_LEVEL = "XRGAME_XR_FOVEATION_LEVEL"
    const val ENV_OUTPUT = "XRGAME_XR_OUTPUT_PERCENT"
    const val BACKEND_VULKAN = "vulkan"
    const val BACKEND_GLES = "gles"
    const val DEFAULT_BACKEND = BACKEND_GLES
    val BACKENDS = listOf(BACKEND_GLES, BACKEND_VULKAN)
    val SHARPNESS_STEPS = listOf(0, 25, 50, 75, 100)
    val OUTPUT_STEPS = listOf(50, 60, 70, 80, 90, 100)

    /** Android build of the bundled Turnip, loaded into the app process through adrenotools. */
    const val DRIVER_LIBRARY = "libvulkan_freedreno_android.so"
    const val EYE_TRACKING_PERMISSION = "com.picovr.permission.EYE_TRACKING"

    enum class Filter(val id: Int, val key: String) { OFF(0, "off"), FSR1(1, "fsr1"), SGSR(2, "sgsr") }
    enum class Foveation(val id: Int, val key: String) { OFF(0, "off"), FIXED(1, "fixed"), EYE(2, "eye") }
    enum class Level(val id: Int, val key: String) { LOW(0, "low"), BALANCED(1, "balanced"), HIGH(2, "high") }

    data class Settings(
        val filter: Filter = Filter.SGSR,
        val sharpness: Int = 50,
        val foveation: Foveation = Foveation.EYE,
        val level: Level = Level.BALANCED,
        val outputPercent: Int = 100,
        val debug: Boolean = false,
    )

    @Volatile var settings: Settings = Settings()
        private set

    /** Native switch; replaced in unit tests. */
    internal var sink: (Settings) -> Unit = {
        XrNative.nativeSetUpscale(it.filter.id, it.sharpness, it.foveation.id, it.level.id, it.outputPercent, it.debug)
    }

    fun backend(envVars: String): String = EnvVars(envVars).get(ENV_BACKEND).takeIf { it in BACKENDS } ?: DEFAULT_BACKEND

    fun backend(container: Container): String = backend(container.envVars)

    fun withBackend(envVars: String, backend: String): String = EnvVars(envVars).apply {
        put(ENV_BACKEND, backend.takeIf { it in BACKENDS } ?: DEFAULT_BACKEND)
    }.toString()

    fun fromEnvVars(envVars: String): Settings {
        val vars = EnvVars(envVars)
        val defaults = Settings()
        return Settings(
            filter = Filter.entries.firstOrNull { it.key == vars.get(ENV_FILTER) } ?: defaults.filter,
            sharpness = vars.get(ENV_SHARPNESS).toIntOrNull()?.coerceIn(0, 100) ?: defaults.sharpness,
            foveation = Foveation.entries.firstOrNull { it.key == vars.get(ENV_FOVEATION) } ?: defaults.foveation,
            level = Level.entries.firstOrNull { it.key == vars.get(ENV_LEVEL) } ?: defaults.level,
            outputPercent = vars.get(ENV_OUTPUT).toIntOrNull()?.coerceIn(50, 100) ?: defaults.outputPercent,
        )
    }

    fun fromContainer(container: Container): Settings = fromEnvVars(container.envVars)

    /** Stores everything except the runtime-only debug overlay. */
    fun withSettings(envVars: String, settings: Settings): String = EnvVars(envVars).apply {
        put(ENV_FILTER, settings.filter.key)
        put(ENV_SHARPNESS, settings.sharpness.coerceIn(0, 100))
        put(ENV_FOVEATION, settings.foveation.key)
        put(ENV_LEVEL, settings.level.key)
        put(ENV_OUTPUT, settings.outputPercent.coerceIn(50, 100))
    }.toString()

    @Synchronized
    fun set(newSettings: Settings) {
        sink(newSettings)
        settings = newSettings
    }

    /**
     * Applies `filter=off|fsr1|sgsr`, `sharp=0..100`, `fov=off|fixed|eye`,
     * `level=low|balanced|high`, `out=50..100` and `debug=0|1`. Returns an error token, or null
     * when every argument was valid (then all of them are applied).
     */
    @Synchronized
    fun apply(arguments: List<String>): String? {
        var next = settings
        for (argument in arguments) {
            val separator = argument.indexOf('=')
            if (separator <= 0) return "malformed_argument"
            val value = argument.substring(separator + 1)
            next = when (argument.substring(0, separator)) {
                "filter" -> next.copy(filter = Filter.entries.firstOrNull { it.key == value } ?: return "invalid_filter")
                "sharp" -> next.copy(sharpness = value.toIntOrNull()?.takeIf { it in 0..100 } ?: return "invalid_sharp")
                "fov" -> next.copy(foveation = Foveation.entries.firstOrNull { it.key == value } ?: return "invalid_fov")
                "level" -> next.copy(level = Level.entries.firstOrNull { it.key == value } ?: return "invalid_level")
                "out" -> next.copy(outputPercent = value.toIntOrNull()?.takeIf { it in 50..100 } ?: return "invalid_out")
                "debug" -> next.copy(debug = when (value) {
                    "0" -> false
                    "1" -> true
                    else -> return "invalid_debug"
                })
                else -> return "unknown_argument"
            }
        }
        if (next != settings) set(next)
        return null
    }
}
