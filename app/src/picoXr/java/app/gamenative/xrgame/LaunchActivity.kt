// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame

import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.os.Bundle
import android.view.KeyEvent
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Apps
import androidx.compose.material.icons.filled.ArrowForward
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.draw.clip
import androidx.compose.foundation.selection.toggleable
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.delay
import kotlinx.coroutines.withTimeout
import java.util.Locale
import app.gamenative.xrgame.steam.LinuxSteamService
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import app.gamenative.MainActivity
import app.gamenative.R
import app.gamenative.ui.icons.Steam
import app.gamenative.ui.theme.PluviaTheme
import app.gamenative.xrgame.steam.LinuxSteamActivity
import app.gamenative.xrgame.steam.SteamClientDisplay
import app.gamenative.ui.screen.xr.XrScreenPresets

/**
 * Home: "Steam" opens the official client (the default entry); "Other" opens the existing library
 * for other stores and custom apps, where the JavaSteam library stays as a compatibility mode.
 * Both keep the existing app identity and data. On a headset the top row picks how Steam shows,
 * as a 2D window or on the XR big screen; Steam's ordinary games follow that choice. With the big
 * screen, two rows below set its size and curve (shared with those games), passthrough behind
 * Steam's page and Steam's own resolution (games keep theirs). The page is always in English.
 */
class LaunchActivity : ComponentActivity() {
    companion object {
        /** Steam's XR page hides this page's task while the big screen runs and brings it back after. */
        @Volatile var current: java.lang.ref.WeakReference<LaunchActivity>? = null
    }

    override fun attachBaseContext(base: Context) {
        val english = Configuration(base.resources.configuration).apply { setLocale(Locale.ENGLISH) }
        super.attachBaseContext(base.createConfigurationContext(english))
    }

    private var switching by mutableStateOf(false)
    private var display by mutableStateOf(XrGameOfficialSteam.display)
    private var xrScreen by mutableStateOf(XrGameOfficialSteam.XrScreen(XrScreenPresets.DEFAULT.scale, XrScreenPresets.DEFAULT.distanceMeters, false))
    private var xrResolution by mutableStateOf(SteamClientDisplay.XR_DEFAULT_RESOLUTION)
    private var xrPassthrough by mutableStateOf(false)
    /** The XR sessions' status layer, Steam's and every game's (the launch panels show it too). */
    private var statusLayer by mutableStateOf(true)
    /** Shown under the big screen's options while the client starts again at its resolution. */
    private var restarting by mutableStateOf<String?>(null)
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_A) {
            return super.dispatchKeyEvent(KeyEvent(event.downTime, event.eventTime, event.action,
                KeyEvent.KEYCODE_ENTER, event.repeatCount, event.metaState, event.deviceId, 0, event.flags, event.source))
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onResume() {
        super.onResume()
        // A game or Steam's page may have resized the shared screen, or switched Steam to 2D, since.
        display = XrGameOfficialSteam.display
        xrScreen = XrGameOfficialSteam.xrScreen
        xrResolution = SteamClientDisplay.xrResolution(this)
        xrPassthrough = XrGameOfficialSteam.xrPassthrough
        statusLayer = app.gamenative.PrefManager.xrgameStatusLayer
    }

    override fun onDestroy() {
        if (current?.get() === this) current = null
        super.onDestroy()
    }

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        current = java.lang.ref.WeakReference(this)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowCompat.getInsetsController(window, window.decorView).hide(WindowInsetsCompat.Type.systemBars())
        setContent {
            PluviaTheme {
                Box(Modifier.fillMaxSize().background(Brush.linearGradient(listOf(Color(0xFF08111D), Color(0xFF14243A))))) {
                    // Only the cutout is stable: hidden system bars would shift the page when they hide.
                    Column(Modifier.windowInsetsPadding(WindowInsets.displayCutout).padding(horizontal = 36.dp, vertical = 24.dp),
                        verticalArrangement = Arrangement.Center) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Image(painterResource(R.drawable.ic_launcher_xr_foreground), null, Modifier.size(42.dp))
                            Text("SteamPSP", color = Color.White, style = MaterialTheme.typography.titleLarge)
                            Spacer(Modifier.weight(1f))
                            if (MainActivity.isHeadset(this@LaunchActivity)) DisplayChoice()
                        }
                        if (MainActivity.isHeadset(this@LaunchActivity) && display == XrGameOfficialSteam.Display.XR) XrScreenChoice()
                        Spacer(Modifier.height(24.dp))
                        BoxWithConstraints(Modifier.weight(1f)) {
                            if (maxWidth >= 600.dp) {
                                Row(horizontalArrangement = Arrangement.spacedBy(24.dp)) {
                                    ModeCard(true, Modifier.weight(1.35f).fillMaxHeight())
                                    ModeCard(false, Modifier.weight(1f).fillMaxHeight())
                                }
                            } else {
                                Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
                                    ModeCard(true, Modifier.weight(1.35f).fillMaxWidth())
                                    ModeCard(false, Modifier.weight(1f).fillMaxWidth())
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /** 2D window or XR big screen for Steam; SBS is not offered. */
    @Composable
    private fun DisplayChoice() {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(R.string.steam_display_label), color = Color.White.copy(alpha = .75f),
                style = MaterialTheme.typography.bodyMedium)
            for ((choice, label) in listOf(XrGameOfficialSteam.Display.FLAT to R.string.steam_display_2d,
                XrGameOfficialSteam.Display.XR to R.string.steam_display_xr)) {
                OptionChip(stringResource(label), display == choice) { display = choice; XrGameOfficialSteam.display = choice }
            }
        }
    }

    /**
     * Two rows under the title, each a one-of-many choice and, at the right edge, an on/off switch
     * set apart from it: the big screen's size and curve (shared with Steam's games), then Steam's
     * own resolution and passthrough behind its page. The note below ends in the status layer's
     * switch, shared with every game.
     */
    @Composable
    private fun XrScreenChoice() {
        val preset = XrScreenPresets.match(xrScreen.scale, xrScreen.distance)
        Column(Modifier.fillMaxWidth().padding(top = 12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            OptionRow(R.string.steam_xr_screen_size, trailing = {
                OptionSwitch(R.string.steam_xr_curved, xrScreen.curved) {
                    xrScreen = xrScreen.copy(curved = it)
                    XrGameOfficialSteam.xrScreen = xrScreen
                }
            }) {
                for (choice in XrScreenPresets.PRESETS) OptionChip("${choice.diagonalInches}″", choice == preset) {
                    xrScreen = xrScreen.copy(scale = choice.scale, distance = choice.distanceMeters)
                    XrGameOfficialSteam.xrScreen = xrScreen
                }
                if (preset == null) OptionChip(stringResource(R.string.steam_xr_custom), true) {}
            }
            OptionRow(R.string.steam_xr_resolution, trailing = {
                OptionSwitch(R.string.steam_xr_menu_passthrough, xrPassthrough) {
                    xrPassthrough = it
                    XrGameOfficialSteam.xrPassthrough = it
                }
            }) {
                for (size in SteamClientDisplay.XR_RESOLUTIONS) OptionChip(SteamClientDisplay.label(size).replace('x', '×'), size == xrResolution) {
                    xrResolution = size
                    SteamClientDisplay.setXrResolution(this@LaunchActivity, size)
                }
            }
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                restarting?.let { Text(it, Modifier.weight(1f), color = Color(0xFF6AC9FF), style = MaterialTheme.typography.bodySmall) }
                    ?: Text(stringResource(R.string.steam_xr_screen_note), Modifier.weight(1f), color = Color.White.copy(alpha = .55f),
                        style = MaterialTheme.typography.bodySmall)
                OptionSwitch(R.string.steam_xr_status_layer, statusLayer) {
                    statusLayer = it
                    app.gamenative.PrefManager.xrgameStatusLayer = it
                }
            }
        }
    }

    /**
     * A labelled row of choices and its switch: the labels share one column and the switches
     * the right edge, so the rows line up.
     */
    @Composable
    private fun OptionRow(label: Int, trailing: @Composable () -> Unit, content: @Composable RowScope.() -> Unit) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(label), Modifier.width(136.dp), color = Color.White.copy(alpha = .75f),
                style = MaterialTheme.typography.bodyMedium)
            content()
            Spacer(Modifier.weight(1f))
            trailing()
        }
    }

    /** An on/off setting: its name and a switch, the whole of it one target for the ray and the D-pad. */
    @Composable
    private fun OptionSwitch(label: Int, checked: Boolean, onChange: (Boolean) -> Unit) {
        Row(
            Modifier.clip(RoundedCornerShape(20.dp)).toggleable(value = checked, role = Role.Switch, onValueChange = onChange)
                .padding(horizontal = 10.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Text(stringResource(label), color = Color.White.copy(alpha = .85f), style = MaterialTheme.typography.bodyMedium)
            Switch(
                checked = checked,
                onCheckedChange = null,
                colors = SwitchDefaults.colors(checkedTrackColor = Color(0xFF23669A), checkedThumbColor = Color.White),
            )
        }
    }

    @Composable
    private fun OptionChip(text: String, selected: Boolean, onClick: () -> Unit) {
        FilterChip(
            selected = selected,
            onClick = onClick,
            label = { Text(text) },
            colors = FilterChipDefaults.filterChipColors(
                labelColor = Color.White.copy(alpha = .8f),
                selectedContainerColor = Color(0xFF23669A),
                selectedLabelColor = Color.White,
            ),
        )
    }

    @Composable
    private fun ModeCard(official: Boolean, modifier: Modifier) {
        var focused by remember { mutableStateOf(false) }
        // Steam is the default: it takes the first focus for controllers and the D-pad.
        val focus = remember { FocusRequester() }
        if (official) LaunchedEffect(Unit) { runCatching { focus.requestFocus() } }
        val accent = if (official) Color(0xFF6AC9FF) else Color(0xFF7EF1D8)
        Card(onClick = {
            if (!switching) lifecycleScope.launch {
                switching = true
                try {
                    check(app.gamenative.PluviaApp.xEnvironment == null && app.gamenative.service.ActiveGameRegistry.get() == null) {
                        "Exit the running game before switching Steam modes."
                    }
                    val xr = MainActivity.isHeadset(this@LaunchActivity) && display == XrGameOfficialSteam.Display.XR
                    if (official) {
                        // The client's X server keeps its size for the whole run: a running client at
                        // another size than the big screen's choice starts again.
                        val wanted = if (xr) SteamClientDisplay.xrResolution(this@LaunchActivity) else null
                        val shown = LinuxSteamService.xServerState.value?.screenInfo?.let { it.width.toInt() to it.height.toInt() }
                        if (wanted != null && shown != null && shown != wanted && LinuxSteamService.running) {
                            restarting = getString(R.string.steam_xr_restarting, SteamClientDisplay.label(wanted))
                            try {
                                stopService(Intent(this@LaunchActivity, LinuxSteamService::class.java))
                                withTimeout(30000) { while (LinuxSteamService.running) delay(100) }
                            } finally { restarting = null }
                        }
                        startForegroundService(Intent(this@LaunchActivity, LinuxSteamService::class.java))
                    } else {
                        stopService(Intent(this@LaunchActivity, LinuxSteamService::class.java))
                        withTimeout(15000) { while (XrGameOfficialSteam.ownsLibrary || LinuxSteamService.running) delay(100) }
                        XrGameOfficialSteam.selectCustom()
                        startForegroundService(Intent(this@LaunchActivity, app.gamenative.service.SteamService::class.java))
                    }
                    // Restore Custom's entry page only after its client owns the session.
                    val page = when {
                        !official -> MainActivity::class.java
                        xr -> app.gamenative.xrgame.steam.SteamXrActivity::class.java
                        else -> LinuxSteamActivity::class.java
                    }
                    // One page draws the client's X server at a time: the other display's page goes.
                    if (official) (if (xr) LinuxSteamActivity.current else app.gamenative.xrgame.steam.SteamXrActivity.current)
                        ?.get()?.finish()
                    startActivity(Intent(this@LaunchActivity, page)
                        .addFlags(if (official) Intent.FLAG_ACTIVITY_REORDER_TO_FRONT else Intent.FLAG_ACTIVITY_CLEAR_TOP))
                } catch (e: Exception) {
                    android.app.AlertDialog.Builder(this@LaunchActivity).setMessage(e.message ?: "Steam mode switch failed")
                        .setPositiveButton(android.R.string.ok, null).show()
                } finally { switching = false }
            }
        }, modifier = modifier.focusRequester(focus).onFocusChanged { focused = it.hasFocus }, shape = RoundedCornerShape(24.dp),
            border = BorderStroke(if (focused) 3.dp else 1.dp, if (focused) accent else accent.copy(alpha = .25f))) {
            Box(Modifier.fillMaxSize().background(Brush.linearGradient(if (official)
                listOf(Color(0xFF23669A), Color(0xFF10263E)) else listOf(Color(0xFF226260), Color(0xFF172B40))))) {
                Canvas(Modifier.fillMaxSize()) {
                    val center = Offset(size.width * .83f, size.height * .28f)
                    for (i in 1..4) drawCircle(accent.copy(alpha = .09f), size.width * (.2f + i * .14f), center, style = Stroke(2.dp.toPx()))
                    drawCircle(Brush.radialGradient(listOf(accent.copy(alpha = .2f), Color.Transparent), center, size.width * .8f), size.width * .8f, center)
                }
                if (official) Icon(Icons.Filled.Steam, null, tint = Color.White,
                    modifier = Modifier.align(Alignment.Center).padding(bottom = 40.dp).size(108.dp))
                else Icon(Icons.Filled.Apps, null, tint = Color.White,
                    modifier = Modifier.align(Alignment.Center).padding(bottom = 40.dp).size(96.dp))
                Row(Modifier.align(Alignment.BottomStart).fillMaxWidth().padding(26.dp), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(stringResource(if (official) R.string.steam_mode_steam_title else R.string.steam_mode_other_title),
                            color = Color.White, style = MaterialTheme.typography.headlineSmall)
                        Spacer(Modifier.height(4.dp))
                        Text(stringResource(if (official) R.string.steam_mode_official_short else R.string.steam_mode_other_short),
                            color = Color.White.copy(alpha = .75f), style = MaterialTheme.typography.bodyMedium)
                    }
                    Icon(Icons.Filled.ArrowForward, null, tint = accent)
                }
            }
        }
    }
}
