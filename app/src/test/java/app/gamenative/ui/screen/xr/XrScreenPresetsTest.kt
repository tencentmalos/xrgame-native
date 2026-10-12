package app.gamenative.ui.screen.xr

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class XrScreenPresetsTest {
    @Test fun defaultIsOneHundredInchesOnTheFocalPlane() {
        val preset = XrScreenPresets.DEFAULT
        assertEquals(100, preset.diagonalInches)
        assertEquals(1.5f, preset.distanceMeters, 0f)
        // A 16:9 100-inch screen is 2.214 m wide; the base screen is 1.6 m.
        assertEquals(2.2139f / ImmersiveControls.BASE_WIDTH_METERS, preset.scale, 0.001f)
        assertEquals(73, preset.horizontalDegrees)
    }

    @Test fun everyPresetIsLargerThanTheOldSeventyTwoInchesAtTwoMeters() {
        // The upstream default, 1.6 m wide at 2 m, covers about 44 degrees, which is too small on Swan.
        for (preset in XrScreenPresets.PRESETS) assertTrue(preset.key, preset.horizontalDegrees > 44)
        assertEquals(listOf(51, 61, 73, 83), XrScreenPresets.PRESETS.map { it.horizontalDegrees })
    }

    @Test fun everyPresetFitsTheImmersiveRanges() {
        for (preset in XrScreenPresets.PRESETS) {
            assertTrue(preset.key, preset.scale in ImmersiveControls.MIN_SCALE..ImmersiveControls.MAX_SCALE)
            assertTrue(preset.key, preset.distanceMeters in ImmersiveControls.MIN_DISTANCE..ImmersiveControls.MAX_DISTANCE)
        }
    }

    @Test fun storedScreensMatchTheirPresetOrNone() {
        val preset = XrScreenPresets.byKey("65")!!
        assertEquals(preset, XrScreenPresets.match(preset.scale.toString().toFloat(), preset.distanceMeters))
        assertNull(XrScreenPresets.match(1.0f, 2.0f))
    }
}
