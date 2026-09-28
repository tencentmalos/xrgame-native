package app.gamenative.xrgame

import java.io.File
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class XrGameRuntimeFilesTest {
    @get:Rule val temp = TemporaryFolder()
    private val sha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"

    @Test fun missingAndCorruptDllsAreRepairedAndInvalidSourcesLeaveExistingFileIntact() {
        val source = temp.newFile("bundled.dll").apply { writeText("abc") }
        val target = File(temp.root, "prefix/d3d11.dll")
        assertTrue(XrGameRuntimeFiles.copyVerified(source, target, sha))
        assertFalse(XrGameRuntimeFiles.copyVerified(source, target, sha))
        target.writeText("damaged")
        assertTrue(XrGameRuntimeFiles.copyVerified(source, target, sha))
        source.writeText("bad")
        assertThrows(IllegalStateException::class.java) { XrGameRuntimeFiles.copyVerified(source, target, sha) }
        assertEquals("abc", target.readText())
    }

    @Test fun corruptInstallationIsReplacedAndFailedRepairRollsBack() = runBlocking {
        val directory = temp.newFolder("runtime")
        val dll = File(directory, "runtime.dll").apply { writeText("old") }
        var repairs = 0
        val verify = { dll.isFile && dll.readText() == "abc" }
        try {
            XrGameRuntimeFiles.ensureInstalled(directory, verify, { repairs++ }) {
                directory.mkdirs(); dll.writeText("partial"); error("interrupted install")
            }
            fail("Expected failure")
        } catch (_: IllegalStateException) { }
        assertEquals("old", dll.readText())
        XrGameRuntimeFiles.ensureInstalled(directory, verify, { repairs++ }) {
            directory.mkdirs(); dll.writeText("abc")
        }
        XrGameRuntimeFiles.ensureInstalled(directory, verify, { error("Must not repair verified runtime") }) {
            error("Must not reinstall verified runtime")
        }
        assertEquals(2, repairs)
        assertFalse(File(temp.root, ".runtime.xrgame-repair").exists())
    }

    @Test fun processDeathBackupIsRecoveredBeforeAnotherInstall() = runBlocking {
        val directory = File(temp.root, "runtime")
        val backup = File(temp.root, ".runtime.xrgame-repair").apply { mkdirs() }
        File(backup, "runtime.dll").writeText("abc")
        directory.mkdirs(); File(directory, "runtime.dll").writeText("partial")
        XrGameRuntimeFiles.ensureInstalled(directory,
            { File(directory, "runtime.dll").readText() == "abc" }, {}) { error("Recovered runtime must be reused") }
        assertEquals("abc", File(directory, "runtime.dll").readText())
        assertFalse(backup.exists())
    }

    @Test fun activeGameBlocksRepairBeforeFilesMove() = runBlocking {
        val directory = temp.newFolder("runtime")
        File(directory, "runtime.dll").writeText("keep")
        try {
            XrGameRuntimeFiles.ensureInstalled(directory, { false }, { error("Game is running") }) { fail("Must not install") }
            fail("Expected failure")
        } catch (_: IllegalStateException) { }
        assertEquals("keep", File(directory, "runtime.dll").readText())
    }
}
