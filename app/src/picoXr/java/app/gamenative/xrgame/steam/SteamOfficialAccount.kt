// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import `in`.dragonbra.javasteam.types.KeyValue
import java.io.File

/** Identity only, from the official client's own login record. Never copies authentication tokens. */
object SteamOfficialAccount {
    fun read(client: File): Long? {
        val file = File(client, "config/loginusers.vdf")
        if (!file.isFile || file.length() > 1024 * 1024) return null
        return select(file.readText())
    }
    internal fun select(text: String): Long? {
        val users = KeyValue.loadFromString(text)?.takeIf { it.name.equals("users", true) } ?: return null
        val records = users.children.filter { it.name?.toLongOrNull()?.let { id -> id > 76561197960265728L } == true }
        val recent = records.filter { it["MostRecent"].value == "1" }
        return (recent.singleOrNull() ?: records.singleOrNull())?.name?.toLongOrNull()
    }
}
