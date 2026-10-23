// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Reads single keys of one app from the official client's PICS cache, `appcache/appinfo.vdf`
 * (binary KeyValues, format v28 or v29 with its key table at the end). The client keeps it current
 * for the account's apps, so it answers what JavaSteam's database never stored.
 */
object SteamAppInfo {
    private const val V28 = 0x07564428
    private const val V29 = 0x07564429
    // appid, size, then per app: infostate, last updated, access token, SHA-1, change number, binary SHA-1.
    private const val APP_HEADER = 4 + 4 + 8 + 20 + 4 + 20

    private var cachedFile: File? = null
    private var cachedStamp = 0L
    private var cachedValues = emptyMap<Int, Boolean>()

    /** `common/onlyvrsupport` of [appId]: games that only run in VR, such as Half-Life: Alyx. */
    @Synchronized
    fun onlyVr(client: File, appId: Int): Boolean {
        val file = File(client, "appcache/appinfo.vdf")
        val stamp = file.lastModified()
        if (file != cachedFile || stamp != cachedStamp) {
            cachedFile = file
            cachedStamp = stamp
            cachedValues = emptyMap()
        }
        cachedValues[appId]?.let { return it }
        val value = if (file.isFile) runCatching { read(file.readBytes(), appId, listOf("common", "onlyvrsupport")) }
            .getOrNull() == "1" else false
        cachedValues = cachedValues + (appId to value)
        return value
    }

    /** The value at [path] in [appId]'s KeyValues, or null when the app or key is absent. */
    internal fun read(bytes: ByteArray, appId: Int, path: List<String>): String? {
        val data = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        val magic = data.getInt(0)
        require(magic == V28 || magic == V29) { "Unknown appinfo format" }
        var position = 8
        val keys = if (magic == V29) {
            val table = data.getLong(position).toInt()
            position += 8
            val count = data.getInt(table)
            var at = table + 4
            List(count) {
                val end = terminator(bytes, at)
                String(bytes, at, end - at, Charsets.UTF_8).also { at = end + 1 }
            }
        } else null
        while (position + 8 <= bytes.size) {
            val id = data.getInt(position)
            if (id == 0) return null
            val size = data.getInt(position + 4)
            if (id == appId) return Reader(bytes, data, keys).find(position + 8 + APP_HEADER, path)
            position += 8 + size
        }
        return null
    }

    private fun terminator(bytes: ByteArray, from: Int): Int {
        var at = from
        while (bytes[at] != 0.toByte()) at++
        return at
    }

    private class Reader(private val bytes: ByteArray, private val data: ByteBuffer, private val keys: List<String>?) {
        private var at = 0

        fun find(start: Int, path: List<String>): String? {
            at = start
            // The app's tree is wrapped in one node named "appinfo".
            if (bytes[at].toInt() == 0) { at++; key() }
            return section(path, 0)
        }

        /** Walks one node; returns the value at [path] from [depth], skipping everything else. */
        private fun section(path: List<String>, depth: Int): String? {
            var found: String? = null
            while (true) {
                val type = bytes[at++].toInt()
                if (type == 8) return found
                val name = key()
                val wanted = found == null && depth < path.size && name.equals(path[depth], ignoreCase = true)
                when (type) {
                    // A node off the path is still walked, at a depth that never matches.
                    0 -> section(path, if (wanted) depth + 1 else path.size).let { if (wanted && it != null) found = it }
                    1 -> {
                        val end = terminator(bytes, at)
                        val value = String(bytes, at, end - at, Charsets.UTF_8)
                        at = end + 1
                        if (wanted && depth == path.size - 1) found = value
                    }
                    2 -> { val value = data.getInt(at); at += 4; if (wanted && depth == path.size - 1) found = value.toString() }
                    7 -> at += 8
                    else -> throw IllegalStateException("KeyValues type $type")
                }
            }
        }

        private fun key(): String = if (keys != null) {
            keys[data.getInt(at)].also { at += 4 }
        } else {
            val end = terminator(bytes, at)
            String(bytes, at, end - at, Charsets.UTF_8).also { at = end + 1 }
        }
    }
}
