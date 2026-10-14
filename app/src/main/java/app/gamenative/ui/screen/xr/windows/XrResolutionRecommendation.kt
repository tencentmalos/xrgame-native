package app.gamenative.ui.screen.xr.windows

/**
 * Eye-buffer size for Windows VR games.
 *
 * The default follows shadPS4's Android Render Scale (0.5): Swan's GPU is already saturated at
 * the runtime's full recommendation. The limit mirrors Foundation's ResolveTopSurfaceExtent:
 * scale the runtime recommendation and keep the side-by-side eye atlas within the swapchain size
 * the Windows runtime advertises. A custom scale is honoured as chosen.
 *
 * Percentages up to 100 scale the recommendation. Above 100 each axis goes from the
 * recommendation towards the runtime's largest eye size: [HIGH_PERCENT] is halfway and
 * [LIMIT_PERCENT] the limit itself, like the XR output tiers (WindowsVrUpscale.EyeBuffer).
 */
object XrResolutionRecommendation {
    const val RECOMMENDED_PERCENT = 50
    const val MIN_PERCENT = 25
    const val MAX_PERCENT = 100
    const val HIGH_PERCENT = 150
    const val LIMIT_PERCENT = 200

    /** maxSwapchainImageWidth/Height reported by gamenative_openxr_runtime. */
    const val MAX_SWAPCHAIN_EXTENT = 16384L

    const val EXTRA_MODE = "xrRenderScaleMode"
    const val MODE_RECOMMENDED = "recommended"
    const val MODE_CUSTOM = "custom"

    /**
     * Even per-eye size at [percent] for a runtime recommendation of [width]x[height] and a
     * largest eye of [maxWidth]x[maxHeight] (0 when unknown, which caps the size at the
     * recommendation).
     */
    fun eyeSize(
        width: Long,
        height: Long,
        maxWidth: Long,
        maxHeight: Long,
        percent: Int,
        limitToSwapchain: Boolean,
    ): Pair<Long, Long> {
        if (width <= 0 || height <= 0) return 0L to 0L
        val clamped = percent.coerceIn(MIN_PERCENT, LIMIT_PERCENT)
        val eyeWidth: Long
        val eyeHeight: Long
        if (clamped <= MAX_PERCENT) {
            var scale = clamped / 100.0
            if (limitToSwapchain) {
                scale = minOf(scale, MAX_SWAPCHAIN_EXTENT / (2.0 * width), MAX_SWAPCHAIN_EXTENT / height.toDouble())
            }
            eyeWidth = (width * scale).toLong()
            eyeHeight = (height * scale).toLong()
        } else {
            val toward = (clamped - MAX_PERCENT) / (LIMIT_PERCENT - MAX_PERCENT).toDouble()
            eyeWidth = width + ((maxOf(width, maxWidth) - width) * toward).toLong()
            eyeHeight = height + ((maxOf(height, maxHeight) - height) * toward).toLong()
        }
        return (eyeWidth and 1L.inv()).coerceAtLeast(2L) to (eyeHeight and 1L.inv()).coerceAtLeast(2L)
    }
}
