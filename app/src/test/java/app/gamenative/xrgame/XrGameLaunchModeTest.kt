package app.gamenative.xrgame

import app.gamenative.xrgame.XrGameLaunchMode.Mode
import com.winlator.container.ContainerData
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(manifest = Config.NONE)
class XrGameLaunchModeTest {
    private val plain = ContainerData(screenSize = "1280x720")

    @Test fun headsetsStartUnsetGamesInXrAndPhonesIn2d() {
        assertEquals(Mode.XR, XrGameLaunchMode.current(plain, headset = true))
        assertEquals(Mode.FLAT, XrGameLaunchMode.current(plain, headset = false))
    }

    @Test fun xrCapsTheResolutionAndOtherModesKeepIt() {
        val large = plain.copy(screenSize = "2560x1440")
        assertEquals("1920x1080", XrGameLaunchMode.apply(large, Mode.XR, vrGame = false).screenSize)
        assertEquals("1920x1080", XrGameLaunchMode.apply(large, Mode.XR, vrGame = true).screenSize)
        assertEquals("2560x1440", XrGameLaunchMode.apply(large, Mode.FLAT, vrGame = false).screenSize)
        assertEquals("1600x900", XrGameLaunchMode.apply(plain.copy(screenSize = "1600x900"), Mode.XR, vrGame = false).screenSize)
    }

    @Test fun phonesOfferOnly2dAndSbs() {
        assertEquals(listOf(Mode.FLAT, Mode.SBS), XrGameLaunchMode.available(headset = false))
        assertEquals(listOf(Mode.FLAT, Mode.SBS, Mode.XR), XrGameLaunchMode.available(headset = true))
        assertFalse(XrGameLaunchMode.allowed(Mode.XR, vrGame = false, headset = false))
    }

    @Test fun vrGamesCannotRunIn2d() {
        assertFalse(XrGameLaunchMode.allowed(Mode.FLAT, vrGame = true, headset = true))
        assertEquals(Mode.XR, XrGameLaunchMode.fallback(Mode.FLAT, vrGame = true, headset = true))
        assertEquals(Mode.SBS, XrGameLaunchMode.fallback(Mode.FLAT, vrGame = true, headset = false))
    }

    @Test fun containersFromBeforeThePanelKeepTheirMode() {
        val openXr = plain.copy(windowsVrEnabled = true, xrPresentationMode = "openxr")
        assertEquals(Mode.XR, XrGameLaunchMode.current(openXr, headset = true))
        assertEquals(Mode.SBS, XrGameLaunchMode.current(openXr, headset = false))
        val cinema = plain.copy(sbsTheaterEnabled = true)
        assertEquals(Mode.SBS, XrGameLaunchMode.current(cinema, headset = true))
        val bigScreen = plain.copy(launchImmersiveMode = true)
        assertEquals(Mode.XR, XrGameLaunchMode.current(bigScreen, headset = true))
        assertEquals(Mode.FLAT, XrGameLaunchMode.current(bigScreen, headset = false))
    }

    @Test fun aSaved2dChoiceStays2dOnAHeadset() {
        val saved = XrGameLaunchMode.apply(plain, Mode.FLAT, vrGame = false)
        assertEquals(Mode.FLAT, XrGameLaunchMode.current(saved, headset = true))
    }

    @Test fun everyChoiceReadsBackAndSetsTheRuntimeFields() {
        for (vrGame in listOf(false, true)) {
            for (mode in Mode.entries) {
                if (!XrGameLaunchMode.allowed(mode, vrGame, headset = true)) continue
                val saved = XrGameLaunchMode.apply(plain, mode, vrGame)
                assertEquals("$mode vr=$vrGame", mode, XrGameLaunchMode.current(saved, headset = true))
                assertEquals(vrGame, saved.windowsVrEnabled)
                // At most one presentation path is armed.
                val armed = listOf(saved.windowsVrEnabled, saved.sbsTheaterEnabled, saved.launchImmersiveMode).count { it }
                assertTrue("$mode vr=$vrGame", armed <= 1)
            }
        }
        val vrXr = XrGameLaunchMode.apply(plain, Mode.XR, vrGame = true)
        assertEquals("openxr", vrXr.xrPresentationMode)
        val screen = XrGameLaunchMode.apply(plain, Mode.XR, vrGame = false)
        assertTrue(screen.launchImmersiveMode)
        assertEquals("sbs", screen.xrPresentationMode)
    }
}
