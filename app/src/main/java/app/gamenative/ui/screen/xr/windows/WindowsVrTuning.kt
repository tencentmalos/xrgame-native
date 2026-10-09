package app.gamenative.ui.screen.xr.windows

import app.gamenative.ui.screen.xr.XrNative
import kotlin.math.ceil

/**
 * Pacing and latency experiments for Windows VR games, switched at runtime through DebugBus
 * (`vr_tuning`). Everything is off by default, which keeps the behaviour measured in
 * docs/validation/xr-stage-timing-20261009.md.
 *
 * - [pacing]: how many XR frames each game frame spans. HALF holds FRAME_SYNC to every second XR
 *   frame (36 Hz at 72 Hz) so each game frame is shown exactly twice; AUTO picks 1-3 from the
 *   game's own frame time.
 * - [frameStartTargetUs]: the bridge delays FRAME_SYNC while the game is blocked on its previous
 *   frame's GPU work, keeping this much of that wait (µs). 0 disables the delay.
 * - [extendedPrediction]: poses are located at the measured display time of the game's frames
 *   instead of the next XR frame's.
 */
object WindowsVrTuning {
    enum class Pacing { OFF, AUTO, HALF }

    @Volatile var pacing: Pacing = Pacing.OFF
    @Volatile var frameStartTargetUs: Long = 0
    @Volatile var extendedPrediction: Boolean = false
        private set

    /** Native switch for [extendedPrediction]; replaced in unit tests. */
    internal var predictionSink: (Boolean) -> Unit = { XrNative.nativeSetWindowsPrediction(it) }

    /**
     * Applies `pacing=off|auto|half`, `start=<µs 0..20000>` and `predict=0|1`. Returns an error
     * token, or null when every argument was valid (then all of them are applied).
     */
    @Synchronized
    fun apply(arguments: List<String>): String? {
        var newPacing = pacing
        var newStart = frameStartTargetUs
        var newPredict = extendedPrediction
        for (argument in arguments) {
            val separator = argument.indexOf('=')
            if (separator <= 0) return "malformed_argument"
            val value = argument.substring(separator + 1)
            when (argument.substring(0, separator)) {
                "pacing" -> newPacing = Pacing.entries.firstOrNull { it.name.equals(value, ignoreCase = true) }
                    ?: return "invalid_pacing"
                "start" -> newStart = value.toLongOrNull()?.takeIf { it in 0..20_000 } ?: return "invalid_start"
                "predict" -> newPredict = when (value) {
                    "0" -> false
                    "1" -> true
                    else -> return "invalid_predict"
                }
                else -> return "unknown_argument"
            }
        }
        pacing = newPacing
        frameStartTargetUs = newStart
        if (newPredict != extendedPrediction) {
            predictionSink(newPredict)
            extendedPrediction = newPredict
        }
        return null
    }
}

/**
 * Chooses how many XR frames each game frame spans. The game's frame time is the time from a
 * FRAME_SYNC reply to its next request, without the delay the bridge chose to add; AUTO switches
 * only after the new choice held for [SWITCH_FRAMES] requests.
 */
class WindowsVrPacing {
    private var workEmaNs = 0.0
    private var step = 1
    private var candidate = 1
    private var candidateFrames = 0
    private var lastReplyNs = 0L

    @Synchronized
    fun onRequest(nowNs: Long, delayNs: Long) {
        if (lastReplyNs == 0L) return
        val work = (nowNs - lastReplyNs - delayNs).coerceAtLeast(0L).toDouble()
        workEmaNs = if (workEmaNs == 0.0) work else workEmaNs + EMA_WEIGHT * (work - workEmaNs)
    }

    @Synchronized
    fun onReply(nowNs: Long) {
        lastReplyNs = nowNs
    }

    @Synchronized
    fun step(mode: WindowsVrTuning.Pacing, periodNs: Long): Int {
        step = when (mode) {
            WindowsVrTuning.Pacing.OFF -> 1
            WindowsVrTuning.Pacing.HALF -> 2
            WindowsVrTuning.Pacing.AUTO -> autoStep(periodNs)
        }
        return step
    }

    private fun autoStep(periodNs: Long): Int {
        if (periodNs <= 0 || workEmaNs == 0.0) return step
        val desired = ceil((workEmaNs + MARGIN_NS) / periodNs).toInt().coerceIn(1, 3)
        if (desired == step) {
            candidateFrames = 0
            return step
        }
        if (desired != candidate) {
            candidate = desired
            candidateFrames = 0
        }
        if (++candidateFrames >= SWITCH_FRAMES) {
            candidateFrames = 0
            return desired
        }
        return step
    }

    companion object {
        private const val EMA_WEIGHT = 0.1
        private const val MARGIN_NS = 1_000_000.0
        const val SWITCH_FRAMES = 30
    }
}
