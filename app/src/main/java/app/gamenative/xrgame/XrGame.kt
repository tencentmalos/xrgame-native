package app.gamenative.xrgame

import android.app.Application
import app.gamenative.BuildConfig

/**
 * Entry point for XRGame Native (picoXr) behavior that lives in the shared source set.
 * Everything here is switched by [BuildConfig.XRGAME], which only the picoXr flavor sets, so
 * upstream flavors behave exactly as before. Spec: docs/specs/xrgame-native-v1.md.
 */
object XrGame {
    val enabled: Boolean
        get() = BuildConfig.XRGAME

    /** Called once from PluviaApp.onCreate, before any network client is created. */
    fun install(app: Application) {
        if (!enabled) return
        XrGameEgress.install()
        XrGameStorage.install(app)
    }
}
