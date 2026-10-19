// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import android.content.Context
import androidx.annotation.Keep
import app.gamenative.PrefManager
import app.gamenative.data.AppInfo
import app.gamenative.db.PluviaDatabase
import app.gamenative.service.SteamService
import app.gamenative.xrgame.XrGameOfficialSteam
import app.gamenative.xrgame.XrGameStorage
import `in`.dragonbra.javasteam.types.KeyValue
import java.io.File
import org.json.JSONObject
import java.nio.file.Files
import app.gamenative.xrgame.steam.SteamLibraryManifest.child
import app.gamenative.xrgame.steam.SteamLibraryManifest.put

/** Shared files, with one ACF per installation and no changes to the per-app Wine container. */
class LinuxSteamLibrary(private val context: Context, private val db: PluviaDatabase) {
    companion object {
        fun sharedRoot(): File = File(checkNotNull(XrGameStorage.installRoot), "Steam").canonicalFile
        fun roots(context: Context): List<File> = listOf(sharedRoot())

        /** One physical steamapps for both clients, including manifests, staging and downloads. */
        internal fun linkLibrary(official: File, shared: File) {
            check(shared.isDirectory || shared.mkdirs())
            if (Files.isSymbolicLink(official.toPath())) {
                check(official.canonicalFile == shared.canonicalFile) { "Official Steam points to another library; existing data was preserved." }
                return
            }
            // Do not relocate a populated, older private library implicitly. The current
            // candidate has none; this guard preserves unexpected installations for review.
            check(official.listFiles().orEmpty().none { it.name.startsWith("appmanifest_") } &&
                listOf("common", "downloading", "compatdata", "shadercache", "sourcemods").all {
                    File(official, it).listFiles().isNullOrEmpty()
                } && official.listFiles().orEmpty().all {
                    it.name == "libraryfolders.vdf" || it.name == "libraryfolders.vdf.steampsp-before-sharing" ||
                        (it.isDirectory && it.listFiles().isNullOrEmpty())
                }) { "Official Steam already has private game data. Move it to the shared library before enabling the common directory; no files were replaced." }
            val backup = File(official.parentFile, "steamapps.before-shared-directory")
            if (official.exists()) {
                check(!backup.exists()) { "A previous library backup needs review." }
                check(official.renameTo(backup))
            }
            try { Files.createSymbolicLink(official.toPath(), shared.canonicalFile.toPath()) }
            catch (error: Exception) { if (backup.exists()) backup.renameTo(official); throw error }
        }

        fun manifest(context: Context, appId: Int): File? = roots(context)
            .map { File(it, "steamapps/appmanifest_$appId.acf") }.firstOrNull { it.isFile }

        fun install(context: Context, appId: Int): SteamLibraryManifest.Install? = manifest(context, appId)?.let(SteamLibraryManifest::read)

        // Retain the identity after Steam removes its ACF. Leftover Custom markers must not
        // resurrect an uninstalled game, nor export its stale version on the next startup.
        @Synchronized private fun index(context: Context): JSONObject {
            val file = File(LinuxSteamRuntime(context).directory, "shared-library.json")
            return if (file.isFile) JSONObject(file.readText()) else JSONObject()
        }
        fun managed(context: Context, appId: Int): Boolean = index(context).has(appId.toString())
        @Synchronized fun remember(context: Context, appId: Int, file: File) {
            val data = index(context)
            if (data.optString(appId.toString()) == file.path) return
            data.put(appId.toString(), file.path)
            val target = File(LinuxSteamRuntime(context).directory, "shared-library.json")
            check(target.parentFile!!.isDirectory || target.parentFile!!.mkdirs())
            val temp = File(target.parentFile, "shared-library.json.tmp")
            temp.writeText(data.toString())
            check(temp.renameTo(target))
        }

        fun exportCustom(context: Context, appId: Int, directory: File, name: String, branch: String, replace: Boolean) {
            val common = directory.parentFile ?: return
            if (common.name != "common" || common.parentFile?.name != "steamapps") return
            if (common.parentFile!!.canonicalFile != File(sharedRoot(), "steamapps").canonicalFile) return
            val target = File(common.parentFile, "appmanifest_$appId.acf")
            if (target.exists() && !replace) return
            val kv = SteamLibraryManifest.export(appId, name, directory, branch, PrefManager.containerLanguage) ?: return
            SteamLibraryManifest.write(target, kv)
            remember(context, appId, target)
        }

        fun backup(file: File) {
            if (file.isFile) {
                val backup = File(file.parentFile, "${file.name}.steampsp-before-sharing")
                if (!backup.exists()) file.copyTo(backup)
            }
        }
    }

    suspend fun prepare(): Int {
        check(XrGameStorage.hasAllFilesAccess()) { "Open Custom Steam once and allow game-storage access before sharing the library." }
        XrGameStorage.requireSingleLibrary()
        val standard = File(XrGameStorage.installRoot!!, "Steam/steamapps/common")
        check(standard.isDirectory || standard.mkdirs())
        XrGameStorage.rememberLibraryRoot()
        var exported = 0
        for (installed in db.appInfoDao().getAll()) {
            val app = db.steamAppDao().findApp(installed.id) ?: continue
            val names = listOf(app.config.installDir, app.name).filter { SteamLibraryManifest.safeName(it) }.distinct()
            val directory = if (installed.isImported) File(installed.customInstallPath) else
                (listOf(standard.path) + SteamService.allInstallPaths).asSequence().flatMap { root -> names.asSequence().map { File(root, it) } }
                    .firstOrNull { File(it, ".download_complete").isFile } ?: continue
            if (manifest(context, app.id) == null && !managed(context, app.id)) {
                exportCustom(context, app.id, directory, app.name, installed.branch, false)
                if (manifest(context, app.id) != null) exported++
            }
        }
        // Custom Steam counts a game as installed from its completion marker alone
        // (SteamService.isAppInstalled), so a library copied in or restored without
        // app_info rows still plays there. Export those too, by their unique install folder.
        for (directory in standard.listFiles().orEmpty()) {
            if (!directory.isDirectory || !File(directory, ".download_complete").isFile) continue
            val app = db.steamAppDao().findSteamAppWithInstallDir(directory.name)
                // PICS stores 0 for a base game; older rows carry INVALID_APP_ID.
                .filter { it.dlcForAppId == 0 || it.dlcForAppId == SteamService.INVALID_APP_ID }.singleOrNull() ?: continue
            if (manifest(context, app.id) == null && !managed(context, app.id)) {
                exportCustom(context, app.id, directory, app.name, "public", false)
                if (manifest(context, app.id) != null) exported++
            }
        }
        linkLibrary(File(LinuxSteamRuntime(context).client, "steamapps"), File(sharedRoot(), "steamapps"))
        registerLibraries()
        reconcile()
        return exported
    }

    private fun registerLibraries() {
        val client = LinuxSteamRuntime(context).client
        val file = File(client, "steamapps/libraryfolders.vdf")
        backup(file)
        // Steam requires its client installation as library 0. Its steamapps is the
        // same directory above, so even the default Install action uses shared storage.
        val kv = KeyValue("libraryfolders")
        kv.child("0").apply {
            put("path", client.path); put("label", "SteamPSP shared games")
            File(sharedRoot(), "steamapps").listFiles().orEmpty().forEach {
                SteamLibraryManifest.read(it)?.let { install -> child("apps").put(install.appId.toString(), install.size.toString()) }
            }
        }
        SteamLibraryManifest.write(file, kv)
    }

    /** Read only manifests while the official client owns them; never rewrite a live ACF. */
    suspend fun reconcile() {
        val seen = mutableSetOf<Int>()
        for (root in roots(context)) {
            for (file in File(root, "steamapps").listFiles().orEmpty()) {
                val install = SteamLibraryManifest.read(file) ?: continue
                if (!seen.add(install.appId)) continue
                remember(context, install.appId, file)
                // App metadata and entitlement still come from JavaSteam; this is not an entitlement grant.
                if (db.steamAppDao().findApp(install.appId) == null) continue
                val old = db.appInfoDao().getInstalledApp(install.appId) ?: AppInfo(install.appId)
                val updated = old.copy(isDownloaded = install.complete, branch = install.branch,
                    downloadedDepots = if (install.complete) install.depots.keys.sorted() else old.downloadedDepots,
                    recoveredInstallSizeBytes = install.size)
                if (updated != old) db.appInfoDao().insert(updated)
            }
        }
        val known = index(context)
        for (key in known.keys()) {
            val id = key.toIntOrNull() ?: continue
            if (id in seen) continue
            val file = File(known.getString(key))
            // A missing removable volume is not an uninstall. A mounted library without
            // the recorded ACF is, but preserve cloud history and the Wine prefix.
            if (!file.exists() && file.parentFile?.isDirectory == true) {
                val old = db.appInfoDao().getInstalledApp(id) ?: continue
                if (old.isDownloaded) db.appInfoDao().insert(old.copy(isDownloaded = false))
            }
        }
    }
}

@dagger.hilt.EntryPoint
@dagger.hilt.InstallIn(dagger.hilt.components.SingletonComponent::class)
interface SteamLibraryDatabaseEntryPoint { fun database(): PluviaDatabase }

@Keep
class LinuxSteamLibraryBridge(private val context: Context) : XrGameOfficialSteam.Library {
    private val db get() = dagger.hilt.android.EntryPointAccessors.fromApplication(context, SteamLibraryDatabaseEntryPoint::class.java).database()
    override fun accountId64(): Long? = SteamOfficialAccount.read(LinuxSteamRuntime(context).client)
    override fun cachedApp(appId: Int) = kotlinx.coroutines.runBlocking(kotlinx.coroutines.Dispatchers.IO) { db.steamAppDao().findApp(appId) }
    override fun cachedInstall(appId: Int) = kotlinx.coroutines.runBlocking(kotlinx.coroutines.Dispatchers.IO) { db.appInfoDao().getInstalledApp(appId) }
    override fun customInstallRemoved(appId: Int) {
        check(!XrGameOfficialSteam.ownsLibrary)
        val file = LinuxSteamLibrary.manifest(context, appId) ?: return
        LinuxSteamLibrary.remember(context, appId, file)
        check(file.delete()) { "Could not remove shared installation record." }
    }
    override fun installed(appId: Int): Boolean? {
        if (SteamService.getAppDownloadInfo(appId)?.isActive() == true) return false
        val file = LinuxSteamLibrary.manifest(context, appId)
            ?: return if (LinuxSteamLibrary.managed(context, appId)) false else null
        // A malformed or interrupted official install must not fall back to a stale completion marker.
        return SteamLibraryManifest.read(file)?.complete ?: false
    }
    override fun directory(appId: Int): String? {
        LinuxSteamLibrary.install(context, appId)?.let { return it.directory.path }
        val app = SteamService.getAppInfoOf(appId) ?: return null
        val imported = SteamService.getInstalledApp(appId)?.customInstallPath.orEmpty()
        if (imported.isNotBlank()) return imported // Explicit external imports are not silently moved.
        val name = app.config.installDir.ifBlank { app.name }
        return if (SteamLibraryManifest.safeName(name)) File(LinuxSteamLibrary.sharedRoot(), "steamapps/common/$name").path else null
    }
    override fun customInstallChanged(appId: Int) {
        check(!XrGameOfficialSteam.ownsLibrary)
        val file = LinuxSteamLibrary.manifest(context, appId)
        val prior = file?.let(SteamLibraryManifest::read)
        if (prior != null && !prior.directory.exists()) { check(file.delete()); return }
        val app = SteamService.getAppInfoOf(appId) ?: return
        val directory = File(SteamService.getAppDirPath(appId))
        LinuxSteamLibrary.exportCustom(context, appId, directory, app.name, SteamService.getInstalledApp(appId)?.branch ?: "public", true)
    }
}
