package com.winlator.xserver

import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.mockito.kotlin.mock
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33], manifest = Config.NONE)
class KeyboardUnicodeKeycodeTest {
    private val keyboard = Keyboard(mock<XServer>())

    // What InputDeviceManager.onKeyPress does before the press: map the keycode to the keysym.
    private fun type(char: Char): Byte {
        val keysym = 0x01000000 or char.code
        val keycode = keyboard.unicodeKeycode(keysym)
        keyboard.setKeysyms(keycode, keysym, keysym)
        return keycode
    }

    @Test
    fun `successive characters take different keycodes`() {
        // Wine looks a key up when the window translates it, so 你 must keep its keycode while 好 is typed.
        val ni = type('你')
        val hao = type('好')
        assertEquals(XKeycode.KEY_UNICODE.id, ni)
        assertEquals((XKeycode.KEY_UNICODE.id + 1).toByte(), hao)
        assertEquals(0x01000000 or '你'.code, keyboard.keysyms[(ni - 8) * Keyboard.KEYSYMS_PER_KEYCODE])
    }

    @Test
    fun `a character already mapped keeps its keycode`() {
        val hao = type('好')
        type('你')
        assertEquals(hao, type('好'))
    }

    @Test
    fun `the least recently used keycode takes a new character`() {
        val text = "一二三四五六七八九十百千"
        val keycodes = text.map(::type)
        assertEquals((XKeycode.KEY_UNICODE.id..Keyboard.LAST_UNICODE_KEYCODE).map { it.toByte() }, keycodes)
        // 一 was typed again, so 二 is now the oldest.
        assertEquals(keycodes[0], type('一'))
        assertEquals(keycodes[1], type('万'))
    }
}
