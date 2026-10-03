package app.gamenative.xrgame

import android.content.Context
import androidx.annotation.Keep
import java.io.File
import java.security.MessageDigest
import java.util.concurrent.atomic.AtomicBoolean

/** The only SDK owner is libxrgame_debugbus in the Android host, never the Wine child. */
@Keep
class LitepProfiler(context: Context) : XrGameProfiler.Backend {
    companion object { init { System.loadLibrary("xrgame_debugbus") } }
    init {
        val catalog = context.assets.open(XrGameComponents.ASSET).use { it.readBytes() }
        val sha = MessageDigest.getInstance("SHA-256").digest(catalog).joinToString("") { "%02x".format(it) }
        initialize(File(context.filesDir, "xrgame/profiles").absolutePath, sha)
    }
    private external fun initialize(directory: String, catalogSha: String)
    private external fun begin(name: String): LongArray?
    private external fun end(token: LongArray)
    private external fun bookmark(name: String)
    private external fun frame()
    override external fun counter(id: Int, value: Long)
    override fun region(name: String): XrGameProfiler.Region {
        val token = begin(name) ?: return XrGameProfiler.noop()
        val closed = AtomicBoolean()
        return XrGameProfiler.Region { if (closed.compareAndSet(false, true)) end(token) }
    }
    override fun mark(name: String) = bookmark(name)
    override fun present() = frame()
}
