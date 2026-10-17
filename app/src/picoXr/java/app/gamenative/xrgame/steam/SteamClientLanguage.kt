// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import java.util.Locale

/** Android's ordered system locales -> Valve's documented language identifiers. */
object SteamClientLanguage {
    private val languages = mapOf(
        "ar" to "arabic", "bg" to "bulgarian", "cs" to "czech", "da" to "danish",
        "de" to "german", "el" to "greek", "en" to "english", "fi" to "finnish",
        "fr" to "french", "hu" to "hungarian", "id" to "indonesian", "it" to "italian",
        "ja" to "japanese", "ko" to "koreana", "nl" to "dutch", "no" to "norwegian",
        "nb" to "norwegian", "nn" to "norwegian", "pl" to "polish", "ro" to "romanian",
        "ru" to "russian", "sv" to "swedish", "th" to "thai", "tr" to "turkish",
        "uk" to "ukrainian", "vi" to "vietnamese",
    )
    private val latinAmerica = setOf("419", "AR", "BO", "CL", "CO", "CR", "CU", "DO", "EC", "GT", "HN", "MX", "NI", "PA", "PE", "PR", "PY", "SV", "UY", "VE")

    fun select(locales: List<Locale>): String = locales.firstNotNullOfOrNull { locale ->
        when (locale.language) {
            "zh" -> when (locale.script) {
                "Hant" -> "tchinese"
                "Hans" -> "schinese"
                else -> if (locale.country in setOf("TW", "HK", "MO")) "tchinese" else "schinese"
            }
            "pt" -> if (locale.country == "BR") "brazilian" else "portuguese"
            "es" -> if (locale.country in latinAmerica) "latam" else "spanish"
            else -> languages[locale.language]
        }
    } ?: "english"

    // Fontconfig rejects Android tags containing script subtags (zh-Hans-CN).
    fun fontconfig(locales: List<Locale>): String = when (val language = select(locales)) {
        "schinese" -> "zh-cn"
        "tchinese" -> "zh-tw"
        "brazilian" -> "pt-br"
        "portuguese" -> "pt"
        "latam", "spanish" -> "es"
        else -> languages.entries.firstOrNull { it.value == language }?.key ?: "en"
    }
}
