// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import android.content.Intent
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Handler
import android.os.Looper
import android.util.Size
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.TextView
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import app.gamenative.R
import app.gamenative.ui.screen.xr.XrHostedClient
import app.gamenative.xrgame.XrGameOfficialSteam
import com.winlator.renderer.VulkanRenderer
import com.winlator.widget.XServerRendererView
import com.winlator.widget.XServerView
import com.winlator.widget.XServerViewGL
import com.winlator.xserver.Pointer
import com.winlator.xserver.WindowManager
import com.winlator.xserver.XServer
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch

// The keyboard icon's centre, as a fraction of the page height (as on the 2D page).
private const val KEYBOARD_Y = 0.3f

/**
 * The official client on the XR big screen. The page holds the client's renderer view (sampled by
 * the big screen), the keyboard icon, its startup status, a small menu and error notes, which the
 * session draws as the overlay. The controller rays arrive as pen events: a tap clicks and a drag
 * scrolls ([SteamTouchPointer]), and hover moves the X pointer. Input reaches the X server on the
 * main thread.
 */
class SteamXrClient(private val activity: SteamXrActivity) : XrHostedClient {
    private val main = Handler(Looper.getMainLooper())
    private val changed = AtomicBoolean(true)
    @Volatile private var server: XServer? = null
    @Volatile private var renderer: XServerRendererView? = null
    // XR keyboard text for the client, Chinese included; one per X server.
    private var ime: SteamImeSession? = null
    private lateinit var page: Page
    private lateinit var surface: FrameLayout
    private lateinit var loading: SteamLaunchView
    private lateinit var menu: LinearLayout
    private lateinit var note: TextView
    private val hideNote = Runnable { note.visibility = View.GONE }

    override val settingsName = app.gamenative.xrgame.XrGameOfficialSteam.XR_SETTINGS
    override val rendererView: XServerRendererView? get() = renderer
    override val screenSize: Size?
        get() = server?.screenInfo?.let { Size(it.width.toInt(), it.height.toInt()) }

    /**
     * The page root: a redraw below it means the overlay must be drawn again, except the renderer
     * view's own. Its frames are not part of the overlay, and the session flips its alpha around
     * every overlay drawing, which would otherwise mark the page changed on every pass.
     */
    private inner class Page : FrameLayout(activity) {
        override fun onDescendantInvalidated(child: View, target: View) {
            super.onDescendantInvalidated(child, target)
            if (target !== renderer) changed.set(true)
        }
    }

    override fun createContent(): View {
        // Transparent: the session draws this page as the overlay above the big screen's frames.
        page = Page()
        surface = FrameLayout(activity)
        page.addView(surface, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        loading = SteamLaunchView(activity).apply { setStatus(activity.getString(R.string.steam_client_preparing)) }
        page.addView(loading, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        addKeyboardButton()
        menu = buildMenu()
        page.addView(menu, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER))
        note = TextView(activity).apply {
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
            val padding = dp(16)
            setPadding(padding, padding, padding, padding)
            background = GradientDrawable().apply { setColor(0xE0202A35.toInt()); cornerRadius = dp(16).toFloat() }
            visibility = View.GONE
        }
        page.addView(note, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
            Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL).apply { bottomMargin = dp(32) })

        activity.lifecycleScope.launch {
            var started = false
            LinuxSteamService.status.combine(LinuxSteamService.phase) { text, phase -> text to phase }.collect { (text, phase) ->
                if (phase != LinuxSteamPhase.STOPPED) started = true
                if (!started && text == LinuxSteamService.IDLE_STATUS) return@collect
                loading.setStatus(if (phase == LinuxSteamPhase.STARTING) activity.getString(R.string.steam_client_starting) else text)
                loading.setPhase(phase)
            }
        }
        activity.lifecycleScope.launch { LinuxSteamService.displayReady.collect { showLoading(!it || server == null) } }
        activity.lifecycleScope.launch { LinuxSteamService.xServerState.collect { attach(it) } }
        activity.lifecycleScope.launch {
            activity.repeatOnLifecycle(Lifecycle.State.RESUMED) {
                launch { SteamGameHandoff.pending.collect { request ->
                    if (request != null && !request.dispatched && request.file.isFile &&
                        System.currentTimeMillis() - request.file.lastModified() < 10000) {
                        request.dispatched = true
                        // Chosen and prepared on the big screen itself; the game's own XR page follows.
                        activity.launchGame("STEAM_${request.appId}")
                    }
                } }
                launch { SteamGameHandoff.error.collect { message ->
                    if (message != null) {
                        SteamGameHandoff.error.value = null
                        showNote(message)
                    }
                } }
            }
        }
        activity.startForegroundService(Intent(activity, LinuxSteamService::class.java))
        return page
    }

    private fun dp(value: Int) = (value * activity.resources.displayMetrics.density).toInt()

    /**
     * The keyboard icon, as on the 2D page: the ray taps it to show or hide the XR keyboard. Right
     * edge, above the middle, where Big Picture keeps nothing of its own.
     */
    private fun addKeyboardButton() {
        val keyboard = ImageButton(activity).apply {
            setImageResource(R.drawable.icon_keyboard)
            scaleType = android.widget.ImageView.ScaleType.FIT_CENTER
            val padding = dp(12)
            setPadding(padding, padding, padding, padding)
            isFocusable = false
            contentDescription = activity.getString(R.string.steam_client_keyboard)
            setColorFilter(Color.WHITE)
            background = GradientDrawable().apply { setColor(0x99202A35.toInt()); cornerRadius = dp(24).toFloat() }
            setOnClickListener { activity.toggleKeyboard() }
        }
        val size = dp(48)
        page.addView(keyboard, FrameLayout.LayoutParams(size, size, Gravity.END or Gravity.TOP).apply { rightMargin = dp(8) })
        page.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
            keyboard.translationY = page.height * KEYBOARD_Y - size / 2f
        }
    }

    private fun buildMenu(): LinearLayout = LinearLayout(activity).apply {
        orientation = LinearLayout.VERTICAL
        val padding = dp(20)
        setPadding(padding, padding, padding, padding)
        background = GradientDrawable().apply { setColor(0xF0101826.toInt()); cornerRadius = dp(20).toFloat() }
        visibility = View.GONE
        addView(TextView(activity).apply {
            text = activity.getString(R.string.steam_xr_menu_title)
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
            setPadding(0, 0, 0, dp(12))
        })
        fun item(label: Int, action: () -> Unit) = addView(Button(activity).apply {
            text = activity.getString(label)
            isAllCaps = false
            setOnClickListener { action() }
        }, LinearLayout.LayoutParams(dp(320), ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(6) })
        item(R.string.steam_xr_menu_2d) { switchTo2d() }
        item(R.string.steam_xr_menu_keyboard) { menu(false); activity.openKeyboard() }
        item(R.string.steam_xr_menu_recenter) { activity.resetScreen() }
        item(R.string.steam_xr_menu_resize) { activity.resizeHandles = !activity.resizeHandles; menu(false) }
        item(R.string.steam_xr_menu_passthrough) { activity.passthrough = !activity.passthrough }
        item(R.string.steam_xr_menu_home) { goHome() }
        item(R.string.steam_xr_menu_close) { menu(false) }
        addView(TextView(activity).apply {
            text = activity.getString(R.string.steam_xr_menu_hint)
            setTextColor(0xB3FFFFFF.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            setPadding(0, dp(12), 0, 0)
        })
    }

    private fun menu(show: Boolean) {
        menu.visibility = if (show) View.VISIBLE else View.GONE
        changed.set(true)
    }

    // The home page's task went to the back while this page ran, and comes forward again as this
    // page finishes. Pages leaving for it start from the home page, so they land on top of it.
    private fun homeOrThis(): android.app.Activity =
        app.gamenative.xrgame.LaunchActivity.current?.get()?.takeUnless { it.isFinishing || it.isDestroyed } ?: activity

    private fun switchTo2d() {
        XrGameOfficialSteam.display = XrGameOfficialSteam.Display.FLAT
        val from = homeOrThis()
        from.startActivity(Intent(from, LinuxSteamActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT))
        activity.finish()
    }

    private fun goHome() {
        val from = homeOrThis()
        from.startActivity(Intent().setClassName(from, "app.gamenative.xrgame.LaunchActivity")
            .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT))
        activity.finish()
    }

    private fun showNote(text: String) {
        note.text = text
        note.visibility = View.VISIBLE
        main.removeCallbacks(hideNote)
        main.postDelayed(hideNote, 8000)
        changed.set(true)
    }

    private fun showLoading(show: Boolean) {
        loading.visibility = if (show) View.VISIBLE else View.GONE
        changed.set(true)
    }

    private fun attach(value: XServer?) {
        showLoading(value == null || !LinuxSteamService.displayReady.value)
        if (server === value) return
        detachRenderer()
        surface.removeAllViews()
        server = value
        renderer = null
        ime?.close()
        ime = null
        if (value == null) return
        ime = SteamImeSession(activity, value) { showNote(activity.getString(R.string.steam_client_input_failed)) }
        // GPU frames arrive as hardware buffers through Present, which the Vulkan renderer takes.
        val view: XServerRendererView =
            if (LinuxSteamService.gpuDisplay.value) XServerView(activity, value, "vulkan") else XServerViewGL(activity, value)
        value.renderer = view.renderer
        renderer = view
        (view as View).apply {
            surface.addView(this)
            // The ray's pen: a tap clicks, a drag scrolls the page, as a finger does on the 2D page.
            val touch = SteamTouchPointer(value, this)
            setOnTouchListener { _, event -> touch.onTouch(event); true }
        }
        view.onResume()
        changed.set(true)
        // A client started again at another resolution needs a layer of its size.
        activity.screenChanged()
    }

    private fun detachRenderer() {
        val current = server
        val view = renderer
        if (current != null && view != null) current.lockAll().use {
            (view.renderer as? WindowManager.OnWindowModificationListener)?.let { current.windowManager.removeOnWindowModificationListener(it) }
            (view.renderer as? Pointer.OnPointerMotionListener)?.let { current.pointer.removeOnPointerMotionListener(it) }
            if (current.renderer === view.renderer) current.renderer = null
        }
        view?.onPause()
        // The X server outlives this page; another page makes its own renderer.
        (view?.renderer as? VulkanRenderer)?.release()
    }

    /** [x], [y] in the renderer view's coordinates to the X screen, through its aspect-preserving letterbox. */
    private fun toScreen(value: XServer, view: View, x: Float, y: Float): Pair<Int, Int>? {
        val width = value.screenInfo.width.toInt()
        val height = value.screenInfo.height.toInt()
        val scale = minOf(view.width / width.toFloat(), view.height / height.toFloat())
        if (scale <= 0f) return null
        val sx = ((x - (view.width - width * scale) / 2) / scale).toInt().coerceIn(0, width - 1)
        val sy = ((y - (view.height - height * scale) / 2) / scale).toInt().coerceIn(0, height - 1)
        return sx to sy
    }

    override fun hover(x: Float, y: Float) {
        val value = server ?: return
        val view = renderer as? View ?: return
        // The page and the renderer view share their origin; both fill the page.
        val (sx, sy) = toScreen(value, view, x, y) ?: return
        value.injectPointerMove(sx, sy)
    }

    override fun scroll(steps: Int, horizontal: Boolean) {
        main.post {
            val value = server ?: return@post
            val button = when {
                horizontal && steps > 0 -> Pointer.Button.BUTTON_SCROLL_CLICK_RIGHT
                horizontal -> Pointer.Button.BUTTON_SCROLL_CLICK_LEFT
                steps > 0 -> Pointer.Button.BUTTON_SCROLL_DOWN
                else -> Pointer.Button.BUTTON_SCROLL_UP
            }
            repeat(kotlin.math.abs(steps)) {
                value.injectPointerButtonPress(button)
                value.injectPointerButtonRelease(button)
            }
        }
    }

    override fun key(keyCode: Int, down: Boolean) {
        main.post {
            // With the menu open, A and B act on it as Enter and Escape would.
            if (menu.visibility == View.VISIBLE) {
                if (!down && keyCode == KeyEvent.KEYCODE_ESCAPE) menu(false)
                if (!down && keyCode == KeyEvent.KEYCODE_ENTER) menu.findFocus()?.performClick()
                return@post
            }
            val now = android.os.SystemClock.uptimeMillis()
            server?.keyboard?.onVirtualKeyEvent(KeyEvent(now, now, if (down) KeyEvent.ACTION_DOWN else KeyEvent.ACTION_UP, keyCode, 0))
        }
    }

    override fun type(text: String) {
        val session = ime ?: return
        // One queue keeps the order: Chinese is pasted, which takes longer than a key press.
        val chunk = StringBuilder()
        fun flush() { if (chunk.isNotEmpty()) { session.commit(chunk.toString()); chunk.setLength(0) } }
        for (ch in text) {
            when (ch) {
                '' -> { flush(); session.press(com.winlator.xserver.XKeycode.KEY_BKSP) }
                '' -> { flush(); session.press(com.winlator.xserver.XKeycode.KEY_ENTER) }
                else -> chunk.append(ch)
            }
        }
        flush()
    }

    override fun toggleMenu() = menu(menu.visibility != View.VISIBLE)

    override fun dispatchKey(event: KeyEvent): Boolean {
        if (menu.visibility == View.VISIBLE) {
            if (event.keyCode == KeyEvent.KEYCODE_BUTTON_B || event.keyCode == KeyEvent.KEYCODE_BACK) {
                if (event.action == KeyEvent.ACTION_UP) menu(false)
                return true
            }
            return false // The menu's buttons take focus navigation and clicks.
        }
        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_SELECT || event.keyCode == KeyEvent.KEYCODE_F6) {
            if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) activity.openKeyboard()
            return true
        }
        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_MODE) {
            if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) toggleMenu()
            return true
        }
        val code = when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER -> KeyEvent.KEYCODE_ENTER
            KeyEvent.KEYCODE_BUTTON_B -> KeyEvent.KEYCODE_ESCAPE
            else -> event.keyCode
        }
        if (code == KeyEvent.KEYCODE_BACK) return false
        val mapped = if (code == event.keyCode) event else KeyEvent(event.downTime, event.eventTime,
            event.action, code, event.repeatCount, event.metaState, event.deviceId, 0, event.flags, event.source)
        return server?.keyboard?.onVirtualKeyEvent(mapped) == true
    }

    override fun consumeContentChanged(): Boolean = changed.getAndSet(false)

    override fun onResume() { renderer?.onResume() }

    override fun onPause() {
        server?.keyboard?.releaseAllKeys()
        renderer?.onPause()
    }

    override fun release() {
        main.removeCallbacksAndMessages(null)
        ime?.close()
        ime = null
        detachRenderer()
    }
}
