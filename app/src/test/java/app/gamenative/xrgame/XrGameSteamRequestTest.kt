package app.gamenative.xrgame

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.test.runTest
import org.junit.Assert.*
import org.junit.Test

class XrGameSteamRequestTest {
    @Test fun `nullable result is not a timeout`() = runTest {
        assertNull(XrGameSteamRequest.run<String?>("test") { null })
    }

    @Test fun `deadline cancels the request child before returning`() = runTest {
        var childStopped = false
        try {
            XrGameSteamRequest.run("servers", 100) {
                async {
                    try { awaitCancellation() } finally { childStopped = true }
                }.await()
            }
            fail("request must time out")
        } catch (e: XrGameSteamRequest.TimedOut) {
            assertEquals("servers", e.stage)
            assertTrue(childStopped)
        }
    }

    @Test fun `user cancellation is preserved`() = runTest {
        val cancellation = CancellationException("paused")
        try {
            XrGameSteamRequest.run("depot") { throw cancellation }
            fail("request must cancel")
        } catch (e: CancellationException) {
            assertEquals(cancellation.message, e.message)
        }
    }

    @Test fun `nested deadline remains cancellation`() = runTest {
        try {
            XrGameSteamRequest.run("outer", 100) {
                kotlinx.coroutines.withTimeout(10) { awaitCancellation() }
            }
            fail("nested deadline must fire")
        } catch (e: kotlinx.coroutines.TimeoutCancellationException) {
            assertEquals(10L, testScheduler.currentTime)
        }
    }

    @Test fun `request failure is preserved`() = runTest {
        val failure = IllegalStateException("denied")
        try {
            XrGameSteamRequest.run("depot") { throw failure }
            fail("request must fail")
        } catch (e: IllegalStateException) {
            assertEquals(failure.message, e.message)
        }
    }
}
