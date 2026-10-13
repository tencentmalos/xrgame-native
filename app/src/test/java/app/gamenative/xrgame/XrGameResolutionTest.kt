package app.gamenative.xrgame

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class XrGameResolutionTest {
    @Test fun sizesWithinTheCapStay() {
        assertEquals("1280x720", XrGameResolution.capped("1280x720"))
        assertEquals("1920x1080", XrGameResolution.capped("1920x1080"))
        assertEquals("1280x1024", XrGameResolution.capped("1280x1024"))
        assertTrue(XrGameResolution.fits("1600x900"))
    }

    @Test fun largerSizesShrinkToFitKeepingTheirAspect() {
        assertEquals("1920x1080", XrGameResolution.capped("2560x1440"))
        assertEquals("1920x1080", XrGameResolution.capped("3840x2160"))
        assertEquals("1728x1080", XrGameResolution.capped("1920x1200"))
        assertEquals("1920x640", XrGameResolution.capped("3000x1000"))
        assertFalse(XrGameResolution.fits("1920x1200"))
    }

    @Test fun unparsableSizesStay() {
        assertEquals("native", XrGameResolution.capped("native"))
        assertFalse(XrGameResolution.fits("0x720"))
    }
}
