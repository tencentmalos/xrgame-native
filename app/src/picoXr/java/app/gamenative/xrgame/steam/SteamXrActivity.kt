// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import app.gamenative.ui.screen.xr.ImmersiveXrActivity
import app.gamenative.ui.screen.xr.XrHostedClient

/**
 * Official Big Picture on the XR big screen: the immersive session hosts the client's X server
 * instead of a Wine game ([SteamXrClient]). A game started from it runs in its own session and
 * layer; this page gives the headset up and stops drawing while the game is in front, and takes
 * both back afterwards. The screen's size and curve are shared with those games, its resolution
 * is Steam's own.
 */
class SteamXrActivity : ImmersiveXrActivity() {
    companion object {
        @Volatile var current: java.lang.ref.WeakReference<SteamXrActivity>? = null
    }

    override fun createHostedClient(): XrHostedClient = SteamXrClient(this)

    // The home page stays in front of the big screen unless its task goes to the back.
    override fun launcherActivity(): android.app.Activity? = app.gamenative.xrgame.LaunchActivity.current?.get()

    override fun onResume() {
        current = java.lang.ref.WeakReference(this)
        super.onResume()
    }

    override fun onDestroy() {
        if (current?.get() === this) current = null
        super.onDestroy()
    }

    // The client's menu acts on the session through these.
    internal var passthrough: Boolean
        get() = hostedPassthrough
        set(value) = setHostedPassthrough(value)
    internal var resizeHandles: Boolean
        get() = hostedResizeHandles
        set(value) { hostedResizeHandles = value }
    internal fun resetScreen() = resetHostedScreen()
    internal fun screenChanged() = hostedScreenChanged()
    internal fun launchGame(appId: String) = launchHostedGame(appId)
    internal fun openKeyboard() = showXrKeyboard()
    internal fun toggleKeyboard() = if (xrKeyboardOpen) hideXrKeyboard() else showXrKeyboard()
}
