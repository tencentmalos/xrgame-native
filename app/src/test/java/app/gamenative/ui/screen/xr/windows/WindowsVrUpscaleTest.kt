package app.gamenative.ui.screen.xr.windows

import com.winlator.core.envvars.EnvVars
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class WindowsVrUpscaleTest {
    private val sent = mutableListOf<WindowsVrUpscale.Settings>()

    @Before fun setUp() {
        WindowsVrUpscale.sink = { sent += it }
        WindowsVrUpscale.set(WindowsVrUpscale.Settings())
        sent.clear()
    }

    @After fun tearDown() {
        WindowsVrUpscale.set(WindowsVrUpscale.Settings())
    }

    private val display = WindowsVrUpscale.Display(2592, 2400, 4096, 4096, -1.2f, 0.8f)

    @Test fun eyeBufferTiersRunFromTheRecommendationToTheLimit() {
        assertEquals(2592 to 2400, WindowsVrUpscale.eyeExtent(display, WindowsVrUpscale.EyeBuffer.RECOMMENDED))
        assertEquals(3344 to 3248, WindowsVrUpscale.eyeExtent(display, WindowsVrUpscale.EyeBuffer.HIGH))
        assertEquals(4096 to 4096, WindowsVrUpscale.eyeExtent(display, WindowsVrUpscale.EyeBuffer.MAXIMUM))
        val small = display.copy(maxWidth = 2000, maxHeight = 2000)
        assertEquals(2000 to 2000, WindowsVrUpscale.eyeExtent(small, WindowsVrUpscale.EyeBuffer.RECOMMENDED))
    }

    // An eye of 2000 px (limit 4000) over tangents -1..1; a 1.5 m screen at 1.5 m covers half of it.
    private val square = WindowsVrUpscale.Display(2000, 2000, 4000, 4000, -1f, 1f)

    @Test fun screenCoverageCountsEyePixelsAcrossTheScreen() {
        assertEquals(1000, WindowsVrUpscale.screenCoverage(square, WindowsVrUpscale.EyeBuffer.RECOMMENDED, 1.5f, 1.5f))
        assertEquals(1500, WindowsVrUpscale.screenCoverage(square, WindowsVrUpscale.EyeBuffer.HIGH, 1.5f, 1.5f))
        assertEquals(2000, WindowsVrUpscale.screenCoverage(square, WindowsVrUpscale.EyeBuffer.MAXIMUM, 1.5f, 1.5f))
        // Swan's recorded eye views and the 80-inch preset at 1.5 m: about 1402 to 2250 px.
        val swan = WindowsVrUpscale.Display(2592, 2400, 4160, 3552, -1.091255f, 1.091255f)
        val screen = app.gamenative.ui.screen.xr.XrScreenPresets.byKey("80")!!
        assertTrue(WindowsVrUpscale.screenCoverage(swan, WindowsVrUpscale.EyeBuffer.RECOMMENDED, screen.widthMeters, screen.distanceMeters) in 1400..1404)
        assertTrue(WindowsVrUpscale.screenCoverage(swan, WindowsVrUpscale.EyeBuffer.MAXIMUM, screen.widthMeters, screen.distanceMeters) in 2248..2252)
    }

    @Test fun bigScreenLayerStaysWithinTheEffectiveRange() {
        val tier = WindowsVrUpscale.EyeBuffer.RECOMMENDED
        // No reconstruction, or no recorded display: the Wine screen, at least 1280 wide.
        assertEquals(1920 to 1080, WindowsVrUpscale.screenSwapchain(1920, 1080))
        assertEquals(1280 to 960, WindowsVrUpscale.screenSwapchain(640, 480))
        assertEquals(1280 to 720, WindowsVrUpscale.screenSwapchain(1280, 720, tier))
        // A Wine screen already above the tier stays as it is.
        assertEquals(1280 to 720, WindowsVrUpscale.screenSwapchain(1280, 720, tier, square, 1.5f, 1.5f))
        // Below the tier it is reconstructed up to it.
        assertEquals(1500 to 842, WindowsVrUpscale.screenSwapchain(1280, 720, WindowsVrUpscale.EyeBuffer.HIGH, square, 1.5f, 1.5f))
        assertEquals(2000 to 1124, WindowsVrUpscale.screenSwapchain(1280, 720, WindowsVrUpscale.EyeBuffer.MAXIMUM, square, 1.5f, 1.5f))
        // Above the eye limit it is brought down to it.
        assertEquals(2000 to 1124, WindowsVrUpscale.screenSwapchain(2560, 1440, tier, square, 1.5f, 1.5f))
        // Never above 3840x2160.
        assertEquals(3840 to 2160, WindowsVrUpscale.screenSwapchain(1920, 1080, WindowsVrUpscale.EyeBuffer.MAXIMUM, square, 6f, 1.5f))
    }

    @Test fun displayRoundTripsAndRejectsGarbage() {
        assertEquals(display, WindowsVrUpscale.Display.decode(display.encode()))
        assertEquals(display, WindowsVrUpscale.Display.fromNative(floatArrayOf(2592f, 2400f, 4096f, 4096f, -1.2f, 0.8f, 1f, -1f)))
        assertNull(WindowsVrUpscale.Display.decode(""))
        assertNull(WindowsVrUpscale.Display.decode("1,2,3"))
        assertNull(WindowsVrUpscale.Display.decode("2592,2400,0,4096,-1,1"))
        assertNull(WindowsVrUpscale.Display.fromNative(null))
    }

    @Test fun bigScreensDefaultToVulkanAndVrGamesToGles() {
        assertEquals(WindowsVrUpscale.BACKEND_VULKAN, WindowsVrUpscale.compositeBackend("", vrGame = false))
        assertEquals(WindowsVrUpscale.BACKEND_GLES, WindowsVrUpscale.compositeBackend("", vrGame = true))
        val gles = WindowsVrUpscale.withBackend("", WindowsVrUpscale.BACKEND_GLES)
        assertEquals(WindowsVrUpscale.BACKEND_GLES, WindowsVrUpscale.compositeBackend(gles, vrGame = false))
    }

    @Test fun eyeBufferRoundTripsThroughEnvVars() {
        val stored = WindowsVrUpscale.withSettings("A=1", WindowsVrUpscale.Settings(eyeBuffer = WindowsVrUpscale.EyeBuffer.HIGH))
        assertEquals(WindowsVrUpscale.EyeBuffer.HIGH, WindowsVrUpscale.fromEnvVars(stored).eyeBuffer)
        assertTrue(EnvVars(stored).has("A"))
        assertEquals(WindowsVrUpscale.EyeBuffer.RECOMMENDED, WindowsVrUpscale.fromEnvVars("XRGAME_XR_EYE_BUFFER=huge").eyeBuffer)
        assertNull(WindowsVrUpscale.apply(listOf("eye=maximum")))
        assertEquals(WindowsVrUpscale.EyeBuffer.MAXIMUM, WindowsVrUpscale.settings.eyeBuffer)
        assertEquals("invalid_eye", WindowsVrUpscale.apply(listOf("eye=huge")))
    }

    @Test fun appliesAllArgumentsOrNone() {
        assertNull(WindowsVrUpscale.apply(listOf("filter=fsr1", "sharp=20", "fov=fixed", "level=high")))
        val applied = WindowsVrUpscale.settings
        assertEquals(WindowsVrUpscale.Filter.FSR1, applied.filter)
        assertEquals(20, applied.sharpness)
        assertEquals(WindowsVrUpscale.Foveation.FIXED, applied.foveation)
        assertEquals(WindowsVrUpscale.Level.HIGH, applied.level)
        assertEquals(listOf(applied), sent)

        assertEquals("invalid_out", WindowsVrUpscale.apply(listOf("filter=off", "out=40")))
        assertEquals(applied, WindowsVrUpscale.settings)
        assertEquals("unknown_argument", WindowsVrUpscale.apply(listOf("scale=2")))
        assertEquals("malformed_argument", WindowsVrUpscale.apply(listOf("debug")))
        assertEquals(1, sent.size)
    }

    @Test fun unchangedSettingsAreNotResent() {
        assertNull(WindowsVrUpscale.apply(emptyList()))
        assertNull(WindowsVrUpscale.apply(listOf("filter=sgsr")))
        assertTrue(sent.isEmpty())
        assertNull(WindowsVrUpscale.apply(listOf("debug=1", "out=75")))
        assertTrue(WindowsVrUpscale.settings.debug)
        assertEquals(75, WindowsVrUpscale.settings.outputPercent)
        assertNull(WindowsVrUpscale.apply(listOf("debug=0")))
        assertFalse(WindowsVrUpscale.settings.debug)
        assertEquals(2, sent.size)
    }

    @Test fun envVarsRoundTripKeepsOtherVariables() {
        val chosen = WindowsVrUpscale.Settings(
            filter = WindowsVrUpscale.Filter.FSR1,
            sharpness = 25,
            foveation = WindowsVrUpscale.Foveation.FIXED,
            level = WindowsVrUpscale.Level.HIGH,
            outputPercent = 80,
            frameGen = WindowsVrUpscale.FrameGen.X3,
            flowPercent = 75,
            debug = true,
        )
        val stored = WindowsVrUpscale.withSettings("WINEESYNC=1", chosen)
        assertEquals(chosen, WindowsVrUpscale.fromEnvVars(stored))
        assertEquals("1", EnvVars(stored).get("WINEESYNC"))
        assertEquals(WindowsVrUpscale.BACKEND_GLES, WindowsVrUpscale.backend(stored))
        val vulkan = WindowsVrUpscale.withBackend(stored, WindowsVrUpscale.BACKEND_VULKAN)
        assertEquals(WindowsVrUpscale.BACKEND_VULKAN, WindowsVrUpscale.backend(vulkan))
        assertEquals(chosen, WindowsVrUpscale.fromEnvVars(vulkan))
        // Switching the debug view off removes its variable again.
        val off = WindowsVrUpscale.withSettings(vulkan, chosen.copy(debug = false))
        assertFalse(EnvVars(off).has(WindowsVrUpscale.ENV_DEBUG))
        assertEquals(chosen.copy(debug = false), WindowsVrUpscale.fromEnvVars(off))
    }

    @Test fun frameGenerationAppliesAndStaysOffByDefault() {
        assertEquals(WindowsVrUpscale.FrameGen.OFF, WindowsVrUpscale.Settings().frameGen)
        assertNull(WindowsVrUpscale.apply(listOf("fg=4x", "flow=25")))
        assertEquals(WindowsVrUpscale.FrameGen.X4, WindowsVrUpscale.settings.frameGen)
        assertEquals(4, sent.last().frameGen.multiplier)
        assertEquals(25, sent.last().flowPercent)
        assertEquals("invalid_fg", WindowsVrUpscale.apply(listOf("fg=5x")))
        assertEquals("invalid_flow", WindowsVrUpscale.apply(listOf("flow=10")))
        val parsed = WindowsVrUpscale.fromEnvVars("XRGAME_XR_FRAMEGEN=8x XRGAME_XR_FRAMEGEN_FLOW=300")
        assertEquals(WindowsVrUpscale.FrameGen.OFF, parsed.frameGen)
        assertEquals(100, parsed.flowPercent)
    }

    @Test fun invalidStoredValuesFallBackOrClamp() {
        assertEquals(WindowsVrUpscale.BACKEND_GLES, WindowsVrUpscale.backend("XRGAME_XR_COMPOSITE=metal"))
        val parsed = WindowsVrUpscale.fromEnvVars(
            "XRGAME_XR_UPSCALE=bogus XRGAME_XR_SHARPNESS=500 XRGAME_XR_OUTPUT_PERCENT=10 XRGAME_XR_FOVEATION=eye",
        )
        assertEquals(WindowsVrUpscale.Settings().filter, parsed.filter)
        assertEquals(100, parsed.sharpness)
        assertEquals(50, parsed.outputPercent)
        assertEquals(WindowsVrUpscale.Foveation.EYE, parsed.foveation)
        assertEquals(WindowsVrUpscale.Settings(), WindowsVrUpscale.fromEnvVars(""))
    }
}
