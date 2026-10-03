package com.winlator.xserver.extensions

import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33], manifest = Config.NONE)
class PresentCopySettingsTest {
    @Test
    fun `empty pacer sleeps until work and close prevents resurrection`() {
        val present = PresentExtension()
        val type = PresentExtension::class.java
        val start = type.getDeclaredMethod("startCpuPacer").apply { isAccessible = true }
        val threadField = type.getDeclaredField("cpuPacerThread").apply { isAccessible = true }
        val lock = type.getDeclaredField("choreographerLock").apply { isAccessible = true }.get(present)
        synchronized(lock) { start.invoke(present) }
        val thread = threadField.get(present) as Thread
        fun awaitState(state: Thread.State) {
            val deadline = System.nanoTime() + 2_000_000_000L
            while (thread.state != state && System.nanoTime() < deadline) Thread.sleep(1)
            assertEquals(state, thread.state)
        }
        try {
            awaitState(Thread.State.WAITING)
            val idleType = type.declaredClasses.single { it.simpleName == "PendingIdle" }
            val constructor = idleType.declaredConstructors.single().apply { isAccessible = true }
            val idle = constructor.newInstance(null, null, 0, 0, System.nanoTime() + 60_000_000_000L, 0, null)
            type.getDeclaredMethod("enqueueCpuIdle", idleType).apply { isAccessible = true }.invoke(present, idle)
            awaitState(Thread.State.TIMED_WAITING)
        } finally {
            present.close()
            thread.join(2000)
        }
        assertFalse(thread.isAlive)
        synchronized(lock) { start.invoke(present) }
        assertNull(threadField.get(present))
    }

    @Test
    fun `pipeline switch waits for outstanding copies and limiter retains request`() {
        org.junit.Assume.assumeTrue(app.gamenative.BuildConfig.XRGAME)
        val present = PresentExtension()
        present.setAsyncCopy(true)
        val outstanding = PresentExtension::class.java.getDeclaredField("outstandingCopies").apply { isAccessible = true }
        outstanding.setInt(present, 1)
        present.setCopyPipeline(true)
        assertEquals(false, present.diagnosticSnapshot()["copyPipelineActive"])
        assertEquals(true, present.diagnosticSnapshot()["copyPipelineRequested"])
        outstanding.setInt(present, 0)
        PresentExtension::class.java.getDeclaredMethod("applyCopySettingsWhenDrained").apply {
            isAccessible = true
            invoke(present)
        }
        assertEquals(true, present.diagnosticSnapshot()["copyPipelineActive"])
        present.setFrameRateLimit(60)
        assertEquals(false, present.diagnosticSnapshot()["copyPipelineActive"])
        present.setFrameRateLimit(0)
        assertEquals(true, present.diagnosticSnapshot()["copyPipelineActive"])
    }

    @Test
    fun `render ahead persists without altering copy mode or escaped environment values`() {
        val settings = app.gamenative.xrgame.XrGamePresentSettings
        val original = com.winlator.core.envvars.EnvVars().apply {
            put("TEST_PATH", "C:\\Directory with spaces\\config")
            put(settings.ASYNC_COPY_ENV, "1")
        }
        assertFalse(settings.renderAheadEnabled(original.toString()))
        val enabled = settings.withRenderAhead(original.toString(), true)
        assertTrue(settings.renderAheadEnabled(enabled))
        assertTrue(settings.asyncCopyEnabled(enabled))
        val disabled = settings.withRenderAhead(enabled, false)
        assertFalse(settings.renderAheadEnabled(disabled))
        assertTrue(settings.asyncCopyEnabled(disabled))
        assertEquals(original.get("TEST_PATH"), com.winlator.core.envvars.EnvVars(disabled).get("TEST_PATH"))
    }

    @Test
    fun `limiter defers until accepted copies retire and preserves user opt in`() {
        org.junit.Assume.assumeTrue(app.gamenative.BuildConfig.XRGAME)
        val present = PresentExtension()
        present.setAsyncCopy(true)
        assertEquals(true, present.diagnosticSnapshot()["asyncCopyActive"])
        // Model a native copy whose fence has not signalled yet.
        val outstanding = PresentExtension::class.java.getDeclaredField("outstandingCopies").apply { isAccessible = true }
        outstanding.setInt(present, 1)
        present.setFrameRateLimit(60)
        assertEquals(0, present.diagnosticSnapshot()["frameRateLimit"])
        assertEquals(60, present.diagnosticSnapshot()["requestedFrameRateLimit"])
        assertEquals(true, present.diagnosticSnapshot()["asyncCopyActive"])
        outstanding.setInt(present, 0)
        PresentExtension::class.java.getDeclaredMethod("applyCopySettingsWhenDrained").apply {
            isAccessible = true
            invoke(present)
        }
        assertEquals(60, present.diagnosticSnapshot()["frameRateLimit"])
        assertEquals(false, present.diagnosticSnapshot()["asyncCopyActive"])
        assertEquals(true, present.diagnosticSnapshot()["asyncCopyRequested"])
        present.setFrameRateLimit(0)
        assertEquals(true, present.diagnosticSnapshot()["asyncCopyActive"])
        present.setAsyncCopy(false)
        assertEquals(false, present.diagnosticSnapshot()["asyncCopyActive"])
    }
}
