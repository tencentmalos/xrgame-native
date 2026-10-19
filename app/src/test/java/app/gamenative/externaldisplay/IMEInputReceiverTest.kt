package app.gamenative.externaldisplay

import android.app.Activity
import android.app.Application
import android.os.Looper
import android.view.KeyEvent
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import com.winlator.xserver.Keyboard
import com.winlator.xserver.XKeycode
import com.winlator.xserver.XServer
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.mockito.kotlin.*
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.util.ReflectionHelpers

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], manifest = Config.NONE, application = Application::class)
class IMEInputReceiverTest {
    private lateinit var server: XServer
    private lateinit var receiver: IMEInputReceiver

    @Before
    fun setUp() {
        val activity = Robolectric.buildActivity(Activity::class.java).setup().get()
        server = mock()
        ReflectionHelpers.setField(server, "keyboard", Keyboard(server))
        receiver = IMEInputReceiver(activity, server)
        activity.setContentView(receiver)
    }

    private fun connect(): InputConnection {
        receiver.showKeyboard()
        shadowOf(Looper.getMainLooper()).idle()
        assertTrue(receiver.hasFocus())
        assertTrue(receiver.onCheckIsTextEditor())
        return receiver.onCreateInputConnection(EditorInfo())!!
    }

    @Test
    fun `unicode transport receives one commit and no composing fragments`() {
        val activity = Robolectric.buildActivity(Activity::class.java).setup().get()
        val commits = mutableListOf<String>()
        receiver = IMEInputReceiver(activity, server, textHandler = { commits.add(it) })
        activity.setContentView(receiver)
        connect()
        val attributes = EditorInfo()
        val connection = receiver.onCreateInputConnection(attributes)!!
        assertEquals(EditorInfo.TYPE_TEXT_VARIATION_NORMAL, attributes.inputType and EditorInfo.TYPE_MASK_VARIATION)
        assertNotEquals(0, attributes.imeOptions and EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING)
        connection.setComposingText("zhong", 1)
        assertTrue(commits.isEmpty())
        connection.commitText("中文😀", 1)
        connection.finishComposingText()
        assertEquals(listOf("中文😀"), commits)
        verifyNoInteractions(server)
        receiver.hideKeyboard()
        assertFalse(connection.commitText("stale", 1))
        assertEquals(1, commits.size)
    }

    @Test
    fun `replacement backspaces over the changed tail in code points`() {
        assertEquals(0 to "h", IMEInputReceiver.replacement("", "h"))
        assertEquals(0 to "", IMEInputReceiver.replacement("abc", "abc"))
        assertEquals(1 to "", IMEInputReceiver.replacement("abc", "ab"))
        assertEquals(2 to "he ", IMEInputReceiver.replacement("teh", "the "))
        assertEquals(5 to "你好", IMEInputReceiver.replacement("nihao", "你好"))
        assertEquals(3 to "'hao", IMEInputReceiver.replacement("nihao", "ni'hao"))
        assertEquals(2 to "", IMEInputReceiver.replacement("😀😀", ""))
        // Same high surrogate, different emoji: replace the whole code point.
        assertEquals(1 to "😁", IMEInputReceiver.replacement("a😀", "a😁"))
    }

    /** A Steam-style receiver that streams composition; returns the ops it sends, in order. */
    private fun streaming(): MutableList<String> {
        val activity = Robolectric.buildActivity(Activity::class.java).setup().get()
        val ops = mutableListOf<String>()
        receiver = IMEInputReceiver(activity, server, textHandler = { ops.add(it) },
            keyHandler = { ops.add(it.name) }, streamComposition = true)
        activity.setContentView(receiver)
        return ops
    }

    @Test
    fun `streamed composition shows each letter and autocorrect replaces only the difference`() {
        val ops = streaming()
        val connection = connect()
        connection.setComposingText("t", 1)
        connection.setComposingText("te", 1)
        connection.setComposingText("teh", 1)
        assertEquals(listOf("t", "e", "h"), ops)
        connection.commitText("the ", 1)
        connection.finishComposingText()
        assertEquals(listOf("t", "e", "h", "KEY_BKSP", "KEY_BKSP", "he "), ops)
        verifyNoInteractions(server)
    }

    @Test
    fun `streamed pinyin is replaced by the committed characters`() {
        val ops = streaming()
        val connection = connect()
        connection.setComposingText("ni", 1)
        connection.setComposingText("nihao", 1)
        connection.commitText("你", 1)
        connection.setComposingText("hao", 1)
        connection.commitText("好", 1)
        assertEquals(listOf("ni", "hao") + List(5) { "KEY_BKSP" } + listOf("你", "hao") + List(3) { "KEY_BKSP" } + listOf("好"), ops)
    }

    @Test
    fun `streamed backspace removes composed letters and then committed text`() {
        val ops = streaming()
        val connection = connect()
        connection.setComposingText("ab", 1)
        connection.setComposingText("a", 1)
        connection.setComposingText("", 1)
        connection.commitText("", 1)
        connection.deleteSurroundingText(1, 0)
        connection.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DEL))
        assertEquals(listOf("ab", "KEY_BKSP", "KEY_BKSP", "KEY_BKSP"), ops)
        // The Delete key event itself goes to the guest keyboard.
        verify(server).injectKeyPress(XKeycode.KEY_BKSP, 0)
    }

    @Test
    fun `streamed composition is not repeated by finish enter or a later connection`() {
        val ops = streaming()
        var connection = connect()
        connection.setComposingText("ok", 1)
        connection.performEditorAction(EditorInfo.IME_ACTION_DONE)
        assertEquals(listOf("ok", "KEY_ENTER"), ops)
        // Hiding with a pending composition leaves what the guest shows.
        connection.setComposingText("hi", 1)
        receiver.hideKeyboard()
        assertFalse(connection.finishComposingText())
        assertFalse(connection.commitText("hi", 1))
        connection = connect()
        connection.commitText("x", 1)
        assertEquals(listOf("ok", "KEY_ENTER", "hi", "x"), ops)
    }

    @Test
    fun `client keyboard dismiss key is consumed before the IME`() {
        val activity = Robolectric.buildActivity(Activity::class.java).setup().get()
        receiver = IMEInputReceiver(activity, server, dismissKeys = setOf(KeyEvent.KEYCODE_BUTTON_SELECT))
        activity.setContentView(receiver)
        val connection = connect()
        assertTrue(receiver.onKeyPreIme(KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_SELECT)))
        assertTrue(receiver.isKeyboardRequested)
        assertTrue(receiver.onKeyPreIme(KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_SELECT)))
        assertFalse(receiver.isKeyboardRequested)
        assertFalse(connection.commitText("stale", 1))
        verifyNoInteractions(server)
    }

    @Test
    fun `release of the key that opened the keyboard does not dismiss it`() {
        val activity = Robolectric.buildActivity(Activity::class.java).setup().get()
        receiver = IMEInputReceiver(activity, server, dismissKeys = setOf(KeyEvent.KEYCODE_BUTTON_SELECT))
        activity.setContentView(receiver)
        assertFalse(receiver.onKeyPreIme(KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_SELECT)))
        // Activity.dispatchKeyEvent handles this DOWN by opening the editor.
        connect()
        receiver.onKeyPreIme(KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_SELECT))
        assertTrue(receiver.isKeyboardRequested)
        verifyNoInteractions(server)
    }

    @Test
    fun `inactive receiver is not an editor and has no connection`() {
        assertFalse(receiver.onCheckIsTextEditor())
        assertNull(receiver.onCreateInputConnection(EditorInfo()))
    }

    @Test
    fun `committed uppercase and symbols preserve shift and release keys`() {
        connect().commitText("A!a", 1)
        verify(server, times(2)).injectKeyPress(XKeycode.KEY_SHIFT_L)
        verify(server).injectKeyPress(XKeycode.KEY_A, 'A'.code)
        verify(server).injectKeyPress(XKeycode.KEY_1, '!'.code)
        verify(server).injectKeyPress(XKeycode.KEY_A, 'a'.code)
        verify(server, times(2)).injectKeyRelease(XKeycode.KEY_A)
        verify(server).injectKeyRelease(XKeycode.KEY_1)
        verify(server, times(3)).injectKeyRelease(XKeycode.KEY_SHIFT_L)
        verifyNoMoreInteractions(server)
    }

    @Test
    fun `composition updates are not sent before commit or duplicated on finish`() {
        val connection = connect()
        connection.setComposingText("h", 1)
        connection.setComposingText("he", 1)
        verifyNoInteractions(server)
        connection.commitText("Hi", 1)
        connection.finishComposingText()
        verify(server).injectKeyPress(XKeycode.KEY_H, 'H'.code)
        verify(server).injectKeyPress(XKeycode.KEY_I, 'i'.code)
        verify(server, never()).injectKeyPress(XKeycode.KEY_E, 'e'.code)
    }

    @Test
    fun `finish commits pending composition only once`() {
        val connection = connect()
        connection.setComposingText("a", 1)
        connection.finishComposingText()
        connection.finishComposingText()
        verify(server, times(1)).injectKeyPress(XKeycode.KEY_A, 'a'.code)
    }

    @Test
    fun `delete and editor action reach guest without activity dispatch`() {
        val connection = connect()
        connection.deleteSurroundingText(2, 1)
        connection.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DEL))
        connection.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_DEL))
        connection.performEditorAction(EditorInfo.IME_ACTION_DONE)
        verify(server, times(3)).injectKeyPress(XKeycode.KEY_BKSP, 0)
        verify(server, times(3)).injectKeyRelease(XKeycode.KEY_BKSP)
        verify(server).injectKeyPress(XKeycode.KEY_DEL, 0)
        verify(server).injectKeyRelease(XKeycode.KEY_DEL)
        verify(server).injectKeyPress(XKeycode.KEY_ENTER, 0)
        verify(server).injectKeyRelease(XKeycode.KEY_ENTER)
    }

    @Test
    fun `old connections cannot type after restart hide or reopen`() {
        val old = connect()
        val current = receiver.onCreateInputConnection(EditorInfo())!!
        assertFalse(old.commitText("a", 1))
        receiver.hideKeyboard()
        assertFalse(current.commitText("a", 1))
        assertFalse(receiver.onCheckIsTextEditor())
        connect()
        assertFalse(current.performEditorAction(EditorInfo.IME_ACTION_DONE))
        verifyNoInteractions(server)
    }

    @Test
    fun `hide cancels a queued show request`() {
        receiver.showKeyboard()
        receiver.hideKeyboard()
        shadowOf(Looper.getMainLooper()).idle()
        assertFalse(receiver.isKeyboardRequested)
        assertFalse(receiver.onCheckIsTextEditor())
        assertNull(receiver.onCreateInputConnection(EditorInfo()))
    }
}
