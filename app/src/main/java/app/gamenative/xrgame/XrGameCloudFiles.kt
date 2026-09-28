package app.gamenative.xrgame

import java.io.IOException
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardCopyOption
import java.security.MessageDigest

/** Commit a complete Steam Cloud download without exposing a truncated save to the game. */
object XrGameCloudFiles {
    fun isStagingFile(path: Path): Boolean = path.fileName.toString().let {
        it.startsWith(".xrgame-cloud-") && it.endsWith(".part")
    }

    fun commit(staged: Path, destination: Path, expectedSize: Long, expectedSha: ByteArray) {
        if (expectedSha.size != 20 || Files.size(staged) != expectedSize) {
            throw IOException("Steam Cloud save size or SHA metadata is invalid")
        }
        val digest = MessageDigest.getInstance("SHA-1")
        Files.newInputStream(staged).buffered().use { input ->
            val buffer = ByteArray(65536)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
        }
        if (!MessageDigest.isEqual(digest.digest(), expectedSha)) {
            throw IOException("Steam Cloud save SHA does not match the CM file list")
        }
        Files.move(staged, destination, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
    }
}
