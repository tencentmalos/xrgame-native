package app.gamenative.ui.screen.xr.sbs

import org.junit.Assert.*
import org.junit.Test
import kotlin.math.PI
import kotlin.math.sqrt

class SbsTrackingTest {
    @Test fun controllersStayFixedWhileHeadAndBothHandsInputChange() {
        val source = SbsFrameSource()
        source.setActive(true)
        source.tick(1_000_000_000L, 16_666_667L, 960, 1080)
        val before = source.latest()!!
        for (i in 0..100) source.gyro(0f, (PI/2).toFloat(), 0f, 1_000_000_000L+i*10_000_000L)
        val axes = floatArrayOf(-0.5f, 0.75f, 0.25f, -0.8f, 0.6f, 0.9f, 1f, 0.4f)
        axes.forEachIndexed(source::axis)
        for (bit in intArrayOf(0, 2, 7, 8, 9)) source.button(bit, true)
        source.tick(2_016_666_667L, 16_666_667L, 960, 1080)
        val after = source.latest()!!
        assertFalse(before.views.contentEquals(after.views))
        for (hand in 0..1) {
            val base = hand*18
            assertArrayEquals(before.input.copyOfRange(base+4, base+18), after.input.copyOfRange(base+4, base+18), 0f)
            assertArrayEquals(floatArrayOf(axes[4+hand], axes[6+hand], axes[hand*2], axes[hand*2+1]), after.input.copyOfRange(base, base+4), 0f)
        }
        assertEquals(1 or 4 or 128 or 256 or 512, after.flags[1])
        assertEquals(3, after.flags[2])
        source.recenter()
        source.tick(2_033_333_334L, 16_666_667L, 960, 1080)
        assertArrayEquals(after.input, source.latest()!!.input, 0f)
    }

    @Test fun integratesNinetyDegreesAndRejectsGapsAndInvalidSamples() {
        val t = SbsTracking()
        val rate = (PI/2).toFloat()
        for (i in 0..100) t.gyro(0f, rate, 0f, 1_000_000_000L+i*10_000_000L)
        val left = SbsTracking.rotate(t.orientation, -0.032f, 0f, 0f)
        assertEquals(0f, left[0], 0.00001f)
        assertEquals(0.032f, left[2], 0.00001f)
        val before = t.orientation.copyOf()
        t.gyro(Float.NaN, 0f, 0f, 2_010_000_000L)
        t.gyro(0f, rate, 0f, 1_000_000_000L)
        t.gyro(0f, rate, 0f, 5_000_000_000L)
        assertArrayEquals(before, t.orientation, 0f)
        t.recenter()
        assertArrayEquals(floatArrayOf(0f, 0f, 0f, 1f), t.orientation, 0f)
        assertEquals(1L, t.recenterSerial)
    }

    @Test fun stereoHasRotatedIpdAndMenuKeepsFramesWithoutInput() {
        val source = SbsFrameSource()
        source.setActive(true)
        source.button(0, true)
        source.axis(4, 1f)
        source.tick(1_000_000_000L, 16_666_667L, 960, 1080)
        val a = source.waitFrame(0, 0)!!
        assertEquals(960L, a.timing[5])
        assertEquals(0.064f, a.views[15]-a.views[4], 0.000001f)
        assertEquals(1f, a.input[0], 0f)
        assertEquals(1, a.flags[1])
        assertEquals(3, a.flags[0]) // valid poses; never claim tracked position
        source.setFocused(false)
        source.axis(4, 1f)
        source.button(0, true)
        source.tick(1_016_666_667L, 16_666_667L, 960, 1080)
        val b = source.waitFrame(a.timing[0], 0)!!
        assertTrue(b.timing[0] > a.timing[0])
        assertEquals(4L, b.timing[3]) // VISIBLE
        assertEquals(1L, b.timing[4]) // still render while menu has focus
        assertEquals(0, b.flags[1]); assertEquals(0, b.flags[2])
        assertEquals(0f, b.input[0], 0f)
        source.setActive(false)
        assertNull(source.latest()); assertNull(source.waitFrame(0, 0))
        source.setActive(true)
        source.tick(2_000_000_000L, 16_666_667L, 960, 1080)
        assertTrue(source.latest()!!.timing[0] > b.timing[0])
        source.detach()
        assertNull(source.latest()); assertNull(source.waitFrame(0, 1000))
    }

    @Test fun gravityCalibratesFoldableBaseWithoutSpuriousHeadRoll() {
        val neutral = VrGyroAxes.NeutralFrame()
        repeat(8) { neutral.observeGravity(0f, 0f, 9.81f, 1) }
        val rate = neutral.toHead(0f, 0f, 1f)!!
        assertEquals(1f, sqrt(rate.sumOf { it.toDouble()*it }.toFloat()), 0.0001f)
        assertEquals(0f, rate[2], 0.0001f) // spin about gravity is head yaw, not roll
        assertEquals(1f, kotlin.math.abs(rate[1]), 0.0001f)
    }
}
