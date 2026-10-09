package app.gamenative.ui.screen.xr.windows

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.text.TextUtils
import org.json.JSONObject

/**
 * SteamVR draws some application UI that the app drives through IVRMailbox messages. Half-Life:
 * Alyx shows its loading interstitial that way ("hlvr/interstitials": begin_loading, show_message,
 * end_loading). Without it the headset kept the game's last, faded frame for the whole load and
 * never showed "press Trigger to begin". Our OpenComposite build forwards the messages through
 * the OpenXR runtime's xrSendMailboxMessageGNX to the control server's MAILBOX command.
 */
class WindowsVrInterstitials(private val publish: (State?) -> Unit) {
    data class State(
        val loadingText: String,
        val description: String,
        val tip: String,
        val message: String,
    )

    private var state: State? = null

    /** Returns false for mailboxes or messages this class does not handle. */
    @Synchronized
    fun onMailboxMessage(mailbox: String, json: String): Boolean {
        if (mailbox != MAILBOX) return false
        val message = runCatching { JSONObject(json) }.getOrNull() ?: return false
        val next = when (message.optString("type")) {
            "begin_loading" -> State(
                loadingText = message.text("loadingText"),
                description = message.text("description"),
                tip = message.text("tipText"),
                message = "",
            )
            // Alyx sends it while loading; keep the loading text if one is shown.
            "show_message" -> (state ?: State("", "", "", "")).copy(message = message.text("text"))
            "end_loading" -> null
            else -> return false
        }
        if (next != state) {
            state = next
            publish(next)
        }
        return true
    }

    @Synchronized
    fun clear() {
        if (state != null) {
            state = null
            publish(null)
        }
    }

    private fun JSONObject.text(key: String): String = optString(key).trim().take(MAX_TEXT)

    companion object {
        const val MAILBOX = "hlvr/interstitials"
        private const val MAX_TEXT = 400
    }
}

/** Draws an interstitial as an opaque 16:9 RGBA_8888 panel. */
object WindowsVrInterstitialRenderer {
    const val WIDTH = 1600
    const val HEIGHT = 900
    private const val MARGIN = 96f

    fun render(state: WindowsVrInterstitials.State): Bitmap {
        val bitmap = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        canvas.drawColor(Color.rgb(14, 16, 20))
        val frame = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            style = Paint.Style.STROKE
            strokeWidth = 4f
            color = Color.rgb(58, 64, 74)
        }
        canvas.drawRoundRect(RectF(12f, 12f, WIDTH - 12f, HEIGHT - 12f), 36f, 36f, frame)

        val contentWidth = (WIDTH - 2 * MARGIN).toInt()
        var y = MARGIN
        val title = state.loadingText.ifEmpty { if (state.message.isEmpty()) "Loading" else "" }
        if (title.isNotEmpty()) {
            y = draw(canvas, title, y, contentWidth, 72f, Color.WHITE, bold = true, maxLines = 1) + 36f
        }
        if (state.description.isNotEmpty()) {
            y = draw(canvas, state.description, y, contentWidth, 38f, Color.rgb(214, 218, 224), maxLines = 5) + 28f
        }
        if (state.tip.isNotEmpty()) {
            draw(canvas, state.tip, y, contentWidth, 34f, Color.rgb(150, 158, 168), maxLines = 4)
        }
        if (state.message.isNotEmpty()) {
            draw(canvas, state.message, HEIGHT - MARGIN - 80f, contentWidth, 64f, Color.rgb(255, 176, 32),
                bold = true, maxLines = 1, center = true)
        }
        return bitmap
    }

    private fun draw(
        canvas: Canvas,
        text: String,
        top: Float,
        width: Int,
        size: Float,
        color: Int,
        bold: Boolean = false,
        maxLines: Int,
        center: Boolean = false,
    ): Float {
        val paint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
            textSize = size
            this.color = color
            typeface = if (bold) Typeface.DEFAULT_BOLD else Typeface.DEFAULT
        }
        val layout = StaticLayout.Builder.obtain(text, 0, text.length, paint, width)
            .setAlignment(if (center) Layout.Alignment.ALIGN_CENTER else Layout.Alignment.ALIGN_NORMAL)
            .setLineSpacing(0f, 1.15f)
            .setMaxLines(maxLines)
            .setEllipsize(TextUtils.TruncateAt.END)
            .build()
        canvas.save()
        canvas.translate(MARGIN, top)
        layout.draw(canvas)
        canvas.restore()
        return top + layout.height
    }
}
