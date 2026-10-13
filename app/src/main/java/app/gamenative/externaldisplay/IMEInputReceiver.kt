package app.gamenative.externaldisplay

import android.content.Context
import android.os.Build
import android.view.KeyEvent
import android.view.WindowInsets
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import android.widget.FrameLayout
import com.winlator.xserver.XServer
import com.winlator.xserver.XKeycode
import timber.log.Timber

/**
 * Invisible view that receives IME (soft keyboard) input from Android system keyboard
 * and forwards it to XServer as keyboard events for the game.
 * 
 * Both handheld and external displays need a focused editor with an InputConnection.
 */
class IMEInputReceiver(
    context: Context,
    private val xServer: XServer,
    private val displayContext: Context = context,
) : FrameLayout(context) {

    companion object {
        private val SHIFTED_SYMBOLS = setOf(
            ')', '!', '@', '#', '$', '%', '^', '&', '*', '(',
            '_', '+', '{', '}', '|', ':', '"', '<', '>', '?', '~',
        )

        /** Presses and releases one key in the guest. */
        fun pressKey(xServer: XServer, keyCode: XKeycode) {
            xServer.injectKeyPress(keyCode, 0)
            xServer.injectKeyRelease(keyCode)
        }

        /** Types [char] as the US key that produces it; false when none does. */
        fun typeCharacter(xServer: XServer, char: Char): Boolean {
            val keyDispatch = charToKeyDispatch(char) ?: return false
            val metaState = if (keyDispatch.requiresShift) KeyEvent.META_SHIFT_ON else 0
            xServer.keyboard.onVirtualKeyEvent(KeyEvent(0L, 0L, KeyEvent.ACTION_DOWN, keyDispatch.keyCode, 0, metaState))
            xServer.keyboard.onVirtualKeyEvent(KeyEvent(0L, 0L, KeyEvent.ACTION_UP, keyDispatch.keyCode, 0, metaState))
            return true
        }

        private fun charToKeyDispatch(char: Char): KeyDispatch? {
            val isUppercaseLetter = char in 'A'..'Z'
            val normalizedChar = if (isUppercaseLetter) char.lowercaseChar() else char

            val keyCode = when (normalizedChar) {
                'a' -> KeyEvent.KEYCODE_A
                'b' -> KeyEvent.KEYCODE_B
                'c' -> KeyEvent.KEYCODE_C
                'd' -> KeyEvent.KEYCODE_D
                'e' -> KeyEvent.KEYCODE_E
                'f' -> KeyEvent.KEYCODE_F
                'g' -> KeyEvent.KEYCODE_G
                'h' -> KeyEvent.KEYCODE_H
                'i' -> KeyEvent.KEYCODE_I
                'j' -> KeyEvent.KEYCODE_J
                'k' -> KeyEvent.KEYCODE_K
                'l' -> KeyEvent.KEYCODE_L
                'm' -> KeyEvent.KEYCODE_M
                'n' -> KeyEvent.KEYCODE_N
                'o' -> KeyEvent.KEYCODE_O
                'p' -> KeyEvent.KEYCODE_P
                'q' -> KeyEvent.KEYCODE_Q
                'r' -> KeyEvent.KEYCODE_R
                's' -> KeyEvent.KEYCODE_S
                't' -> KeyEvent.KEYCODE_T
                'u' -> KeyEvent.KEYCODE_U
                'v' -> KeyEvent.KEYCODE_V
                'w' -> KeyEvent.KEYCODE_W
                'x' -> KeyEvent.KEYCODE_X
                'y' -> KeyEvent.KEYCODE_Y
                'z' -> KeyEvent.KEYCODE_Z
                '0', ')' -> KeyEvent.KEYCODE_0
                '1', '!' -> KeyEvent.KEYCODE_1
                '2', '@' -> KeyEvent.KEYCODE_2
                '3', '#' -> KeyEvent.KEYCODE_3
                '4', '$' -> KeyEvent.KEYCODE_4
                '5', '%' -> KeyEvent.KEYCODE_5
                '6', '^' -> KeyEvent.KEYCODE_6
                '7', '&' -> KeyEvent.KEYCODE_7
                '8', '*' -> KeyEvent.KEYCODE_8
                '9', '(' -> KeyEvent.KEYCODE_9
                ' ' -> KeyEvent.KEYCODE_SPACE
                '\n' -> KeyEvent.KEYCODE_ENTER
                '-', '_' -> KeyEvent.KEYCODE_MINUS
                '=', '+' -> KeyEvent.KEYCODE_EQUALS
                '[', '{' -> KeyEvent.KEYCODE_LEFT_BRACKET
                ']', '}' -> KeyEvent.KEYCODE_RIGHT_BRACKET
                '\\', '|' -> KeyEvent.KEYCODE_BACKSLASH
                ';', ':' -> KeyEvent.KEYCODE_SEMICOLON
                '\'', '"' -> KeyEvent.KEYCODE_APOSTROPHE
                ',', '<' -> KeyEvent.KEYCODE_COMMA
                '.', '>' -> KeyEvent.KEYCODE_PERIOD
                '/', '?' -> KeyEvent.KEYCODE_SLASH
                '`', '~' -> KeyEvent.KEYCODE_GRAVE
                else -> null
            } ?: return null

            val requiresShift = isUppercaseLetter || char in SHIFTED_SYMBOLS

            return KeyDispatch(keyCode = keyCode, requiresShift = requiresShift)
        }
    }

    var isKeyboardRequested = false
        private set
    private var connectionGeneration = 0
    private var imeWasVisible = false
    private val inputMethodManager
        get() = displayContext.getSystemService(Context.INPUT_METHOD_SERVICE) as? InputMethodManager

    private val showIme = Runnable {
        if (isKeyboardRequested && isAttachedToWindow && hasWindowFocus() && requestFocus()) {
            // Explicit user request: SHOW_IMPLICIT can be suppressed by a hardware keyboard.
            inputMethodManager?.restartInput(this)
            inputMethodManager?.showSoftInput(this, 0)
        }
    }

    init {
        isFocusable = false
        isFocusableInTouchMode = false
    }

    override fun onCheckIsTextEditor(): Boolean = isKeyboardRequested

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? {
        if (!isKeyboardRequested || !hasFocus()) return null
        val generation = ++connectionGeneration
        outAttrs.inputType = EditorInfo.TYPE_CLASS_TEXT or
            EditorInfo.TYPE_TEXT_FLAG_NO_SUGGESTIONS or EditorInfo.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN or
            EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_ACTION_NONE

        // Keep composition local until committed. BaseInputConnection's dummy-editor
        // mode sends composing text as Activity key events, bypassing this bridge.
        return object : BaseInputConnection(this, true) {
            private fun isCurrent() = isKeyboardRequested && generation == connectionGeneration

            override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                if (!isCurrent()) return false
                editable?.clear()
                text?.forEach(::sendCharacterToGame)
                return true
            }

            override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean =
                isCurrent() && super.setComposingText(text, newCursorPosition)

            override fun finishComposingText(): Boolean {
                if (!isCurrent()) return false
                val text = editable?.toString().orEmpty()
                editable?.clear()
                text.forEach(::sendCharacterToGame)
                return super.finishComposingText()
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                if (!isCurrent() || beforeLength < 0 || afterLength < 0) return false
                if (!editable.isNullOrEmpty()) return super.deleteSurroundingText(beforeLength, afterLength)
                repeat(beforeLength) { sendKey(XKeycode.KEY_BKSP) }
                repeat(afterLength) { sendKey(XKeycode.KEY_DEL) }
                return true
            }

            override fun deleteSurroundingTextInCodePoints(beforeLength: Int, afterLength: Int): Boolean {
                if (!isCurrent()) return false
                return if (!editable.isNullOrEmpty()) {
                    super.deleteSurroundingTextInCodePoints(beforeLength, afterLength)
                } else {
                    deleteSurroundingText(beforeLength, afterLength)
                }
            }

            override fun sendKeyEvent(event: KeyEvent): Boolean =
                isCurrent() && xServer.keyboard.onVirtualKeyEvent(event)

            override fun performEditorAction(actionCode: Int): Boolean {
                if (!isCurrent()) return false
                finishComposingText()
                sendKey(XKeycode.KEY_ENTER)
                return true
            }
        }
    }

    override fun onWindowFocusChanged(hasWindowFocus: Boolean) {
        super.onWindowFocusChanged(hasWindowFocus)
        if (hasWindowFocus && isKeyboardRequested) scheduleShow()
    }

    override fun onApplyWindowInsets(insets: WindowInsets): WindowInsets {
        if (Build.VERSION.SDK_INT >= 30) {
            val visible = insets.isVisible(WindowInsets.Type.ime())
            if (imeWasVisible && !visible) hideKeyboard()
            imeWasVisible = visible
        }
        return super.onApplyWindowInsets(insets)
    }

    override fun onKeyPreIme(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK && isKeyboardRequested) {
            if (event.action == KeyEvent.ACTION_UP) hideKeyboard()
            return true
        }
        return super.onKeyPreIme(keyCode, event)
    }

    override fun onDetachedFromWindow() {
        hideKeyboard()
        super.onDetachedFromWindow()
    }

    private fun sendKey(keyCode: XKeycode) = pressKey(xServer, keyCode)

    private data class KeyDispatch(
        val keyCode: Int,
        val requiresShift: Boolean,
    )

    private fun sendCharacterToGame(char: Char) {
        if (!typeCharacter(xServer, char)) {
            // Do not put user-entered text in logcat.
            Timber.w("IMEInputReceiver: Character is outside the guest keyboard mapping")
        }
    }

    private fun scheduleShow() {
        removeCallbacks(showIme)
        post(showIme)
    }

    fun showKeyboard() {
        isKeyboardRequested = true
        isFocusableInTouchMode = true
        isFocusable = true
        requestFocus()
        scheduleShow()
    }

    fun hideKeyboard() {
        isKeyboardRequested = false
        connectionGeneration++
        imeWasVisible = false
        removeCallbacks(showIme)
        inputMethodManager?.hideSoftInputFromWindow(windowToken, 0)
        clearFocus()
        isFocusable = false
        isFocusableInTouchMode = false
    }
}
