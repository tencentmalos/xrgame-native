// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import android.view.MotionEvent
import android.view.View
import com.winlator.xserver.Pointer
import com.winlator.xserver.XServer

/**
 * Touches on a Steam page (fingers on the 2D page, the controller ray's pen on the XR big screen)
 * as the X pointer. A tap clicks. A drag scrolls the page with wheel steps along the axis it
 * starts on: Chromium does not scroll on a held mouse button, so a pressed drag only selected or
 * dragged items. Holding still presses the button, for sliders and drag and drop. Coordinates
 * account for the renderer's aspect-preserving letterbox. Main thread.
 */
internal class SteamTouchPointer(private val server: XServer, private val view: View) {
    private val slop = android.view.ViewConfiguration.get(view.context).scaledTouchSlop * 2f
    private var mode = Mode.IDLE
    private var downX = 0f
    private var downY = 0f
    private var screenX = 0
    private var screenY = 0
    private var horizontal = false
    private var scrollX = 0f
    private var scrollY = 0f
    // X-screen pixels a wheel step moves Steam's pages, so the page keeps up with the drag.
    private val step = server.screenInfo.height * STEP_PER_SCREEN_HEIGHT
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
     * finger, so dragging up scrolls down. A step goes once the drag is half a step past the
     * last one, so the page is never more than half a step ahead or behind. The pointer stays
     * where the touch began, over the list being scrolled.
     */
    private fun scroll(totalX: Float, totalY: Float) {
        val total = if (horizontal) totalX else totalY
        val done = if (horizontal) scrollX else scrollY
        val steps = Math.round((total - done) / step)
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
        if (horizontal) scrollX += steps * step else scrollY += steps * step
    }

    private enum class Mode { IDLE, PENDING, SCROLL, HELD }

    private companion object {
        // Holding a touch still this long presses the mouse button.
        const val HOLD_MS = 400L
        // A wheel step moves Steam's library and store 170 X-screen pixels on a 1920x1080 screen
        // (measured on Swan; Big Picture scales with the screen height). The home page snaps to
        // its rows instead.
        const val STEP_PER_SCREEN_HEIGHT = 170f / 1080f
    }
}
