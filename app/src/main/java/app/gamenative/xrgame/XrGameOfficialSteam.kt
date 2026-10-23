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
     * What official launches resolve first (user, 2026-10-18: a build without JavaSteam's launch
     * metadata). Null when Steam's command was absent or unusable; JavaSteam's cached launch
     * configuration then applies.
     */
    @Synchronized fun gameCommand(appId: Int): Command? = if (isOfficialGame(appId)) officialCommand else null
    /** The executable Steam started for this game, for process and window matching; else [fallback]. */
    fun gameExecutable(appId: Int, fallback: String): String = gameCommand(appId)?.executable ?: fallback
    fun requireLaunchOwner(appId: Int) {
        check(!active || isOfficialGame(appId)) { "Launch this game from Official Steam, or switch to Custom Steam on the launch page." }
    }

    /**
     * How the home page's Steam entry shows Steam on a headset: as a 2D window or on the XR big
     * screen. Its ordinary games follow it; there is no per-game display choice in Official Steam.
     */
    enum class Display(val key: String) { FLAT("2d"), XR("xr") }
    var display: Display
        get() = if (!available) Display.FLAT
            else Display.entries.firstOrNull { it.key == preferences().getString("display", null) } ?: Display.FLAT
        set(value) { if (available) preferences().edit().putString("display", value.key).apply() }

    /** The display mode of an official launch: VR games always in a VR session, others as the entry. */
    fun launchMode(vrGame: Boolean, headset: Boolean, entry: Display = display): XrGameLaunchMode.Mode = XrGameLaunchMode.fallback(
        if (vrGame || headset && entry == Display.XR) XrGameLaunchMode.Mode.XR else XrGameLaunchMode.Mode.FLAT,
        vrGame,
        headset,
    )

    /**
     * The XR big screen's size, distance and curve, shared by Steam's page and the ordinary games
     * it starts: the home page, the launch panel and resizing in either session all change it.
     * Resolution is not shared: Steam's page is light and games are heavy, so each keeps its own.
     */
    data class XrScreen(val scale: Float, val distance: Float, val curved: Boolean)

    /** Steam's XR page settings; [xrScreen] lives under the keys that page reads. */
    const val XR_SETTINGS = "steam-xr"
    private fun xrPreferences() = appContext.getSharedPreferences(XR_SETTINGS, Context.MODE_PRIVATE)
    private const val SCALE = app.gamenative.ui.screen.xr.SbsTheaterSettings.SCALE
    private const val DISTANCE = app.gamenative.ui.screen.xr.SbsTheaterSettings.DISTANCE
    private const val CURVED = app.gamenative.ui.screen.xr.ImmersiveXrActivity.EXTRA_CURVED_SCREEN
    private const val PASSTHROUGH = app.gamenative.ui.screen.xr.ImmersiveXrActivity.EXTRA_PASSTHROUGH_ENABLED

    var xrScreen: XrScreen
        get() = xrPreferences().let {
            val preset = app.gamenative.ui.screen.xr.XrScreenPresets.DEFAULT
            XrScreen(it.getFloat(SCALE, preset.scale), it.getFloat(DISTANCE, preset.distanceMeters), it.getBoolean(CURVED, false))
        }
        set(value) {
            xrPreferences().edit().putFloat(SCALE, value.scale).putFloat(DISTANCE, value.distance)
                .putBoolean(CURVED, value.curved).apply()
        }

    /** Passthrough (VST) behind Steam's own big screen, as its page loads it when its session starts. */
    var xrPassthrough: Boolean
        get() = xrPreferences().getBoolean(PASSTHROUGH, false)
        set(value) {
            xrPreferences().edit().putBoolean(PASSTHROUGH, value).apply()
        }

    /**
     * Writes the display mode of an official launch into [appId]'s container before the launch
     * panel or the game reads it, and for an ordinary game on the XR entry the shared [xrScreen].
     * A game counts as VR when Steam marks it VR-only or its container already runs it in VR. IO thread.
     */
    fun applyDisplay(context: Context, appId: String) {
        val headset = app.gamenative.MainActivity.isHeadset(context)
        val data = app.gamenative.utils.ContainerUtils.toContainerData(
            app.gamenative.utils.ContainerUtils.getOrCreateContainer(context, appId))
        val gameId = app.gamenative.utils.ContainerUtils.extractGameIdFromContainerId(appId)
        val vrGame = data.windowsVrEnabled || onlyVr(gameId)
        val mode = launchMode(vrGame, headset)
        var next = XrGameLaunchMode.apply(data, mode, vrGame)
        val screen = xrScreen.takeIf { mode == XrGameLaunchMode.Mode.XR && !vrGame }
        if (screen != null) next = next.copy(
            xrScreenPreset = app.gamenative.ui.screen.xr.XrScreenPresets.match(screen.scale, screen.distance)?.key ?: "",
            xrCurvedScreen = screen.curved,
        )
        if (next != data) app.gamenative.utils.ContainerUtils.applyToContainer(context, appId, next)
        // A screen resized by hand matches no preset, which the container data cannot carry.
        if (screen != null) app.gamenative.utils.ContainerUtils.getContainer(context, appId).run {
            if (getExtra(SCALE, "") != screen.scale.toString() || getExtra(DISTANCE, "") != screen.distance.toString()) {
                putExtra(SCALE, screen.scale.toString())
                putExtra(DISTANCE, screen.distance.toString())
                saveData()
            }
        }
        timber.log.Timber.i("Official launch of %s: %s (vr=%b, entry=%s)", appId, mode.key, vrGame, display.key)
    }

    /** Whether [appId] (a container id) is the official game shown on the XR entry's big screen. */
    fun sharesXrScreen(appId: String): Boolean = display == Display.XR &&
        isOfficialGame(app.gamenative.utils.ContainerUtils.extractGameIdFromContainerId(appId))

    /** The page an official game returns to: the Steam window of the entry's display. */
    fun homeActivity(headset: Boolean): String =
        if (headset && display == Display.XR) "app.gamenative.xrgame.steam.SteamXrActivity"
        else "app.gamenative.xrgame.steam.LinuxSteamActivity"

    /** Whether Steam marks [appId] as playable only in VR (appinfo `common/onlyvrsupport`). */
    fun onlyVr(appId: Int): Boolean = available && runCatching { library.onlyVr(appId) }.getOrDefault(false)

    interface Library {
        fun accountId64(): Long?
        fun cachedApp(appId: Int): app.gamenative.data.SteamApp?
        fun cachedInstall(appId: Int): app.gamenative.data.AppInfo?
        fun installed(appId: Int): Boolean?
        fun directory(appId: Int): String?
        fun customInstallChanged(appId: Int)
        fun customInstallRemoved(appId: Int)
        fun onlyVr(appId: Int): Boolean
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
