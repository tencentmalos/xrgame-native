package app.gamenative.xrgame

import android.content.Context
import app.gamenative.utils.ManifestData
import app.gamenative.utils.ManifestEntry
import app.gamenative.utils.ManifestRepository
import java.io.File
import java.io.IOException
import java.net.URI
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.MessageDigest
import java.util.concurrent.TimeUnit
import okhttp3.OkHttpClient
import okhttp3.Request

/** The APK carries the component catalog; downloads never switch catalogs or fall back upstream. */
object XrGameComponents {
    const val ASSET = "xrgame/manifest.json"
    private val hashPattern = Regex("[0-9a-f]{64}")
    private val client by lazy {
        OkHttpClient.Builder().proxySelector(XrGameEgress.componentProxySelector())
            .followRedirects(false).followSslRedirects(false)
            .connectTimeout(30, TimeUnit.SECONDS).readTimeout(120, TimeUnit.SECONDS).build()
    }

    fun load(context: Context): ManifestData {
        val text = context.assets.open(ASSET).bufferedReader().use { it.readText() }
        val manifest = ManifestRepository.parseManifest(text) ?: throw IOException("Invalid XRGame component catalog")
        val ids = mutableSetOf<String>()
        val urls = mutableSetOf<String>()
        for (entry in manifest.items.values.flatten()) {
            validate(entry)
            require(ids.add(entry.id)) { "Duplicate component id: ${entry.id}" }
            require(urls.add(entry.url)) { "Duplicate component URL: ${entry.url}" }
        }
        return manifest
    }

    fun validate(entry: ManifestEntry) {
        val uri = URI(entry.url)
        require(uri.scheme == "https" && uri.host == "github.com" && uri.userInfo == null && uri.port == -1 &&
            uri.rawQuery == null && uri.rawFragment == null &&
            uri.rawPath.matches(Regex("/tencentmalos/xrgame-native/releases/download/[A-Za-z0-9._-]+/[A-Za-z0-9._-]+"))) {
            "Component must use an XRGame release URL: ${entry.id}"
        }
        require(entry.sha256?.matches(hashPattern) == true) { "Missing SHA-256: ${entry.id}" }
        require(!entry.license.isNullOrBlank() && !entry.source.isNullOrBlank()) { "Missing component provenance: ${entry.id}" }
    }

    fun verify(file: File, sha256: String): Boolean {
        if (!file.isFile || !sha256.matches(hashPattern)) return false
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().buffered().use { input ->
            val bytes = ByteArray(64 * 1024)
            while (true) {
                val size = input.read(bytes)
                if (size < 0) break
                digest.update(bytes, 0, size)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) } == sha256
    }

    fun byFileName(context: Context, fileName: String): ManifestEntry =
        load(context).items.values.flatten().singleOrNull { URI(it.url).path.substringAfterLast('/') == fileName }
            ?: throw IOException("Component is not in the XRGame catalog: $fileName")

    @JvmStatic
    fun requireVerified(context: Context, fileName: String, file: File) {
        val entry = byFileName(context, fileName)
        if (!verify(file, requireNotNull(entry.sha256))) {
            throw IOException("Missing or corrupt XRGame component: $fileName")
        }
    }

    fun download(context: Context, entry: ManifestEntry, dest: File, onProgress: (Float) -> Unit = {}) {
        validate(entry)
        require(load(context).items.values.flatten().contains(entry)) { "Component is not in this APK's catalog" }
        val sha = requireNotNull(entry.sha256)
        if (verify(dest, sha)) { onProgress(1f); return }
        dest.parentFile?.mkdirs()
        val partial = File.createTempFile("xrgame-component-", ".part", dest.parentFile)
        try {
            val fileName = URI(entry.url).path.substringAfterLast('/')
            if (context.assets.list("xrgame/components")?.contains(fileName) == true) {
                context.assets.open("xrgame/components/$fileName").use { input ->
                    partial.outputStream().use { output ->
                        input.copyTo(output)
                        output.fd.sync()
                    }
                }
                if (!verify(partial, sha)) throw IOException("Bundled component SHA-256 mismatch: ${entry.id}")
                Files.move(partial.toPath(), dest.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
                android.util.Log.i("XrGameComponents", "Installed bundled archive: ${entry.id}")
                onProgress(1f)
                return
            }
            // Internal debug validation can supply the same catalog-pinned release
            // bytes over ADB before publication. It cannot substitute a catalog.
            val local = File(context.filesDir, "xrgame/components/${URI(entry.url).path.substringAfterLast('/')}")
            if (app.gamenative.BuildConfig.DEBUG && verify(local, sha)) {
                local.copyTo(partial, overwrite = true)
                if (!verify(partial, sha)) throw IOException("Local component changed during copy: ${entry.id}")
                Files.move(partial.toPath(), dest.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
                onProgress(1f)
                return
            }
            var url = entry.url
            repeat(5) {
                client.newCall(Request.Builder().url(url).build()).execute().use { response ->
                    if (response.code in listOf(301, 302, 303, 307, 308)) {
                        val next = response.header("Location")?.let { URI(url).resolve(it) }
                            ?: throw IOException("Component redirect has no Location")
                        require(next.scheme == "https" && next.userInfo == null && next.port == -1 &&
                            next.host in setOf("release-assets.githubusercontent.com", "objects.githubusercontent.com")) {
                            "Unexpected component redirect origin"
                        }
                        url = next.toString()
                    } else {
                        if (!response.isSuccessful) throw IOException("Component download failed: HTTP ${response.code}")
                        val body = response.body ?: throw IOException("Empty component response")
                        val length = body.contentLength()
                        body.byteStream().use { input ->
                            partial.outputStream().use { out ->
                                val buffer = ByteArray(64 * 1024)
                                var total = 0L
                                while (true) {
                                    val count = input.read(buffer)
                                    if (count < 0) break
                                    out.write(buffer, 0, count)
                                    total += count
                                    if (length > 0) onProgress((total.toFloat() / length).coerceAtMost(0.99f))
                                }
                                out.fd.sync()
                            }
                        }
                        if (!verify(partial, sha)) throw IOException("Component SHA-256 mismatch: ${entry.id}")
                        Files.move(partial.toPath(), dest.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
                        onProgress(1f)
                        return
                    }
                }
            }
            throw IOException("Too many component redirects")
        } finally {
            partial.delete()
        }
    }
}
