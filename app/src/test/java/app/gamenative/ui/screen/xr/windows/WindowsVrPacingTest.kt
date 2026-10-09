package app.gamenative.ui.screen.xr.windows

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class WindowsVrPacingTest {
    private val periodNs = 13_889_000L
    private val predictionCalls = mutableListOf<Boolean>()

    @Before fun setUp() {
        WindowsVrTuning.predictionSink = { predictionCalls += it }
        WindowsVrTuning.apply(listOf("pacing=off", "start=0", "predict=0"))
        predictionCalls.clear()
    }

    @After fun tearDown() {
        WindowsVrTuning.apply(listOf("pacing=off", "start=0", "predict=0"))
    }

    @Test fun tuningAppliesAllArgumentsOrNone() {
        assertNull(WindowsVrTuning.apply(listOf("pacing=half", "start=3000", "predict=1")))
        assertEquals(WindowsVrTuning.Pacing.HALF, WindowsVrTuning.pacing)
        assertEquals(3000L, WindowsVrTuning.frameStartTargetUs)
        assertTrue(WindowsVrTuning.extendedPrediction)
        assertEquals(listOf(true), predictionCalls)

        assertEquals("invalid_start", WindowsVrTuning.apply(listOf("pacing=auto", "start=99999")))
        assertEquals(WindowsVrTuning.Pacing.HALF, WindowsVrTuning.pacing)
        assertEquals("unknown_argument", WindowsVrTuning.apply(listOf("rate=36")))
        assertEquals("malformed_argument", WindowsVrTuning.apply(listOf("predict")))

        assertNull(WindowsVrTuning.apply(listOf("predict=0")))
        assertFalse(WindowsVrTuning.extendedPrediction)
        assertEquals(listOf(true, false), predictionCalls)
    }

    @Test fun fixedModesIgnoreTheGameFrameTime() {
        val pacing = WindowsVrPacing()
        assertEquals(1, pacing.step(WindowsVrTuning.Pacing.OFF, periodNs))
        assertEquals(2, pacing.step(WindowsVrTuning.Pacing.HALF, periodNs))
    }

    @Test fun autoFollowsTheGameFrameTimeAfterItHolds() {
        val pacing = WindowsVrPacing()
        var now = 1_000_000_000L
        // run22: the game needs about 26.6 ms per frame, a little under two 72 Hz periods.
        fun frame(workNs: Long, delayNs: Long = 0): Int {
            pacing.onReply(now)
            now += workNs + delayNs
            pacing.onRequest(now, delayNs)
            return pacing.step(WindowsVrTuning.Pacing.AUTO, periodNs)
        }
        var step = 1
        repeat(WindowsVrPacing.SWITCH_FRAMES - 1) { step = frame(26_600_000) }
        assertEquals(1, step)
        step = frame(26_600_000)
        assertEquals(2, step)

        // The bridge's own frame-start delay does not count as game work.
        repeat(100) { step = frame(26_600_000, delayNs = 10_000_000) }
        assertEquals(2, step)

        // A lighter scene returns to every XR frame once the average has settled.
        repeat(200) { step = frame(9_000_000) }
        assertEquals(1, step)
    }
}
