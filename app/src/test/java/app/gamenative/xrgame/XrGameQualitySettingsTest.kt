package app.gamenative.xrgame

import com.winlator.core.envvars.EnvVars
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class XrGameQualitySettingsTest {
    @Test fun unsetOrUnknownValuesMeanTheDefaults() {
        assertEquals("high", XrGameQualitySettings.textureQuality("WINEESYNC=1"))
        assertEquals("high", XrGameQualitySettings.textureQuality("XRGAME_TEXTURE_QUALITY=ultra"))
        assertEquals("100", XrGameQualitySettings.renderScale("WINEESYNC=1"))
        assertEquals("100", XrGameQualitySettings.renderScale("XRGAME_RENDER_SCALE=60"))
    }

    @Test fun valuesRoundTripAndDefaultsRemoveTheVariables() {
        val reduced = XrGameQualitySettings.withRenderScale(XrGameQualitySettings.withTextureQuality("WINEESYNC=1", "low"), "37.5")
        assertEquals("low", XrGameQualitySettings.textureQuality(reduced))
        assertEquals("37.5", XrGameQualitySettings.renderScale(reduced))
        assertEquals("37.5", EnvVars(reduced).get(XrGameQualitySettings.RENDER_SCALE_ENV))
        assertEquals("1", EnvVars(reduced).get("WINEESYNC"))

        val restored = XrGameQualitySettings.withRenderScale(XrGameQualitySettings.withTextureQuality(reduced, "high"), "100")
        assertFalse(EnvVars(restored).has(XrGameQualitySettings.TEXTURE_QUALITY_ENV))
        assertFalse(EnvVars(restored).has(XrGameQualitySettings.RENDER_SCALE_ENV))
        assertEquals("1", EnvVars(restored).get("WINEESYNC"))
    }
}
