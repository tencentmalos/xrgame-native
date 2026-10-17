package app.gamenative.xrgame.steam

import java.util.Locale
import org.junit.Assert.assertEquals
import org.junit.Test

class SteamClientLanguageTest {
    private fun select(vararg tags: String) = SteamClientLanguage.select(tags.map(Locale::forLanguageTag))

    @Test fun chineseScriptTakesPriorityOverRegion() {
        assertEquals("schinese", select("zh-Hans-CN"))
        assertEquals("tchinese", select("zh-Hant-CN"))
        assertEquals("schinese", select("zh-Hans-HK"))
    }
    @Test fun traditionalRegionsWithoutScript() {
        for (tag in listOf("zh-TW", "zh-HK", "zh-MO")) assertEquals("tchinese", select(tag))
        assertEquals("schinese", select("zh-SG"))
    }
    @Test fun regionalSteamNames() {
        assertEquals("brazilian", select("pt-BR"))
        assertEquals("portuguese", select("pt-PT"))
        assertEquals("latam", select("es-MX"))
        assertEquals("latam", select("es-419"))
        assertEquals("spanish", select("es-ES"))
    }
    @Test fun orderedFallback() {
        assertEquals("japanese", select("xx-XX", "ja-JP", "en-US"))
        assertEquals("english", select("xx-XX"))
        assertEquals("english", select())
    }
    @Test fun fontconfigDoesNotReceiveAndroidScriptSubtags() {
        assertEquals("zh-cn", SteamClientLanguage.fontconfig(listOf(Locale.forLanguageTag("zh-Hans-CN"))))
        assertEquals("zh-tw", SteamClientLanguage.fontconfig(listOf(Locale.forLanguageTag("zh-Hant-HK"))))
        assertEquals("ja", SteamClientLanguage.fontconfig(listOf(Locale.JAPAN)))
    }
}
