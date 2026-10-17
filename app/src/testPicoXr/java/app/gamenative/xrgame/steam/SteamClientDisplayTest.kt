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
    @Test fun missingDisplayUsesBoundedFallback() {
        assertEquals(1280 to 800, SteamClientDisplay.resolution(0, 0))
        assertEquals(1280 to 320, SteamClientDisplay.resolution(Int.MAX_VALUE, 1))
    }
}
