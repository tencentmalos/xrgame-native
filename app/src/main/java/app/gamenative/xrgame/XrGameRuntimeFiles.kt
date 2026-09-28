package app.gamenative.xrgame

import java.io.File
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/** Repairs only runtime-owned files; prefix settings, saves and game files are never replaced. */
object XrGameRuntimeFiles {
    fun copyVerified(source: File, target: File, sha256: String): Boolean {
        check(XrGameComponents.verify(source, sha256)) { "Bundled runtime file is corrupt: ${source.name}" }
        if (XrGameComponents.verify(target, sha256)) return false
        check(target.parentFile!!.mkdirs() || target.parentFile!!.isDirectory)
        val partial = File.createTempFile(".xrgame-", ".part", target.parentFile)
        try {
            source.inputStream().use { input -> partial.outputStream().use { out -> input.copyTo(out); out.fd.sync() } }
            check(XrGameComponents.verify(partial, sha256)) { "Runtime copy failed: ${source.name}" }
            Files.move(partial.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
            check(XrGameComponents.verify(target, sha256)) { "Runtime installation failed: ${source.name}" }
        } finally { partial.delete() }
        return true
    }

    /** A deterministic backup also recovers interruption between moving the old tree and installing. */
    internal suspend fun ensureInstalled(directory: File, verify: () -> Boolean,
                                         beforeRepair: () -> Unit, install: suspend () -> Unit) {
        val backup = File(directory.parentFile, ".${directory.name}.xrgame-repair")
        if (verify()) {
            if (backup.exists()) check(backup.deleteRecursively())
            return
        }
        beforeRepair()
        if (backup.exists()) {
            check(!directory.exists() || directory.deleteRecursively())
            check(backup.renameTo(directory)) { "Cannot recover interrupted runtime repair" }
            if (verify()) return
        }
        val hadPrevious = directory.exists()
        if (hadPrevious) check(directory.renameTo(backup)) { "Cannot prepare runtime repair" }
        try {
            install()
            check(verify()) { "Repaired runtime failed verification: ${directory.name}" }
        } catch (failure: Throwable) {
            check(!directory.exists() || directory.deleteRecursively()) { "Cannot remove incomplete runtime installation" }
            if (hadPrevious) check(backup.renameTo(directory)) { "Cannot restore previous runtime installation" }
            throw failure
        }
        check(!backup.exists() || backup.deleteRecursively())
    }
}
