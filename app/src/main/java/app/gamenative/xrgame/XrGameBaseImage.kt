package app.gamenative.xrgame

import android.content.Context
import com.winlator.core.ProcessHelper
import com.winlator.core.TarCompressorUtils
import com.winlator.xenvironment.ImageFs
import org.json.JSONObject
import java.io.File
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.MessageDigest

/** Upgrade runtime-owned base files without replacing homes, prefixes or saves. */
object XrGameBaseImage {
    fun ensure(context: Context, archive: File, archiveSha: String) {
        val root = ImageFs.find(context).rootDir
        // Initial image creation still belongs to ImageFsInstaller.
        if (!File(root, "usr").isDirectory) return
        val receipt = File(root, ".xrgame-base-sha256")
        if (receipt.isFile && receipt.readText().trim() == archiveSha) return
        check(ProcessHelper.listRunningWineProcesses().isEmpty()) { "Close the game before updating media libraries" }
        check(XrGameComponents.verify(archive, archiveSha)) { "Base runtime archive is corrupt" }
        val stage = File(context.cacheDir, "xrgame-base-upgrade")
        check(!stage.exists() || stage.deleteRecursively())
        check(stage.mkdirs())
        try {
            check(TarCompressorUtils.extract(TarCompressorUtils.Type.XZ, archive, stage)) { "Cannot extract base runtime" }
            val sourceRecord = File(stage, "xrgame-imagefs.json")
            val record = JSONObject(sourceRecord.readText())
            installFiles(stage, root, record)
            val recordSha = MessageDigest.getInstance("SHA-256").digest(sourceRecord.readBytes()).joinToString("") { "%02x".format(it) }
            XrGameRuntimeFiles.copyVerified(sourceRecord, File(root, sourceRecord.name), recordSha)
            val temporary = File(root, ".xrgame-base-sha256.part")
            temporary.writeText(archiveSha + "\n")
            Files.move(temporary.toPath(), receipt.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
        } finally {
            stage.deleteRecursively()
        }
    }

    internal fun installFiles(stage: File, root: File, record: JSONObject) {
        fun destination(name: String): File {
            require(name.startsWith("usr/lib/") || name.startsWith("usr/share/") || name.startsWith("usr/etc/fonts/"))
            require(name.split('/').none { it == ".." || it == "." })
            val file = File(root, name)
            require(!Files.isSymbolicLink(file.toPath()) && file.canonicalFile.toPath().startsWith(root.canonicalFile.toPath()))
            return file
        }
        val files = record.getJSONObject("files")
        // Validate the complete input before updating any live file.
        for (name in files.keys()) {
            destination(name)
            val source = File(stage, name)
            require(source.canonicalFile.toPath().startsWith(stage.canonicalFile.toPath()))
            check(XrGameComponents.verify(source, files.getJSONObject(name).getString("sha256")))
        }
        val retired = record.optJSONObject("retiredFiles") ?: JSONObject()
        for (name in retired.keys()) {
            val target = destination(name)
            check(!target.exists() || XrGameComponents.verify(target, retired.getString(name))) {
                "Cannot retire modified runtime file: $name"
            }
        }
        for (name in files.keys()) {
            val source = File(stage, name)
            val target = destination(name)
            XrGameRuntimeFiles.copyVerified(source, target, files.getJSONObject(name).getString("sha256"))
            if (source.canExecute()) check(target.setExecutable(true, true))
        }
        for (name in retired.keys()) {
            val target = destination(name)
            check(!target.exists() || target.delete())
        }
    }
}
