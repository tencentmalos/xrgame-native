// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame

import android.content.Intent
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
import androidx.compose.material.icons.filled.ArrowForward
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
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
import app.gamenative.xrgame.steam.LinuxSteamService
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import app.gamenative.MainActivity
import app.gamenative.R
import app.gamenative.ui.icons.Steam
import app.gamenative.ui.theme.PluviaTheme
import app.gamenative.xrgame.steam.LinuxSteamActivity

/** Two visual entry points; both keep the existing app identity and data. */
class LaunchActivity : ComponentActivity() {
    private var switching by mutableStateOf(false)
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.keyCode == KeyEvent.KEYCODE_BUTTON_A) {
            return super.dispatchKeyEvent(KeyEvent(event.downTime, event.eventTime, event.action,
                KeyEvent.KEYCODE_ENTER, event.repeatCount, event.metaState, event.deviceId, 0, event.flags, event.source))
        }
        return super.dispatchKeyEvent(event)
    }

    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowCompat.getInsetsController(window, window.decorView).hide(WindowInsetsCompat.Type.systemBars())
        setContent {
            PluviaTheme {
                Box(Modifier.fillMaxSize().background(Brush.linearGradient(listOf(Color(0xFF08111D), Color(0xFF14243A))))) {
                    Column(Modifier.safeDrawingPadding().padding(horizontal = 36.dp, vertical = 24.dp),
                        verticalArrangement = Arrangement.Center) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Image(painterResource(R.drawable.ic_launcher_xr_foreground), null, Modifier.size(42.dp))
                            Text("SteamPSP", color = Color.White, style = MaterialTheme.typography.titleLarge)
                        }
                        Spacer(Modifier.height(24.dp))
                        BoxWithConstraints(Modifier.weight(1f)) {
                            if (maxWidth >= 600.dp) {
                                Row(horizontalArrangement = Arrangement.spacedBy(24.dp)) {
                                    ModeCard(true, Modifier.weight(1f).fillMaxHeight())
                                    ModeCard(false, Modifier.weight(1f).fillMaxHeight())
                                }
                            } else {
                                Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
                                    ModeCard(true, Modifier.weight(1f).fillMaxWidth())
                                    ModeCard(false, Modifier.weight(1f).fillMaxWidth())
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    @Composable
    private fun ModeCard(official: Boolean, modifier: Modifier) {
        var focused by remember { mutableStateOf(false) }
        val accent = if (official) Color(0xFF6AC9FF) else Color(0xFF7EF1D8)
        Card(onClick = {
            if (!switching) lifecycleScope.launch {
                switching = true
                try {
                    check(app.gamenative.PluviaApp.xEnvironment == null && app.gamenative.service.ActiveGameRegistry.get() == null) {
                        "Exit the running game before switching Steam modes."
                    }
                    if (official) {
                        startForegroundService(Intent(this@LaunchActivity, LinuxSteamService::class.java))
                    } else {
                        stopService(Intent(this@LaunchActivity, LinuxSteamService::class.java))
                        withTimeout(15000) { while (XrGameOfficialSteam.ownsLibrary || LinuxSteamService.running) delay(100) }
                        XrGameOfficialSteam.selectCustom()
                        startForegroundService(Intent(this@LaunchActivity, app.gamenative.service.SteamService::class.java))
                    }
                    // Restore Custom's entry page only after its client owns the session.
                    startActivity(Intent(this@LaunchActivity, if (official) LinuxSteamActivity::class.java else MainActivity::class.java)
                        .addFlags(if (official) Intent.FLAG_ACTIVITY_REORDER_TO_FRONT else Intent.FLAG_ACTIVITY_CLEAR_TOP))
                } catch (e: Exception) {
                    android.app.AlertDialog.Builder(this@LaunchActivity).setMessage(e.message ?: "Steam mode switch failed")
                        .setPositiveButton(android.R.string.ok, null).show()
                } finally { switching = false }
            }
        }, modifier = modifier.onFocusChanged { focused = it.hasFocus }, shape = RoundedCornerShape(24.dp),
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
                else Image(painterResource(R.drawable.ic_launcher_xr_foreground), null,
                    Modifier.align(Alignment.Center).padding(bottom = 40.dp).size(148.dp))
                Row(Modifier.align(Alignment.BottomStart).fillMaxWidth().padding(26.dp), verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(if (official) "Official Steam" else "Custom Steam", color = Color.White, style = MaterialTheme.typography.headlineSmall)
                        Spacer(Modifier.height(4.dp))
                        Text(stringResource(if (official) R.string.steam_mode_official_short else R.string.steam_mode_custom_short),
                            color = Color.White.copy(alpha = .75f), style = MaterialTheme.typography.bodyMedium)
                    }
                    Icon(Icons.Filled.ArrowForward, null, tint = accent)
                }
            }
        }
    }
}
