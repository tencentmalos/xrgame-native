package app.gamenative.xrgame

import app.gamenative.utils.ManifestEntry
import java.io.File
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class XrGameComponentsTest {
    @get:Rule val temporary = TemporaryFolder()
    private val entry = ManifestEntry("wine", "Wine", "https://github.com/tencentmalos/xrgame-native/releases/download/runtime-1/wine.wcp",
        sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", license = "LGPL-2.1-or-later", source = "references/proton-wine@5d0d333e")

    @Test fun rejectsUpstreamMutableAndAmbiguousUrls() {
        for (url in listOf("https://downloads.gamenative.app/wine.wcp",
            "https://github.com/GameNative/proton-wine/releases/download/v1/wine.wcp",
            "https://github.com/tencentmalos/xrgame-native/raw/malos/main/wine.wcp",
            "https://github.com/tencentmalos/xrgame-native/releases/download/v1/../wine.wcp",
            "https://github.com/tencentmalos/xrgame-native/releases/download/v1/wine.wcp?redirect=x",
            "http://github.com/tencentmalos/xrgame-native/releases/download/v1/wine.wcp")) {
            assertThrows(IllegalArgumentException::class.java) { XrGameComponents.validate(entry.copy(url = url)) }
        }
        XrGameComponents.validate(entry)
    }

    @Test fun requiresDigestAndProvenance() {
        assertThrows(IllegalArgumentException::class.java) { XrGameComponents.validate(entry.copy(sha256 = null)) }
        assertThrows(IllegalArgumentException::class.java) { XrGameComponents.validate(entry.copy(license = "")) }
        assertThrows(IllegalArgumentException::class.java) { XrGameComponents.validate(entry.copy(source = null)) }
    }

    @Test fun rejectsCorruptedMissingAndTruncatedFiles() {
        val file = temporary.newFile()
        file.writeText("abc")
        assertTrue(XrGameComponents.verify(file, entry.sha256!!))
        file.writeText("ab")
        assertFalse(XrGameComponents.verify(file, entry.sha256!!))
        file.writeText("abd")
        assertFalse(XrGameComponents.verify(file, entry.sha256!!))
        assertFalse(XrGameComponents.verify(File(temporary.root, "missing"), entry.sha256!!))
    }
}
