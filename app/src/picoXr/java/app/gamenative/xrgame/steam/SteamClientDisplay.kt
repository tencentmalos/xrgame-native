// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import android.content.Context
import app.gamenative.xrgame.XrGameOfficialSteam
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

object SteamClientDisplay {
    /** Fit the landscape device ratio without making the software client render native 4K. */
    fun resolution(width: Int, height: Int): Pair<Int, Int> {
        if (width <= 0 || height <= 0) return 1280 to 800
        val longEdge = max(width, height)
        val shortEdge = min(width, height)
        val h = (1280.0 * shortEdge / longEdge / 2).roundToInt() * 2
        return 1280 to h.coerceIn(320, 1280)
    }

    /**
     * Steam's own screen on the XR big screen, chosen on the home page. Games keep their own
     * resolution: Steam's page is light, a game is heavy. The client's X server has one size for
     * its whole run, so a new choice applies when the client next starts.
     */
    val XR_RESOLUTIONS = listOf(1280 to 720, 1600 to 900, 1920 to 1080)
    val XR_DEFAULT_RESOLUTION = 1920 to 1080
    private const val XR_RESOLUTION = "resolution"

    fun label(size: Pair<Int, Int>) = "${size.first}x${size.second}"

    /** A stored choice, or 1920x1080 when it is missing or not offered. */
    fun xrResolution(stored: String?): Pair<Int, Int> = XR_RESOLUTIONS.firstOrNull { label(it) == stored } ?: XR_DEFAULT_RESOLUTION

    private fun preferences(context: Context) = context.getSharedPreferences(XrGameOfficialSteam.XR_SETTINGS, Context.MODE_PRIVATE)

    fun xrResolution(context: Context): Pair<Int, Int> = xrResolution(preferences(context).getString(XR_RESOLUTION, null))

    fun setXrResolution(context: Context, size: Pair<Int, Int>) {
        preferences(context).edit().putString(XR_RESOLUTION, label(size)).apply()
    }

    /** The client's screen: the XR choice when a headset's Steam entry is the big screen, else fitted to the display. */
    fun resolution(context: Context, headset: Boolean, displayWidth: Int, displayHeight: Int): Pair<Int, Int> =
        if (headset && XrGameOfficialSteam.display == XrGameOfficialSteam.Display.XR) xrResolution(context)
        else resolution(displayWidth, displayHeight)
}
