package app.gamenative.xrgame

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class XrGameCommandLineTest {
    @Test fun wineExplorerReceivesAnExecutablePathWithoutLiteralQuotes() {
        assertArrayEquals(arrayOf("wine", "explorer", "/desktop=shell,1280x720", "A:\\Game Folder\\7zFM.exe"),
            XrGameCommandLine.split("wine explorer /desktop=shell,1280x720 \"A:\\Game Folder\\7zFM.exe\""))
    }

    @Test fun quotedArgumentsKeepSpacesAndWindowsBackslashes() {
        assertArrayEquals(arrayOf("wine", "", "--key=two words", "C:\\data", "literal\"quote", "&&"),
            XrGameCommandLine.split("wine \"\" --key=\"two words\" C:\\data \"literal\\\"quote\" &&"))
        assertArrayEquals(arrayOf("wine", "two words"), XrGameCommandLine.split("wine 'two words'"))
        assertThrows(IllegalArgumentException::class.java) { XrGameCommandLine.split("wine \"unfinished") }
    }
}
