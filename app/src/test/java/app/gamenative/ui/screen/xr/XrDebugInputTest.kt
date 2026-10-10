package app.gamenative.ui.screen.xr

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Before
import org.junit.Test

class XrDebugInputTest {
    private val sent = mutableListOf<XrDebugInput.State>()
    private var now = 1_000L

    @Before fun setUp() {
        XrDebugInput.sink = { sent += it }
        XrDebugInput.clock = { now }
        XrDebugInput.apply(listOf("release"))
        sent.clear()
    }

    @After fun tearDown() {
        XrDebugInput.apply(listOf("release"))
    }

    @Test fun buttonsCombineAndHoldForTheirDuration() {
        assertNull(XrDebugInput.apply(listOf("btn", "a_rb_menu", "700")))
        val state = sent.single()
        assertEquals(
            (1 shl XrGamepadBridge.BUTTON_A) or (1 shl XrGamepadBridge.BUTTON_RB) or XrDebugInput.MENU_BIT,
            state.buttons,
        )
        assertEquals(700, state.durationMs)
        assertEquals(listOf("a", "rb", "menu"), XrDebugInput.buttonNames(state.buttons))
        now += 200
        assertEquals(500L, XrDebugInput.remainingMs())
        assertNull(XrDebugInput.apply(listOf("btn", "start")))
        assertEquals(XrDebugInput.DEFAULT_BUTTON_MS, sent.last().durationMs)
    }

    @Test fun anAxisReplacesOnlyItself() {
        assertNull(XrDebugInput.apply(listOf("axis", "ly", "-1")))
        val state = sent.single()
        assertEquals(1 shl XrDebugInput.AXES.indexOf("ly"), state.axisMask)
        assertEquals(-1f, state.axes[1])
        assertEquals(XrDebugInput.DEFAULT_AXIS_MS, state.durationMs)
    }

    @Test fun invalidRequestsSendNothing() {
        assertEquals("unknown_button", XrDebugInput.apply(listOf("btn", "a_z")))
        assertEquals("invalid_ms", XrDebugInput.apply(listOf("btn", "a", "0")))
        assertEquals("invalid_ms", XrDebugInput.apply(listOf("btn", "a", "20000")))
        assertEquals("unknown_axis", XrDebugInput.apply(listOf("axis", "lz", "1")))
        assertEquals("invalid_value", XrDebugInput.apply(listOf("axis", "lx", "2")))
        assertEquals("usage_axis", XrDebugInput.apply(listOf("axis", "lx")))
        assertEquals("unexpected_arguments", XrDebugInput.apply(listOf("release", "now")))
        assertEquals(emptyList<XrDebugInput.State>(), sent)
    }

    @Test fun releaseClearsTheOverride() {
        assertNull(XrDebugInput.apply(listOf("btn", "b", "5000")))
        assertNull(XrDebugInput.apply(listOf("release")))
        assertEquals(XrDebugInput.State(), sent.last())
        assertEquals(0L, XrDebugInput.remainingMs())
    }
}
