package app.gamenative.ui.screen.xr

import android.os.SystemClock
import com.winlator.inputcontrols.ExternalController
import com.winlator.inputcontrols.GamepadState
import com.winlator.winhandler.WinHandler
import kotlin.math.abs

/**
 * Feeds the XR controllers into Wine as a standard Xbox-layout gamepad. A physical gamepad used in
 * the same session (a Bluetooth Xbox pad) shares the slot: whichever input was pressed last drives
 * it, so neither overwrites the other while it rests.
 */
class XrGamepadBridge(
    private val winHandler: WinHandler,
    private val slot: Int = 0,
) {
    companion object {
        // Bit indices, matching the layout WinHandler.sendVirtualGamepadState already assumes.
        const val BUTTON_A = 0
        const val BUTTON_B = 1
        const val BUTTON_X = 2
        const val BUTTON_Y = 3
        const val BUTTON_LB = 4
        const val BUTTON_RB = 5
        const val BUTTON_BACK = 6
        const val BUTTON_START = 7
        const val BUTTON_L3 = 8
        const val BUTTON_R3 = 9

        /** Stick, trigger or grip travel that counts as using an input, not resting drift. */
        const val ACTIVE_THRESHOLD = 0.35f
        const val XR_INPUT = "XR controllers"

        fun isSignificant(state: GamepadState): Boolean =
            abs(state.thumbLX) > ACTIVE_THRESHOLD || abs(state.thumbLY) > ACTIVE_THRESHOLD ||
                abs(state.thumbRX) > ACTIVE_THRESHOLD || abs(state.thumbRY) > ACTIVE_THRESHOLD ||
                state.triggerL > ACTIVE_THRESHOLD || state.triggerR > ACTIVE_THRESHOLD || state.dpad.any { it }

        /** A short ASCII label for the status layer. */
        fun shortName(name: String): String = when {
            name.contains("xbox", ignoreCase = true) -> "Xbox pad"
            name.contains("dualsense", ignoreCase = true) -> "DualSense"
            name.contains("dualshock", ignoreCase = true) || name.startsWith("Wireless Controller") -> "DualShock"
            else -> name.filter { it.code in 32..126 }.take(16)
        }
    }

    @Volatile private var physical: ExternalController? = null
    @Volatile private var physicalUsedAt = 0L
    @Volatile private var xrUsedAt = 0L

    private val physicalActive: Boolean
        get() = physical != null && physicalUsedAt > xrUsedAt

    /** The input currently driving the slot, for the status layer. */
    val activeInput: String
        get() = physical?.takeIf { physicalActive }?.let { shortName(it.name) } ?: XR_INPUT

    /** The physical gamepad driving the slot, or -1 while the XR controllers do. */
    val activePhysicalDeviceId: Int
        get() = physical?.takeIf { physicalActive }?.deviceId ?: -1

    /** A physical gamepad was pressed (UI thread); it drives the slot until the XR controllers are. */
    fun notePhysicalInput(controller: ExternalController) {
        physical = controller
        physicalUsedAt = SystemClock.uptimeMillis()
    }

    /** Writes the physical gamepad's state while it is the active input; true when it did. */
    fun commitPhysicalIfActive(): Boolean {
        val controller = physical ?: return false
        if (!physicalActive) return false
        winHandler.sendVirtualGamepadState(controller.state, slot)
        return true
    }

    enum class Stick { LEFT, RIGHT }
    enum class Trigger { LEFT, RIGHT }

    private val state = GamepadState()

    fun updateButton(buttonIndex: Int, pressed: Boolean) {
        state.setPressed(buttonIndex, pressed)
    }

    fun updateThumbstick(stick: Stick, x: Float, y: Float) {
        val cx = x.coerceIn(-1f, 1f)
        val cy = y.coerceIn(-1f, 1f)
        when (stick) {
            Stick.LEFT -> {
                state.thumbLX = cx
                state.thumbLY = cy
            }
            Stick.RIGHT -> {
                state.thumbRX = cx
                state.thumbRY = cy
            }
        }
    }

    fun updateTrigger(trigger: Trigger, value: Float) {
        val clamped = value.coerceIn(0f, 1f)
        when (trigger) {
            Trigger.LEFT -> state.triggerL = clamped
            Trigger.RIGHT -> state.triggerR = clamped
        }
    }

    /** Pushes the accumulated state to Wine. Call once per frame after the update* calls above. */
    fun commit() {
        winHandler.sendVirtualGamepadState(state, slot)
    }

    /**
     * Clears every button and axis and commits the neutral state, so nothing stays latched in the
     * game while input is being routed somewhere else (quick menu, XR pointer mode).
     */
    fun reset() {
        for (bit in BUTTON_A..BUTTON_R3) {
            state.setPressed(bit, false)
        }
        state.thumbLX = 0f
        state.thumbLY = 0f
        state.thumbRX = 0f
        state.thumbRY = 0f
        state.triggerL = 0f
        state.triggerR = 0f
        commit()
    }

    /**
     * Applies one raw snapshot as read from [XrNative.nativePollSnapshot] and commits it.
     * buttonBitmask bit layout matches [BUTTON_A]..[BUTTON_R3]; axes is
     * [leftX, leftY, rightX, rightY, triggerL, triggerR].
     */
    fun applySnapshot(buttonBitmask: Int, axes: FloatArray) {
        if (buttonBitmask != 0 || axes.any { abs(it) > ACTIVE_THRESHOLD }) xrUsedAt = SystemClock.uptimeMillis()
        if (commitPhysicalIfActive()) return
        for (bit in BUTTON_A..BUTTON_R3) {
            updateButton(bit, (buttonBitmask and (1 shl bit)) != 0)
        }
        updateThumbstick(Stick.LEFT, axes[0], axes[1])
        updateThumbstick(Stick.RIGHT, axes[2], axes[3])
        updateTrigger(Trigger.LEFT, axes[4])
        updateTrigger(Trigger.RIGHT, axes[5])
        commit()
    }
}
