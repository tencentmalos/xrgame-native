// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

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
}
