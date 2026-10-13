package app.gamenative.ui.screen.xr.windows

import app.gamenative.ui.screen.xr.XrNative
import com.winlator.container.Container
import com.winlator.core.envvars.EnvVars
import kotlin.math.roundToInt

/**
 * Composite backend and reconstruction for XR sessions: Windows VR games and the 2D big screen.
 *
 * With the Vulkan composite (the app-side Turnip through XR_KHR_vulkan_enable2) the game's eye
 * images are reconstructed to the runtime's recommended size with FSR1 or Qualcomm SGSR1, and the
 * reconstruction pass can be foveated around a fixed centre or the tracked gaze (ETFR). The game's
 * own render scale, poses and FOV are unchanged; the eye size follows the XR output tier
 * ([EyeBuffer], as shadPS4's XR output resolution). The 2D big screen is a quad layer that the XR
 * compositor draws into its own eyes, so what it can show is bounded by how many eye pixels the
 * screen covers ([screenCoverage]): about 1:1 at the recommended eye size, at most at the eye
 * limit. Its layer follows the same tier within that range ([screenSwapchain]), and the filter
 * reconstructs the Wine screen into it only when the Wine screen is smaller, foveated where the gaze
 * meets the screen. Frame generation (Foundation's Lossless FG) interpolates the big screen's game
 * frames at the Wine screen's size before that reconstruction, so real and generated frames take
 * the same filter; [FrameGen] caps the output at a multiple of the game's frame rate, and the
 * output follows the display rate within it. The launch panel stores the choices in the
 * container's environment variables, like the other XRGame presentation options; DebugBus
 * `vr_upscale` changes them while a game runs.
 */
object WindowsVrUpscale {
    const val ENV_BACKEND = "XRGAME_XR_COMPOSITE"
    const val ENV_FILTER = "XRGAME_XR_UPSCALE"
    const val ENV_SHARPNESS = "XRGAME_XR_SHARPNESS"
    const val ENV_FOVEATION = "XRGAME_XR_FOVEATION"
    const val ENV_LEVEL = "XRGAME_XR_FOVEATION_LEVEL"
    const val ENV_OUTPUT = "XRGAME_XR_OUTPUT_PERCENT"
    const val ENV_EYE_BUFFER = "XRGAME_XR_EYE_BUFFER"
    const val ENV_FRAME_GEN = "XRGAME_XR_FRAMEGEN"
    const val ENV_FLOW = "XRGAME_XR_FRAMEGEN_FLOW"
    const val BACKEND_VULKAN = "vulkan"
    const val BACKEND_GLES = "gles"
    const val DEFAULT_BACKEND = BACKEND_GLES
    val BACKENDS = listOf(BACKEND_GLES, BACKEND_VULKAN)
    val SHARPNESS_STEPS = listOf(0, 25, 50, 75, 100)
    val OUTPUT_STEPS = listOf(50, 60, 70, 80, 90, 100)
    val FLOW_STEPS = listOf(25, 50, 75, 100)
    private const val SCREEN_MIN_WIDTH = 1280
    private const val SCREEN_MAX_WIDTH = 3840
    private const val SCREEN_MAX_HEIGHT = 2160

    /** Android build of the bundled Turnip, loaded into the app process through adrenotools. */
    const val DRIVER_LIBRARY = "libvulkan_freedreno_android.so"
    const val EYE_TRACKING_PERMISSION = "com.picovr.permission.EYE_TRACKING"

    enum class Filter(val id: Int, val key: String) { OFF(0, "off"), FSR1(1, "fsr1"), SGSR(2, "sgsr") }
    enum class Foveation(val id: Int, val key: String) { OFF(0, "off"), FIXED(1, "fixed"), EYE(2, "eye") }
    enum class Level(val id: Int, val key: String) { LOW(0, "low"), BALANCED(1, "balanced"), HIGH(2, "high") }
    /** XR output tiers: the runtime's recommended eye size, halfway to its limit, or the limit. */
    enum class EyeBuffer(val id: Int, val key: String) { RECOMMENDED(0, "recommended"), HIGH(1, "high"), MAXIMUM(2, "maximum") }
    /** Big-screen frame generation: the largest output multiple of the game's frame rate. */
    enum class FrameGen(val multiplier: Int, val key: String) { OFF(1, "off"), X2(2, "2x"), X3(3, "3x"), X4(4, "4x") }

    /**
     * The headset's eye views as an XR session reported them (recommended and largest per-eye
     * size, left eye FOV tangents); kept in [app.gamenative.PrefManager.xrDisplayInfo] so the
     * launch panel can show the eye sizes before a session starts.
     */
    data class Display(
        val recommendedWidth: Int,
        val recommendedHeight: Int,
        val maxWidth: Int,
        val maxHeight: Int,
        val tanLeft: Float,
        val tanRight: Float,
    ) {
        fun encode(): String = listOf(recommendedWidth, recommendedHeight, maxWidth, maxHeight, tanLeft, tanRight).joinToString(",")

        companion object {
            fun decode(text: String): Display? {
                val parts = text.split(",")
                if (parts.size != 6) return null
                val sizes = parts.take(4).map { it.toIntOrNull()?.takeIf { value -> value > 0 } ?: return null }
                val tangents = parts.drop(4).map { it.toFloatOrNull() ?: return null }
                if (tangents[1] <= tangents[0]) return null
                return Display(sizes[0], sizes[1], sizes[2], sizes[3], tangents[0], tangents[1])
            }

            /** From XrNative.nativeXrDisplayInfo: [rec w, rec h, max w, max h, tan l, r, u, d]. */
            fun fromNative(values: FloatArray?): Display? {
                if (values == null || values.size < 6) return null
                return decode(listOf(values[0].toInt(), values[1].toInt(), values[2].toInt(), values[3].toInt(), values[4], values[5]).joinToString(","))
            }
        }
    }

    data class Settings(
        val filter: Filter = Filter.SGSR,
        val sharpness: Int = 50,
        val foveation: Foveation = Foveation.EYE,
        val level: Level = Level.BALANCED,
        val outputPercent: Int = 100,
        val eyeBuffer: EyeBuffer = EyeBuffer.RECOMMENDED,
        val frameGen: FrameGen = FrameGen.OFF,
        val flowPercent: Int = 50,
        val debug: Boolean = false,
    )

    @Volatile var settings: Settings = Settings()
        private set

    /** Native switch; replaced in unit tests. */
    internal var sink: (Settings) -> Unit = {
        XrNative.nativeSetUpscale(
            it.filter.id, it.sharpness, it.foveation.id, it.level.id, it.outputPercent, it.eyeBuffer.id,
            it.frameGen.multiplier, it.flowPercent, it.debug,
        )
    }

    fun backend(envVars: String): String = EnvVars(envVars).get(ENV_BACKEND).takeIf { it in BACKENDS } ?: DEFAULT_BACKEND

    fun backend(container: Container): String = backend(container.envVars)

    /** The composite an XR session uses: the stored choice, else GLES for VR games and Vulkan for the big screen. */
    fun compositeBackend(envVars: String, vrGame: Boolean): String =
        EnvVars(envVars).get(ENV_BACKEND).takeIf { it in BACKENDS } ?: if (vrGame) DEFAULT_BACKEND else BACKEND_VULKAN

    /** Per-eye size of [tier]; each axis on its own, as the projection takes any buffer aspect. */
    fun eyeExtent(display: Display, tier: EyeBuffer): Pair<Int, Int> {
        fun axis(suggested: Int, maximum: Int): Int {
            val base = minOf(suggested, maximum)
            return when (tier) {
                EyeBuffer.RECOMMENDED -> base
                EyeBuffer.HIGH -> base + (maximum - base) / 2
                EyeBuffer.MAXIMUM -> maximum
            }
        }
        return axis(display.recommendedWidth, display.maxWidth) to axis(display.recommendedHeight, display.maxHeight)
    }

    /**
     * How many pixels across an eye of [tier] has over a screen [screenWidth] metres wide at
     * [distance] facing the viewer (a plane parallel to the eye maps linearly onto it).
     */
    fun screenCoverage(display: Display, tier: EyeBuffer, screenWidth: Float, distance: Float): Int =
        (screenWidth.toDouble() / distance * eyeExtent(display, tier).first / (display.tanRight - display.tanLeft)).roundToInt()

    /**
     * The big screen's layer for a Wine screen of [width]x[height]. Without reconstruction ([tier]
     * null) or before a session recorded the [display], it is the Wine screen, at least 1280 wide.
     * Otherwise it is as wide as the larger of the Wine screen and the coverage of [tier], but no
     * wider than the coverage of the eye limit, beyond which the XR compositor shows nothing more;
     * the Wine screen's aspect, even sides, at most 3840x2160.
     */
    fun screenSwapchain(
        width: Int,
        height: Int,
        tier: EyeBuffer? = null,
        display: Display? = null,
        screenWidth: Float = 0f,
        distance: Float = 0f,
    ): Pair<Int, Int> {
        if (tier == null || display == null || screenWidth <= 0f || distance <= 0f) {
            if (width >= SCREEN_MIN_WIDTH) return width to height
            return SCREEN_MIN_WIDTH to SCREEN_MIN_WIDTH * height / width
        }
        val limit = screenCoverage(display, EyeBuffer.MAXIMUM, screenWidth, distance)
        val target = minOf(maxOf(width, screenCoverage(display, tier, screenWidth, distance)), limit)
        val w = minOf(target, SCREEN_MAX_WIDTH, (SCREEN_MAX_HEIGHT.toLong() * width / height).toInt())
        val h = (w.toLong() * height / width).toInt()
        return w / 2 * 2 to h / 2 * 2
    }

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
            eyeBuffer = EyeBuffer.entries.firstOrNull { it.key == vars.get(ENV_EYE_BUFFER) } ?: defaults.eyeBuffer,
            frameGen = FrameGen.entries.firstOrNull { it.key == vars.get(ENV_FRAME_GEN) } ?: defaults.frameGen,
            flowPercent = vars.get(ENV_FLOW).toIntOrNull()?.coerceIn(25, 100) ?: defaults.flowPercent,
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
        put(ENV_EYE_BUFFER, settings.eyeBuffer.key)
        put(ENV_FRAME_GEN, settings.frameGen.key)
        put(ENV_FLOW, settings.flowPercent.coerceIn(25, 100))
    }.toString()

    @Synchronized
    fun set(newSettings: Settings) {
        sink(newSettings)
        settings = newSettings
    }

    /**
     * Applies `filter=off|fsr1|sgsr`, `sharp=0..100`, `fov=off|fixed|eye`,
     * `level=low|balanced|high`, `out=50..100`, `eye=recommended|high|maximum`,
     * `fg=off|2x|3x|4x`, `flow=25..100` and `debug=0|1`.
     * Returns an error token, or null
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
                "eye" -> next.copy(eyeBuffer = EyeBuffer.entries.firstOrNull { it.key == value } ?: return "invalid_eye")
                "fg" -> next.copy(frameGen = FrameGen.entries.firstOrNull { it.key == value } ?: return "invalid_fg")
                "flow" -> next.copy(flowPercent = value.toIntOrNull()?.takeIf { it in 25..100 } ?: return "invalid_flow")
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
