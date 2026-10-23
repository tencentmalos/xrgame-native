// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.ui.screen.xr

import android.util.Size
import android.view.KeyEvent
import android.view.View
import com.winlator.widget.XServerRendererView

/**
 * What an [ImmersiveXrActivity] subclass shows on the big screen in place of a Wine game, such as
 * the official Steam client: its page, the renderer view the big screen samples, and its input.
 * The activity keeps the session, the big screen, the controller rays (pen events into the page)
 * and the XR keyboard. Main thread unless noted.
 */
interface XrHostedClient {
    /** Name of the preferences that keep this client's big screen placement. */
    val settingsName: String

    /** The page: the client's renderer view and anything drawn over it (the big screen's overlay). */
    fun createContent(): View

    /** The renderer view of the client's X server, once it has one. Any thread. */
    val rendererView: XServerRendererView?

    /** The client screen's size in pixels, for the big screen's layer; null until known. Any thread. */
    val screenSize: Size?

    /** The ray's hover point, in [createContent]'s coordinates: moves the client's pointer. */
    fun hover(x: Float, y: Float)

    /** Wheel steps from the right stick; positive steps scroll down, or right. Polling thread. */
    fun scroll(steps: Int, horizontal: Boolean)

    /** A controller button as an Android key code, pressed or released. Polling thread. */
    fun key(keyCode: Int, down: Boolean)

    /**
     * Text from the XR keyboard, in order, with U+E000 for Backspace and U+E001 for Enter; it may
     * hold Chinese from the keyboard's pinyin. UI thread.
     */
    fun type(text: String)

    /** Start held: the client's own menu. */
    fun toggleMenu()

    /** Physical keys (gamepads, keyboards); true when the client used it. */
    fun dispatchKey(event: KeyEvent): Boolean

    /** Whether the page's views changed since the last call, so the overlay must be drawn again. Any thread. */
    fun consumeContentChanged(): Boolean

    fun onResume()
    fun onPause()
    fun release()
}
