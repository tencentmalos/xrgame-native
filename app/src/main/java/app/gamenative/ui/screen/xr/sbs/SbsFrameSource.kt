package app.gamenative.ui.screen.xr.sbs

import app.gamenative.ui.screen.xr.windows.WindowsVrFrameSource
import app.gamenative.ui.screen.xr.windows.WindowsVrRuntimeSnapshot
import kotlin.math.atan
import kotlin.math.tan

/** Android display ticks drive the same frame protocol used by the headset backend. */
class SbsFrameSource : WindowsVrFrameSource {
    private val lock = Object()
    private val tracking = SbsTracking()
    private var latestFrame: WindowsVrRuntimeSnapshot? = null
    private var serial = 0L
    private var active = false
    private var focused = true
    private var closed = false
    private var buttons = 0
    private val axes = FloatArray(8) // sticks, triggers, grips

    fun setActive(value: Boolean) = synchronized(lock) {
        if (closed) return@synchronized
        active = value
        tracking.suspendSampling()
        if (!value) { buttons = 0; axes.fill(0f); latestFrame = null }
        lock.notifyAll()
    }
    fun setFocused(value: Boolean) = synchronized(lock) {
        focused = value
        if (!value) { buttons = 0; axes.fill(0f) }
    }
    fun recenter() = synchronized(lock) { tracking.recenter() }
    fun gyro(x: Float, y: Float, z: Float, ns: Long) = synchronized(lock) {
        if (active) tracking.gyro(x, y, z, ns)
    }
    fun button(bit: Int, down: Boolean) = synchronized(lock) {
        if (bit in 0..9 && active && focused) buttons = if (down) buttons or (1 shl bit) else buttons and (1 shl bit).inv()
    }
    fun axis(index: Int, value: Float) = synchronized(lock) {
        if (active && focused && index in axes.indices && value.isFinite()) axes[index] = value.coerceIn(-1f, 1f)
    }

    fun tick(nowNs: Long, periodNs: Long, eyeWidth: Int, eyeHeight: Int) = synchronized(lock) {
        if (!active || closed || nowNs <= 0L || eyeWidth <= 0 || eyeHeight <= 0) return@synchronized
        val period = periodNs.coerceIn(5_000_000L, 50_000_000L)
        val q = tracking.orientation
        val views = FloatArray(22)
        val input = FloatArray(36)
        val vertical = 0.75f
        val horizontal = atan(tan(vertical) * eyeWidth.toFloat()/eyeHeight)
        for (eye in 0..1) {
            val base = eye*11
            q.copyInto(views, base)
            val p = SbsTracking.rotate(q, if (eye == 0) -0.032f else 0.032f, 0f, 0f)
            views[base+4] = p[0]; views[base+5] = 1.65f+p[1]; views[base+6] = p[2]
            views[base+7] = -horizontal; views[base+8] = horizontal
            views[base+9] = vertical; views[base+10] = -vertical
            val hand = eye*18
            input[hand] = axes[4+eye].coerceIn(0f, 1f)
            input[hand+1] = axes[6+eye].coerceIn(0f, 1f)
            input[hand+2] = axes[eye*2]; input[hand+3] = axes[eye*2+1]
            // Fixed synthetic controller poses in the reference space, independent
            // of head rotation. Buttons and analog controls remain fully live.
            for (offset in intArrayOf(4, 11)) {
                input[hand+offset+3] = 1f
                input[hand+offset+4] = if (eye == 0) -0.22f else 0.22f
                input[hand+offset+5] = 1.35f
                input[hand+offset+6] = -0.4f
            }
        }
        // FOCUSED, valid synthetic poses, 2 m virtual stage. Never claim tracked position.
        latestFrame = WindowsVrRuntimeSnapshot(
            longArrayOf(++serial, nowNs+period, period, if (focused) 5 else 4, 1, eyeWidth.toLong(), eyeHeight.toLong(),
                1, 2_000_000, 2_000_000, 1, tracking.recenterSerial),
            views, input, intArrayOf(3, buttons, if (focused) 3 else 0),
        )
        lock.notifyAll()
    }

    override fun waitFrame(afterSerial: Long, timeoutMs: Int): WindowsVrRuntimeSnapshot? = synchronized(lock) {
        val deadline = System.nanoTime()+timeoutMs.coerceIn(0, 1000)*1_000_000L
        while (!closed && active) {
            latestFrame?.takeIf { it.timing[0] > afterSerial }?.let { return@synchronized it }
            val remaining = deadline-System.nanoTime()
            if (remaining <= 0) break
            try { lock.wait(remaining/1_000_000L, (remaining%1_000_000L).toInt()) }
            catch (_: InterruptedException) { Thread.currentThread().interrupt(); break }
        }
        null
    }
    override fun latest(): WindowsVrRuntimeSnapshot? = synchronized(lock) { latestFrame }
    override fun applyHaptic(hand: Int, amplitude: Float, duration: Long, frequency: Float) = false
    override fun detach() = synchronized(lock) {
        closed = true; active = false; latestFrame = null; buttons = 0; axes.fill(0f); lock.notifyAll()
    }
}
