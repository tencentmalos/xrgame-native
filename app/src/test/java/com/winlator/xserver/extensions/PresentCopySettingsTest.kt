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
