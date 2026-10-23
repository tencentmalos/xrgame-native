package app.gamenative.ui.screen.xr

import android.content.Context
import androidx.activity.ComponentActivity
import androidx.lifecycle.lifecycleScope
import app.gamenative.MainActivity
import app.gamenative.PrefManager
import app.gamenative.R
import app.gamenative.ui.component.dialog.state.MessageDialogState
import app.gamenative.ui.preLaunchApp
import app.gamenative.ui.screen.xserver.initialFpsLimiterEnabled
import app.gamenative.ui.screen.xserver.initialFpsLimiterTarget
import app.gamenative.utils.ContainerUtils
import app.gamenative.utils.IntentLaunchManager
import app.gamenative.xrgame.XrGameApiCapture
import app.gamenative.xrgame.XrGameGuestPatches
import app.gamenative.xrgame.XrGameLaunchMode
import app.gamenative.xrgame.XrGameLaunchOptions
import app.gamenative.xrgame.XrGameOfficialSteam
import com.winlator.container.ContainerData
import kotlin.reflect.KFunction2
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import timber.log.Timber

/**
 * A game's launch from a hosted client on the big screen (Steam handing a game over), without a 2D
 * page: the launch panel is drawn on the big screen by xr_launch_panel.cpp, from the same options
 * as the 2D panel (XrGameLaunchOptions), and the launch's preparation shows there too. Start
 * saves the options and prepares the launch as a library launch does; the game's own immersive
 * page then takes the headset. Main thread.
 */
class XrLaunchPanel(
    private val activity: ComponentActivity,
    // MainViewModel::launchApp of the hosting page: starts the game's immersive page.
    private val launchApp: KFunction2<Context, String, Unit>,
) {
    private enum class View(val key: String) { OPTIONS("options"), PROGRESS("progress"), MESSAGE("message") }

    // Read by the hosting page's polling thread (isOpen).
    @Volatile private var appId: String? = null
    private var title = ""
    private var initial: ContainerData? = null
    private var data: ContainerData? = null
    private var vrGame = false
    private var mode = XrGameLaunchMode.Mode.XR
    private var statusLayer = PrefManager.xrgameStatusLayer
    private var initialFrameLimit: XrGameLaunchOptions.FrameLimit? = null
    private var frameLimit: XrGameLaunchOptions.FrameLimit? = null
    private var guestPatches = emptyList<XrGameGuestPatches.Package>()
    private var advanced = false
    private var view = View.PROGRESS
    private var progress = ""
    private var messageTitle = ""
    private var message = ""
    private var launched = false
    private var rows: Pair<List<XrGameLaunchOptions.Row>, List<XrGameLaunchOptions.Row>> = emptyList<XrGameLaunchOptions.Row>() to emptyList()
    private var poller: Job? = null
    private var silentCheck: Job? = null
    private val headset = MainActivity.isHeadset(activity)
    private val captureAvailable = XrGameApiCapture.available(activity)

    val isOpen: Boolean get() = appId != null

    /** Whether the game's page is starting: the hosting page may let the panel go once it stops. */
    val isLaunching: Boolean get() = launched

    fun open(gameAppId: String) {
        if (appId != null) {
            Timber.w("XR launch panel: %s is already open; %s waits", appId, gameAppId)
            return
        }
        appId = gameAppId
        launched = false
        advanced = false
        view = View.PROGRESS
        progress = activity.getString(R.string.xrgame_launch_loading)
        title = gameAppId
        render()
        poller = activity.lifecycleScope.launch {
            while (appId != null) {
                drain()
                delay(POLL_MS)
            }
        }
        activity.lifecycleScope.launch {
            val loaded = runCatching {
                withContext(Dispatchers.IO) {
                    XrGameOfficialSteam.applyDisplay(activity, gameAppId)
                    val container = ContainerUtils.getOrCreateContainer(activity, gameAppId)
                    val patches = if (!gameAppId.startsWith("STEAM_")) emptyList() else runCatching {
                        XrGameGuestPatches.list(ContainerUtils.extractGameIdFromContainerId(gameAppId)).packages
                    }.getOrDefault(emptyList())
                    Loaded(
                        ContainerUtils.toContainerData(container),
                        XrGameLaunchOptions.title(gameAppId),
                        XrGameLaunchOptions.FrameLimit(initialFpsLimiterEnabled(container), initialFpsLimiterTarget(container)),
                        patches,
                    )
                }
            }.onFailure { Timber.e(it, "XR launch panel: loading %s failed", gameAppId) }.getOrNull()
            if (appId != gameAppId) return@launch
            if (loaded == null) {
                showMessage(activity.getString(R.string.xrgame_launch_xr_failed), "")
                return@launch
            }
            title = loaded.title
            initial = loaded.data
            data = loaded.data
            vrGame = loaded.data.windowsVrEnabled
            mode = XrGameLaunchMode.current(loaded.data, headset)
            initialFrameLimit = loaded.frameLimit
            frameLimit = loaded.frameLimit
            guestPatches = loaded.guestPatches
            statusLayer = PrefManager.xrgameStatusLayer
            if (PrefManager.xrgameLaunchPanel) {
                view = View.OPTIONS
                render()
            } else {
                start()
            }
        }
    }

    /** The hosting page goes: a launch still being chosen is cancelled, a starting one goes on. */
    fun abandon() {
        if (launched) dismiss() else cancel()
    }

    /** Lets the panel go without touching the client's launch (the game's page took over). */
    fun dismiss() {
        appId = null
        poller?.cancel()
        silentCheck?.cancel()
        XrNative.nativeSetLaunchPanel(null)
    }

    private data class Loaded(
        val data: ContainerData,
        val title: String,
        val frameLimit: XrGameLaunchOptions.FrameLimit,
        val guestPatches: List<XrGameGuestPatches.Package>,
    )

    private fun drain() {
        val events = XrNative.nativeDrainLaunchPanel() ?: return
        for (event in events.lineSequence().filter { it.isNotBlank() }) {
            val parts = event.split(' ')
            when (parts[0]) {
                "select" -> if (view == View.OPTIONS && parts.size == 4) select(parts[1].toInt(), parts[2].toInt(), parts[3].toInt())
                "advanced" -> if (view == View.OPTIONS) {
                    advanced = !advanced
                    render()
                }
                "start" -> if (view == View.OPTIONS) start()
                "cancel" -> if (view == View.OPTIONS) cancel()
                "ok" -> if (view == View.MESSAGE) cancel()
            }
        }
    }

    private fun select(section: Int, index: Int, choice: Int) {
        val row = (if (section == 0) rows.first else rows.second).getOrNull(index) ?: return
        if (choice !in row.choices.indices || !row.enabled(choice)) return
        row.onSelect(choice)
        render()
    }

    private fun start() {
        val id = appId ?: return
        val draft = data ?: return
        view = View.PROGRESS
        progress = activity.getString(R.string.xrgame_launch_loading)
        render()
        activity.lifecycleScope.launch {
            withContext(Dispatchers.IO) {
                XrGameLaunchOptions.save(activity, id, initial, draft, mode, vrGame, initialFrameLimit, frameLimit, statusLayer,
                    followsEntry = true)
            }
            if (appId != id) return@launch
            // As a library launch prepares; its loading text and messages show on the panel.
            preLaunchApp(
                context = activity,
                appId = id,
                useTemporaryOverride = IntentLaunchManager.hasTemporaryOverride(id),
                setLoadingDialogVisible = { visible -> if (!visible) activity.runOnUiThread { expectOutcome(id) } },
                setLoadingProgress = {},
                setLoadingMessage = { text -> activity.runOnUiThread { if (appId == id && view == View.PROGRESS) { progress = text; render() } } },
                setMessageDialogState = { state -> activity.runOnUiThread { if (appId == id && state.visible) showMessage(state) } },
                onSuccess = ::startGame,
                isOffline = false,
            )
        }
    }

    /** preLaunchApp's success: the game's immersive page starts and takes the headset. */
    @Suppress("UNUSED_PARAMETER")
    private fun startGame(context: Context, gameAppId: String) {
        activity.runOnUiThread {
            if (appId != gameAppId) return@runOnUiThread
            launched = true
            silentCheck?.cancel()
            progress = activity.getString(R.string.xrgame_launch_xr_starting)
            render()
            launchApp(activity, gameAppId)
        }
    }

    // Some preparation failures only show a snackbar on the 2D page: say so here instead of waiting.
    private fun expectOutcome(id: String) {
        silentCheck?.cancel()
        silentCheck = activity.lifecycleScope.launch {
            delay(SILENT_FAILURE_MS)
            if (appId == id && !launched && view == View.PROGRESS) showMessage(activity.getString(R.string.xrgame_launch_xr_failed), "")
        }
    }

    private fun showMessage(state: MessageDialogState) =
        showMessage(state.title ?: activity.getString(R.string.xrgame_launch_xr_failed), state.message.orEmpty())

    private fun showMessage(heading: String, text: String) {
        view = View.MESSAGE
        messageTitle = heading
        message = text
        render()
    }

    /** Cancel: Steam stops waiting for this launch. */
    private fun cancel() {
        val id = appId ?: return
        runCatching { ContainerUtils.extractGameIdFromContainerId(id) }.getOrNull()?.let { XrGameOfficialSteam.finishGame(it) }
        Timber.i("XR launch panel: %s cancelled", id)
        dismiss()
    }

    private fun render() {
        if (appId == null) return
        val draft = data
        rows = if (draft != null && view == View.OPTIONS) {
            XrGameLaunchOptions.rows(
                activity,
                XrGameLaunchOptions.Draft(draft, headset, vrGame, mode, false, statusLayer, frameLimit, guestPatches, captureAvailable),
                XrGameLaunchOptions.Changes(
                    onData = { data = it },
                    onVrGame = { vrGame = it; mode = XrGameLaunchMode.fallback(mode, it, headset) },
                    onMode = { mode = it },
                    onStatusLayer = { statusLayer = it },
                    onFrameLimit = { frameLimit = it },
                ),
            )
        } else {
            emptyList<XrGameLaunchOptions.Row>() to emptyList()
        }
        val model = StringBuilder()
        fun line(vararg fields: String) {
            model.append(fields.joinToString("\t") { field(it) }).append('\n')
        }
        line("T", activity.getString(R.string.xrgame_launch_header), title,
            if (draft != null) XrGameLaunchOptions.summary(activity, mode, vrGame) else "")
        // The panel draws each button's controller glyph (A, B) and icon itself.
        line("F", activity.getString(R.string.xrgame_launch_xr_footer),
            activity.getString(R.string.xrgame_launch_cancel),
            activity.getString(R.string.xrgame_launch_start),
            activity.getString(R.string.xrgame_launch_advanced),
            activity.getString(android.R.string.ok))
        line("V", view.key)
        line("P", progress)
        line("G", messageTitle, message)
        line("A", if (advanced) "1" else "0")
        for ((section, list) in listOf(0 to rows.first, 1 to rows.second)) {
            list.forEachIndexed { index, row ->
                model.append(
                    listOf(
                        "R", section.toString(), index.toString(), field(row.label), row.selected.toString(),
                        row.choices.joinToString("\u001f") { field(it).replace("\u001f", "") },
                        row.choices.indices.joinToString("") { if (row.enabled(it)) "0" else "1" },
                        field(row.note.orEmpty()),
                    ).joinToString("\t"),
                ).append('\n')
            }
        }
        XrNative.nativeSetLaunchPanel(model.toString())
    }

    // A field may not hold the separators; line breaks travel as U+001E.
    private fun field(text: String) = text.replace('\t', ' ').replace("\r", "").replace('\n', '\u001e')

    private companion object {
        const val POLL_MS = 30L
        const val SILENT_FAILURE_MS = 1500L
    }
}
