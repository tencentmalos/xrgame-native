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
import android.widget.TextView
import android.widget.ImageButton
import androidx.activity.ComponentActivity
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.repeatOnLifecycle
import app.gamenative.externaldisplay.IMEInputReceiver
import app.gamenative.R
import com.winlator.widget.XServerViewGL
import com.winlator.xserver.Pointer
import com.winlator.xserver.XServer
import kotlinx.coroutines.launch

/** Official Big Picture window with the existing Android renderer and IME. */
class LinuxSteamActivity : ComponentActivity() {
    private var server: XServer? = null
    private var renderer: XServerViewGL? = null
    private var ime: IMEInputReceiver? = null
    private var imeSession: SteamImeSession? = null
    private var multiTouch = false
    private lateinit var surface: FrameLayout
    private lateinit var loading: TextView

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
        loading = TextView(this).apply {
            gravity = Gravity.CENTER
            text = getString(R.string.steam_client_preparing)
            setTextColor(android.graphics.Color.WHITE)
            setBackgroundColor(0xFF0B1420.toInt())
        }
        page.addView(loading, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        setContentView(page)
        immersive()
        lifecycleScope.launch { LinuxSteamService.status.collect { loading.text = it } }
        lifecycleScope.launch { LinuxSteamService.displayReady.collect {
            loading.visibility = if (it) View.GONE else View.VISIBLE
        } }
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

    private fun attach(value: XServer?) {
        loading.visibility = if (value != null && LinuxSteamService.displayReady.value) View.GONE else View.VISIBLE
        if (server === value && imeSession != null) return
        detachRenderer()
        surface.removeAllViews()
        server = value; renderer = null; ime = null
        if (value == null) return
        val view = XServerViewGL(this, value)
        value.renderer = view.renderer
        renderer = view
        surface.addView(view)
        val session = SteamImeSession(this, value) {
            android.app.AlertDialog.Builder(this).setMessage(R.string.steam_client_input_failed)
                .setPositiveButton(android.R.string.ok, null).show()
        }
        imeSession = session
        val input = session.receiver(this)
        ime = input
        surface.addView(input, FrameLayout.LayoutParams(1, 1))
        val keyboard = ImageButton(this).apply {
            setImageResource(R.drawable.icon_keyboard)
            scaleType = android.widget.ImageView.ScaleType.FIT_CENTER
            val padding = (12 * resources.displayMetrics.density).toInt()
            setPadding(padding, padding, padding, padding)
            isFocusable = false
            contentDescription = getString(R.string.steam_client_keyboard)
            tooltipText = contentDescription
            setColorFilter(android.graphics.Color.WHITE)
            background = android.graphics.drawable.GradientDrawable().apply {
                setColor(0x99202A35.toInt()); cornerRadius = 24 * resources.displayMetrics.density
            }
            setOnClickListener { toggleKeyboard() }
        }
        val size = (48 * resources.displayMetrics.density).toInt()
        surface.addView(keyboard, FrameLayout.LayoutParams(size, size, Gravity.END or Gravity.CENTER_VERTICAL).apply {
            rightMargin = (8 * resources.displayMetrics.density).toInt()
        })
        // Direct touch coordinates account for the renderer's aspect-preserving letterbox.
        view.setOnTouchListener { _, event ->
            if (event.actionMasked == MotionEvent.ACTION_DOWN) multiTouch = false
            if (event.actionMasked == MotionEvent.ACTION_POINTER_DOWN && event.pointerCount == 2) {
                multiTouch = true
                value.injectPointerButtonRelease(Pointer.Button.BUTTON_LEFT)
                toggleKeyboard()
            }
            if (multiTouch) return@setOnTouchListener true
            val width = value.screenInfo.width.toInt()
            val height = value.screenInfo.height.toInt()
            val scale = minOf(view.width / width.toFloat(), view.height / height.toFloat())
            if (scale <= 0) return@setOnTouchListener true
            val x = ((event.x - (view.width - width * scale) / 2) / scale).toInt().coerceIn(0, width - 1)
            val y = ((event.y - (view.height - height * scale) / 2) / scale).toInt().coerceIn(0, height - 1)
            value.injectPointerMove(x, y)
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> value.injectPointerButtonPress(Pointer.Button.BUTTON_LEFT)
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> value.injectPointerButtonRelease(Pointer.Button.BUTTON_LEFT)
            }
            true
        }
        view.onResume()
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
            current.windowManager.removeOnWindowModificationListener(view.renderer)
            current.pointer.removeOnPointerMotionListener(view.renderer)
            if (current.renderer === view.renderer) current.renderer = null
        }
        view?.onPause()
    }
    override fun onDestroy() { detachRenderer(); super.onDestroy() }
}
