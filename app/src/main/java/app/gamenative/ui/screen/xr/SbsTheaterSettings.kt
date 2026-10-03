package app.gamenative.ui.screen.xr

import app.gamenative.BuildConfig
import com.winlator.container.Container
import com.winlator.renderer.VulkanRenderer

/** Reuses the headset theater's persisted quad dimensions, without a Windows VR runtime. */
object SbsTheaterSettings {
    const val ENABLED = "sbsTheaterEnabled"
    const val SCALE = "immersiveQuadScale"
    const val DISTANCE = "immersiveQuadDistance"

    fun isEnabled(container: Container): Boolean = BuildConfig.XRGAME &&
        container.getExtra(ENABLED, "false").toBoolean() &&
        !container.getExtra("windowsVrEnabled", "false").toBoolean() &&
        !container.graphicsDriver.equals("virgl", ignoreCase = true)

    fun scale(container: Container): Float = bounded(container.getExtra(SCALE, ""),
        ImmersiveControls.DEFAULT_SCALE, ImmersiveControls.MIN_SCALE, ImmersiveControls.MAX_SCALE)
    fun distance(container: Container): Float = bounded(container.getExtra(DISTANCE, ""),
        ImmersiveControls.DEFAULT_DISTANCE, ImmersiveControls.MIN_DISTANCE, ImmersiveControls.MAX_DISTANCE)

    internal fun bounded(value: String, fallback: Float, min: Float, max: Float): Float =
        value.toFloatOrNull()?.takeIf { it.isFinite() }?.coerceIn(min, max) ?: fallback

    fun apply(renderer: VulkanRenderer, container: Container) {
        if (isEnabled(container)) renderer.setSbsTheater(true,
            ImmersiveControls.BASE_WIDTH_METERS * scale(container), distance(container))
    }
}
