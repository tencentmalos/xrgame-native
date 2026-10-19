// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import android.app.Application
import android.view.KeyEvent
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], manifest = Config.NONE, application = Application::class)
class SteamImeKeyTest {
    @Test
    fun `IME keys reach Steam once on their down event`() {
        for (key in listOf(KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_DEL, KeyEvent.KEYCODE_DPAD_LEFT)) {
            assertTrue(SteamImeSession.forwardsKey(KeyEvent(KeyEvent.ACTION_DOWN, key)))
            // Pico's IME may never send the release; the press already carries one.
            assertFalse(SteamImeSession.forwardsKey(KeyEvent(KeyEvent.ACTION_UP, key)))
        }
    }
}
