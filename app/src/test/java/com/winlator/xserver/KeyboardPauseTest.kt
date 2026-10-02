package com.winlator.xserver

import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.mockito.kotlin.mock
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33], manifest = Config.NONE)
class KeyboardPauseTest {
    @Test
    fun `pause releases held modifier and ordinary keys exactly once`() {
        val keyboard = Keyboard(mock<XServer>())
        val released = mutableListOf<Byte>()
        keyboard.addOnKeyboardListener(object : Keyboard.OnKeyboardListener {
            override fun onKeyPress(keycode: Byte, keysym: Int) = Unit
            override fun onKeyRelease(keycode: Byte) { released.add(keycode) }
        })
        val shift = XKeycode.KEY_SHIFT_L.id
        val key = XKeycode.KEY_UP.id
        keyboard.setKeyPress(shift, 0)
        keyboard.setKeyPress(key, 0)
        assertFalse(keyboard.modifiersMask.isEmpty)
        keyboard.releaseAllKeys()
        assertTrue(keyboard.pressedKeys.isEmpty())
        assertTrue(keyboard.modifiersMask.isEmpty)
        assertEquals(setOf(shift, key), released.toSet())
        // Key-up may arrive after the overlay has already released the keys.
        keyboard.setKeyRelease(shift)
        keyboard.setKeyRelease(key)
        keyboard.releaseAllKeys()
        assertEquals(2, released.size)
    }
}
