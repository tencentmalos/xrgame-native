package app.gamenative.xrgame.steam

import org.junit.Assert.assertEquals
import org.junit.Test

class SteamClientDisplayTest {
    @Test fun handheldAndTabletRatios() {
        assertEquals(1280 to 720, SteamClientDisplay.resolution(1920, 1080))
        assertEquals(1280 to 720, SteamClientDisplay.resolution(1080, 1920))
        assertEquals(1280 to 800, SteamClientDisplay.resolution(2560, 1600))
        assertEquals(1280 to 960, SteamClientDisplay.resolution(2048, 1536))
    }
    @Test fun xrResolutionKeepsOnlyOfferedSizes() {
        assertEquals(1920 to 1080, SteamClientDisplay.xrResolution("1920x1080"))
        assertEquals(1600 to 900, SteamClientDisplay.xrResolution("1600x900"))
        assertEquals(1280 to 720, SteamClientDisplay.xrResolution("1280x720"))
        assertEquals(1920 to 1080, SteamClientDisplay.xrResolution(null))
        assertEquals(1920 to 1080, SteamClientDisplay.xrResolution("2560x1440"))
        assertEquals(1920 to 1080, SteamClientDisplay.xrResolution("maximum"))
        assertEquals("1600x900", SteamClientDisplay.label(1600 to 900))
    }
    @Test fun missingDisplayUsesBoundedFallback() {
        assertEquals(1280 to 800, SteamClientDisplay.resolution(0, 0))
        assertEquals(1280 to 320, SteamClientDisplay.resolution(Int.MAX_VALUE, 1))
    }
}
