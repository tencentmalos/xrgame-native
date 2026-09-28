package app.gamenative.xrgame

import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import java.io.File
import java.nio.file.Files

@RunWith(RobolectricTestRunner::class)
class XrGameInstalledComponentsTest {
    @get:Rule val temp = TemporaryFolder()
    private val digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    private fun record(name: String) = JSONObject().put("files", JSONObject()
        .put(name, JSONObject().put("sha256", digest)))

    @Test fun changedAndMissingInstalledFilesAreRejected() {
        val root = temp.newFolder("runtime")
        val dll = File(root, "runtime.dll").apply { writeText("abc") }
        assertTrue(XrGameInstalledComponents.verifyTree(root, record(dll.name)))
        dll.writeText("abd")
        assertFalse(XrGameInstalledComponents.verifyTree(root, record(dll.name)))
        dll.delete()
        assertFalse(XrGameInstalledComponents.verifyTree(root, record(dll.name)))
    }

    @Test fun relativeLoadersAreAcceptedButEscapingLinksAreRejected() {
        val root = temp.newFolder("runtime")
        File(root, "wine").writeText("abc")
        val link = File(root, "loader").toPath()
        Files.createSymbolicLink(link, File("wine").toPath())
        val record = record("wine")
        record.getJSONObject("files").put("loader", JSONObject().put("symlink", "wine"))
        assertTrue(XrGameInstalledComponents.verifyTree(root, record))
        File(temp.root, "external").writeText("abc")
        Files.delete(link)
        Files.createSymbolicLink(link, File("../external").toPath())
        record.getJSONObject("files").put("loader", JSONObject().put("symlink", "../external"))
        assertFalse(XrGameInstalledComponents.verifyTree(root, record))
        assertFalse(XrGameInstalledComponents.verifyTree(root, record("../external")))
    }
}
