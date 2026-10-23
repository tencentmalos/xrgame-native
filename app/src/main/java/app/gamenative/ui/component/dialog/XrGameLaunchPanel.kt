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
import app.gamenative.ui.screen.xserver.initialFpsLimiterEnabled
import app.gamenative.ui.screen.xserver.initialFpsLimiterTarget
import app.gamenative.utils.ContainerUtils
import app.gamenative.xrgame.XrGameApiCapture
import app.gamenative.xrgame.XrGameGuestPatches
import app.gamenative.xrgame.XrGameLaunchMode
import app.gamenative.xrgame.XrGameLaunchMode.Mode
import app.gamenative.xrgame.XrGameLaunchOptions
import com.winlator.container.ContainerData
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import timber.log.Timber

private val LABEL_WIDTH = 100.dp
private val CONTROL_HEIGHT = 30.dp
private const val MAX_BUTTONS = 3

private typealias FrameLimit = XrGameLaunchOptions.FrameLimit

private typealias OptionItem = @Composable () -> Unit

/**
 * XRGame's launch panel, shown when a game is started from the library: the display mode (2D,
 * SBS, and XR on headsets) and the options that mode uses, in two columns between a title line
 * and a bottom bar with the full settings, cancel and launch. Changes are a draft until launch,
 * which saves them to the game's container; cancel drops them. Shortcuts, external intents and
 * DebugBus launches skip the panel and use the saved settings. Gamepads: A activates the focused
 * control, B cancels, Y opens the full settings. With [followsEntry] (launches Official Steam
 * handed over) the display mode and game type are not choices: they come from the home page's
 * entry and the game's VR flag, written before the panel opens.
 */
@Composable
fun XrGameLaunchPanel(
    appId: String,
    followsEntry: Boolean = false,
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
    // This game's guest patch packages; empty for other sources or when there are none.
    var guestPatches by remember(appId) { mutableStateOf(emptyList<XrGameGuestPatches.Package>()) }
    val launchFocus = remember { FocusRequester() }

    LaunchedEffect(appId) {
        val (data, name, limit) = withContext(Dispatchers.IO) {
            val container = ContainerUtils.getOrCreateContainer(context, appId)
            Triple(
                ContainerUtils.toContainerData(container),
                XrGameLaunchOptions.title(appId),
                FrameLimit(initialFpsLimiterEnabled(container), initialFpsLimiterTarget(container)),
            )
        }
        title = name
        guestPatches = withContext(Dispatchers.IO) {
            if (!appId.startsWith("STEAM_")) emptyList()
            else runCatching { XrGameGuestPatches.list(ContainerUtils.extractGameIdFromContainerId(appId)).packages }
                .onFailure { Timber.w(it, "Launch panel: listing guest patches of %s failed", appId) }
                .getOrDefault(emptyList())
        }
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
        scope.launch {
            withContext(Dispatchers.IO) {
                XrGameLaunchOptions.save(context, appId, initial, data, mode, vrGame, initialFrameLimit, frameLimit, statusLayer, followsEntry)
            }
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
                        Text(XrGameLaunchOptions.summary(context, mode, vrGame), style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.primary)
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
                                choosesMode = !followsEntry,
                                statusLayer = statusLayer,
                                frameLimit = frameLimit,
                                guestPatches = guestPatches,
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
    choosesMode: Boolean,
    statusLayer: Boolean,
    frameLimit: FrameLimit?,
    guestPatches: List<XrGameGuestPatches.Package>,
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
    val context = LocalContext.current
    val (main, extra) = XrGameLaunchOptions.rows(
        context,
        XrGameLaunchOptions.Draft(data, headset, vrGame, mode, choosesMode, statusLayer, frameLimit, guestPatches, captureAvailable),
        XrGameLaunchOptions.Changes(onData, onVrGame, onMode, onStatusLayer, onFrameLimit),
    )
    val item = { row: XrGameLaunchOptions.Row ->
        val option: OptionItem = { ChoiceRow(row.label, row.choices, row.selected, row.onSelect, row.enabled, row.note) }
        option
    }
    OptionColumns(main.map(item), twoColumns)
    TextButton(onClick = { onAdvanced(!advanced) }, modifier = Modifier.focusOutline()) {
        Text(stringResource(R.string.xrgame_launch_advanced) + if (advanced) "  ▾" else "  ▸", style = MaterialTheme.typography.labelMedium)
    }
    if (advanced) OptionColumns(extra.map(item), twoColumns)
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
