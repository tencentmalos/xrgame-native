package app.gamenative.xrgame

import com.winlator.core.envvars.EnvVars
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class XrGameMsaaTest {
    private val turnip = "turnip-adb7e30-xrg16"

    @Test fun gameKeepsItsOwnMsaaUnlessEyeTrackedFoveationDrivesAVrGame() {
        val flat = EnvVars("XRGAME_XR_FOVEATION=eye")
        XrGameMsaa.configure(flat, vrGame = false, turnipVersion = turnip)
        assertFalse(flat.has(XrGameMsaa.TURNIP_ENV))

        val vr = EnvVars("XRGAME_XR_FOVEATION=eye XRGAME_MSAA=2x")
        XrGameMsaa.configure(vr, vrGame = true, turnipVersion = turnip)
        assertEquals("1", vr.get(XrGameMsaa.TURNIP_ENV))

        val fixed = EnvVars("XRGAME_XR_FOVEATION=fixed XRGAME_MSAA=2x")
        XrGameMsaa.configure(fixed, vrGame = true, turnipVersion = turnip)
        assertEquals("2", fixed.get(XrGameMsaa.TURNIP_ENV))
    }

    @Test fun storedModeRoundTripsAndOlderTurnipGetsNoCap() {
        val stored = XrGameMsaa.withMode("WINEESYNC=1", XrGameMsaa.Mode.OFF)
        assertEquals(XrGameMsaa.Mode.OFF, XrGameMsaa.stored(stored))
        assertEquals(XrGameMsaa.Mode.GAME, XrGameMsaa.stored(XrGameMsaa.withMode(stored, XrGameMsaa.Mode.GAME)))
        assertFalse(EnvVars(XrGameMsaa.withMode(stored, XrGameMsaa.Mode.GAME)).has(XrGameMsaa.ENV))

        val older = EnvVars("XRGAME_MSAA=off TU_MAX_SAMPLES=1")
        XrGameMsaa.configure(older, vrGame = false, turnipVersion = "turnip-adb7e30-xrg14")
        assertFalse(older.has(XrGameMsaa.TURNIP_ENV))
    }
}
