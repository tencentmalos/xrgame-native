package app.gamenative.ui.screen.xr

import android.content.Context
import java.io.File
import java.util.Locale
import timber.log.Timber

/**
 * The XR keyboard's pinyin: AOSP PinyinIME's system dictionary, unpacked from the APK once per
 * install (the engine opens a path), and a user dictionary next to it that learns chosen words.
 * The keyboard opens in pinyin when the device speaks Chinese. Builds without the dictionary keep
 * a Latin keyboard.
 */
object XrPinyinDictionary {
    private const val ASSET = "pinyin/dict_pinyin.dat"
    @Volatile private var prepared = false

    /** IO thread; once per process. */
    fun prepare(context: Context) {
        if (prepared) return
        prepared = true
        val dir = File(context.filesDir, "pinyin")
        val system = File(dir, "dict_pinyin.dat")
        val installed = runCatching { context.packageManager.getPackageInfo(context.packageName, 0).lastUpdateTime }.getOrDefault(0L)
        val ready = runCatching {
            if (!system.isFile || system.lastModified() < installed) {
                dir.mkdirs()
                val partial = File(dir, "dict_pinyin.dat.partial")
                context.assets.open(ASSET).use { input -> partial.outputStream().use { input.copyTo(it) } }
                check(partial.renameTo(system)) { "rename failed" }
            }
            true
        }.onFailure { Timber.i("XR keyboard: no pinyin dictionary (%s)", it.javaClass.simpleName) }.getOrDefault(false)
        val chinese = Locale.getDefault().language == "zh"
        XrNative.nativeSetPinyin(
            if (ready) system.path else null,
            File(dir, "user_dict.dat").path,
            chinese,
        )
    }
}
