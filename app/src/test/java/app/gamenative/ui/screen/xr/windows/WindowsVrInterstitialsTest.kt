package app.gamenative.ui.screen.xr.windows

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class WindowsVrInterstitialsTest {
    private val published = mutableListOf<WindowsVrInterstitials.State?>()
    private val interstitials = WindowsVrInterstitials { published += it }

    @Test fun alyxLoadingSequenceShowsTextThenTriggerPromptThenHides() {
        // Messages as Half-Life: Alyx sent them on Swan (OpenComposite log, 2026-10-09).
        assertTrue(interstitials.onMailboxMessage("hlvr/interstitials", """{
            "type": "begin_loading",
            "mapName": "a1_intro_world",
            "chapter": "",
            "tipText": "",
            "description": "小窍门：手腕附近的位置可以存放额外的物品，将物品放入该处即可。",
            "loadingText": "正在加载",
            "facingAngle": 4.752517
        }"""))
        assertTrue(interstitials.onMailboxMessage("hlvr/interstitials",
            """{ "type": "show_message", "text": "按下 Trigger 以开始" }"""))
        assertTrue(interstitials.onMailboxMessage("hlvr/interstitials", """{ "type": "end_loading" }"""))

        assertEquals(3, published.size)
        assertEquals(WindowsVrInterstitials.State(
            loadingText = "正在加载",
            description = "小窍门：手腕附近的位置可以存放额外的物品，将物品放入该处即可。",
            tip = "",
            message = "",
        ), published[0])
        assertEquals("按下 Trigger 以开始", published[1]!!.message)
        assertEquals("正在加载", published[1]!!.loadingText)
        assertNull(published[2])
    }

    @Test fun otherMailboxesAndMessageTypesAreIgnored() {
        assertFalse(interstitials.onMailboxMessage("hlvr/other", """{ "type": "begin_loading" }"""))
        assertFalse(interstitials.onMailboxMessage("hlvr/interstitials", """{ "type": "set_paths" }"""))
        assertFalse(interstitials.onMailboxMessage("hlvr/interstitials", "not json"))
        assertTrue(published.isEmpty())
    }

    @Test fun repeatedMessagesPublishOnceAndEndWithoutBeginIsQuiet() {
        val begin = """{ "type": "begin_loading", "loadingText": "Loading" }"""
        assertTrue(interstitials.onMailboxMessage("hlvr/interstitials", begin))
        assertTrue(interstitials.onMailboxMessage("hlvr/interstitials", begin))
        assertEquals(1, published.size)
        interstitials.clear()
        interstitials.clear()
        assertEquals(listOf(published[0], null), published)
        assertTrue(interstitials.onMailboxMessage("hlvr/interstitials", """{ "type": "end_loading" }"""))
        assertEquals(2, published.size)
    }

    @Test fun showMessageWithoutLoadingStillShowsThePrompt() {
        assertTrue(interstitials.onMailboxMessage("hlvr/interstitials", """{ "type": "show_message", "text": "Press Trigger" }"""))
        assertEquals(WindowsVrInterstitials.State("", "", "", "Press Trigger"), published.single())
    }
}
