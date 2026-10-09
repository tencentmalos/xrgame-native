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
            debug = true,
        )
        val stored = WindowsVrUpscale.withSettings("WINEESYNC=1", chosen)
        assertEquals(chosen.copy(debug = false), WindowsVrUpscale.fromEnvVars(stored))
        assertEquals("1", EnvVars(stored).get("WINEESYNC"))
        assertEquals(WindowsVrUpscale.BACKEND_GLES, WindowsVrUpscale.backend(stored))
        val vulkan = WindowsVrUpscale.withBackend(stored, WindowsVrUpscale.BACKEND_VULKAN)
        assertEquals(WindowsVrUpscale.BACKEND_VULKAN, WindowsVrUpscale.backend(vulkan))
        assertEquals(chosen.copy(debug = false), WindowsVrUpscale.fromEnvVars(vulkan))
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
