package app.gamenative.ui.screen.xr.windows

/**
 * Recommended eye-buffer scale for Windows VR games.
 *
 * The default follows shadPS4's Android Render Scale (0.5): Swan's GPU is already saturated at
 * the runtime's full recommendation, which on current Pico firmware is a supersampled 3424x3170
 * per eye. The limit mirrors Foundation's ResolveTopSurfaceExtent: scale the runtime
 * recommendation, clamp to 25-100 %, and keep the side-by-side eye atlas within the swapchain
 * size the Windows runtime advertises. A custom scale is honoured as chosen.
 */
object XrResolutionRecommendation {
    const val RECOMMENDED_PERCENT = 50
    const val MIN_PERCENT = 25
    const val MAX_PERCENT = 100

    /** maxSwapchainImageWidth/Height reported by gamenative_openxr_runtime. */
    const val MAX_SWAPCHAIN_EXTENT = 4096L

    const val EXTRA_MODE = "xrRenderScaleMode"
    const val MODE_RECOMMENDED = "recommended"
    const val MODE_CUSTOM = "custom"

    /** Even per-eye size for a runtime recommendation of [width]x[height] at [percent]. */
    fun eyeSize(width: Long, height: Long, percent: Int, limitToSwapchain: Boolean): Pair<Long, Long> {
        if (width <= 0 || height <= 0) return 0L to 0L
        var scale = percent.coerceIn(MIN_PERCENT, MAX_PERCENT) / 100.0
        if (limitToSwapchain) {
            scale = minOf(scale, MAX_SWAPCHAIN_EXTENT / (2.0 * width), MAX_SWAPCHAIN_EXTENT / height.toDouble())
        }
        val eyeWidth = (width * scale).toLong() and 1L.inv()
        val eyeHeight = (height * scale).toLong() and 1L.inv()
        return eyeWidth.coerceAtLeast(2L) to eyeHeight.coerceAtLeast(2L)
    }
}
