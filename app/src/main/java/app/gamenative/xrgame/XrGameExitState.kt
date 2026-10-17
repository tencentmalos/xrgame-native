// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame

import java.util.Locale

/** Armed by a disappearing game window, never by a transiently empty startup process list. */
class XrGameExitState(target: String, essential: Set<String>, private val graceMs: Long = 3_000L) {
    private val target = normalize(target)
    private val essential = essential.map(::normalize).toSet()
    private var emptySince: Long? = null

    fun update(names: List<String>, complete: Boolean, nowMs: Long): Boolean {
        // An unreadable snapshot is not evidence that a game exited. Keep children and
        // self-restarts alive even if the original process/window has already disappeared.
        if (!complete || names.any { normalize(it).let { name -> name == target || name !in essential } }) {
            emptySince = null
            return false
        }
        val since = emptySince ?: nowMs.also { emptySince = it }
        return nowMs - since >= graceMs
    }

    companion object {
        fun normalize(name: String): String = name.trim().trim('"').replace('\\', '/')
            .substringAfterLast('/').lowercase(Locale.ROOT).removeSuffix(".exe")
    }
}
