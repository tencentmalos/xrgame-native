package app.gamenative.ui.screen.xr.sbs

import android.os.Bundle
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.activity.viewModels
import androidx.compose.foundation.layout.*
import androidx.compose.material3.Button
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import app.gamenative.PluviaApp
import app.gamenative.R
import app.gamenative.service.SteamService
import app.gamenative.ui.model.MainViewModel
import app.gamenative.ui.screen.xr.ImmersiveSessionHooks
import app.gamenative.ui.screen.xr.windows.WindowsVrRuntimeService
import app.gamenative.ui.screen.xserver.XServerScreen
import app.gamenative.ui.theme.PluviaTheme
import com.winlator.core.AppUtils
import dagger.hilt.android.AndroidEntryPoint

@AndroidEntryPoint
class SbsVrActivity : ComponentActivity() {
    private val viewModel: MainViewModel by viewModels()
    private val frames = SbsFrameSource()
    private lateinit var vrView: SbsVrView
    private lateinit var runtime: WindowsVrRuntimeService
    private var backAction: (() -> Unit)? = null
    private var toggleMenu: (() -> Unit)? = null
    private var menuVisible by mutableStateOf(false)
    private var presented by mutableLongStateOf(0L)
    private val inputBlocked: Boolean get() = menuVisible || PluviaApp.isOverlayPaused

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val appId = intent.getStringExtra("app_id") ?: run { finish(); return }
        val endpoint = "@xrgame-sbs-${android.os.Process.myPid()}"
        runtime = WindowsVrRuntimeService(this, frames, endpoint)
        vrView = SbsVrView(this, frames, endpoint) { count ->
            presented = count
            runtime.onPresentationState("SBS frames=$count")
            if (count < 0) {
                runtime.onGuestProcessError("Android SBS renderer initialization failed")
                app.gamenative.ui.util.SnackbarManager.show("Android SBS renderer initialization failed")
                finish()
            }
        }
        AppUtils.keepScreenOn(this)
        AppUtils.hideSystemUI(this)
        setContent {
            PluviaTheme {
                BackHandler { backAction?.invoke() ?: finish() }
                // Manual-resume and editor overlays must remain visible after the menu closes.
                // Keep synthetic controller input inactive until the guest is resumed.
                SideEffect { frames.setFocused(!inputBlocked) }
                Box(Modifier.fillMaxSize()) {
                    XServerScreen(
                        appId = appId, bootToContainer = false,
                        isOffline = intent.getBooleanExtra("is_offline", false),
                        registerBackAction = { backAction = it }, navigateBack = { finish() },
                        onExit = { complete -> viewModel.exitSteamApp(this@SbsVrActivity, appId, allowUiPrompts = false) {
                            complete?.invoke(); finish()
                        } },
                        onWindowMapped = { ctx, window -> viewModel.onWindowMapped(ctx, window, appId) },
                        onGameLaunchError = { error ->
                            runtime.onGuestProcessError(error)
                            viewModel.onGameLaunchError(error)
                            finish()
                        },
                        immersiveHooks = ImmersiveSessionHooks(
                            windowsVr = runtime, registerToggle = { toggleMenu = it },
                            onQuickMenuVisibilityChanged = {
                                menuVisible = it
                            },
                        ),
                    )
                    AndroidView(factory = { vrView }, modifier = Modifier.fillMaxSize(), update = {
                        it.alpha = if (presented > 0 && !inputBlocked) 1f else 0f
                        it.isEnabled = presented > 0 && !inputBlocked
                    })
                    if (!inputBlocked) Row(
                        Modifier.align(Alignment.TopCenter).padding(6.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(stringResource(if (presented > 0) R.string.xrgame_sbs_controls else R.string.xrgame_sbs_waiting), color = Color.White)
                        Spacer(Modifier.width(10.dp))
                        Button(onClick = { vrView.recenter() }) { Text(stringResource(R.string.xrgame_vr_recenter)) }
                        Spacer(Modifier.width(6.dp))
                        Button(onClick = { toggleMenu?.invoke() }) { Text(stringResource(R.string.xrgame_vr_menu)) }
                    }
                }
            }
        }
    }

    override fun onResume() {
        super.onResume()
        PluviaApp.isImmersiveActivityResumed = true
        PluviaApp.isActivityInForeground = true
        if (::vrView.isInitialized) vrView.onResume()
        if (SteamService.keepAlive && PluviaApp.hasValidSuspendPolicyState() &&
            !(PluviaApp.isOverlayPaused && PluviaApp.isManualSuspendMode())) PluviaApp.xEnvironment?.onResume()
    }
    override fun onPause() {
        PluviaApp.isImmersiveActivityResumed = false
        PluviaApp.isActivityInForeground = false
        if (isFinishing) PluviaApp.shutdownEnvironment()
        else if (!PluviaApp.isNeverSuspendMode()) PluviaApp.xEnvironment?.onPause()
        if (::vrView.isInitialized) vrView.onPause()
        super.onPause()
    }
    override fun onDestroy() {
        PluviaApp.shutdownEnvironment()
        if (::runtime.isInitialized) runtime.close()
        if (::vrView.isInitialized) vrView.release()
        frames.detach()
        super.onDestroy()
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (!inputBlocked && event.isFromSource(InputDevice.SOURCE_JOYSTICK)) {
            val ids = intArrayOf(MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z,
                MotionEvent.AXIS_RZ, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER)
            ids.forEachIndexed { i, axis ->
                val value = event.getAxisValue(axis)
                frames.axis(i, if (i == 1 || i == 3) -value else value)
            }
            return true
        }
        return super.dispatchGenericMotionEvent(event)
    }
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (!inputBlocked && event.keyCode == KeyEvent.KEYCODE_BUTTON_START) {
            if (event.action == KeyEvent.ACTION_DOWN && event.repeatCount == 0) toggleMenu?.invoke()
            return true
        }
        if (!inputBlocked) {
            val bit = when (event.keyCode) {
                KeyEvent.KEYCODE_BUTTON_A -> 0; KeyEvent.KEYCODE_BUTTON_B -> 1
                KeyEvent.KEYCODE_BUTTON_X -> 2; KeyEvent.KEYCODE_BUTTON_Y -> 3
                KeyEvent.KEYCODE_BUTTON_SELECT -> 7 // virtual left controller menu
                KeyEvent.KEYCODE_BUTTON_THUMBL -> 8; KeyEvent.KEYCODE_BUTTON_THUMBR -> 9
                else -> -1
            }
            val down = event.action == KeyEvent.ACTION_DOWN
            if (bit >= 0) { frames.button(bit, down); return true }
            val grip = when (event.keyCode) { KeyEvent.KEYCODE_BUTTON_L1 -> 6; KeyEvent.KEYCODE_BUTTON_R1 -> 7; else -> -1 }
            if (grip >= 0) { frames.axis(grip, if (down) 1f else 0f); return true }
            val trigger = when (event.keyCode) { KeyEvent.KEYCODE_BUTTON_L2 -> 4; KeyEvent.KEYCODE_BUTTON_R2 -> 5; else -> -1 }
            if (trigger >= 0) { frames.axis(trigger, if (down) 1f else 0f); return true }
        }
        return super.dispatchKeyEvent(event)
    }
}
