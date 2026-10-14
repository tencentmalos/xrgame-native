package app.gamenative.xrgame

import com.winlator.core.envvars.EnvVars
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class XrGameQualitySettingsTest {
    @Test fun unsetOrUnknownValuesMeanTheDefaults() {
        assertEquals("high", XrGameQualitySettings.textureQuality("WINEESYNC=1"))
        assertEquals("high", XrGameQualitySettings.textureQuality("XRGAME_TEXTURE_QUALITY=ultra"))
    }

    @Test fun valuesRoundTripAndDefaultsRemoveTheVariables() {
        val reduced = XrGameQualitySettings.withTextureQuality("WINEESYNC=1", "low")
        assertEquals("low", XrGameQualitySettings.textureQuality(reduced))
        assertEquals("low", EnvVars(reduced).get(XrGameQualitySettings.TEXTURE_QUALITY_ENV))
        assertEquals("1", EnvVars(reduced).get("WINEESYNC"))

        val restored = XrGameQualitySettings.withTextureQuality(reduced, "high")
        assertFalse(EnvVars(restored).has(XrGameQualitySettings.TEXTURE_QUALITY_ENV))
        assertEquals("1", EnvVars(restored).get("WINEESYNC"))
    }
}
