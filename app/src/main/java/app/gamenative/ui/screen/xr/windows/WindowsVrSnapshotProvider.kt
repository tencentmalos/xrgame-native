package app.gamenative.ui.screen.xr.windows

import android.graphics.Bitmap
import app.gamenative.ui.screen.xr.XrNative

data class WindowsVrRuntimeSnapshot(
    val timing: LongArray,
    val views: FloatArray,
    val input: FloatArray,
    val flags: IntArray,
)

interface WindowsVrFrameSource {
    fun waitFrame(afterSerial: Long, timeoutMs: Int): WindowsVrRuntimeSnapshot?
    fun latest(): WindowsVrRuntimeSnapshot?
    fun applyHaptic(hand: Int, amplitude: Float, duration: Long, frequency: Float): Boolean
    fun detach()

    /** Shows [bitmap] as a world-locked panel instead of the game's frames; null hides it. */
    fun setInterstitial(bitmap: Bitmap?) {}
}

class WindowsVrSnapshotProvider : WindowsVrFrameSource {
    @Volatile
    private var handle = 0L
    @Volatile
    private var latest: WindowsVrRuntimeSnapshot? = null
    private var interstitial: Bitmap? = null

    private val lock = Any()

    fun attach(handle: Long) {
        synchronized(lock) {
            this.handle = handle
            if (handle != 0L) XrNative.nativeSetWindowsInterstitial(handle, interstitial)
        }
    }

    override fun setInterstitial(bitmap: Bitmap?) {
        synchronized(lock) {
            interstitial = bitmap
            if (handle != 0L) XrNative.nativeSetWindowsInterstitial(handle, bitmap)
        }
    }

    override fun detach() {
        synchronized(lock) {
            handle = 0L
            latest = null
        }
    }

    override fun waitFrame(afterSerial: Long, timeoutMs: Int): WindowsVrRuntimeSnapshot? {
        val activeHandle = handle
        if (activeHandle == 0L) return null
        val snapshot = WindowsVrRuntimeSnapshot(LongArray(12), FloatArray(22), FloatArray(36), IntArray(3))
        if (!XrNative.nativeWaitWindowsFrame(
                activeHandle,
                afterSerial,
                timeoutMs,
                snapshot.timing,
                snapshot.views,
                snapshot.input,
                snapshot.flags,
            )) return null
        synchronized(lock) {
            if (handle != activeHandle) return null
            val current = latest
            if (current == null || current.timing[0] <= snapshot.timing[0]) latest = snapshot
        }
        return snapshot
    }

    override fun latest(): WindowsVrRuntimeSnapshot? = latest

    override fun applyHaptic(hand: Int, amplitude: Float, duration: Long, frequency: Float): Boolean {
        val activeHandle = handle
        return activeHandle != 0L && XrNative.nativeApplyWindowsHaptic(
            activeHandle,
            hand,
            amplitude,
            duration,
            frequency,
        )
    }
}
