package app.gamenative.xrgame

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class XrGameDebugLaunchTest {
    @Test fun saveNamesAreRelativeIdentifiers() {
        assertTrue(XrGameDebugLaunch.isSaveName("s0/autosave"))
        assertTrue(XrGameDebugLaunch.isSaveName("quick"))
        assertFalse(XrGameDebugLaunch.isSaveName("../autosave"))
        assertFalse(XrGameDebugLaunch.isSaveName("s0/autosave;quit"))
        assertFalse(XrGameDebugLaunch.isSaveName("/s0/autosave"))
        assertFalse(XrGameDebugLaunch.isSaveName("a/b/c"))
    }

    @Test fun argumentsApplyOnceAndOnlyToTheirGame() {
        XrGameDebugLaunch.loadSave(546560, "s0/autosave")
        assertEquals("+load s0/autosave", XrGameDebugLaunch.pendingArguments())
        assertEquals("", XrGameDebugLaunch.take("STEAM_1145350"))
        assertEquals(" +load s0/autosave", XrGameDebugLaunch.take("STEAM_546560"))
        assertEquals("", XrGameDebugLaunch.take("STEAM_546560"))
        assertNull(XrGameDebugLaunch.pendingArguments())
    }
}
