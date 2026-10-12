package app.gamenative.ui.screen.xr

import kotlin.math.abs
import kotlin.math.atan
import kotlin.math.roundToInt
import kotlin.math.sqrt

/**
 * Screen presets for the 2D big screen (XR): a 16:9 screen of a given diagonal at a given
 * distance, stored as the container's `immersiveQuadScale` / `immersiveQuadDistance` (a scale of
 * [ImmersiveControls]' 1.6 x 0.9 m base screen). Swan's optics focus at 1.5 m, so every preset
 * sits on that plane and only the size changes; 100 inches (about 73 degrees wide) is the default.
 */
object XrScreenPresets {
    data class Preset(val key: String, val diagonalInches: Int, val distanceMeters: Float) {
        val widthMeters: Float
            get() = diagonalInches * METERS_PER_INCH * 16f / sqrt(16f * 16f + 9f * 9f)

        val scale: Float
            get() = widthMeters / ImmersiveControls.BASE_WIDTH_METERS

        /** Horizontal field of view the screen covers, in whole degrees. */
        val horizontalDegrees: Int
            get() = Math.toDegrees(2.0 * atan(widthMeters / 2.0 / distanceMeters)).roundToInt()
    }

    private const val METERS_PER_INCH = 0.0254f
    private const val TOLERANCE = 0.01f
    private const val FOCAL_DISTANCE_METERS = 1.5f

    val PRESETS = listOf(65, 80, 100, 120).map { Preset(it.toString(), it, FOCAL_DISTANCE_METERS) }
    val DEFAULT = PRESETS[2]

    fun byKey(key: String): Preset? = PRESETS.firstOrNull { it.key == key }

    /** The preset a stored screen matches, or null when it was resized or moved by hand. */
    fun match(scale: Float, distance: Float): Preset? = PRESETS.firstOrNull {
        abs(it.scale - scale) < TOLERANCE && abs(it.distanceMeters - distance) < TOLERANCE
    }
}
