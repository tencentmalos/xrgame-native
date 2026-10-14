package app.gamenative.ui.component.dialog

import android.view.KeyEvent
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Check
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringArrayResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import app.gamenative.MainActivity
import app.gamenative.PrefManager
import app.gamenative.R
import app.gamenative.data.GameSource
import app.gamenative.service.SteamService
import app.gamenative.ui.screen.xr.XrScreenPresets
import app.gamenative.ui.screen.xr.windows.WindowsVrUpscale
import app.gamenative.ui.screen.xr.windows.XrResolutionRecommendation
import app.gamenative.ui.screen.xserver.FPS_LIMITER_ENABLED_EXTRA
import app.gamenative.ui.screen.xserver.FPS_LIMITER_TARGET_EXTRA
import app.gamenative.ui.screen.xserver.initialFpsLimiterEnabled
import app.gamenative.ui.screen.xserver.initialFpsLimiterTarget
import app.gamenative.utils.ContainerUtils
import app.gamenative.xrgame.XrGameApiCapture
import app.gamenative.xrgame.XrGameLaunchMode
import app.gamenative.xrgame.XrGameLaunchMode.Mode
import app.gamenative.xrgame.XrGameMemoryBudget
import app.gamenative.xrgame.XrGamePresentSettings
import app.gamenative.xrgame.XrGameQualitySettings
import app.gamenative.xrgame.XrGameResolution
import com.winlator.container.ContainerData
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import timber.log.Timber

/** VR render resolution: percentages of the recommended eye, then XrResolutionRecommendation's high and limit. */
private val RENDER_SCALES = listOf(50, 75, 100, XrResolutionRecommendation.HIGH_PERCENT, XrResolutionRecommendation.LIMIT_PERCENT)
private val LABEL_WIDTH = 100.dp
private val CONTROL_HEIGHT = 30.dp
private const val MAX_BUTTONS = 3
private val REFRESH_RATES = listOf(72, 90, 120)
/** Frame limit presets in FPS; XR sessions run at 72 or 90 Hz on Swan. */
private val FRAME_LIMITS = listOf(30, 36, 45, 60, 72, 90)

/** The quick menu's FPS limiter, as XServerScreen keeps it in the container extras. */
private data class FrameLimit(val enabled: Boolean, val target: Int)

private typealias OptionItem = @Composable () -> Unit

/**
 * XRGame's launch panel, shown when a game is started from the library: the display mode (2D,
 * SBS, and XR on headsets) and the options that mode uses, in two columns between a title line
 * and a bottom bar with the full settings, cancel and launch. Changes are a draft until launch,
 * which saves them to the game's container; cancel drops them. Shortcuts, external intents and
 * DebugBus launches skip the panel and use the saved settings. Gamepads: A activates the focused
 * control, B cancels, Y opens the full settings.
 */
@Composable
fun XrGameLaunchPanel(
    appId: String,
    onLaunch: () -> Unit,
    onDismiss: () -> Unit,
    onOpenSettings: () -> Unit,
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val headset = remember { MainActivity.isHeadset(context) }
    val captureAvailable = remember { XrGameApiCapture.available(context) }
    var title by remember(appId) { mutableStateOf(appId) }
    var initial by remember(appId) { mutableStateOf<ContainerData?>(null) }
    var draft by remember(appId) { mutableStateOf<ContainerData?>(null) }
    var vrGame by remember(appId) { mutableStateOf(false) }
    var mode by remember(appId) { mutableStateOf(Mode.FLAT) }
    var statusLayer by remember { mutableStateOf(PrefManager.xrgameStatusLayer) }
    // The quick menu's frame limiter: container extras, applied by XServerScreen at start.
    var initialFrameLimit by remember(appId) { mutableStateOf<FrameLimit?>(null) }
    var frameLimit by remember(appId) { mutableStateOf<FrameLimit?>(null) }
    var advanced by remember { mutableStateOf(false) }
    var saving by remember { mutableStateOf(false) }
    val launchFocus = remember { FocusRequester() }

    LaunchedEffect(appId) {
        val (data, name, limit) = withContext(Dispatchers.IO) {
            val container = ContainerUtils.getOrCreateContainer(context, appId)
            Triple(
                ContainerUtils.toContainerData(container),
                gameTitle(appId),
                FrameLimit(initialFpsLimiterEnabled(container), initialFpsLimiterTarget(container)),
            )
        }
        title = name
        initial = data
        draft = data
        initialFrameLimit = limit
        frameLimit = limit
        vrGame = data.windowsVrEnabled
        mode = XrGameLaunchMode.current(data, headset)
    }
    LaunchedEffect(draft != null) {
        if (draft != null) runCatching { launchFocus.requestFocus() }
    }

    val launch: () -> Unit = start@{
        val data = draft ?: return@start
        if (saving) return@start
        saving = true
        if (statusLayer != PrefManager.xrgameStatusLayer) PrefManager.xrgameStatusLayer = statusLayer
        val saved = XrGameLaunchMode.apply(data, mode, vrGame)
        scope.launch {
            if (saved != initial) {
                runCatching { withContext(Dispatchers.IO) { ContainerUtils.applyToContainer(context, appId, saved) } }
                    .onFailure { Timber.e(it, "Launch panel: saving %s failed", appId) }
            }
            val limit = frameLimit
            if (limit != null && limit != initialFrameLimit) {
                runCatching {
                    withContext(Dispatchers.IO) {
                        val container = ContainerUtils.getContainer(context, appId)
                        container.putExtra(FPS_LIMITER_ENABLED_EXTRA, limit.enabled)
                        container.putExtra(FPS_LIMITER_TARGET_EXTRA, limit.target)
                        container.saveData()
                    }
                }.onFailure { Timber.e(it, "Launch panel: saving the frame limit of %s failed", appId) }
            }
            Timber.i("Launch panel: %s starts in %s (vr=%b)", appId, mode.key, vrGame)
            onLaunch()
        }
    }

    Dialog(onDismissRequest = onDismiss, properties = DialogProperties(usePlatformDefaultWidth = false)) {
        Surface(
            shape = RoundedCornerShape(20.dp),
            tonalElevation = 6.dp,
            modifier = Modifier
                .fillMaxWidth(0.92f)
                .widthIn(max = 1100.dp)
                .heightIn(max = 680.dp)
                .onPreviewKeyEvent { event ->
                    val down = event.nativeKeyEvent.action == KeyEvent.ACTION_DOWN
                    when (event.nativeKeyEvent.keyCode) {
                        KeyEvent.KEYCODE_BUTTON_Y -> {
                            if (down) onOpenSettings()
                            true
                        }
                        KeyEvent.KEYCODE_BUTTON_B -> {
                            if (down) onDismiss()
                            true
                        }
                        else -> false
                    }
                },
        ) {
            BoxWithConstraints(Modifier.padding(horizontal = 22.dp, vertical = 14.dp)) {
                val twoColumns = maxWidth >= 640.dp
                Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                        Column(Modifier.weight(1f)) {
                            Text(
                                stringResource(R.string.xrgame_launch_header),
                                style = MaterialTheme.typography.labelSmall,
                                color = MaterialTheme.colorScheme.primary,
                            )
                            Text(title, style = MaterialTheme.typography.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        }
                        Text(summary(mode, vrGame), style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.primary)
                    }
                    HorizontalDivider()
                    Column(
                        modifier = Modifier.weight(1f, fill = false).verticalScroll(rememberScrollState()),
                        verticalArrangement = Arrangement.spacedBy(6.dp),
                    ) {
                        val data = draft
                        if (data == null) {
                            Row(horizontalArrangement = Arrangement.spacedBy(12.dp), verticalAlignment = Alignment.CenterVertically) {
                                CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
                                Text(stringResource(R.string.xrgame_launch_loading), style = MaterialTheme.typography.bodySmall)
                            }
                        } else {
                            LaunchOptions(
                                data = data,
                                headset = headset,
                                vrGame = vrGame,
                                mode = mode,
                                statusLayer = statusLayer,
                                frameLimit = frameLimit,
                                advanced = advanced,
                                captureAvailable = captureAvailable,
                                twoColumns = twoColumns,
                                onData = { draft = it },
                                onVrGame = {
                                    vrGame = it
                                    mode = XrGameLaunchMode.fallback(mode, it, headset)
                                },
                                onMode = { mode = it },
                                onStatusLayer = { statusLayer = it },
                                onFrameLimit = { frameLimit = it },
                                onAdvanced = { advanced = it },
                            )
                        }
                    }
                    HorizontalDivider()
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        Text(
                            stringResource(R.string.xrgame_launch_footer),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.weight(1f),
                        )
                        TextButton(onClick = onOpenSettings, modifier = Modifier.focusOutline()) {
                            Text(stringResource(R.string.xrgame_launch_settings) + "  Ⓨ", style = MaterialTheme.typography.labelMedium)
                        }
                        OutlinedButton(onClick = onDismiss, modifier = Modifier.focusOutline()) {
                            Text(stringResource(R.string.xrgame_launch_cancel) + "  Ⓑ", style = MaterialTheme.typography.labelMedium)
                        }
                        Button(
                            onClick = launch,
                            enabled = draft != null && !saving,
                            modifier = Modifier.widthIn(min = 140.dp).focusOutline().focusRequester(launchFocus),
                        ) { Text(stringResource(R.string.xrgame_launch_start) + "  Ⓐ", style = MaterialTheme.typography.labelMedium) }
                    }
                }
            }
        }
    }
}

@Composable
private fun LaunchOptions(
    data: ContainerData,
    headset: Boolean,
    vrGame: Boolean,
    mode: Mode,
    statusLayer: Boolean,
    frameLimit: FrameLimit?,
    advanced: Boolean,
    captureAvailable: Boolean,
    twoColumns: Boolean,
    onData: (ContainerData) -> Unit,
    onVrGame: (Boolean) -> Unit,
    onMode: (Mode) -> Unit,
    onStatusLayer: (Boolean) -> Unit,
    onFrameLimit: (FrameLimit) -> Unit,
    onAdvanced: (Boolean) -> Unit,
) {
    val off = stringResource(R.string.xrgame_launch_off)
    val on = stringResource(R.string.xrgame_launch_on)
    val modes = XrGameLaunchMode.available(headset)
    val modeLabels = mapOf(
        Mode.FLAT to stringResource(R.string.xrgame_launch_mode_2d),
        Mode.SBS to stringResource(R.string.xrgame_launch_mode_sbs),
        Mode.XR to stringResource(R.string.xrgame_launch_mode_xr),
    )
    val vulkanComposite = WindowsVrUpscale.backend(data.envVars) == WindowsVrUpscale.BACKEND_VULKAN
    val upscale = WindowsVrUpscale.fromEnvVars(data.envVars)
    val updateUpscale = { next: WindowsVrUpscale.Settings ->
        onData(data.copy(envVars = WindowsVrUpscale.withSettings(data.envVars, next)))
    }
    val toggle = { label: Int, value: Boolean, set: (Boolean) -> Unit ->
        val item: OptionItem = {
            ChoiceRow(stringResource(label), listOf(off, on), if (value) 1 else 0, onSelect = { set(it == 1) })
        }
        item
    }

    val main = mutableListOf<OptionItem>()
    main += {
        ChoiceRow(
            label = stringResource(R.string.xrgame_launch_mode),
            choices = modes.map { modeLabels.getValue(it) },
            selected = modes.indexOf(mode),
            enabled = { XrGameLaunchMode.allowed(modes[it], vrGame, headset) },
            note = if (vrGame) stringResource(R.string.xrgame_launch_vr_needs_3d) else null,
            onSelect = { onMode(modes[it]) },
        )
    }
    main += {
        ChoiceRow(
            label = stringResource(R.string.xrgame_launch_type),
            choices = listOf(stringResource(R.string.xrgame_launch_type_normal), stringResource(R.string.xrgame_launch_type_vr)),
            selected = if (vrGame) 1 else 0,
            onSelect = { onVrGame(it == 1) },
        )
    }
    if (mode == Mode.XR && !vrGame) {
        main += {
            val presets = XrScreenPresets.PRESETS
            val selected = presets.indexOfFirst { it.key == data.xrScreenPreset }
            ChoiceRow(
                label = stringResource(R.string.xrgame_screen_size),
                choices = presets.map { "${it.diagonalInches}″ · ${it.horizontalDegrees}°" } +
                    if (selected < 0) listOf(stringResource(R.string.xrgame_launch_custom)) else emptyList(),
                selected = if (selected >= 0) selected else presets.size,
                onSelect = { idx -> presets.getOrNull(idx)?.let { onData(data.copy(xrScreenPreset = it.key)) } },
            )
        }
        main += {
            // The standard sizes up to 1920x1080, plus the saved one when it is a custom size.
            val sizes = stringArrayResource(R.array.screen_size_entries).drop(1)
                .filter { XrGameResolution.fits(it.substringBefore(' ')) }
            val saved = XrGameResolution.capped(data.screenSize)
            val index = sizes.indexOfFirst { it.substringBefore(' ') == saved }
            val choices = if (index >= 0) sizes else sizes + "$saved (${stringResource(R.string.xrgame_launch_custom)})"
            // Where the Wine screen falls in what the XR compositor can show of this screen.
            val display = remember { WindowsVrUpscale.Display.decode(PrefManager.xrDisplayInfo) }
            val preset = XrScreenPresets.byKey(data.xrScreenPreset) ?: XrScreenPresets.DEFAULT
            val wineWidth = XrGameResolution.parse(saved)?.first
            val range = if (display != null && wineWidth != null) {
                val low = WindowsVrUpscale.screenCoverage(display, WindowsVrUpscale.EyeBuffer.RECOMMENDED, preset.widthMeters, preset.distanceMeters)
                val high = WindowsVrUpscale.screenCoverage(display, WindowsVrUpscale.EyeBuffer.MAXIMUM, preset.widthMeters, preset.distanceMeters)
                stringResource(
                    R.string.xrgame_launch_screen_range,
                    "${low}x${low * 9 / 16}",
                    "${high}x${high * 9 / 16}",
                    stringResource(
                        when {
                            wineWidth < low -> R.string.xrgame_launch_screen_below
                            wineWidth > high -> R.string.xrgame_launch_screen_above
                            else -> R.string.xrgame_launch_screen_inside
                        },
                    ),
                )
            } else {
                stringResource(R.string.xrgame_launch_screen_unknown)
            }
            ChoiceRow(
                label = stringResource(R.string.xrgame_launch_resolution),
                choices = choices,
                selected = if (index >= 0) index else sizes.size,
                note = if (saved != data.screenSize) {
                    stringResource(R.string.xrgame_launch_resolution_capped, data.screenSize) + "\n" + range
                } else {
                    range
                },
                onSelect = { idx -> onData(data.copy(screenSize = choices[idx].substringBefore(' '))) },
            )
        }
        main += toggle(R.string.xrgame_vst, data.xrPassthrough) { onData(data.copy(xrPassthrough = it)) }
        main += toggle(R.string.xrgame_curved_screen, data.xrCurvedScreen) { onData(data.copy(xrCurvedScreen = it)) }
    }
    if (mode == Mode.XR) {
        main += {
            ChoiceRow(
                label = stringResource(R.string.xrgame_launch_refresh),
                choices = REFRESH_RATES.map { "$it Hz" },
                selected = REFRESH_RATES.indexOf(data.xrRefreshRate),
                onSelect = { onData(data.copy(xrRefreshRate = REFRESH_RATES[it])) },
            )
        }
    }
    // Reconstruction runs on the Vulkan composite, so it reads as off on GLES and choosing a
    // filter switches the game to Vulkan. VR games reconstruct their eyes, the big screen its quad.
    val reconstructs = WindowsVrUpscale.compositeBackend(data.envVars, vrGame) == WindowsVrUpscale.BACKEND_VULKAN
    val filter = if (reconstructs) upscale.filter else WindowsVrUpscale.Filter.OFF
    val upscaling = mode == Mode.XR && filter != WindowsVrUpscale.Filter.OFF
    if (mode == Mode.XR) {
        main += {
            ChoiceRow(
                label = stringResource(R.string.xrgame_launch_upscale),
                choices = listOf(stringResource(R.string.xrgame_vr_upscale_off), "FSR1", "SGSR"),
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
        }
    }
    if (upscaling && vrGame) {
        main += {
            // Eye sizes from the eye views the last XR session recorded.
            val display = remember { WindowsVrUpscale.Display.decode(PrefManager.xrDisplayInfo) }
            val tier = upscale.eyeBuffer
            ChoiceRow(
                label = stringResource(R.string.xrgame_launch_eye_buffer),
                choices = listOf(
                    stringResource(R.string.xrgame_launch_recommended),
                    stringResource(R.string.xrgame_launch_eye_buffer_high),
                    stringResource(R.string.xrgame_launch_eye_buffer_max),
                ),
                selected = tier.ordinal,
                note = display?.let { WindowsVrUpscale.eyeExtent(it, tier) }
                    ?.let { (w, h) -> stringResource(R.string.xrgame_launch_eye_buffer_eye, "${w}x$h") },
                // A tier is the whole eye size; the older percentage of it no longer applies.
                onSelect = { updateUpscale(upscale.copy(eyeBuffer = WindowsVrUpscale.EyeBuffer.entries[it], outputPercent = 100)) },
            )
        }
    }
    if (upscaling && !vrGame) {
        main += {
            // The layer follows the tier within the screen's effective range; the filter runs only
            // when the Wine screen is smaller than the layer.
            val display = remember { WindowsVrUpscale.Display.decode(PrefManager.xrDisplayInfo) }
            val preset = XrScreenPresets.byKey(data.xrScreenPreset) ?: XrScreenPresets.DEFAULT
            val tier = upscale.eyeBuffer
            val wine = XrGameResolution.parse(XrGameResolution.capped(data.screenSize))
            val layer = wine?.let { (w, h) ->
                WindowsVrUpscale.screenSwapchain(w, h, tier, display, preset.widthMeters, preset.distanceMeters)
            }
            ChoiceRow(
                label = stringResource(R.string.xrgame_launch_screen_layer),
                choices = listOf(
                    stringResource(R.string.xrgame_launch_recommended),
                    stringResource(R.string.xrgame_launch_eye_buffer_high),
                    stringResource(R.string.xrgame_launch_eye_buffer_max),
                ),
                selected = tier.ordinal,
                note = when {
                    display == null || wine == null || layer == null -> stringResource(R.string.xrgame_launch_screen_unknown)
                    layer == wine -> stringResource(R.string.xrgame_launch_screen_layer_same, "${wine.first}x${wine.second}")
                    else -> stringResource(R.string.xrgame_launch_screen_layer_note, "${wine.first}x${wine.second}",
                        "${layer.first}x${layer.second}")
                },
                onSelect = { updateUpscale(upscale.copy(eyeBuffer = WindowsVrUpscale.EyeBuffer.entries[it], outputPercent = 100)) },
            )
        }
    }
    if (upscaling) {
        main += {
            ChoiceRow(
                label = stringResource(R.string.xrgame_launch_foveation),
                choices = listOf(
                    stringResource(R.string.xrgame_vr_foveation_off),
                    stringResource(R.string.xrgame_vr_foveation_fixed),
                    stringResource(R.string.xrgame_vr_foveation_eye),
                ),
                selected = upscale.foveation.ordinal,
                onSelect = { updateUpscale(upscale.copy(foveation = WindowsVrUpscale.Foveation.entries[it])) },
            )
        }
        if (upscale.foveation != WindowsVrUpscale.Foveation.OFF) {
            main += {
                ChoiceRow(
                    label = stringResource(R.string.xrgame_vr_foveation_level),
                    choices = listOf(
                        stringResource(R.string.xrgame_vr_foveation_low),
                        stringResource(R.string.xrgame_vr_foveation_balanced),
                        stringResource(R.string.xrgame_vr_foveation_high),
                    ),
                    selected = upscale.level.ordinal,
                    onSelect = { updateUpscale(upscale.copy(level = WindowsVrUpscale.Level.entries[it])) },
                )
            }
        }
    }
    if (frameLimit != null) {
        main += {
            // The game's own frame rate, capped by the X server as the quick menu's FPS limiter
            // does; the saved target stays selectable when it is not one of the presets.
            val (enabled, target) = frameLimit
            val steps = (FRAME_LIMITS + if (enabled) listOf(target) else emptyList()).distinct().sorted()
            ChoiceRow(
                label = stringResource(R.string.xrgame_launch_frame_limit),
                choices = listOf(stringResource(R.string.performance_hud_fps_limiter_off)) +
                    steps.map { stringResource(R.string.performance_hud_fps_limiter_value, it) },
                selected = if (enabled) 1 + steps.indexOf(target) else 0,
                note = stringResource(R.string.xrgame_launch_frame_limit_note),
                onSelect = { idx ->
                    onFrameLimit(if (idx == 0) frameLimit.copy(enabled = false) else FrameLimit(true, steps[idx - 1]))
                },
            )
        }
    }
    if (vrGame) {
        main += {
            val scaleIndex = if (data.xrRenderScaleRecommended) {
                0
            } else {
                1 + RENDER_SCALES.indices.minBy { kotlin.math.abs(RENDER_SCALES[it] - data.xrRenderScale) }
            }
            // The eye size the game renders at, from the eye views the last XR session recorded,
            // as the Windows VR runtime hands it out.
            val display = remember { WindowsVrUpscale.Display.decode(PrefManager.xrDisplayInfo) }
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
            val high = stringResource(R.string.xrgame_launch_eye_buffer_high)
            val limit = stringResource(R.string.xrgame_launch_eye_buffer_max)
            ChoiceRow(
                label = stringResource(R.string.xrgame_launch_render_scale),
                choices = listOf(stringResource(R.string.xrgame_launch_recommended)) + RENDER_SCALES.map {
                    when (it) {
                        XrResolutionRecommendation.HIGH_PERCENT -> high
                        XrResolutionRecommendation.LIMIT_PERCENT -> limit
                        else -> "$it%"
                    }
                },
                selected = scaleIndex,
                note = eye?.let { (w, h) -> stringResource(R.string.xrgame_launch_eye_buffer_eye, "${w}x$h") },
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
    }
    if (!vrGame && mode != Mode.SBS) {
        // The SBS cinema always uses the trackpad; elsewhere a tap can land where it is.
        main += {
            ChoiceRow(
                label = stringResource(R.string.xrgame_launch_touch),
                choices = listOf(stringResource(R.string.xrgame_launch_touch_direct), stringResource(R.string.xrgame_launch_touch_trackpad)),
                selected = if (data.touchscreenMode) 0 else 1,
                onSelect = { onData(data.copy(touchscreenMode = it == 0)) },
            )
        }
    }
    if (mode == Mode.XR) {
        // The world-locked panel of XR sessions; one switch for every game.
        main += toggle(R.string.xrgame_launch_status_layer, statusLayer, onStatusLayer)
    }
    main += {
        // Done in the D3D translation layers; the game keeps its own texture sizes.
        val qualities = XrGameQualitySettings.TEXTURE_QUALITIES
        val quality = XrGameQualitySettings.textureQuality(data.envVars)
        ChoiceRow(
            label = stringResource(R.string.xrgame_launch_texture_quality),
            choices = listOf(
                stringResource(R.string.xrgame_launch_texture_quality_high),
                stringResource(R.string.xrgame_launch_texture_quality_medium),
                stringResource(R.string.xrgame_launch_texture_quality_low),
            ),
            selected = qualities.indexOf(quality),
            note = XrGameQualitySettings.textureCap(quality)?.let { stringResource(R.string.xrgame_launch_texture_quality_note, it) },
            onSelect = { onData(data.copy(envVars = XrGameQualitySettings.withTextureQuality(data.envVars, qualities[it]))) },
        )
    }

    val extra = mutableListOf<OptionItem>()
    if (mode == Mode.XR && vrGame) {
        extra += {
            ChoiceRow(
                label = stringResource(R.string.xrgame_vr_composite),
                choices = listOf(stringResource(R.string.xrgame_vr_composite_gles), stringResource(R.string.xrgame_vr_composite_vulkan)),
                selected = if (vulkanComposite) 1 else 0,
                onSelect = { onData(data.copy(envVars = WindowsVrUpscale.withBackend(data.envVars, WindowsVrUpscale.BACKENDS[it]))) },
            )
        }
    }
    if (upscaling) {
        extra += {
            val steps = WindowsVrUpscale.SHARPNESS_STEPS
            ChoiceRow(
                label = stringResource(R.string.xrgame_vr_sharpness),
                choices = steps.map { "$it%" },
                selected = steps.indices.minBy { kotlin.math.abs(steps[it] - upscale.sharpness) },
                onSelect = { updateUpscale(upscale.copy(sharpness = steps[it])) },
            )
        }
    }
    if (vrGame) {
        extra += toggle(R.string.xr_open_composite_toggle, data.openCompositeEnabled) { onData(data.copy(openCompositeEnabled = it)) }
    }
    extra += {
        // The video memory D3D games are told they have; they size textures and caches from it.
        val steps = XrGameMemoryBudget.STEPS_MB
        val budget = XrGameMemoryBudget.fromConfig(data.dxwrapperConfig)
        ChoiceRow(
            label = stringResource(R.string.xrgame_launch_memory_budget),
            choices = steps.map { "${it / 1024}G" },
            selected = steps.indices.minBy { kotlin.math.abs(steps[it] - budget) },
            note = stringResource(R.string.xrgame_launch_memory_budget_note),
            onSelect = { onData(data.copy(dxwrapperConfig = XrGameMemoryBudget.withBudget(data.dxwrapperConfig, steps[it]))) },
        )
    }
    extra += toggle(R.string.xrgame_async_present, XrGamePresentSettings.asyncCopyEnabled(data.envVars)) {
        onData(data.copy(envVars = XrGamePresentSettings.withAsyncCopy(data.envVars, it)))
    }
    extra += toggle(R.string.xrgame_render_ahead, XrGamePresentSettings.renderAheadEnabled(data.envVars)) {
        onData(data.copy(envVars = XrGamePresentSettings.withRenderAhead(data.envVars, it)))
    }
    if (captureAvailable) {
        extra += {
            ChoiceRow(
                label = stringResource(R.string.xrgame_api_capture),
                choices = listOf(
                    stringResource(R.string.xrgame_api_capture_off),
                    stringResource(R.string.xrgame_api_capture_d3d12),
                    stringResource(R.string.xrgame_api_capture_vulkan),
                ),
                selected = XrGameApiCapture.MODES.indexOf(XrGameApiCapture.mode(data.envVars)),
                onSelect = { onData(data.copy(envVars = XrGameApiCapture.withMode(data.envVars, XrGameApiCapture.MODES[it]))) },
            )
        }
    }

    OptionColumns(main, twoColumns)
    TextButton(onClick = { onAdvanced(!advanced) }, modifier = Modifier.focusOutline()) {
        Text(stringResource(R.string.xrgame_launch_advanced) + if (advanced) "  ▾" else "  ▸", style = MaterialTheme.typography.labelMedium)
    }
    if (advanced) OptionColumns(extra, twoColumns)
}

/** Options in two columns, filled top to bottom on the left first; one column when narrow. */
@Composable
private fun OptionColumns(items: List<OptionItem>, twoColumns: Boolean) {
    if (!twoColumns) {
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) { items.forEach { it() } }
        return
    }
    val split = (items.size + 1) / 2
    Row(horizontalArrangement = Arrangement.spacedBy(28.dp)) {
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) { items.take(split).forEach { it() } }
        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) { items.drop(split).forEach { it() } }
    }
}

/**
 * One option on one line: its label, then the choices. Up to [MAX_BUTTONS] choices show as
 * buttons; longer lists collapse into a dropdown that shows the selected one.
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ChoiceRow(
    label: String,
    choices: List<String>,
    selected: Int,
    onSelect: (Int) -> Unit,
    enabled: (Int) -> Boolean = { true },
    note: String? = null,
) {
    val pick = { index: Int -> if (index != selected) onSelect(index) }
    Column(Modifier.fillMaxWidth()) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text(
                label,
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 2,
                modifier = Modifier.width(LABEL_WIDTH).padding(end = 8.dp),
            )
            if (choices.size > MAX_BUTTONS) {
                ChoiceDropdown(choices, selected, enabled, pick)
            } else {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    choices.forEachIndexed { index, text ->
                        ChoiceButton(text, selected = index == selected, enabled = enabled(index), onClick = { pick(index) })
                    }
                }
            }
        }
        if (note != null) {
            Text(
                note,
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = LABEL_WIDTH, top = 2.dp),
            )
        }
    }
}

/** The selected choice with a ▾; the menu lists every choice and ticks the selected one. */
@Composable
private fun ChoiceDropdown(choices: List<String>, selected: Int, enabled: (Int) -> Boolean, onSelect: (Int) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    var focused by remember { mutableStateOf(false) }
    val colors = MaterialTheme.colorScheme
    Box {
        Surface(
            shape = RoundedCornerShape(8.dp),
            color = colors.surfaceVariant,
            contentColor = colors.onSurface,
            border = if (focused) BorderStroke(3.dp, colors.tertiary) else BorderStroke(1.dp, colors.outline),
            modifier = Modifier
                .heightIn(min = CONTROL_HEIGHT)
                .widthIn(min = 132.dp)
                .onFocusChanged { focused = it.isFocused }
                .clickable(role = Role.DropdownList) { expanded = true },
        ) {
            Row(
                modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text(
                    choices.getOrElse(selected) { "" },
                    style = MaterialTheme.typography.labelMedium,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier.weight(1f, fill = false),
                )
                Text("▾", style = MaterialTheme.typography.labelMedium)
            }
        }
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            choices.forEachIndexed { index, text ->
                DropdownMenuItem(
                    text = {
                        Text(
                            text,
                            style = MaterialTheme.typography.labelMedium,
                            fontWeight = if (index == selected) FontWeight.Bold else FontWeight.Normal,
                        )
                    },
                    leadingIcon = {
                        if (index == selected) Icon(Icons.Default.Check, contentDescription = null, modifier = Modifier.size(18.dp))
                    },
                    enabled = enabled(index),
                    onClick = {
                        expanded = false
                        onSelect(index)
                    },
                )
            }
        }
    }
}

/**
 * A choice that reads at a glance in a headset: the selected one is filled with the primary colour
 * and ticked, the others are flat, and the gamepad focus draws a thick outline.
 */
@Composable
private fun ChoiceButton(text: String, selected: Boolean, enabled: Boolean, onClick: () -> Unit) {
    var focused by remember { mutableStateOf(false) }
    val colors = MaterialTheme.colorScheme
    val container = if (selected) colors.primary else colors.surfaceVariant
    val content = if (selected) colors.onPrimary else colors.onSurfaceVariant
    Surface(
        shape = RoundedCornerShape(8.dp),
        color = if (enabled) container else container.copy(alpha = 0.35f),
        contentColor = if (enabled) content else content.copy(alpha = 0.45f),
        border = when {
            focused -> BorderStroke(3.dp, colors.tertiary)
            selected -> null
            else -> BorderStroke(1.dp, colors.outlineVariant)
        },
        modifier = Modifier
            .heightIn(min = CONTROL_HEIGHT)
            .onFocusChanged { focused = it.isFocused }
            .selectable(selected = selected, enabled = enabled, role = Role.RadioButton, onClick = onClick),
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            if (selected) Icon(Icons.Default.Check, contentDescription = null, modifier = Modifier.size(14.dp))
            Text(
                text,
                style = MaterialTheme.typography.labelMedium,
                fontWeight = if (selected) FontWeight.Bold else FontWeight.Normal,
            )
        }
    }
}

/** A thick outline while focused, so the gamepad focus on a button is plain to see. */
@Composable
private fun Modifier.focusOutline(): Modifier {
    var focused by remember { mutableStateOf(false) }
    val outline = if (focused) Modifier.border(3.dp, MaterialTheme.colorScheme.tertiary, RoundedCornerShape(24.dp)) else Modifier
    return then(outline).onFocusChanged { focused = it.isFocused }
}

@Composable
private fun summary(mode: Mode, vrGame: Boolean): String = stringResource(
    when (mode) {
        Mode.FLAT -> R.string.xrgame_launch_summary_2d
        Mode.SBS -> if (vrGame) R.string.xrgame_launch_summary_sbs_vr else R.string.xrgame_launch_summary_sbs_cinema
        Mode.XR -> if (vrGame) R.string.xrgame_launch_summary_xr_vr else R.string.xrgame_launch_summary_xr_screen
    },
)

private fun gameTitle(appId: String): String = runCatching {
    if (ContainerUtils.extractGameSourceFromContainerId(appId) == GameSource.STEAM) {
        SteamService.getAppInfoOf(ContainerUtils.extractGameIdFromContainerId(appId))?.name
    } else {
        null
    }
}.getOrNull()?.takeIf { it.isNotBlank() } ?: appId
