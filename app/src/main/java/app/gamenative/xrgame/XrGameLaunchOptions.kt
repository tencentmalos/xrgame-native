package app.gamenative.xrgame

import android.content.Context
import app.gamenative.PrefManager
import app.gamenative.R
import app.gamenative.ui.screen.xr.XrScreenPresets
import app.gamenative.ui.screen.xr.windows.WindowsVrUpscale
import app.gamenative.ui.screen.xr.windows.XrResolutionRecommendation
import app.gamenative.ui.screen.xserver.FPS_LIMITER_ENABLED_EXTRA
import app.gamenative.ui.screen.xserver.FPS_LIMITER_TARGET_EXTRA
import app.gamenative.utils.ContainerUtils
import app.gamenative.xrgame.XrGameLaunchMode.Mode
import com.winlator.container.ContainerData
import timber.log.Timber

/**
 * The launch panel's options as plain rows, shared by the 2D panel (Compose) and the panel the XR
 * big screen draws itself: each row is a label, its choices, the selected one and what choosing
 * does to the draft. Rows only read the draft; [Changes] applies a choice.
 */
object XrGameLaunchOptions {
    /** VR render resolution: percentages of the recommended eye, then XrResolutionRecommendation's high and limit. */
    private val RENDER_SCALES = listOf(50, 75, 100, XrResolutionRecommendation.HIGH_PERCENT, XrResolutionRecommendation.LIMIT_PERCENT)
    private val REFRESH_RATES = listOf(72, 90, 120)
    /** Frame limit presets in FPS; XR sessions run at 72 or 90 Hz on Swan. */
    private val FRAME_LIMITS = listOf(30, 36, 45, 60, 72, 90)

    /** The quick menu's FPS limiter, as XServerScreen keeps it in the container extras. */
    data class FrameLimit(val enabled: Boolean, val target: Int)

    data class Row(
        val label: String,
        val choices: List<String>,
        val selected: Int,
        val note: String? = null,
        val enabled: (Int) -> Boolean = { true },
        val onSelect: (Int) -> Unit,
    )

    /** What the rows read. */
    data class Draft(
        val data: ContainerData,
        val headset: Boolean,
        val vrGame: Boolean,
        val mode: Mode,
        val choosesMode: Boolean,
        val statusLayer: Boolean,
        val frameLimit: FrameLimit?,
        val guestPatches: List<XrGameGuestPatches.Package>,
        val captureAvailable: Boolean,
    )

    /** What choosing changes. */
    class Changes(
        val onData: (ContainerData) -> Unit,
        val onVrGame: (Boolean) -> Unit,
        val onMode: (Mode) -> Unit,
        val onStatusLayer: (Boolean) -> Unit,
        val onFrameLimit: (FrameLimit) -> Unit,
    )

    /** The main rows and the advanced ones. */
    fun rows(context: Context, draft: Draft, changes: Changes): Pair<List<Row>, List<Row>> {
        val data = draft.data
        val vrGame = draft.vrGame
        val mode = draft.mode
        val onData = changes.onData
        fun text(id: Int, vararg args: Any) = context.getString(id, *args)
        val off = text(R.string.xrgame_launch_off)
        val on = text(R.string.xrgame_launch_on)
        val modes = XrGameLaunchMode.available(draft.headset)
        val modeLabels = mapOf(
            Mode.FLAT to text(R.string.xrgame_launch_mode_2d),
            Mode.SBS to text(R.string.xrgame_launch_mode_sbs),
            Mode.XR to text(R.string.xrgame_launch_mode_xr),
        )
        val vulkanComposite = WindowsVrUpscale.backend(data.envVars) == WindowsVrUpscale.BACKEND_VULKAN
        val upscale = WindowsVrUpscale.fromEnvVars(data.envVars)
        val updateUpscale = { next: WindowsVrUpscale.Settings ->
            onData(data.copy(envVars = WindowsVrUpscale.withSettings(data.envVars, next)))
        }
        fun toggle(label: Int, value: Boolean, set: (Boolean) -> Unit) =
            Row(text(label), listOf(off, on), if (value) 1 else 0, onSelect = { set(it == 1) })
        val display = WindowsVrUpscale.Display.decode(PrefManager.xrDisplayInfo)

        val main = mutableListOf<Row>()
        if (draft.choosesMode) {
            main += Row(
                label = text(R.string.xrgame_launch_mode),
                choices = modes.map { modeLabels.getValue(it) },
                selected = modes.indexOf(mode),
                enabled = { XrGameLaunchMode.allowed(modes[it], vrGame, draft.headset) },
                note = if (vrGame) text(R.string.xrgame_launch_vr_needs_3d) else null,
                onSelect = { changes.onMode(modes[it]) },
            )
            main += Row(
                label = text(R.string.xrgame_launch_type),
                choices = listOf(text(R.string.xrgame_launch_type_normal), text(R.string.xrgame_launch_type_vr)),
                selected = if (vrGame) 1 else 0,
                onSelect = { changes.onVrGame(it == 1) },
            )
        }
        if (mode == Mode.XR && !vrGame) {
            val presets = XrScreenPresets.PRESETS
            val selected = presets.indexOfFirst { it.key == data.xrScreenPreset }
            main += Row(
                label = text(R.string.xrgame_screen_size),
                choices = presets.map { "${it.diagonalInches}″ · ${it.horizontalDegrees}°" } +
                    if (selected < 0) listOf(text(R.string.xrgame_launch_custom)) else emptyList(),
                selected = if (selected >= 0) selected else presets.size,
                note = if (!draft.choosesMode) text(R.string.xrgame_launch_screen_shared) else null,
                onSelect = { idx -> presets.getOrNull(idx)?.let { onData(data.copy(xrScreenPreset = it.key)) } },
            )
            // The standard sizes up to 1920x1080, plus the saved one when it is a custom size.
            val sizes = context.resources.getStringArray(R.array.screen_size_entries).drop(1)
                .filter { XrGameResolution.fits(it.substringBefore(' ')) }
            val saved = XrGameResolution.capped(data.screenSize)
            val index = sizes.indexOfFirst { it.substringBefore(' ') == saved }
            val choices = if (index >= 0) sizes else sizes + "$saved (${text(R.string.xrgame_launch_custom)})"
            // Where the Wine screen falls in what the XR compositor can show of this screen.
            val preset = XrScreenPresets.byKey(data.xrScreenPreset) ?: XrScreenPresets.DEFAULT
            val wineWidth = XrGameResolution.parse(saved)?.first
            val range = if (display != null && wineWidth != null) {
                val low = WindowsVrUpscale.screenCoverage(display, WindowsVrUpscale.EyeBuffer.RECOMMENDED, preset.widthMeters, preset.distanceMeters)
                val high = WindowsVrUpscale.screenCoverage(display, WindowsVrUpscale.EyeBuffer.MAXIMUM, preset.widthMeters, preset.distanceMeters)
                text(
                    R.string.xrgame_launch_screen_range,
                    "${low}x${low * 9 / 16}",
                    "${high}x${high * 9 / 16}",
                    text(
                        when {
                            wineWidth < low -> R.string.xrgame_launch_screen_below
                            wineWidth > high -> R.string.xrgame_launch_screen_above
                            else -> R.string.xrgame_launch_screen_inside
                        },
                    ),
                )
            } else {
                text(R.string.xrgame_launch_screen_unknown)
            }
            main += Row(
                label = text(R.string.xrgame_launch_resolution),
                choices = choices,
                selected = if (index >= 0) index else sizes.size,
                note = if (saved != data.screenSize) text(R.string.xrgame_launch_resolution_capped, data.screenSize) + "\n" + range else range,
                onSelect = { idx -> onData(data.copy(screenSize = choices[idx].substringBefore(' '))) },
            )
            main += toggle(R.string.xrgame_vst, data.xrPassthrough) { onData(data.copy(xrPassthrough = it)) }
            main += toggle(R.string.xrgame_curved_screen, data.xrCurvedScreen) { onData(data.copy(xrCurvedScreen = it)) }
        }
        if (mode == Mode.XR) {
            main += Row(
                label = text(R.string.xrgame_launch_refresh),
                choices = REFRESH_RATES.map { "$it Hz" },
                selected = REFRESH_RATES.indexOf(data.xrRefreshRate),
                onSelect = { onData(data.copy(xrRefreshRate = REFRESH_RATES[it])) },
            )
        }
        // Reconstruction runs on the Vulkan composite, so it reads as off on GLES and choosing a
        // filter switches the game to Vulkan. VR games reconstruct their eyes, the big screen its quad.
        val reconstructs = WindowsVrUpscale.compositeBackend(data.envVars, vrGame) == WindowsVrUpscale.BACKEND_VULKAN
        val filter = if (reconstructs) upscale.filter else WindowsVrUpscale.Filter.OFF
        val upscaling = mode == Mode.XR && filter != WindowsVrUpscale.Filter.OFF
        if (mode == Mode.XR) {
            main += Row(
                label = text(R.string.xrgame_launch_upscale),
                choices = listOf(text(R.string.xrgame_vr_upscale_off), "FSR1", "SGSR"),
                selected = filter.ordinal,
                onSelect = { index ->
                    val next = upscale.copy(filter = WindowsVrUpscale.Filter.entries[index])
                    var envVars = WindowsVrUpscale.withSettings(data.envVars, next)
                    if (next.filter != WindowsVrUpscale.Filter.OFF && !reconstructs) {
                        envVars = WindowsVrUpscale.withBackend(envVars, WindowsVrUpscale.BACKEND_VULKAN)
                    }
                    onData(data.copy(envVars = envVars))
                },
            )
            // The ETFR debug view draws in the Vulkan composite, so switching it on picks Vulkan.
            main += Row(
                label = text(R.string.xrgame_launch_etfr_debug),
                choices = listOf(off, on),
                selected = if (upscale.debug && reconstructs) 1 else 0,
                note = text(R.string.xrgame_launch_etfr_debug_note),
                onSelect = { index ->
                    var envVars = WindowsVrUpscale.withSettings(data.envVars, upscale.copy(debug = index == 1))
                    if (index == 1 && !reconstructs) {
                        envVars = WindowsVrUpscale.withBackend(envVars, WindowsVrUpscale.BACKEND_VULKAN)
                    }
                    onData(data.copy(envVars = envVars))
                },
            )
        }
        val tiers = listOf(
            text(R.string.xrgame_launch_recommended),
            text(R.string.xrgame_launch_eye_buffer_high),
            text(R.string.xrgame_launch_eye_buffer_max),
        )
        if (upscaling && vrGame) {
            // Eye sizes from the eye views the last XR session recorded.
            val tier = upscale.eyeBuffer
            main += Row(
                label = text(R.string.xrgame_launch_eye_buffer),
                choices = tiers,
                selected = tier.ordinal,
                note = display?.let { WindowsVrUpscale.eyeExtent(it, tier) }
                    ?.let { (w, h) -> text(R.string.xrgame_launch_eye_buffer_eye, "${w}x$h") },
                // A tier is the whole eye size; the older percentage of it no longer applies.
                onSelect = { updateUpscale(upscale.copy(eyeBuffer = WindowsVrUpscale.EyeBuffer.entries[it], outputPercent = 100)) },
            )
        }
        if (upscaling && !vrGame) {
            // The layer follows the tier within the screen's effective range; the filter runs only
            // when the Wine screen is smaller than the layer.
            val preset = XrScreenPresets.byKey(data.xrScreenPreset) ?: XrScreenPresets.DEFAULT
            val tier = upscale.eyeBuffer
            val wine = XrGameResolution.parse(XrGameResolution.capped(data.screenSize))
            val layer = wine?.let { (w, h) ->
                WindowsVrUpscale.screenSwapchain(w, h, tier, display, preset.widthMeters, preset.distanceMeters)
            }
            main += Row(
                label = text(R.string.xrgame_launch_screen_layer),
                choices = tiers,
                selected = tier.ordinal,
                note = when {
                    display == null || wine == null || layer == null -> text(R.string.xrgame_launch_screen_unknown)
                    layer == wine -> text(R.string.xrgame_launch_screen_layer_same, "${wine.first}x${wine.second}")
                    else -> text(R.string.xrgame_launch_screen_layer_note, "${wine.first}x${wine.second}", "${layer.first}x${layer.second}")
                },
                onSelect = { updateUpscale(upscale.copy(eyeBuffer = WindowsVrUpscale.EyeBuffer.entries[it], outputPercent = 100)) },
            )
        }
        // VR games also get the eye gaze for their own foveation (ETFR) while this is "eye".
        if (upscaling || (mode == Mode.XR && vrGame)) {
            main += Row(
                label = text(R.string.xrgame_launch_foveation),
                choices = listOf(
                    text(R.string.xrgame_vr_foveation_off),
                    text(R.string.xrgame_vr_foveation_fixed),
                    text(R.string.xrgame_vr_foveation_eye),
                ),
                selected = upscale.foveation.ordinal,
                note = if (!upscaling) text(R.string.xrgame_launch_foveation_game_note) else null,
                onSelect = { updateUpscale(upscale.copy(foveation = WindowsVrUpscale.Foveation.entries[it])) },
            )
        }
        // A cap on the game's MSAA in Turnip; VR games run single-sampled while they get the gaze.
        val forced = XrGameMsaa.forcedOff(data.envVars, mode == Mode.XR && vrGame)
        val msaa = if (forced) XrGameMsaa.Mode.OFF else XrGameMsaa.stored(data.envVars)
        main += Row(
            label = text(R.string.xrgame_launch_msaa),
            choices = listOf(text(R.string.xrgame_launch_msaa_game), off, "2x"),
            selected = msaa.ordinal,
            enabled = { !forced || it == XrGameMsaa.Mode.OFF.ordinal },
            note = if (forced) text(R.string.xrgame_launch_msaa_forced) else null,
            onSelect = { onData(data.copy(envVars = XrGameMsaa.withMode(data.envVars, XrGameMsaa.Mode.entries[it]))) },
        )
        if (upscaling && upscale.foveation != WindowsVrUpscale.Foveation.OFF) {
            main += Row(
                label = text(R.string.xrgame_vr_foveation_level),
                choices = listOf(
                    text(R.string.xrgame_vr_foveation_low),
                    text(R.string.xrgame_vr_foveation_balanced),
                    text(R.string.xrgame_vr_foveation_high),
                ),
                selected = upscale.level.ordinal,
                onSelect = { updateUpscale(upscale.copy(level = WindowsVrUpscale.Level.entries[it])) },
            )
        }
        draft.frameLimit?.let { frameLimit ->
            // The game's own frame rate, capped by the X server as the quick menu's FPS limiter
            // does; the saved target stays selectable when it is not one of the presets.
            val (enabled, target) = frameLimit
            val steps = (FRAME_LIMITS + if (enabled) listOf(target) else emptyList()).distinct().sorted()
            main += Row(
                label = text(R.string.xrgame_launch_frame_limit),
                choices = listOf(text(R.string.performance_hud_fps_limiter_off)) +
                    steps.map { text(R.string.performance_hud_fps_limiter_value, it) },
                selected = if (enabled) 1 + steps.indexOf(target) else 0,
                note = text(R.string.xrgame_launch_frame_limit_note),
                onSelect = { idx ->
                    changes.onFrameLimit(if (idx == 0) frameLimit.copy(enabled = false) else FrameLimit(true, steps[idx - 1]))
                },
            )
        }
        if (vrGame) {
            val scaleIndex = if (data.xrRenderScaleRecommended) {
                0
            } else {
                1 + RENDER_SCALES.indices.minBy { kotlin.math.abs(RENDER_SCALES[it] - data.xrRenderScale) }
            }
            // The eye size the game renders at, from the eye views the last XR session recorded,
            // as the Windows VR runtime hands it out.
            val eye = display?.let {
                XrResolutionRecommendation.eyeSize(
                    it.recommendedWidth.toLong(),
                    it.recommendedHeight.toLong(),
                    it.maxWidth.toLong(),
                    it.maxHeight.toLong(),
                    if (data.xrRenderScaleRecommended) XrResolutionRecommendation.RECOMMENDED_PERCENT else data.xrRenderScale,
                    data.xrRenderScaleRecommended,
                )
            }
            val high = text(R.string.xrgame_launch_eye_buffer_high)
            val limit = text(R.string.xrgame_launch_eye_buffer_max)
            main += Row(
                label = text(R.string.xrgame_launch_render_scale),
                choices = listOf(text(R.string.xrgame_launch_recommended)) + RENDER_SCALES.map {
                    when (it) {
                        XrResolutionRecommendation.HIGH_PERCENT -> high
                        XrResolutionRecommendation.LIMIT_PERCENT -> limit
                        else -> "$it%"
                    }
                },
                selected = scaleIndex,
                note = eye?.let { (w, h) -> text(R.string.xrgame_launch_eye_buffer_eye, "${w}x$h") },
                onSelect = { idx ->
                    onData(
                        if (idx == 0) {
                            data.copy(xrRenderScaleRecommended = true)
                        } else {
                            data.copy(xrRenderScaleRecommended = false, xrRenderScale = RENDER_SCALES[idx - 1])
                        },
                    )
                },
            )
        }
        if (!vrGame && mode != Mode.SBS) {
            // The SBS cinema always uses the trackpad; elsewhere a tap can land where it is.
            main += Row(
                label = text(R.string.xrgame_launch_touch),
                choices = listOf(text(R.string.xrgame_launch_touch_direct), text(R.string.xrgame_launch_touch_trackpad)),
                selected = if (data.touchscreenMode) 0 else 1,
                onSelect = { onData(data.copy(touchscreenMode = it == 0)) },
            )
        }
        if (mode == Mode.XR) {
            // The world-locked panel of XR sessions; one switch for every game.
            main += toggle(R.string.xrgame_launch_status_layer, draft.statusLayer, changes.onStatusLayer)
        }
        // Done in the D3D translation layers; the game keeps its own texture sizes.
        val qualities = XrGameQualitySettings.TEXTURE_QUALITIES
        main += Row(
            label = text(R.string.xrgame_launch_texture_quality),
            choices = listOf(
                text(R.string.xrgame_launch_texture_quality_high),
                text(R.string.xrgame_launch_texture_quality_medium),
                text(R.string.xrgame_launch_texture_quality_low),
            ),
            selected = qualities.indexOf(XrGameQualitySettings.textureQuality(data.envVars)),
            onSelect = { onData(data.copy(envVars = XrGameQualitySettings.withTextureQuality(data.envVars, qualities[it]))) },
        )
        // One switch per guest patch package, installed in name order; a package that changes
        // code a selected one changes cannot be switched on.
        val selectedPatches = XrGameGuestPatches.selection(data.envVars)
        for (patch in draft.guestPatches) {
            val selected = patch.name in selectedPatches
            val blocked = patch.conflicts.filter { it in selectedPatches }
            main += Row(
                label = text(R.string.xrgame_launch_guest_patch, patch.displayName),
                choices = listOf(off, on),
                selected = if (selected) 1 else 0,
                enabled = { it == 0 || selected || (blocked.isEmpty() && selectedPatches.size < XrGameGuestPatches.MAX_PACKAGES) },
                note = if (blocked.isNotEmpty()) text(R.string.xrgame_launch_guest_patch_conflict, blocked.joinToString())
                else patch.description.ifEmpty { null },
                onSelect = { choice ->
                    val names = draft.guestPatches.map { it.name }.filter { name ->
                        if (name == patch.name) choice == 1 else name in selectedPatches
                    }
                    onData(data.copy(envVars = XrGameGuestPatches.withSelection(data.envVars, names)))
                },
            )
        }

        val extra = mutableListOf<Row>()
        if (mode == Mode.XR && vrGame) {
            extra += Row(
                label = text(R.string.xrgame_vr_composite),
                choices = listOf(text(R.string.xrgame_vr_composite_gles), text(R.string.xrgame_vr_composite_vulkan)),
                selected = if (vulkanComposite) 1 else 0,
                onSelect = { onData(data.copy(envVars = WindowsVrUpscale.withBackend(data.envVars, WindowsVrUpscale.BACKENDS[it]))) },
            )
        }
        if (upscaling) {
            val steps = WindowsVrUpscale.SHARPNESS_STEPS
            extra += Row(
                label = text(R.string.xrgame_vr_sharpness),
                choices = steps.map { "$it%" },
                selected = steps.indices.minBy { kotlin.math.abs(steps[it] - upscale.sharpness) },
                onSelect = { updateUpscale(upscale.copy(sharpness = steps[it])) },
            )
        }
        if (vrGame) {
            extra += toggle(R.string.xr_open_composite_toggle, data.openCompositeEnabled) { onData(data.copy(openCompositeEnabled = it)) }
        }
        // The video memory D3D games are told they have; they size textures and caches from it.
        val budgets = XrGameMemoryBudget.STEPS_MB
        val budget = XrGameMemoryBudget.fromConfig(data.dxwrapperConfig)
        extra += Row(
            label = text(R.string.xrgame_launch_memory_budget),
            choices = budgets.map { "${it / 1024}G" },
            selected = budgets.indices.minBy { kotlin.math.abs(budgets[it] - budget) },
            note = text(R.string.xrgame_launch_memory_budget_note),
            onSelect = { onData(data.copy(dxwrapperConfig = XrGameMemoryBudget.withBudget(data.dxwrapperConfig, budgets[it]))) },
        )
        extra += toggle(R.string.xrgame_async_present, XrGamePresentSettings.asyncCopyEnabled(data.envVars)) {
            onData(data.copy(envVars = XrGamePresentSettings.withAsyncCopy(data.envVars, it)))
        }
        extra += toggle(R.string.xrgame_render_ahead, XrGamePresentSettings.renderAheadEnabled(data.envVars)) {
            onData(data.copy(envVars = XrGamePresentSettings.withRenderAhead(data.envVars, it)))
        }
        if (draft.captureAvailable) {
            extra += Row(
                label = text(R.string.xrgame_api_capture),
                choices = listOf(
                    text(R.string.xrgame_api_capture_off),
                    text(R.string.xrgame_api_capture_d3d12),
                    text(R.string.xrgame_api_capture_vulkan),
                ),
                selected = XrGameApiCapture.MODES.indexOf(XrGameApiCapture.mode(data.envVars)),
                onSelect = { onData(data.copy(envVars = XrGameApiCapture.withMode(data.envVars, XrGameApiCapture.MODES[it]))) },
            )
        }
        return main to extra
    }

    /** The game's name for the panel's title, or its container id when the cache has none. */
    fun title(appId: String): String = runCatching {
        if (ContainerUtils.extractGameSourceFromContainerId(appId) == app.gamenative.data.GameSource.STEAM) {
            app.gamenative.service.SteamService.getAppInfoOf(ContainerUtils.extractGameIdFromContainerId(appId))?.name
        } else {
            null
        }
    }.getOrNull()?.takeIf { it.isNotBlank() } ?: appId

    /** The panel's one-line summary of how the game will show. */
    fun summary(context: Context, mode: Mode, vrGame: Boolean): String = context.getString(
        when (mode) {
            Mode.FLAT -> R.string.xrgame_launch_summary_2d
            Mode.SBS -> if (vrGame) R.string.xrgame_launch_summary_sbs_vr else R.string.xrgame_launch_summary_sbs_cinema
            Mode.XR -> if (vrGame) R.string.xrgame_launch_summary_xr_vr else R.string.xrgame_launch_summary_xr_screen
        },
    )

    /**
     * Saves a launch's draft: the container data with its display mode, the frame limit, the
     * status layer switch, and on Steam's XR entry ([followsEntry]) the big screen's shared size
     * and curve (a hand-resized screen, no preset, keeps its size). IO thread.
     */
    fun save(
        context: Context,
        appId: String,
        initial: ContainerData?,
        data: ContainerData,
        mode: Mode,
        vrGame: Boolean,
        initialFrameLimit: FrameLimit?,
        frameLimit: FrameLimit?,
        statusLayer: Boolean,
        followsEntry: Boolean,
    ) {
        if (statusLayer != PrefManager.xrgameStatusLayer) PrefManager.xrgameStatusLayer = statusLayer
        val saved = XrGameLaunchMode.apply(data, mode, vrGame)
        if (saved != initial) {
            runCatching { ContainerUtils.applyToContainer(context, appId, saved) }
                .onFailure { Timber.e(it, "Launch panel: saving %s failed", appId) }
        }
        if (followsEntry && mode == Mode.XR && !vrGame) {
            val shared = XrGameOfficialSteam.xrScreen
            val preset = XrScreenPresets.byKey(saved.xrScreenPreset)
            val next = XrGameOfficialSteam.XrScreen(preset?.scale ?: shared.scale, preset?.distanceMeters ?: shared.distance, saved.xrCurvedScreen)
            if (next != shared) XrGameOfficialSteam.xrScreen = next
        }
        if (frameLimit != null && frameLimit != initialFrameLimit) {
            runCatching {
                val container = ContainerUtils.getContainer(context, appId)
                container.putExtra(FPS_LIMITER_ENABLED_EXTRA, frameLimit.enabled)
                container.putExtra(FPS_LIMITER_TARGET_EXTRA, frameLimit.target)
                container.saveData()
            }.onFailure { Timber.e(it, "Launch panel: saving the frame limit of %s failed", appId) }
        }
        Timber.i("Launch panel: %s starts in %s (vr=%b)", appId, mode.key, vrGame)
    }
}
