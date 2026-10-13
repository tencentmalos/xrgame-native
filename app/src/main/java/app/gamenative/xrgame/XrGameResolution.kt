package app.gamenative.xrgame

/**
 * Game resolutions in XR sessions on headsets: any size up to 1920x1080. The launch panel offers
 * the standard sizes within that; a larger saved size (only the full settings' Custom entry makes
 * one) is scaled down to fit, keeping its aspect ratio, both when the panel saves and when the
 * session starts.
 */
object XrGameResolution {
    const val MAX_WIDTH = 1920
    const val MAX_HEIGHT = 1080

    fun parse(size: String): Pair<Int, Int>? {
        val parts = size.trim().lowercase().split("x")
        if (parts.size != 2) return null
        val width = parts[0].trim().toIntOrNull() ?: return null
        val height = parts[1].trim().toIntOrNull() ?: return null
        return if (width > 0 && height > 0) width to height else null
    }

    fun fits(size: String): Boolean = parse(size)?.let { (w, h) -> w <= MAX_WIDTH && h <= MAX_HEIGHT } ?: false

    /** [size] scaled down to fit 1920x1080 with even sides; sizes that fit or do not parse stay. */
    fun capped(size: String): String {
        val (width, height) = parse(size) ?: return size
        if (width <= MAX_WIDTH && height <= MAX_HEIGHT) return size
        val scale = minOf(MAX_WIDTH.toDouble() / width, MAX_HEIGHT.toDouble() / height)
        val w = maxOf(2, (width * scale).toInt() / 2 * 2)
        val h = maxOf(2, (height * scale).toInt() / 2 * 2)
        return "${w}x$h"
    }
}
