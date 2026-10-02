package app.gamenative.xrgame

import java.io.IOException
import okhttp3.OkHttpClient
import okhttp3.Request
import timber.log.Timber

/** Preserve the local save and report an unsuccessful transfer to Steam's upload commit. */
object XrGameCloudTransfers {
    fun uploadBlock(client: OkHttpClient, request: Request): Boolean = try {
        client.newCall(request).execute().use { it.isSuccessful }
    } catch (error: IOException) {
        // Signed storage URLs and request headers must not enter diagnostics.
        Timber.w("Steam Cloud upload transport failed: %s", error.javaClass.simpleName)
        false
    }
}
