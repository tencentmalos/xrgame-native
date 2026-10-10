package app.gamenative.ui.screen.xr

import android.os.SystemClock

/**
 * DebugBus `input`: synthetic controller state for automation. It is merged into the real
 * controllers in the native session, so it drives the flat game, the immersive quick menu and
 * Windows VR games through their normal paths. It only takes effect while an immersive session
 * is running and expires on its own.
 *
 * - `btn <name>[_<name>...] [ms]` holds buttons (a b x y lb rb back start l3 r3 menu). Menu
 *   held under 600 ms is a Start press for the game; 600 ms or more toggles the quick menu.
 * - `axis <name> <-1..1> [ms]` sets one axis of the controller snapshot: lx ly rx ry (sticks; the
 *   quick menu reads ly > 0 as down, games get the value as their XInput Y unchanged), lt rt
 *   (triggers), lg rg (grips).
 * - `release` drops everything immediately.
 */
object XrDebugInput {
    const val MENU_BIT = 1 shl 16
    const val MAX_DURATION_MS = 10_000
    const val DEFAULT_BUTTON_MS = 150
    const val DEFAULT_AXIS_MS = 300

    private val BUTTONS = mapOf(
        "a" to XrGamepadBridge.BUTTON_A, "b" to XrGamepadBridge.BUTTON_B,
        "x" to XrGamepadBridge.BUTTON_X, "y" to XrGamepadBridge.BUTTON_Y,
        "lb" to XrGamepadBridge.BUTTON_LB, "rb" to XrGamepadBridge.BUTTON_RB,
        "back" to XrGamepadBridge.BUTTON_BACK, "start" to XrGamepadBridge.BUTTON_START,
        "l3" to XrGamepadBridge.BUTTON_L3, "r3" to XrGamepadBridge.BUTTON_R3,
    )
    val AXES = listOf("lx", "ly", "rx", "ry", "lt", "rt", "lg", "rg")

    data class State(val buttons: Int = 0, val axisMask: Int = 0, val axes: List<Float> = List(8) { 0f }, val durationMs: Int = 0)

    /** Native switch; replaced in unit tests. */
    internal var sink: (State) -> Unit = {
        XrNative.nativeSetDebugInput(it.buttons, it.axisMask, it.axes.toFloatArray(), it.durationMs)
    }
    internal var clock: () -> Long = { SystemClock.elapsedRealtime() }

    @Volatile var last: State = State()
        private set

    @Volatile var expiresAtMs = 0L
        private set

    fun remainingMs(): Long = (expiresAtMs - clock()).coerceAtLeast(0L)

    /** Names of the buttons in a state, for status replies. */
    fun buttonNames(buttons: Int): List<String> =
        BUTTONS.filterValues { buttons and (1 shl it) != 0 }.keys.toList() + if (buttons and MENU_BIT != 0) listOf("menu") else emptyList()

    /** Parses and applies one request; returns an error token or null once it was sent. */
    @Synchronized
    fun apply(arguments: List<String>): String? {
        val state = when (arguments.firstOrNull()) {
            "release" -> if (arguments.size == 1) State() else return "unexpected_arguments"
            "btn" -> {
                if (arguments.size !in 2..3) return "usage_btn"
                var buttons = 0
                for (name in arguments[1].split('_')) {
                    buttons = buttons or when (name) {
                        "menu" -> MENU_BIT
                        else -> 1 shl (BUTTONS[name] ?: return "unknown_button")
                    }
                }
                State(buttons = buttons, durationMs = duration(arguments.getOrNull(2), DEFAULT_BUTTON_MS) ?: return "invalid_ms")
            }
            "axis" -> {
                if (arguments.size !in 3..4) return "usage_axis"
                val index = AXES.indexOf(arguments[1]).takeIf { it >= 0 } ?: return "unknown_axis"
                val value = arguments[2].toFloatOrNull()?.takeIf { it in -1f..1f } ?: return "invalid_value"
                State(
                    axisMask = 1 shl index,
                    axes = List(AXES.size) { if (it == index) value else 0f },
                    durationMs = duration(arguments.getOrNull(3), DEFAULT_AXIS_MS) ?: return "invalid_ms",
                )
            }
            else -> return "usage: input btn|axis|release|status"
        }
        sink(state)
        last = state
        expiresAtMs = clock() + state.durationMs
        return null
    }

    private fun duration(text: String?, default: Int): Int? =
        if (text == null) default else text.toIntOrNull()?.takeIf { it in 1..MAX_DURATION_MS }
}
