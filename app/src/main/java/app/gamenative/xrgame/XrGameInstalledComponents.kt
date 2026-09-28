package app.gamenative.xrgame

import android.content.Context
import app.gamenative.utils.ManifestEntry
import org.json.JSONObject
import java.io.File
import java.nio.file.Files
import java.security.MessageDigest

/** Receipts bind extracted file lists to archives verified against this APK's catalog. */
object XrGameInstalledComponents {
    private fun receipt(context: Context, entry: ManifestEntry): File {
        require(entry.id.matches(Regex("[A-Za-z0-9._-]+")))
        return File(context.filesDir, "xrgame/installed/${entry.id}.json")
    }

    fun record(context: Context, entry: ManifestEntry, directory: File) {
        require(XrGameComponents.load(context).items.values.flatten().contains(entry))
        val record = File(directory, "xrgame-build.json")
        require(verifyTree(directory, JSONObject(record.readText()))) { "Incomplete XRGame installation: ${entry.id}" }
        val hash = MessageDigest.getInstance("SHA-256").digest(record.readBytes()).joinToString("") { "%02x".format(it) }
        val dest = receipt(context, entry)
        dest.parentFile?.mkdirs()
        dest.writeText(JSONObject().put("archiveSha256", entry.sha256).put("recordSha256", hash).toString())
    }

    fun verify(context: Context, entry: ManifestEntry, directory: File): Boolean = runCatching {
        val saved = JSONObject(receipt(context, entry).readText())
        val record = File(directory, "xrgame-build.json")
        saved.getString("archiveSha256") == entry.sha256 &&
            XrGameComponents.verify(record, saved.getString("recordSha256")) &&
            verifyTree(directory, JSONObject(record.readText()))
    }.getOrDefault(false)

    internal fun verifyTree(directory: File, record: JSONObject): Boolean = runCatching {
        val root = directory.canonicalFile.toPath()
        val files = record.getJSONObject("files")
        require(files.length() > 0)
        files.keys().asSequence().all { relative ->
            val path = File(directory, relative)
            val details = files.getJSONObject(relative)
            val lexical = path.toPath().normalize()
            lexical.startsWith(directory.toPath().normalize()) && path.canonicalFile.toPath().startsWith(root) &&
                if (details.has("symlink")) {
                    Files.isSymbolicLink(path.toPath()) && path.exists() &&
                        Files.readSymbolicLink(path.toPath()).toString() == details.getString("symlink")
                } else {
                    !Files.isSymbolicLink(path.toPath()) && XrGameComponents.verify(path, details.getString("sha256"))
                }
        }
    }.getOrDefault(false)
}
