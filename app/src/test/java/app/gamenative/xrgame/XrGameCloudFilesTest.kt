package app.gamenative.xrgame

import java.io.IOException
import java.nio.file.Files
import java.security.MessageDigest
import org.junit.Assert.*
import org.junit.Test

class XrGameCloudFilesTest {
    @Test fun incompleteOrCorruptCloudDataNeverReplacesAnExistingSave() {
        val root = Files.createTempDirectory("xrgame-cloud")
        try {
            val save = root.resolve("SAVEDATA1000")
            val staged = root.resolve("download.part")
            val good = "cloud save".toByteArray()
            val sha = MessageDigest.getInstance("SHA-1").digest(good)
            Files.write(save, "existing save".toByteArray())
            for (bytes in listOf(good.copyOf(3), "bad!! save".toByteArray())) {
                Files.write(staged, bytes)
                assertThrows(IOException::class.java) { XrGameCloudFiles.commit(staged, save, good.size.toLong(), sha) }
                assertEquals("existing save", Files.readAllBytes(save).toString(Charsets.UTF_8))
            }
            Files.write(staged, good)
            assertThrows(IOException::class.java) { XrGameCloudFiles.commit(staged, save, good.size.toLong(), ByteArray(0)) }
            assertEquals("existing save", Files.readAllBytes(save).toString(Charsets.UTF_8))
            XrGameCloudFiles.commit(staged, save, good.size.toLong(), sha)
            assertArrayEquals(good, Files.readAllBytes(save))
            assertFalse(Files.exists(staged))
        } finally { root.toFile().deleteRecursively() }
    }
}
