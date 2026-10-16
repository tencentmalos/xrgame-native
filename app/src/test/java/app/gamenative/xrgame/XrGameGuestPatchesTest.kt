package app.gamenative.xrgame

import java.io.File
import java.nio.file.Files
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class XrGameGuestPatchesTest {
    private fun hook(name: String, offset: Long, size: Int, mode: String = "typed") = JSONObject()
        .put("name", name).put("offset", offset).put("expected", "90".repeat(size)).put("mode", mode)

    private fun pkg(id: String, title: String = "546560", module: String = "hlvr.exe", sha: String = "a".repeat(64),
                    hooks: List<JSONObject> = emptyList(), patches: List<JSONObject> = emptyList(),
                    bindings: List<JSONObject> = emptyList()) = JSONObject()
        .put("schema", "xrgame.guest-patch.v1").put("abi", "x86_64-win64").put("sdk_version", 1)
        .put("id", id).put("title", title).put("module", module).put("module_sha256", sha).put("name", "Name $id")
        .put("hooks", JSONArray(hooks)).put("patches", JSONArray(patches)).put("bindings", JSONArray(bindings))

    @Test fun selectionRoundTripsAndRejectsUnsafeNames() {
        val env = XrGameGuestPatches.withSelection("FOO=1", listOf("sixty_fps", "skip-intro"))
        assertEquals(listOf("sixty_fps", "skip-intro"), XrGameGuestPatches.selection(env))
        assertTrue(env.contains("FOO=1"))
        assertEquals(emptyList<String>(), XrGameGuestPatches.selection(XrGameGuestPatches.withSelection(env, emptyList())))
        for (bad in listOf("../x", "a/b", "a\\b", "c:x", "a,b", ".", "")) {
            assertFalse(bad, XrGameGuestPatches.validName(bad))
            assertThrows(IllegalArgumentException::class.java) { XrGameGuestPatches.withSelection("", listOf(bad)) }
        }
        assertThrows(IllegalArgumentException::class.java) { XrGameGuestPatches.withSelection("", listOf("a", "a")) }
        assertThrows(IllegalArgumentException::class.java) {
            XrGameGuestPatches.withSelection("", (1..9).map { "p$it" })
        }
    }

    @Test fun listingValidatesPackagesAndFindsConflictsOnTheSameBuild() {
        val dir = Files.createTempDirectory("guest-patches").toFile()
        try {
            File(dir, "a.json").writeText(pkg("a", hooks = listOf(hook("h", 0x1000, 7))).toString())
            // Overlaps a's hook: conflict.
            File(dir, "b.json").writeText(pkg("b", patches = listOf(JSONObject().put("name", "p")
                .put("offset", 0x1004).put("expected", "9090"))).toString())
            // Same range, other build: no conflict.
            File(dir, "c.json").writeText(pkg("c", sha = "b".repeat(64), hooks = listOf(hook("h", 0x1000, 7))).toString())
            // Calls a's hooked bytes through a function binding: conflict (only one side changes).
            File(dir, "d.json").writeText(pkg("d", patches = listOf(JSONObject().put("name", "p")
                .put("offset", 0x9000).put("expected", "90")), bindings = listOf(JSONObject().put("name", "f")
                .put("kind", "function").put("offset", 0x1002).put("expected", "9090909090"))).toString())
            File(dir, "other_game.json").writeText(pkg("o", title = "1", hooks = listOf(hook("h", 0, 5))).toString())
            File(dir, "broken.json").writeText("{")
            File(dir, "nothing.json").writeText(pkg("n").toString())
            val listing = XrGameGuestPatches.list(546560, dir)
            assertEquals(listOf("a", "b", "c", "d"), listing.packages.map { it.name })
            assertEquals(listOf("b", "d"), listing.packages.first { it.name == "a" }.conflicts)
            assertEquals(listOf("a", "d"), listing.packages.first { it.name == "b" }.conflicts)
            assertEquals(emptyList<String>(), listing.packages.first { it.name == "c" }.conflicts)
            assertEquals("Name a", listing.packages.first().displayName)
            assertEquals(3, listing.skipped.size)
            assertTrue(listing.skipped.any { it.startsWith("other_game.json: package is for app 1") })
        } finally {
            dir.deleteRecursively()
        }
    }

    @Test fun parseRejectsOtherFormatsAndUnsafeModules() {
        val good = pkg("a", hooks = listOf(hook("h", 0, 5)))
        XrGameGuestPatches.parse("a", good, 546560)
        for ((key, value) in listOf("schema" to "shadps4.guest-functions.v1", "abi" to "x86_64-sysv",
                                    "module" to "..\\hlvr.exe", "module_sha256" to "A".repeat(64), "id" to "1bad")) {
            assertThrows(key, Exception::class.java) {
                XrGameGuestPatches.parse("a", JSONObject(good.toString()).put(key, value), 546560)
            }
        }
    }

    @Test fun statusTextBecomesJsonWithPackagesAndRepeatedLines() {
        val json = XrGameGuestPatches.statusJson("""
            pid: 42
            packages: 1
            installed: 1
            package: probe_fixture
            installed: 1
            hook: add entry=0x1 enabled=0
            hook: read entry=0x2 enabled=1
            counter: 1 add_calls samples=1 last=2
        """.trimIndent())
        assertEquals("42", json.getString("pid"))
        val first = json.getJSONArray("packages").getJSONObject(0)
        assertEquals("probe_fixture", first.getString("package"))
        assertEquals(2, first.getJSONArray("hook").length())
        assertEquals("1 add_calls samples=1 last=2", first.getJSONArray("counter").getString(0))
    }
}
