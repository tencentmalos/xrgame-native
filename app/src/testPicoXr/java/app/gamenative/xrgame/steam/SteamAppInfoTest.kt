// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class SteamAppInfoTest {
    private val keys = listOf("appinfo", "common", "name", "onlyvrsupport", "extended", "openvrsupport")

    private sealed interface Node
    private data class Section(val name: String, val children: List<Node>) : Node
    private data class Value(val name: String, val value: String) : Node

    private fun ByteArrayOutputStream.int(value: Int) =
        write(ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(value).array())

    private fun ByteArrayOutputStream.node(node: Node) {
        when (node) {
            is Section -> { write(0); int(keys.indexOf(node.name)); node.children.forEach { node(it) }; write(8) }
            is Value -> { write(1); int(keys.indexOf(node.name)); write(node.value.toByteArray()); write(0) }
        }
    }

    /** A v29 appinfo.vdf with [apps] and the key table at the end. */
    private fun appinfo(apps: Map<Int, Section>): ByteArray {
        val body = ByteArrayOutputStream()
        for ((id, tree) in apps) {
            val kv = ByteArrayOutputStream().apply { node(tree); write(8) }.toByteArray()
            body.int(id)
            body.int(4 + 4 + 8 + 20 + 4 + 20 + kv.size)
            body.write(ByteArray(4 + 4 + 8 + 20 + 4 + 20))
            body.write(kv)
        }
        body.int(0)
        val table = 16 + body.size()
        return ByteArrayOutputStream().apply {
            int(0x07564429); int(1)
            write(ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(table.toLong()).array())
            write(body.toByteArray())
            int(keys.size)
            keys.forEach { write(it.toByteArray()); write(0) }
        }.toByteArray()
    }

    private val file = appinfo(mapOf(
        // An ordinary game whose extended section happens to carry the key.
        1145350 to Section("appinfo", listOf(
            Section("common", listOf(Value("name", "Hades II"))),
            Section("extended", listOf(Value("onlyvrsupport", "1"))),
        )),
        546560 to Section("appinfo", listOf(
            Section("common", listOf(Value("name", "Half-Life: Alyx"), Value("openvrsupport", "1"), Value("onlyvrsupport", "1"))),
        )),
    ))

    @Test fun `a VR-only game reads its common flag`() {
        assertEquals("1", SteamAppInfo.read(file, 546560, listOf("common", "onlyvrsupport")))
    }

    @Test fun `the same key elsewhere in the tree does not count`() {
        assertNull(SteamAppInfo.read(file, 1145350, listOf("common", "onlyvrsupport")))
        assertEquals("Hades II", SteamAppInfo.read(file, 1145350, listOf("common", "name")))
    }

    @Test fun `an app missing from the cache has no value`() {
        assertNull(SteamAppInfo.read(file, 3444020, listOf("common", "onlyvrsupport")))
    }
}
