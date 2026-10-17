package app.gamenative.xrgame

import org.junit.Assert.*
import org.junit.Test

class XrGameExitStateTest {
    private val helpers = setOf("steam", "explorer", "services", "conhost")
    private fun state() = XrGameExitState("MonsterHunterWorld.exe", helpers)

    @Test fun `official steam helper does not prevent normal exit`() {
        val state = state()
        val remaining = listOf("C:\\Program Files (x86)\\Steam\\steam.exe", "explorer.exe", "conhost.exe")
        assertFalse(state.update(remaining, true, 1000))
        assertFalse(state.update(remaining, true, 3999))
        assertTrue(state.update(remaining, true, 4000))
    }

    @Test fun `unmapped or minimized game that still runs is never stopped`() {
        val state = state()
        assertFalse(state.update(listOf("MONSTERHUNTERWORLD.EXE"), true, 0))
        assertFalse(state.update(listOf("MONSTERHUNTERWORLD.EXE"), true, 60000))
    }

    @Test fun `self restart resets the exit grace period`() {
        val state = state()
        assertFalse(state.update(listOf("steam.exe"), true, 0))
        assertFalse(state.update(listOf("MonsterHunterWorld.exe"), true, 2000))
        assertFalse(state.update(listOf("steam.exe"), true, 4000))
        assertFalse(state.update(listOf("steam.exe"), true, 6000))
        assertTrue(state.update(listOf("steam.exe"), true, 7000))
    }

    @Test fun `surviving child delays session and cloud completion`() {
        val state = state()
        assertFalse(state.update(listOf("save-writer.exe"), true, 0))
        assertFalse(state.update(listOf("save-writer.exe"), true, 9000))
        assertFalse(state.update(emptyList(), true, 10000))
        assertTrue(state.update(emptyList(), true, 13000))
    }

    @Test fun `incomplete snapshots reset grace instead of simulating exit`() {
        val state = state()
        assertFalse(state.update(emptyList(), true, 0))
        assertFalse(state.update(emptyList(), false, 4000))
        assertFalse(state.update(emptyList(), true, 5000))
        assertTrue(state.update(emptyList(), true, 8000))
    }

    @Test fun `custom client loader still owns its process family`() {
        assertFalse(state().update(listOf("ColdClientLoader.exe"), true, 100000))
    }
}
