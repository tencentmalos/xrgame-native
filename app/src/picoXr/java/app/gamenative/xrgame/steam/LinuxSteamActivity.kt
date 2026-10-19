// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import android.content.Intent
import android.os.Bundle
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ViewGroup
import android.view.View
import android.view.Gravity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import android.widget.FrameLayout
import android.widget.ImageButton
import androidx.activity.ComponentActivity
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.repeatOnLifecycle
import app.gamenative.externaldisplay.IMEInputReceiver
import app.gamenative.R
import com.winlator.renderer.VulkanRenderer
import com.winlator.widget.XServerRendererView
import com.winlator.widget.XServerView
import com.winlator.widget.XServerViewGL
import com.winlator.xserver.Pointer
import com.winlator.xserver.WindowManager
import com.winlator.xserver.XServer
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch

/** Official Big Picture window with the existing Android renderer and IME. */
class LinuxSteamActivity : ComponentActivity() {
    companion object {
        /** The page DebugBus `steam_ime` drives in debug builds. */
        @Volatile var current: java.lang.ref.WeakReference<LinuxSteamActivity>? = null
        private const val KEYBOARD_PREFS = "steam_page"
        private const val KEYBOARD_X = "keyboard_button_x"
        private const val KEYBOARD_Y = "keyboard_button_y"
        // Above Big Picture's per-game settings button, which sits at mid-height on the right.
        private const val KEYBOARD_DEFAULT_Y = 0.3f
        // Holding a touch still this long presses the mouse button.
        private const val HOLD_MS = 400L
        // X-screen pixels of drag per wheel step.
        private const val SCROLL_STEP = 40f
    }

    /** This page's IME bridge, for DebugBus `steam_ime`; main thread only. */
    val imeReceiver: IMEInputReceiver? get() = ime
    private var server: XServer? = null
    private var renderer: XServerRendererView? = null
    private var ime: IMEInputReceiver? = null
    private var imeSession: SteamImeSession? = null
    private var multiTouch = false
    private var keyboardLayout: View.OnLayoutChangeListener? = null
    private lateinit var surface: FrameLayout
    private lateinit var loading: SteamLaunchView

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        window.attributes = window.attributes.apply {
            layoutInDisplayCutoutMode = android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }
        val page = FrameLayout(this).apply { setBackgroundColor(android.graphics.Color.BLACK) }
        surface = FrameLayout(this)
        page.addView(surface, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        // Only display startup/failure status before the Steam surface exists.
        // A running client has no Android toolbar, title or diagnostic controls.
        loading = SteamLaunchView(this).apply { setStatus(getString(R.string.steam_client_preparing)) }
        page.addView(loading, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        setContentView(page)
        immersive()
        lifecycleScope.launch {
            // Until this page's session starts, the service still reports its idle state.
            var started = false
            LinuxSteamService.status.combine(LinuxSteamService.phase) { text, phase -> text to phase }.collect { (text, phase) ->
                if (phase != LinuxSteamPhase.STOPPED) started = true
                if (!started && text == LinuxSteamService.IDLE_STATUS) return@collect
                loading.setStatus(if (phase == LinuxSteamPhase.STARTING) getString(R.string.steam_client_starting) else text)
                loading.setPhase(phase)
            }
        }
        lifecycleScope.launch { LinuxSteamService.displayReady.collect { showLoading(!it) } }
        lifecycleScope.launch { LinuxSteamService.xServerState.collect { attach(it) } }
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.RESUMED) {
                launch { SteamGameHandoff.pending.collect { request ->
                    if (request != null && !request.dispatched && request.file.isFile &&
                        System.currentTimeMillis() - request.file.lastModified() < 10000) {
                        request.dispatched = true
                        startActivity(Intent("app.gamenative.LAUNCH_GAME").setClass(this@LinuxSteamActivity, OfficialSteamGameActivity::class.java)
                            .putExtra("app_id", request.appId).putExtra("game_source", "STEAM")
                            .addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP))
                    }
                } }
                launch { SteamGameHandoff.error.collect { message ->
                    if (message != null) {
                        SteamGameHandoff.error.value = null
                        android.app.AlertDialog.Builder(this@LinuxSteamActivity).setMessage(message)
                            .setPositiveButton(android.R.string.ok, null).show()
                    }
                } }
            }
        }
        if (state == null) startForegroundService(Intent(this, LinuxSteamService::class.java))
    }

    private fun immersive() {
        WindowCompat.getInsetsController(window, window.decorView).apply {
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            hide(WindowInsetsCompat.Type.systemBars())
        }
    }

    /** Big Picture replaces the transition page with a short fade instead of a cut. */
    private fun showLoading(show: Boolean) {
        loading.animate().cancel()
        if (show) {
            loading.alpha = 1f
            loading.visibility = View.VISIBLE
        } else if (loading.visibility == View.VISIBLE) {
            loading.animate().alpha(0f).setDuration(420).withEndAction { loading.visibility = View.GONE }.start()
        }
    }

    private fun attach(value: XServer?) {
        showLoading(value == null || !LinuxSteamService.displayReady.value)
        if (server === value) {
            // After a pause only the input queue is new; the renderer keeps its window images.
            if (value != null && imeSession == null) attachInput(value)
            return
        }
        detachRenderer()
        surface.removeAllViews()
        server = value; renderer = null; ime = null
        if (value == null) return
        // GPU frames arrive as hardware buffers through Present, which the Vulkan renderer takes.
        val rendererView: XServerRendererView =
            if (LinuxSteamService.gpuDisplay.value) XServerView(this, value, "vulkan") else XServerViewGL(this, value)
        val view = rendererView as View
        value.renderer = rendererView.renderer
        renderer = rendererView
        surface.addView(view)
        attachInput(value)
        addKeyboardButton()
        val touch = TouchPointer(value, view)
        view.setOnTouchListener { _, event ->
            if (event.actionMasked == MotionEvent.ACTION_DOWN) multiTouch = false
            if (event.actionMasked == MotionEvent.ACTION_POINTER_DOWN && event.pointerCount == 2) {
                multiTouch = true
                touch.cancel()
                toggleKeyboard()
            }
            if (!multiTouch) touch.onTouch(event)
            true
        }
        rendererView.onResume()
    }

    /**
     * The on-screen keyboard button. A tap toggles the IME; a drag moves the button, and the
     * place is kept (as fractions of the page) for the next page.
     */
    private fun addKeyboardButton() {
        val density = resources.displayMetrics.density
        val prefs = getSharedPreferences(KEYBOARD_PREFS, MODE_PRIVATE)
        val keyboard = ImageButton(this).apply {
            setImageResource(R.drawable.icon_keyboard)
            scaleType = android.widget.ImageView.ScaleType.FIT_CENTER
            val padding = (12 * density).toInt()
            setPadding(padding, padding, padding, padding)
            isFocusable = false
            contentDescription = getString(R.string.steam_client_keyboard)
            tooltipText = contentDescription
            setColorFilter(android.graphics.Color.WHITE)
            background = android.graphics.drawable.GradientDrawable().apply {
                setColor(0x99202A35.toInt()); cornerRadius = 24 * density
            }
        }
        val size = (48 * density).toInt()
        val margin = (8 * density).toInt()
        surface.addView(keyboard, FrameLayout.LayoutParams(size, size, Gravity.TOP or Gravity.START))
        // Centre of the button as a fraction of the page.
        fun place(fx: Float, fy: Float) {
            val maxX = (surface.width - size - margin).coerceAtLeast(margin)
            val maxY = (surface.height - size - margin).coerceAtLeast(margin)
            keyboard.x = (fx * surface.width - size / 2f).coerceIn(margin.toFloat(), maxX.toFloat())
            keyboard.y = (fy * surface.height - size / 2f).coerceIn(margin.toFloat(), maxY.toFloat())
        }
        fun placeSaved() {
            if (surface.width == 0) return
            // Right edge, above the middle: Big Picture keeps per-game settings at mid-height.
            val fx = prefs.getFloat(KEYBOARD_X, -1f).takeIf { it >= 0 } ?: ((surface.width - margin - size / 2f) / surface.width)
            place(fx, prefs.getFloat(KEYBOARD_Y, KEYBOARD_DEFAULT_Y))
        }
        keyboardLayout?.let { surface.removeOnLayoutChangeListener(it) }
        keyboardLayout = View.OnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> placeSaved() }
            .also { surface.addOnLayoutChangeListener(it) }
        surface.post { placeSaved() }
        val slop = android.view.ViewConfiguration.get(this).scaledTouchSlop * 2f
        var downX = 0f; var downY = 0f; var startX = 0f; var startY = 0f; var dragging = false
        keyboard.setOnTouchListener { button, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    downX = event.rawX; downY = event.rawY; startX = button.x; startY = button.y; dragging = false
                    button.isPressed = true
                }
                MotionEvent.ACTION_MOVE -> {
                    if (!dragging && Math.hypot((event.rawX - downX).toDouble(), (event.rawY - downY).toDouble()) > slop) {
                        dragging = true
                        button.isPressed = false
                    }
                    if (dragging) {
                        place((startX + event.rawX - downX + size / 2f) / surface.width,
                              (startY + event.rawY - downY + size / 2f) / surface.height)
                    }
                }
                MotionEvent.ACTION_UP -> {
                    button.isPressed = false
                    if (dragging) {
                        prefs.edit().putFloat(KEYBOARD_X, (button.x + size / 2f) / surface.width)
                            .putFloat(KEYBOARD_Y, (button.y + size / 2f) / surface.height).apply()
                    } else {
                        button.performClick()
                        toggleKeyboard()
                    }
                }
                MotionEvent.ACTION_CANCEL -> button.isPressed = false
            }
            true
        }
    }

    /**
     * Touches (fingers, and controller rays in the headset's touch emulation) on the Steam surface,
     * as the X pointer. A tap clicks. A drag scrolls the page with wheel steps along the axis it
     * starts on: Chromium does not scroll on a held mouse button, so a pressed drag only selected
     * or dragged items. Holding still presses the button, for sliders and drag and drop.
     * Coordinates account for the renderer's aspect-preserving letterbox.
     */
    private inner class TouchPointer(private val server: XServer, private val view: View) {
        private val slop = android.view.ViewConfiguration.get(view.context).scaledTouchSlop * 2f
        private var mode = Mode.IDLE
        private var downX = 0f
        private var downY = 0f
        private var screenX = 0
        private var screenY = 0
        private var horizontal = false
        private var scrollX = 0f
        private var scrollY = 0f
        private val hold = Runnable {
            if (mode == Mode.PENDING) {
                mode = Mode.HELD
                server.injectPointerButtonPress(Pointer.Button.BUTTON_LEFT)
            }
        }

        fun onTouch(event: MotionEvent) {
            val width = server.screenInfo.width.toInt()
            val height = server.screenInfo.height.toInt()
            val scale = minOf(view.width / width.toFloat(), view.height / height.toFloat())
            if (scale <= 0) return
            val x = ((event.x - (view.width - width * scale) / 2) / scale).toInt().coerceIn(0, width - 1)
            val y = ((event.y - (view.height - height * scale) / 2) / scale).toInt().coerceIn(0, height - 1)
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    mode = Mode.PENDING
                    downX = event.x; downY = event.y; screenX = x; screenY = y
                    scrollX = 0f; scrollY = 0f
                    server.injectPointerMove(x, y)
                    view.postDelayed(hold, HOLD_MS)
                }
                MotionEvent.ACTION_MOVE -> when (mode) {
                    Mode.PENDING -> {
                        val dx = event.x - downX
                        val dy = event.y - downY
                        if (Math.hypot(dx.toDouble(), dy.toDouble()) > slop) {
                            view.removeCallbacks(hold)
                            mode = Mode.SCROLL
                            horizontal = Math.abs(dx) > Math.abs(dy)
                            scroll(dx / scale, dy / scale)
                        }
                    }
                    Mode.SCROLL -> scroll((event.x - downX) / scale, (event.y - downY) / scale)
                    Mode.HELD -> server.injectPointerMove(x, y)
                    Mode.IDLE -> Unit
                }
                MotionEvent.ACTION_UP -> {
                    view.removeCallbacks(hold)
                    when (mode) {
                        Mode.PENDING -> {
                            server.injectPointerMove(screenX, screenY)
                            server.injectPointerButtonPress(Pointer.Button.BUTTON_LEFT)
                            server.injectPointerButtonRelease(Pointer.Button.BUTTON_LEFT)
                        }
                        Mode.HELD -> {
                            server.injectPointerMove(x, y)
                            server.injectPointerButtonRelease(Pointer.Button.BUTTON_LEFT)
                        }
                        else -> Unit
                    }
                    mode = Mode.IDLE
                }
                MotionEvent.ACTION_CANCEL -> cancel()
            }
        }

        /** Releases a held button and drops the gesture (a second finger, a cancel). */
        fun cancel() {
            view.removeCallbacks(hold)
            if (mode == Mode.HELD) server.injectPointerButtonRelease(Pointer.Button.BUTTON_LEFT)
            mode = Mode.IDLE
        }

        /**
         * [totalX], [totalY]: X-screen pixels dragged since the touch began. Content follows the
         * finger, so dragging up scrolls down. The pointer stays where the touch began, over the
         * list being scrolled.
         */
        private fun scroll(totalX: Float, totalY: Float) {
            val total = if (horizontal) totalX else totalY
            val done = if (horizontal) scrollX else scrollY
            val steps = ((total - done) / SCROLL_STEP).toInt()
            if (steps == 0) return
            val button = when {
                horizontal && steps < 0 -> Pointer.Button.BUTTON_SCROLL_CLICK_RIGHT
                horizontal -> Pointer.Button.BUTTON_SCROLL_CLICK_LEFT
                steps < 0 -> Pointer.Button.BUTTON_SCROLL_DOWN
                else -> Pointer.Button.BUTTON_SCROLL_UP
            }
            repeat(Math.abs(steps)) {
                server.injectPointerButtonPress(button)
                server.injectPointerButtonRelease(button)
            }
            if (horizontal) scrollX += steps * SCROLL_STEP else scrollY += steps * SCROLL_STEP
        }
    }

    private enum class Mode { IDLE, PENDING, SCROLL, HELD }

    /** A bounded input queue for this resume; [onPause] closes it. */
    private fun attachInput(value: XServer) {
        ime?.let { surface.removeView(it) }
        val session = SteamImeSession(this, value) {
            android.app.AlertDialog.Builder(this).setMessage(R.string.steam_client_input_failed)
                .setPositiveButton(android.R.string.ok, null).show()
        }
        imeSession = session
        val input = session.receiver(this)
        ime = input
        surface.addView(input, FrameLayout.LayoutParams(1, 1))
    }

    private fun toggleKeyboard() {
        val input = ime ?: return
        if (input.isKeyboardRequested) input.hideKeyboard() else input.showKeyboard()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_SELECT || event.keyCode == KeyEvent.KEYCODE_F6) {
            if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) toggleKeyboard()
            return true
        }
        if ((event.keyCode == KeyEvent.KEYCODE_BUTTON_B || event.keyCode == KeyEvent.KEYCODE_BACK) && ime?.isKeyboardRequested == true) {
            if (event.action == KeyEvent.ACTION_UP) ime?.hideKeyboard()
            return true
        }
        val code = when (event.keyCode) {
            KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER -> KeyEvent.KEYCODE_ENTER
            KeyEvent.KEYCODE_BUTTON_B -> KeyEvent.KEYCODE_ESCAPE
            else -> event.keyCode
        }
        val mapped = if (code == event.keyCode) event else KeyEvent(event.downTime, event.eventTime,
            event.action, code, event.repeatCount, event.metaState, event.deviceId, 0, event.flags, event.source)
        if (code != KeyEvent.KEYCODE_BACK && server?.keyboard?.onVirtualKeyEvent(mapped) == true) return true
        return super.dispatchKeyEvent(event)
    }
    override fun onResume() {
        super.onResume(); immersive()
        Companion.current = java.lang.ref.WeakReference(this)
        // Recreate the bounded input queue after pause, keeping the same X server.
        val current = server
        if (current != null && imeSession == null) attach(current)
        renderer?.onResume()
    }
    override fun onWindowFocusChanged(hasFocus: Boolean) { super.onWindowFocusChanged(hasFocus); if (hasFocus) immersive() }
    override fun onPause() { ime?.hideKeyboard(); imeSession?.close(); imeSession = null; server?.keyboard?.releaseAllKeys(); renderer?.onPause(); super.onPause() }
    private fun detachRenderer() {
        ime?.hideKeyboard()
        imeSession?.close(); imeSession = null
        val current = server
        val view = renderer
        if (current != null && view != null) current.lockAll().use {
            (view.renderer as? WindowManager.OnWindowModificationListener)?.let { current.windowManager.removeOnWindowModificationListener(it) }
            (view.renderer as? Pointer.OnPointerMotionListener)?.let { current.pointer.removeOnPointerMotionListener(it) }
            if (current.renderer === view.renderer) current.renderer = null
        }
        view?.onPause()
        // The X server outlives this page; a later page makes its own renderer.
        (view?.renderer as? VulkanRenderer)?.release()
    }
    override fun onDestroy() {
        if (Companion.current?.get() === this) Companion.current = null
        detachRenderer(); super.onDestroy()
    }
}
