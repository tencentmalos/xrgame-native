package app.gamenative.xrgame

import java.io.File
import java.nio.file.Files
import java.security.MessageDigest
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class XrGameBaseImageTest {
    @get:Rule val temp = TemporaryFolder()
    private val sha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    private fun record(name: String) = JSONObject().put("files",
        JSONObject().put(name, JSONObject().put("sha256", sha)))

    private fun publishRecord(root: File, record: JSONObject) {
        val bytes = record.toString().toByteArray()
        File(root, "xrgame-imagefs.json").writeBytes(bytes)
        File(root, ".xrgame-base-record-sha256").writeText(
            MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) })
        File(root, ".xrgame-base-sha256").writeText(sha)
    }

    @Test fun matchingReceiptDoesNotHideMissingOrCorruptMediaPlugin() {
        val root = temp.newFolder("imagefs")
        val stage = temp.newFolder("stage")
        val name = "usr/lib/gstreamer-1.0/libgstdebug.so"
        File(stage, name).apply { parentFile.mkdirs(); writeText("abc") }
        val input = record(name)
        publishRecord(root, input)
        assertFalse(XrGameBaseImage.isInstalled(root, sha))
        XrGameBaseImage.installFiles(stage, root, input)
        assertTrue(XrGameBaseImage.isInstalled(root, sha))
        File(root, name).writeText("bad") // Same length, different contents.
        assertFalse(XrGameBaseImage.isInstalled(root, sha))
        XrGameBaseImage.installFiles(stage, root, input)
        assertTrue(XrGameBaseImage.isInstalled(root, sha))
        File(root, name).delete()
        assertFalse(XrGameBaseImage.isInstalled(root, sha))
    }

    @Test fun legacyReceiptOrChangedManifestRequiresRevalidationFromArchive() {
        val root = temp.newFolder("imagefs")
        val name = "usr/lib/media.so"
        File(root, name).apply { parentFile.mkdirs(); writeText("abc") }
        val input = record(name)
        publishRecord(root, input)
        File(root, ".xrgame-base-record-sha256").delete()
        assertFalse(XrGameBaseImage.isInstalled(root, sha))
        publishRecord(root, input)
        assertTrue(XrGameBaseImage.isInstalled(root, sha))
        assertFalse(XrGameBaseImage.isInstalled(root, "0".repeat(64)))
        File(root, "xrgame-imagefs.json").writeText("{\"files\":{}}")
        assertFalse(XrGameBaseImage.isInstalled(root, sha))
    }

    @Test fun turnipOverridesAreNotMistakenForCorruptionButRetiredStubIs() {
        val root = temp.newFolder("imagefs")
        val name = "usr/lib/libxcb-dri3.so"
        File(root, name).apply { parentFile.mkdirs(); writeText("verified by Turnip installer") }
        val input = record(name).put("retiredFiles", JSONObject().put("usr/lib/libmediandk.so", sha))
        publishRecord(root, input)
        assertTrue(XrGameBaseImage.isInstalled(root, sha))
        File(root, "usr/lib/libmediandk.so").writeText("abc")
        assertFalse(XrGameBaseImage.isInstalled(root, sha))
        File(root, "usr/lib/libmediandk.so").delete()
        File(root, name).delete()
        assertFalse(XrGameBaseImage.isInstalled(root, sha))
    }

    @Test fun upgradeAddsMediaAndRetiresOnlyKnownStubWhilePreservingSaves() {
        val stage = temp.newFolder("stage")
        val root = temp.newFolder("imagefs")
        val name = "usr/lib/gstreamer-1.0/libgstlibav.so"
        File(stage, name).apply { parentFile.mkdirs(); writeText("abc") }
        val stub = File(root, "usr/lib/libmediandk.so").apply { parentFile.mkdirs(); writeText("abc") }
        val save = File(root, "home/game/save.dat").apply { parentFile.mkdirs(); writeText("keep save") }
        val custom = File(root, "usr/lib/custom.so").apply { writeText("keep custom") }
        val input = record(name).put("retiredFiles", JSONObject().put("usr/lib/libmediandk.so", sha))
        repeat(2) { XrGameBaseImage.installFiles(stage, root, input) }
        assertEquals("abc", File(root, name).readText())
        assertFalse(stub.exists())
        assertEquals("keep save", save.readText())
        assertEquals("keep custom", custom.readText())
    }

    @Test fun corruptInputOrModifiedRetiredFilePreventsAnyUpgrade() {
        val stage = temp.newFolder("stage")
        val root = temp.newFolder("imagefs")
        val name = "usr/lib/new.so"
        val source = File(stage, name).apply { parentFile.mkdirs(); writeText("corrupt") }
        assertThrows(IllegalStateException::class.java) { XrGameBaseImage.installFiles(stage, root, record(name)) }
        assertFalse(File(root, name).exists())
        source.writeText("abc")
        val stub = File(root, "usr/lib/libmediandk.so").apply { parentFile.mkdirs(); writeText("custom") }
        val input = record(name).put("retiredFiles", JSONObject().put("usr/lib/libmediandk.so", sha))
        assertThrows(IllegalStateException::class.java) { XrGameBaseImage.installFiles(stage, root, input) }
        assertEquals("custom", stub.readText())
        assertFalse(File(root, name).exists())
    }

    @Test fun upgradeRejectsHomePathsAndEscapingSymlinkParents() {
        val stage = temp.newFolder("stage")
        val root = temp.newFolder("imagefs")
        assertThrows(IllegalArgumentException::class.java) {
            XrGameBaseImage.installFiles(stage, root, record("home/game/save.dat"))
        }
        val outside = temp.newFolder("outside")
        File(root, "usr").mkdirs()
        Files.createSymbolicLink(File(root, "usr/lib").toPath(), outside.toPath())
        assertThrows(IllegalArgumentException::class.java) {
            XrGameBaseImage.installFiles(stage, root, record("usr/lib/new.so"))
        }
        assertTrue(outside.listFiles()!!.isEmpty())
    }
}
