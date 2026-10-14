package app.gamenative.xrgame

import android.annotation.SuppressLint
import android.app.Activity
import android.app.Application
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.HardwareRenderer
import android.graphics.PixelFormat
import android.graphics.RenderNode
import android.hardware.HardwareBuffer
import android.media.ImageReader
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.view.Choreographer
import android.view.Surface
import android.view.View
import android.view.WindowManager
import android.view.inspector.WindowInspector
import app.gamenative.BuildConfig
import app.gamenative.ui.screen.xr.ImmersiveXrActivity
import app.gamenative.ui.screen.xr.XrNative
import timber.log.Timber
import java.io.File

/**
 * The scrcpy capture SDK's 2D window producer (internal debug XRGame builds with the SDK). While
 * SteamPSP's own window is in front and no immersive session runs — the library, the launch panel,
 * dialogs — DebugBus `capture_screenshot` and `capture_video` (capturectl.py, liveview.py, the
 * scrcpy MCP) show that window instead of answering that there is no producer.
 *
 * On due frames the resumed activity's window and the dialog windows over it
 * ([WindowInspector.getGlobalWindowViews]) are recorded into a [RenderNode], scaled to the encoded
 * size, and a [HardwareRenderer] of ours draws it into the native side's image reader, which a
 * native thread feeds to the encoder (xr_panel_feed.h). HWUI never renders into the encoder's own
 * Surface: its RenderThread serves every window of the app, and a stalled codec would freeze them
 * all. Stills render at the window's full size into an [ImageReader] and are written as PNG off
 * the main thread. Only SteamPSP's windows are in the picture, not the Pico environment or system
 * overlays. An immersive session takes the producer over; this one attaches again when a SteamPSP
 * window resumes.
 */
// API 29 calls (WindowInspector, ImageReader usage): install() registers nothing below API 29.
@SuppressLint("NewApi")
object XrPanelCapture : Application.ActivityLifecycleCallbacks {
    // Flags of XrNative.nativePanelTick (xr_capture.h, namespace panel).
    private const val DRAW = 1
    private const val STILL = 2
    private const val ACTIVE = 4
    private const val SURFACE_CHANGED = 8
    private const val IDLE_POLL_MS = 100L
    private const val HELD_POLL_MS = 500L

    private val main = Handler(Looper.getMainLooper())
    private val stills by lazy { Handler(HandlerThread("XRGame:PanelStill").apply { start() }.looper) }
    private var activity: Activity? = null
    private var surface: Surface? = null
    private var renderer: HardwareRenderer? = null
    private val content = RenderNode("XRGamePanelCapture")
    private var scheduled = false
    private var stillToken: String? = null

    /** From the Application; does nothing outside debug XRGame builds that link the capture SDK. */
    fun install(application: Application) {
        if (!BuildConfig.XRGAME || !BuildConfig.DEBUG || Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        val available = runCatching { XrNative.nativeCaptureAvailable() }.getOrDefault(false)
        if (!available) return
        application.registerActivityLifecycleCallbacks(this)
        Timber.i("XRGame capture: 2D window producer installed")
    }

    override fun onActivityResumed(resumed: Activity) {
        if (resumed is ImmersiveXrActivity) return
        activity = resumed
        schedule(0)
    }

    override fun onActivityPaused(paused: Activity) {
        if (paused !== activity) return
        activity = null
        stop()
    }

    override fun onActivityCreated(created: Activity, state: Bundle?) = Unit
    override fun onActivityStarted(started: Activity) = Unit
    override fun onActivityStopped(stopped: Activity) = Unit
    override fun onActivitySaveInstanceState(saved: Activity, state: Bundle) = Unit
    override fun onActivityDestroyed(destroyed: Activity) = Unit

    private val frame = Choreographer.FrameCallback { tick() }
    private val poll = Runnable { tick() }

    private fun schedule(delayMs: Long) {
        if (scheduled) return
        scheduled = true
        main.postDelayed(poll, delayMs)
    }

    private fun stop() {
        main.removeCallbacks(poll)
        Choreographer.getInstance().removeFrameCallback(frame)
        scheduled = false
        releaseSurface()
        XrNative.nativePanelDetach()
    }

    // Before the native side frees the reader behind the Surface.
    private fun releaseSurface() {
        renderer?.destroy()
        renderer = null
        surface?.release()
        surface = null
    }

    private fun tick() {
        scheduled = false
        val current = activity ?: return
        // An immersive session holds the producer: look again later.
        if (!XrNative.nativePanelAttach()) {
            schedule(HELD_POLL_MS)
            return
        }
        val root = current.window?.decorView
        val flags = XrNative.nativePanelTick(root?.width ?: 0, root?.height ?: 0)
        if (flags and SURFACE_CHANGED != 0) {
            releaseSurface()
            surface = XrNative.nativePanelSurface()
        }
        if (root != null && root.width > 0 && root.height > 0) {
            if (flags and DRAW != 0) drawFrame(root)
            if (flags and STILL != 0) takeStill(root)
        }
        if (flags and ACTIVE != 0) {
            scheduled = true
            Choreographer.getInstance().postFrameCallback(frame)
        } else {
            schedule(IDLE_POLL_MS)
        }
    }

    private fun drawFrame(root: View) {
        val target = surface ?: return
        val size = XrNative.nativePanelEncodedSize()
        val width = (size ushr 32).toInt()
        val height = (size and 0xffffffffL).toInt()
        if (width <= 0 || height <= 0) return
        val output = renderer ?: HardwareRenderer().also { created ->
            created.setContentRoot(content)
            created.setSurface(target)
            // Shadows as the window draws them (the platform's light defaults), at the encoded scale.
            val px = root.resources.displayMetrics.density * width / root.width
            created.setLightSourceGeometry(width / 2f, 0f, 600f * px, 800f * px)
            created.setLightSourceAlpha(0.039f, 0.19f)
            renderer = created
        }
        content.setPosition(0, 0, width, height)
        val canvas = content.beginRecording(width, height)
        try {
            drawWindows(canvas, root, width, height)
        } finally {
            content.endRecording()
        }
        output.createRenderRequest().setVsyncTime(System.nanoTime()).syncAndDraw()
        XrNative.nativePanelPresented()
    }

    /** The activity's window, then the dialogs over it (dimmed as on screen), scaled to [width] x [height]. */
    private fun drawWindows(canvas: Canvas, root: View, width: Int, height: Int) {
        canvas.drawColor(Color.BLACK)
        canvas.save()
        canvas.scale(width / root.width.toFloat(), height / root.height.toFloat())
        root.draw(canvas)
        val origin = IntArray(2).also { root.getLocationOnScreen(it) }
        val location = IntArray(2)
        for (view in WindowInspector.getGlobalWindowViews()) {
            if (view === root || !view.isAttachedToWindow || view.windowVisibility != View.VISIBLE ||
                view.width == 0 || view.height == 0
            ) {
                continue
            }
            val params = view.layoutParams as? WindowManager.LayoutParams
            if (params != null && params.flags and WindowManager.LayoutParams.FLAG_DIM_BEHIND != 0) {
                canvas.drawColor(Color.argb((params.dimAmount * 255).toInt().coerceIn(0, 255), 0, 0, 0))
            }
            view.getLocationOnScreen(location)
            canvas.save()
            canvas.translate((location[0] - origin[0]).toFloat(), (location[1] - origin[1]).toFloat())
            view.draw(canvas)
            canvas.restore()
        }
        canvas.restore()
    }

    /** Full window size, through a hardware canvas like the video (images may be hardware bitmaps). */
    private fun takeStill(root: View) {
        val target = XrNative.nativePanelStillTarget() ?: return
        val token = target[0]
        val path = target[1]
        if (stillToken == token) return
        stillToken = token
        val width = root.width
        val height = root.height
        val reader = ImageReader.newInstance(
            // Two images: acquireLatestImage looks one past the newest.
            width, height, PixelFormat.RGBA_8888, 2,
            HardwareBuffer.USAGE_CPU_READ_OFTEN or HardwareBuffer.USAGE_GPU_COLOR_OUTPUT,
        )
        reader.setOnImageAvailableListener({ ready ->
            var error = ""
            try {
                ready.acquireLatestImage()?.use { image ->
                    val plane = image.planes[0]
                    val padded = Bitmap.createBitmap(plane.rowStride / plane.pixelStride, height, Bitmap.Config.ARGB_8888)
                    padded.copyPixelsFromBuffer(plane.buffer)
                    val bitmap = if (padded.width == width) padded else Bitmap.createBitmap(padded, 0, 0, width, height)
                    val partial = File("$path.partial")
                    partial.parentFile?.mkdirs()
                    partial.outputStream().use { if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)) error = "PNG encode failed" }
                    if (error.isEmpty() && !partial.renameTo(File(path))) error = "PNG rename failed"
                    if (error.isNotEmpty()) partial.delete()
                } ?: run { error = "no image" }
            } catch (e: Exception) {
                error = "still failed: ${e.javaClass.simpleName}"
            } finally {
                ready.close()
            }
            main.post {
                XrNative.nativePanelStillDone(token, width, height, error)
                if (stillToken == token) stillToken = null
            }
        }, stills)
        val canvas = try {
            reader.surface.lockHardwareCanvas()
        } catch (e: Exception) {
            reader.close()
            XrNative.nativePanelStillDone(token, 0, 0, "still surface unavailable")
            stillToken = null
            return
        }
        try {
            drawWindows(canvas, root, width, height)
        } finally {
            reader.surface.unlockCanvasAndPost(canvas)
        }
    }
}
