package app.gamenative.ui.screen.xr.windows

import org.junit.Assert.assertEquals
import org.junit.Test

class XrResolutionRecommendationTest {
    // Swan (2026-10-14): the runtime recommends 2592x2400 per eye and allows up to 4160x3552.
    private fun swan(percent: Int, recommended: Boolean = false) =
        XrResolutionRecommendation.eyeSize(2592, 2400, 4160, 3552, percent, limitToSwapchain = recommended)

    @Test
    fun swanRecommendationHalvesEachAxis() {
        assertEquals(1296L to 1200L, swan(XrResolutionRecommendation.RECOMMENDED_PERCENT, recommended = true))
    }

    @Test
    fun recommendedModeKeepsTheEyeAtlasWithinTheSwapchainLimit() {
        val (width, height) = XrResolutionRecommendation.eyeSize(9000, 9000, 0, 0, 100, limitToSwapchain = true)
        assertEquals(true, 2 * width <= XrResolutionRecommendation.MAX_SWAPCHAIN_EXTENT)
        assertEquals(true, height <= XrResolutionRecommendation.MAX_SWAPCHAIN_EXTENT)
    }

    @Test
    fun customScaleIsHonouredAndClamped() {
        assertEquals(1944L to 1800L, swan(75))
        assertEquals(2592L to 2400L, swan(100))
        assertEquals(648L to 600L, swan(10))
        assertEquals(0L to 0L, XrResolutionRecommendation.eyeSize(0, 2400, 4160, 3552, 50, limitToSwapchain = true))
    }

    @Test
    fun scalesAboveTheRecommendationReachTheHeadsetLimit() {
        assertEquals(3376L to 2976L, swan(XrResolutionRecommendation.HIGH_PERCENT))
        assertEquals(4160L to 3552L, swan(XrResolutionRecommendation.LIMIT_PERCENT))
        assertEquals(4160L to 3552L, swan(500))
    }

    @Test
    fun anUnknownLimitStopsAtTheRecommendation() {
        assertEquals(2592L to 2400L, XrResolutionRecommendation.eyeSize(2592, 2400, 0, 0, 200, limitToSwapchain = false))
    }
}
