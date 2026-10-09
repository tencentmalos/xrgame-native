package app.gamenative.ui.screen.xr.windows

import org.junit.Assert.assertEquals
import org.junit.Test

class XrResolutionRecommendationTest {
    @Test
    fun swanRecommendationHalvesEachAxis() {
        // Swan's runtime recommends 3424x3170 per eye; 50 % keeps the 3424x1584 atlas under 4096.
        assertEquals(1712L to 1584L, XrResolutionRecommendation.eyeSize(3424, 3170, 50, limitToSwapchain = true))
    }

    @Test
    fun recommendedModeKeepsTheEyeAtlasWithinTheSwapchainLimit() {
        val (width, height) = XrResolutionRecommendation.eyeSize(4160, 3552, 100, limitToSwapchain = true)
        assertEquals(true, 2 * width <= XrResolutionRecommendation.MAX_SWAPCHAIN_EXTENT)
        assertEquals(true, height <= XrResolutionRecommendation.MAX_SWAPCHAIN_EXTENT)
    }

    @Test
    fun customScaleIsHonouredAndClamped() {
        assertEquals(2568L to 2376L, XrResolutionRecommendation.eyeSize(3424, 3170, 75, limitToSwapchain = false))
        assertEquals(856L to 792L, XrResolutionRecommendation.eyeSize(3424, 3170, 10, limitToSwapchain = false))
        assertEquals(0L to 0L, XrResolutionRecommendation.eyeSize(0, 3170, 50, limitToSwapchain = true))
    }
}
