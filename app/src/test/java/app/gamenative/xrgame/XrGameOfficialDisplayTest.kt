package app.gamenative.xrgame

import app.gamenative.xrgame.XrGameLaunchMode.Mode
import app.gamenative.xrgame.XrGameOfficialSteam.Display
import org.junit.Assert.assertEquals
import org.junit.Test

class XrGameOfficialDisplayTest {
    @Test fun `ordinary games follow the entry on a headset`() {
        assertEquals(Mode.XR, XrGameOfficialSteam.launchMode(vrGame = false, headset = true, entry = Display.XR))
        assertEquals(Mode.FLAT, XrGameOfficialSteam.launchMode(vrGame = false, headset = true, entry = Display.FLAT))
    }

    @Test fun `VR games run in VR whatever the entry`() {
        assertEquals(Mode.XR, XrGameOfficialSteam.launchMode(vrGame = true, headset = true, entry = Display.FLAT))
        assertEquals(Mode.XR, XrGameOfficialSteam.launchMode(vrGame = true, headset = true, entry = Display.XR))
    }

    @Test fun `off a headset ordinary games stay 2D and VR games keep SBS`() {
        assertEquals(Mode.FLAT, XrGameOfficialSteam.launchMode(vrGame = false, headset = false, entry = Display.XR))
        assertEquals(Mode.SBS, XrGameOfficialSteam.launchMode(vrGame = true, headset = false, entry = Display.XR))
    }
}
