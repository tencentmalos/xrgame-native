// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame

import android.content.Context
import app.gamenative.BuildConfig
import com.winlator.container.Container
import com.winlator.core.envvars.EnvVars
import java.io.File

/** Flavor boundary: official desktop Steam exists only in picoXr. */
object XrGameOfficialSteam {
    private lateinit var appContext: Context
    fun initialize(context: Context) {
        appContext = context.applicationContext
        active = available && preferences().getBoolean("official", false)
    }
    private fun preferences() = appContext.getSharedPreferences("steam-client-mode", Context.MODE_PRIVATE)
    /** Availability is not ownership. Only the selected client may connect or manage a game. */
    val available: Boolean get() = BuildConfig.XRGAME
    @Volatile var active = false
        private set
    @Volatile private var officialGame: Int? = null
    val gameAppId: Int? get() = officialGame
    /**
     * The game command Official Steam issued: executable and working directory relative to the
     * game's install folder, and the arguments as one Windows command line.
     */
    data class Command(val executable: String, val arguments: String, val workingDirectory: String)
    @Volatile private var officialCommand: Command? = null
    @Synchronized fun selectOfficial() {
        check(available)
        check(customMutations == 0 && !app.gamenative.service.SteamService.hasActiveOperations()) {
            "Finish or pause Custom Steam downloads and cloud sync before switching modes."
        }
        check(app.gamenative.PluviaApp.xEnvironment == null && app.gamenative.service.ActiveGameRegistry.get() == null) {
            "Exit the running game before switching Steam modes."
        }
        active = true
        check(preferences().edit().putBoolean("official", true).commit())
    }
    @Synchronized fun selectCustom() {
        check(!ownsLibrary && app.gamenative.PluviaApp.xEnvironment == null &&
            app.gamenative.service.ActiveGameRegistry.get() == null) { "Wait for Official Steam and its game to stop before switching modes." }
        officialGame = null
        officialCommand = null
        active = false
        check(preferences().edit().putBoolean("official", false).commit())
    }
    @Synchronized fun authorizeGame(appId: Int, command: Command? = null) {
        check(active && ownsLibrary && officialGame == null)
        officialGame = appId
        officialCommand = command
    }
    @Synchronized fun finishGame(appId: Int) {
        if (officialGame == appId) { officialGame = null; officialCommand = null }
    }
    fun isOfficialGame(appId: Int): Boolean = active && officialGame == appId
    /**
     * Diagnostics only (DebugBus): launches still resolve the installed launch configuration from
     * JavaSteam's cached metadata, the validated route (user, 2026-10-17). Null when Steam's
     * command was absent or unusable.
     */
    @Synchronized fun gameCommand(appId: Int): Command? = if (isOfficialGame(appId)) officialCommand else null
    fun requireLaunchOwner(appId: Int) {
        check(!active || isOfficialGame(appId)) { "Launch this game from Official Steam, or switch to Custom Steam on the launch page." }
    }
    interface Library {
        fun accountId64(): Long?
        fun cachedApp(appId: Int): app.gamenative.data.SteamApp?
        fun cachedInstall(appId: Int): app.gamenative.data.AppInfo?
        fun installed(appId: Int): Boolean?
        fun directory(appId: Int): String?
        fun customInstallChanged(appId: Int)
        fun customInstallRemoved(appId: Int)
    }
    interface Launcher {
        fun prepare(context: Context, container: Container, game: File, drive: Char, appId: Int,
                    target: XrGameSteamLaunch.Launch, profile: XrGameSteamLaunch.ClientProfile?, env: EnvVars): XrGameSteamLaunch.Launch
    }
    private val launcher: Launcher by lazy {
        check(available)
        Class.forName("app.gamenative.xrgame.steam.LinuxSteamLauncher").getDeclaredConstructor().newInstance() as Launcher
    }
    private val library: Library by lazy {
        Class.forName("app.gamenative.xrgame.steam.LinuxSteamLibraryBridge")
            .getDeclaredConstructor(Context::class.java).newInstance(appContext) as Library
    }
    fun accountId64(): Long? = if (active) library.accountId64() else null
    fun cachedApp(appId: Int): app.gamenative.data.SteamApp? = if (available) library.cachedApp(appId) else null
    fun cachedInstall(appId: Int): app.gamenative.data.AppInfo? = if (available) library.cachedInstall(appId) else null
    fun installed(appId: Int): Boolean? = if (available) library.installed(appId) else null
    fun directory(appId: Int): String? = if (available) library.directory(appId) else null
    fun customInstallChanged(appId: Int) { if (available) library.customInstallChanged(appId) }
    fun customInstallRemoved(appId: Int) { if (available) library.customInstallRemoved(appId) }

    private var customMutations = 0
    @Volatile var ownsLibrary = false
        private set
    @Synchronized fun claimLibrary() {
        check(!ownsLibrary && customMutations == 0 &&
            active && !app.gamenative.service.SteamService.hasActiveOperations()) {
            "Pause Custom Steam downloads before opening Official Steam."
        }
        ownsLibrary = true
    }
    @Synchronized fun releaseLibrary() { ownsLibrary = false }
    @Synchronized fun beginCustomMutation(): Boolean {
        if (!available) return true
        try { XrGameStorage.requireSingleLibrary() }
        catch (error: IllegalStateException) {
            app.gamenative.ui.util.SnackbarManager.show(error.message.orEmpty())
            return false
        }
        if (active || ownsLibrary) {
            app.gamenative.ui.util.SnackbarManager.show("Switch to Custom Steam on the launch page before downloading, verifying or removing shared games.")
            return false
        }
        customMutations++
        return true
    }
    @Synchronized fun endCustomMutation() { if (available) customMutations-- }
    fun prepare(context: Context, container: Container, game: File, drive: Char, appId: Int,
                target: XrGameSteamLaunch.Launch, profile: XrGameSteamLaunch.ClientProfile?, env: EnvVars): XrGameSteamLaunch.Launch {
        check(isOfficialGame(appId)) { "Only Official Steam can start an official game session." }
        return launcher.prepare(context, container, game, drive, appId, target, profile, env)
    }
}
