package app.gamenative.ui.screen.xr.windows

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Test

class WindowsVrGripCorrectionTest {
    private val sent = mutableListOf<WindowsVrGripCorrection.Settings>()

    @Before fun setUp() {
        WindowsVrGripCorrection.sink = { sent += it }
        WindowsVrGripCorrection.reset("test-device")
        sent.clear()
    }

    @After fun tearDown() {
        WindowsVrGripCorrection.reset("test-device")
    }

    @Test fun appliesAllArgumentsOrNone() {
        assertNull(WindowsVrGripCorrection.apply(listOf("pitch=-12.5", "yaw=3", "z=10")))
        val applied = WindowsVrGripCorrection.settings
        assertEquals(WindowsVrGripCorrection.Settings(pitch = -12.5f, yaw = 3f, zMm = 10f), applied)
        assertEquals(listOf(applied), sent)

        assertEquals("invalid_pitch", WindowsVrGripCorrection.apply(listOf("roll=5", "pitch=120")))
        assertEquals("invalid_x", WindowsVrGripCorrection.apply(listOf("x=abc")))
        assertEquals("unknown_argument", WindowsVrGripCorrection.apply(listOf("scale=2")))
        assertEquals("malformed_argument", WindowsVrGripCorrection.apply(listOf("pitch")))
        assertEquals(applied, WindowsVrGripCorrection.settings)
        assertEquals(1, sent.size)
    }

    @Test fun swanStartsWithItsCalibratedPitch() {
        WindowsVrGripCorrection.reset("swan")
        assertEquals(WindowsVrGripCorrection.Settings(pitch = 24f), WindowsVrGripCorrection.settings)
        assertEquals(WindowsVrGripCorrection.Settings(), WindowsVrGripCorrection.defaultFor("unknown"))
    }

    @Test fun resetRestoresTheDeviceDefault() {
        assertNull(WindowsVrGripCorrection.apply(listOf("pitch=20")))
        assertNull(WindowsVrGripCorrection.apply(listOf("reset=1", "roll=-4")))
        assertEquals(WindowsVrGripCorrection.Settings(roll = -4f), WindowsVrGripCorrection.settings)
        assertEquals("invalid_reset", WindowsVrGripCorrection.apply(listOf("reset=0")))
    }
}
