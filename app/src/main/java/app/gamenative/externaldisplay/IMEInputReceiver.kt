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
    private val textHandler: ((String) -> Unit)? = null,
    private val keyHandler: ((XKeycode) -> Unit)? = null,
    private val keyEventHandler: ((KeyEvent) -> Boolean)? = null,
    private val dismissKeys: Set<Int> = setOf(KeyEvent.KEYCODE_BACK),
    /**
     * Mirror composing text into the guest while it changes, not only at commit. IMEs such as
     * Pico's compose every letter and commit only at the end of a word, so a guest text field
     * (the official Steam client's) would otherwise stay empty while the user types.
     */
    private val streamComposition: Boolean = false,
) : FrameLayout(context) {

    companion object {
        /**
         * Turns guest text [shown] into [text]: how many Backspaces to press over what differs
         * after their common prefix, and what to type after them. Counts code points, which is
         * what one Backspace removes for letters, CJK and single emoji.
         */
        fun replacement(shown: String, text: String): Pair<Int, String> {
            var common = 0
            while (common < shown.length && common < text.length && shown[common] == text[common]) common++
            // Never split a surrogate pair.
            if (common > 0 && Character.isHighSurrogate(text[common - 1])) common--
            return shown.codePointCount(common, shown.length) to text.substring(common)
        }

        private val SHIFTED_SYMBOLS = setOf(
            ')', '!', '@', '#', '$', '%', '^', '&', '*', '(',
            '_', '+', '{', '}', '|', ':', '"', '<', '>', '?', '~',
        )

        /** Presses and releases one key in the guest. */
        fun pressKey(xServer: XServer, keyCode: XKeycode) {
            xServer.injectKeyPress(keyCode, 0)
            xServer.injectKeyRelease(keyCode)
        }

        /**
         * Types [char] as the US key that produces it, else as its Unicode keysym ([typeUnicode]);
         * false for what neither carries (control characters, surrogate pairs).
         */
        fun typeCharacter(xServer: XServer, char: Char): Boolean {
            val keyDispatch = charToKeyDispatch(char) ?: return typeUnicode(xServer, char)
            val metaState = if (keyDispatch.requiresShift) KeyEvent.META_SHIFT_ON else 0
            xServer.keyboard.onVirtualKeyEvent(KeyEvent(0L, 0L, KeyEvent.ACTION_DOWN, keyDispatch.keyCode, 0, metaState))
            xServer.keyboard.onVirtualKeyEvent(KeyEvent(0L, 0L, KeyEvent.ACTION_UP, keyDispatch.keyCode, 0, metaState))
            return true
        }

        /**
         * Types a character no US key has (Chinese, say) on a spare keycode remapped to its keysym:
         * the X server sends MappingNotify first, Wine rereads the map and gives the key a spare
         * virtual key, and winex11's ToUnicodeEx turns a Unicode keysym (0x01000100-0x0100FFFF)
         * into the character, so the window gets WM_KEYDOWN and WM_CHAR. Basic plane only.
         * Successive characters take different keycodes (Keyboard.unicodeKeycode).
         */
        fun typeUnicode(xServer: XServer, char: Char): Boolean {
            if (char.code < 0xA0 || char.isSurrogate()) return false
            // Latin-1 keysyms are the code points themselves; the rest are 0x01000000 + code point.
            val keysym = if (char.code <= 0xFF) char.code else 0x01000000 or char.code
            xServer.injectUnicodeKey(keysym)
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
    private var dismissKeyDown: Int? = null
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
        // The game's key-only transport requests a Latin keyboard. A text
        // transport can accept CJK composition, so must not advertise a password
        // editor (many IMEs force Latin keys for that variation).
        outAttrs.inputType = EditorInfo.TYPE_CLASS_TEXT or EditorInfo.TYPE_TEXT_FLAG_NO_SUGGESTIONS or
            if (textHandler == null) EditorInfo.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD else EditorInfo.TYPE_TEXT_VARIATION_NORMAL
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN or
            EditorInfo.IME_FLAG_NO_EXTRACT_UI or EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING or EditorInfo.IME_ACTION_NONE

        // Keep composition local until committed. BaseInputConnection's dummy-editor
        // mode sends composing text as Activity key events, bypassing this bridge.
        Timber.i("IME connection %d: inputType=0x%x", generation, outAttrs.inputType)
        return object : BaseInputConnection(this, true) {
            /** With [streamComposition]: the composing text the guest already shows. */
            private var streamed = ""

            private fun isCurrent() = (isKeyboardRequested && generation == connectionGeneration).also {
                if (!it) Timber.i("IME connection %d is stale (current %d, requested %s)", generation, connectionGeneration, isKeyboardRequested)
            }

            private fun stream(text: String) {
                val (backspaces, typed) = replacement(streamed, text)
                if (backspaces > 0) Timber.i("IME stream backspaces=%d", backspaces)
                repeat(backspaces) { sendKey(XKeycode.KEY_BKSP) }
                sendTextToGame(typed)
                streamed = text
            }

            override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
                // Lengths only: never log what the user types.
                Timber.i("IME commitText length=%d", text?.length ?: -1)
                if (!isCurrent()) return false
                editable?.clear()
                if (streamComposition) {
                    // The commit replaces the composition the guest shows.
                    stream(text?.toString().orEmpty())
                    streamed = ""
                } else if (text != null) sendTextToGame(text.toString())
                return true
            }

            override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
                Timber.i("IME setComposingText length=%d", text?.length ?: -1)
                if (!isCurrent() || !super.setComposingText(text, newCursorPosition)) return false
                if (streamComposition) stream(text?.toString().orEmpty())
                return true
            }

            override fun finishComposingText(): Boolean {
                if (!isCurrent()) return false
                val text = editable?.toString().orEmpty()
                Timber.i("IME finishComposingText length=%d", text.length)
                editable?.clear()
                // A streamed composition is already in the guest and stays there.
                if (streamComposition) streamed = "" else sendTextToGame(text)
                return super.finishComposingText()
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                Timber.i("IME deleteSurroundingText before=%d after=%d", beforeLength, afterLength)
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

            override fun sendKeyEvent(event: KeyEvent): Boolean {
                Timber.i("IME sendKeyEvent action=%d keyCode=%d characters=%d", event.action, event.keyCode, event.characters?.length ?: 0)
                return isCurrent() && (keyEventHandler?.invoke(event) ?: xServer.keyboard.onVirtualKeyEvent(event))
            }

            override fun performEditorAction(actionCode: Int): Boolean {
                Timber.i("IME performEditorAction %d", actionCode)
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
            if (visible != imeWasVisible) Timber.i("IME insets visible=%s requested=%s", visible, isKeyboardRequested)
            if (imeWasVisible && !visible) hideKeyboard()
            imeWasVisible = visible
        }
        return super.onApplyWindowInsets(insets)
    }

    override fun onKeyPreIme(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode in dismissKeys && isKeyboardRequested) {
            if (event.action == KeyEvent.ACTION_DOWN) {
                dismissKeyDown = keyCode
                return true
            }
            // The Activity can open the keyboard on DOWN. Its matching UP
            // must not immediately dismiss the newly focused editor.
            if (event.action == KeyEvent.ACTION_UP && dismissKeyDown == keyCode) {
                dismissKeyDown = null
                if (!event.isCanceled) hideKeyboard()
                return true
            }
        }
        return super.onKeyPreIme(keyCode, event)
    }

    override fun onDetachedFromWindow() {
        hideKeyboard()
        super.onDetachedFromWindow()
    }

    private fun sendKey(keyCode: XKeycode) {
        keyHandler?.invoke(keyCode) ?: pressKey(xServer, keyCode)
    }

    private fun sendTextToGame(text: String) {
        if (text.isEmpty()) return
        textHandler?.invoke(text) ?: text.forEach(::sendCharacterToGame)
    }

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
        Timber.i("IME show requested")
        isKeyboardRequested = true
        isFocusableInTouchMode = true
        isFocusable = true
        requestFocus()
        scheduleShow()
    }

    fun hideKeyboard() {
        if (isKeyboardRequested) Timber.i("IME hide")
        isKeyboardRequested = false
        dismissKeyDown = null
        connectionGeneration++
        imeWasVisible = false
        removeCallbacks(showIme)
        inputMethodManager?.hideSoftInputFromWindow(windowToken, 0)
        clearFocus()
        isFocusable = false
        isFocusableInTouchMode = false
    }
}
