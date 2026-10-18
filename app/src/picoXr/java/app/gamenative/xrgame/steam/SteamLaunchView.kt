// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.graphics.drawable.Drawable
import android.os.SystemClock
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.util.TypedValue
import android.view.View
import android.view.animation.LinearInterpolator
import app.gamenative.R
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.sin

/** Step shown as current on the transition page; -1 when the client is not starting. */
internal fun launchStep(phase: LinuxSteamPhase): Int = when (phase) {
    LinuxSteamPhase.PREPARING, LinuxSteamPhase.DOWNLOADING -> 0
    LinuxSteamPhase.OPENING -> 1
    LinuxSteamPhase.STARTING -> 2
    LinuxSteamPhase.STOPPED -> -1
}

/** Seconds in the last step before the page explains that a first start or update takes longer. */
internal const val LAUNCH_HINT_AFTER_S = 20f

/**
 * Transition page while the official client starts. A soft glow drifts over the background; the
 * app icon breathes inside an indeterminate ring; the status text crossfades; three dots mark the
 * steps (prepare, start the client, enter Big Picture). In the last step, which waits for
 * Steam's own update check, web helper and sign-in, a note appears after a while. The page only
 * animates while shown.
 */
class SteamLaunchView(context: Context) : View(context) {
    private val density = resources.displayMetrics.density
    private val accent = 0xFF1A9FFF.toInt()
    private val backdrop = Paint()
    private val glow = Paint(Paint.ANTI_ALIAS_FLAG)
    private val glowMatrix = Matrix()
    private val track = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 3 * density; color = 0x22FFFFFF
    }
    private val arc = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; strokeWidth = 3.5f * density; strokeCap = Paint.Cap.ROUND; color = accent
    }
    private val dot = Paint(Paint.ANTI_ALIAS_FLAG)
    private val title = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE; textSize = sp(18f); typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
    }
    private val hint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE; textSize = sp(13f) }
    private val icon: Drawable? = runCatching { context.applicationInfo.loadIcon(context.packageManager) }.getOrNull()
    private val ring = RectF()
    private val hintText = context.getString(R.string.steam_client_launch_hint)
    private val start = SystemClock.uptimeMillis()

    private var text = ""
    private var previous = ""
    private var textLayout: StaticLayout? = null
    private var previousLayout: StaticLayout? = null
    private var hintLayout: StaticLayout? = null
    private var textChangedAt = 0L
    private var phase = LinuxSteamPhase.PREPARING
    private var phaseSince = start

    private val ticker = ValueAnimator.ofFloat(0f, 1f).apply {
        duration = 1000
        repeatCount = ValueAnimator.INFINITE
        interpolator = LinearInterpolator()
        addUpdateListener { invalidate() }
    }

    private fun sp(value: Float) =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, value, resources.displayMetrics)

    fun setStatus(value: String) {
        if (value == text) return
        previous = text
        previousLayout = textLayout
        text = value
        textLayout = null
        textChangedAt = SystemClock.uptimeMillis()
        contentDescription = value
        invalidate()
    }

    fun setPhase(value: LinuxSteamPhase) {
        if (value == phase) return
        phase = value
        phaseSince = SystemClock.uptimeMillis()
        invalidate()
    }

    private fun animate(shown: Boolean) {
        if (shown && isAttachedToWindow) { if (!ticker.isStarted) ticker.start() } else ticker.cancel()
    }

    override fun onAttachedToWindow() { super.onAttachedToWindow(); animate(isShown) }
    override fun onDetachedFromWindow() { ticker.cancel(); super.onDetachedFromWindow() }
    override fun onVisibilityChanged(changedView: View, visibility: Int) {
        super.onVisibilityChanged(changedView, visibility)
        animate(isShown)
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        backdrop.shader = LinearGradient(0f, 0f, 0f, h.toFloat(), 0xFF101C2B.toInt(), 0xFF060B12.toInt(), Shader.TileMode.CLAMP)
        glow.shader = RadialGradient(0f, 0f, 0.55f * max(w, h), intArrayOf(0x381A9FFF, 0x0E1A9FFF, 0x001A9FFF),
            floatArrayOf(0f, 0.55f, 1f), Shader.TileMode.CLAMP)
        textLayout = null; previousLayout = null; hintLayout = null
    }

    private fun layout(value: String, paint: TextPaint): StaticLayout =
        StaticLayout.Builder.obtain(value, 0, value.length, paint, (width * 0.8f).toInt().coerceAtLeast(1))
            .setAlignment(Layout.Alignment.ALIGN_CENTER).setMaxLines(3).build()

    private fun drawCentered(canvas: Canvas, layout: StaticLayout, cx: Float, top: Float) {
        canvas.save()
        canvas.translate(cx - layout.width / 2f, top)
        layout.draw(canvas)
        canvas.restore()
    }

    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        val now = SystemClock.uptimeMillis()
        val t = (now - start) / 1000f
        val step = launchStep(phase)
        val active = step >= 0
        canvas.drawRect(0f, 0f, w, h, backdrop)

        // A slow glow keeps the page alive without drawing attention.
        glowMatrix.setTranslate(w * (0.5f + 0.22f * sin(t * 2 * PI.toFloat() / 13f)),
            h * (0.42f + 0.1f * cos(t * 2 * PI.toFloat() / 9f)))
        glow.shader?.setLocalMatrix(glowMatrix)
        canvas.drawRect(0f, 0f, w, h, glow)

        val cx = w / 2f
        val cy = h * 0.42f
        val radius = 46 * density
        icon?.let {
            val breathe = if (active) 1f + 0.035f * sin(t * 2 * PI.toFloat() / 2.6f) else 1f
            val half = (30 * density * breathe).toInt()
            it.setBounds((cx - half).toInt(), (cy - half).toInt(), (cx + half).toInt(), (cy + half).toInt())
            it.alpha = if (active) 255 else 130
            it.draw(canvas)
        }
        ring.set(cx - radius, cy - radius, cx + radius, cy + radius)
        canvas.drawOval(ring, track)
        if (active) {
            val sweep = 40f + 200f * (0.5f - 0.5f * cos(t * 2 * PI.toFloat() / 1.6f))
            canvas.drawArc(ring, (t * 240f) % 360f, sweep, false, arc)
        }

        // The status crossfades and settles upward when it changes.
        val p = ((now - textChangedAt) / 320f).coerceIn(0f, 1f).let { 1f - (1f - it) * (1f - it) }
        val textTop = cy + radius + 28 * density
        val shift = 10 * density
        if (p < 1f && previous.isNotEmpty()) {
            val old = previousLayout ?: layout(previous, title).also { previousLayout = it }
            title.alpha = ((1f - p) * 255).toInt()
            drawCentered(canvas, old, cx, textTop - shift * p)
        }
        val current = textLayout ?: layout(text, title).also { textLayout = it }
        title.alpha = (p * 255).toInt()
        drawCentered(canvas, current, cx, textTop + shift * (1f - p))
        title.alpha = 255

        var below = textTop + current.height + 22 * density
        if (active) {
            val spacing = 22 * density
            for (i in 0..2) {
                val x = cx + (i - 1) * spacing
                if (i < step) {
                    dot.color = accent
                    canvas.drawCircle(x, below, 3.5f * density, dot)
                } else if (i == step) {
                    val pulse = 0.5f + 0.5f * sin(t * 2 * PI.toFloat() / 1.2f)
                    dot.color = accent
                    dot.alpha = (140 + 115 * pulse).toInt()
                    canvas.drawCircle(x, below, (3.5f + 1.5f * pulse) * density, dot)
                } else {
                    dot.color = 0x40FFFFFF
                    canvas.drawCircle(x, below, 3.5f * density, dot)
                }
            }
            below += 26 * density
        }

        if (phase == LinuxSteamPhase.STARTING) {
            val waited = (now - phaseSince) / 1000f
            if (waited > LAUNCH_HINT_AFTER_S) {
                val layout = hintLayout ?: layout(hintText, hint).also { hintLayout = it }
                hint.alpha = (((waited - LAUNCH_HINT_AFTER_S) / 1.5f).coerceIn(0f, 1f) * 150).toInt()
                drawCentered(canvas, layout, cx, below)
            }
        }
    }
}
