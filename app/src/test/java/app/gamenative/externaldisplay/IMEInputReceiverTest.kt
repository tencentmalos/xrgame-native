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
