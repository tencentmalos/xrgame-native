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
        // The keyboard button's centre, as a fraction of the page height: above Big Picture's
        // per-game settings button, which sits at mid-height on the right.
        private const val KEYBOARD_Y = 0.3f
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
        val touch = SteamTouchPointer(value, view)
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

    /** The on-screen keyboard button: a tap toggles the IME. */
    private fun addKeyboardButton() {
        val density = resources.displayMetrics.density
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
            setOnClickListener { toggleKeyboard() }
        }
        val size = (48 * density).toInt()
        surface.addView(keyboard, FrameLayout.LayoutParams(size, size, Gravity.END or Gravity.TOP).apply {
            rightMargin = (8 * density).toInt()
        })
        // Right edge, above the middle: Big Picture keeps per-game settings at mid-height.
        fun place() { keyboard.translationY = surface.height * KEYBOARD_Y - size / 2f }
        keyboardLayout?.let { surface.removeOnLayoutChangeListener(it) }
        keyboardLayout = View.OnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> place() }
            .also { surface.addOnLayoutChangeListener(it) }
        surface.post { place() }
    }

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
