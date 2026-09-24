package app.gamenative.xrgame

import android.app.Activity
import android.app.AlertDialog
import android.app.Application
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import app.gamenative.MainActivity
import app.gamenative.PrefManager
import app.gamenative.R
import java.io.File
import timber.log.Timber

/**
 * XRGame Native (picoXr) install root on shared external storage (spec WP1-3, WP1-9).
 *
 * Games go to `<primary external storage>/XRGameNative/Steam/steamapps/common`,
 * which survives uninstalling, reinstalling or re-signing the app. That needs "All files access"
 * (MANAGE_EXTERNAL_STORAGE, declared only in the picoXr manifest).
 *
 * On the first MainActivity resume after each install this shows the unofficial-client notice,
 * then asks for All files access. Once access is granted it creates the root and points
 * [PrefManager.useExternalStorage] / [PrefManager.externalStoragePath] at it. After that the
 * user's own storage choice is left alone. Installed by [XrGame.install]; picoXr only.
 */
object XrGameStorage {
    const val ROOT_DIR_NAME = "XRGameNative"
    private const val PREFS = "xrgame"
    private const val KEY_NOTICE_ACCEPTED = "unofficial_client_notice_v1"
    private const val KEY_DEFAULTS_APPLIED = "install_root_defaults_v1"

    /** `<primary external storage>/XRGameNative`, or null for upstream flavors. */
    val installRoot: File?
        get() = if (XrGame.enabled) File(Environment.getExternalStorageDirectory(), ROOT_DIR_NAME) else null

    fun hasAllFilesAccess(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager()

    private var accessRequestedThisProcess = false

    fun install(app: Application) {
        if (!XrGame.enabled) return
        app.registerActivityLifecycleCallbacks(
            object : Application.ActivityLifecycleCallbacks {
                override fun onActivityResumed(activity: Activity) {
                    if (activity is MainActivity) onMainActivityResumed(activity)
                }

                override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) = Unit
                override fun onActivityStarted(activity: Activity) = Unit
                override fun onActivityPaused(activity: Activity) = Unit
                override fun onActivityStopped(activity: Activity) = Unit
                override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) = Unit
                override fun onActivityDestroyed(activity: Activity) = Unit
            },
        )
    }

    private fun onMainActivityResumed(activity: Activity) {
        val prefs = activity.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        when {
            !prefs.getBoolean(KEY_NOTICE_ACCEPTED, false) -> showNotice(activity) {
                prefs.edit().putBoolean(KEY_NOTICE_ACCEPTED, true).apply()
                if (hasAllFilesAccess()) applyDefaults(activity) else requestAllFilesAccess(activity)
            }
            !hasAllFilesAccess() -> if (!accessRequestedThisProcess) requestAllFilesAccess(activity)
            else -> applyDefaults(activity)
        }
    }

    private fun showNotice(activity: Activity, onAccept: () -> Unit) {
        AlertDialog.Builder(activity)
            .setTitle(R.string.xrgame_notice_title)
            .setMessage(activity.getString(R.string.xrgame_notice_message, installRoot?.absolutePath))
            .setCancelable(false)
            .setPositiveButton(R.string.xrgame_notice_accept) { _, _ -> onAccept() }
            .show()
    }

    private fun requestAllFilesAccess(activity: Activity) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
        accessRequestedThisProcess = true
        Timber.i("[XrGameStorage] requesting All files access")
        val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)
            .setData(Uri.parse("package:${activity.packageName}"))
        runCatching { activity.startActivity(intent) }
            .onFailure { activity.startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)) }
    }

    /** Creates the install root and, once per install, points the Steam install path at it. */
    private fun applyDefaults(context: Context) {
        val root = installRoot ?: return
        val common = File(root, "Steam/steamapps/common")
        if (!common.isDirectory && !common.mkdirs()) {
            Timber.w("[XrGameStorage] cannot create %s", common)
            return
        }
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        if (prefs.getBoolean(KEY_DEFAULTS_APPLIED, false)) return
        PrefManager.useExternalStorage = true // clears externalStoragePath, so set it afterwards
        PrefManager.externalStoragePath = root.absolutePath
        prefs.edit().putBoolean(KEY_DEFAULTS_APPLIED, true).apply()
        Timber.i("[XrGameStorage] install root set to %s", root.absolutePath)
    }
}
