package app.gamenative.xrgame

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class XrGameConsoleStateTest {
    private val launch = listOf(
        "10/10 18:26:02 [CommandLine] -vr -steam -noasserts -nopassiveasserts +map startup -condebug",
        "10/10 18:26:28 [HostStateManager] CHostStateMgr::QueueNewRequest( Idle (mainmenu), 1 )",
        "10/10 18:26:29 [HostStateManager] Idle (mainmenu)",
        "10/10 18:26:29 [HostStateManager] CHostStateMgr::QueueNewRequest( Loading (startup), 2 )",
        "10/10 18:26:42 [Server] SV:  Game started",
        "10/10 18:26:42 [HostStateManager] Loading (startup)",
    )
    private val restore = listOf(
        "10/10 18:30:33 [HostStateManager] CHostStateMgr::QueueNewRequest( Restoring Save (s0/autosave), 3 )",
        "10/10 18:30:33 [Server] SV:  Disconnect client 'Player One' from server(1): NETWORK_DISCONNECT_SHUTDOWN",
        "10/10 18:31:00 [HostStateManager] Restoring Save (s0/autosave)",
    )

    private fun parse(lines: List<String>) = XrGameConsoleState.parse(lines.asSequence())

    @Test fun mainMenuIsNotAScene() {
        val state = parse(launch)
        assertEquals("Loading", state.hostState)
        assertEquals("startup", state.target)
        assertNull(state.pendingState)
        assertFalse(state.inScene)
        assertEquals("10/10 18:26:02", state.launchedAt)
    }

    @Test fun queuedRestoreIsStillLoading() {
        val state = parse(launch + restore.first())
        assertEquals("Restoring Save", state.pendingState)
        assertEquals("s0/autosave", state.pendingTarget)
        assertFalse(state.inScene)
    }

    @Test fun restoredSaveIsASceneUntilPaused() {
        assertTrue(parse(launch + restore).inScene)
        val paused = parse(launch + restore + "10/10 18:31:03 [Client] Player One paused the game")
        assertTrue(paused.paused)
        assertFalse(paused.inScene)
        val resumed = parse(launch + restore + listOf(
            "10/10 18:31:03 [Client] Player One paused the game",
            "10/10 18:32:11 [Client] Player One unpaused the game\r",
        ))
        assertFalse(resumed.paused)
        assertTrue(resumed.inScene)
        assertEquals("10/10 18:32:11", resumed.lastEventAt)
    }

    @Test fun aNewProcessResetsTheState() {
        val state = parse(launch + restore + "10/10 19:00:00 [CommandLine] -vr -steam +map startup")
        assertNull(state.hostState)
        assertFalse(state.inScene)
        assertEquals("10/10 19:00:00", state.launchedAt)
    }
}
