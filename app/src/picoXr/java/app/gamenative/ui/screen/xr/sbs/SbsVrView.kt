package app.gamenative.ui.screen.xr.sbs

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.opengl.EGL14
import android.opengl.EGLDisplay
import android.opengl.EGLContext
import android.opengl.EGLSurface
import android.graphics.SurfaceTexture
import android.os.Handler
import android.os.HandlerThread
import android.view.Choreographer
import android.view.Surface
import android.view.TextureView
import android.view.MotionEvent

/** Owns only the Android SBS output and local simulated tracking. No OpenXR instance is created. */
class SbsVrView(
    context: Context,
    private val frames: SbsFrameSource,
    private val endpoint: String,
    private val onFrames: (Long) -> Unit,
) : TextureView(context), SensorEventListener, TextureView.SurfaceTextureListener {
    private val sensors = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
    private val gyro = sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
    private val gravity = sensors.getDefaultSensor(Sensor.TYPE_GRAVITY)
        ?: sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
    private var neutral = VrGyroAxes.NeutralFrame()
    private var handle = 0L // GL thread only
    private var lastReported = 0L
    @Volatile private var outputWidth = 2
    @Volatile private var outputHeight = 1
    private val touches = mutableMapOf<Int, Int>()
    @Volatile private var running = false
    @Volatile private var periodNs = 16_666_667L

    private val renderThread = HandlerThread("XrGame-SBS").apply { start() }
    private val renderHandler = Handler(renderThread.looper)
    // EGL and presenter ownership stay on renderThread, including every lifecycle transition.
    private var eglDisplay: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var eglContext: EGLContext = EGL14.EGL_NO_CONTEXT
    private var eglSurface: EGLSurface = EGL14.EGL_NO_SURFACE
    private var renderSurface: Surface? = null
    private var choreographer: Choreographer? = null
    private val drawCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (handle == 0L || !running) return
            frames.tick(frameTimeNanos, periodNs, outputWidth/2, outputHeight)
            val count = SbsNative.draw(handle, outputWidth, outputHeight)
            if (!EGL14.eglSwapBuffers(eglDisplay, eglSurface)) {
                stopRenderer(); post { onFrames(-1) }; return
            }
            if (count > 0 && (lastReported == 0L || count-lastReported >= 120)) {
                lastReported = count; post { onFrames(count) }
            }
            choreographer?.postFrameCallback(this)
        }
    }

    init {
        isOpaque = true
        surfaceTextureListener = this
    }

    private fun startRenderer(texture: SurfaceTexture) {
        if (handle != 0L || !running) return
        try {
            eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
            val version = IntArray(2)
            check(EGL14.eglInitialize(eglDisplay, version, 0, version, 1))
            val configs = arrayOfNulls<android.opengl.EGLConfig>(1)
            val count = IntArray(1)
            val attributes = intArrayOf(EGL14.EGL_RENDERABLE_TYPE, 0x40,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
                EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8,
                EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8, EGL14.EGL_NONE)
            check(EGL14.eglChooseConfig(eglDisplay, attributes, 0, configs, 0, 1, count, 0) && count[0] > 0)
            eglContext = EGL14.eglCreateContext(eglDisplay, configs[0], EGL14.EGL_NO_CONTEXT,
                intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 3, EGL14.EGL_NONE), 0)
            check(eglContext != EGL14.EGL_NO_CONTEXT)
            renderSurface = Surface(texture)
            eglSurface = EGL14.eglCreateWindowSurface(eglDisplay, configs[0], renderSurface,
                intArrayOf(EGL14.EGL_NONE), 0)
            check(eglSurface != EGL14.EGL_NO_SURFACE)
            check(EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext))
            EGL14.eglSwapInterval(eglDisplay, 1)
            handle = SbsNative.create(endpoint)
            check(handle != 0L)
            lastReported = 0L
            android.util.Log.i("XrGameSbs", "TextureView ready handle=$handle endpoint=$endpoint")
            choreographer = Choreographer.getInstance()
            choreographer?.postFrameCallback(drawCallback)
        } catch (error: Exception) {
            android.util.Log.e("XrGameSbs", "EGL initialization failed", error)
            stopRenderer(); post { onFrames(-1) }
        }
    }

    private fun stopRenderer() {
        choreographer?.removeFrameCallback(drawCallback)
        if (handle != 0L) { SbsNative.destroy(handle); handle = 0L }
        if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            if (eglSurface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(eglDisplay, eglSurface)
            if (eglContext != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(eglDisplay, eglContext)
            EGL14.eglTerminate(eglDisplay)
        }
        eglSurface = EGL14.EGL_NO_SURFACE; eglContext = EGL14.EGL_NO_CONTEXT
        eglDisplay = EGL14.EGL_NO_DISPLAY
        renderSurface?.release(); renderSurface = null
    }

    override fun onSurfaceTextureAvailable(texture: SurfaceTexture, width: Int, height: Int) {
        outputWidth = width; outputHeight = height
        renderHandler.post { startRenderer(texture) }
    }
    override fun onSurfaceTextureSizeChanged(texture: SurfaceTexture, width: Int, height: Int) {
        outputWidth = width; outputHeight = height
    }
    override fun onSurfaceTextureUpdated(texture: SurfaceTexture) = Unit
    override fun onSurfaceTextureDestroyed(texture: SurfaceTexture): Boolean {
        // Retain the texture until queued GPU cleanup completes; never block the UI on fences.
        if (!renderHandler.post { stopRenderer(); texture.release() }) texture.release()
        return false
    }
    fun release() {
        running = false; sensors.unregisterListener(this)
        renderHandler.post { stopRenderer(); renderThread.quitSafely() }
    }

    fun onResume() {
        running = true
        periodNs = (1_000_000_000.0/(display?.refreshRate ?: 60f).coerceIn(20f, 200f)).toLong()
        frames.setActive(true)
        surfaceTexture?.let { texture -> renderHandler.post { startRenderer(texture) } }
        gyro?.let { sensors.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME) }
        gravity?.let { sensors.registerListener(this, it, SensorManager.SENSOR_DELAY_GAME) }
    }

    fun onPause() {
        if (!running) return
        running = false
        sensors.unregisterListener(this)
        touches.clear()
        frames.setActive(false)
        renderHandler.post { stopRenderer() }
    }

    fun recenter() { neutral = VrGyroAxes.NeutralFrame(); frames.recenter() }

    override fun onSensorChanged(event: SensorEvent) {
        if (!running || event.values.size < 3) return
        val v = event.values
        if (event.sensor.type == gravity?.type) {
            neutral.observeGravity(v[0], v[1], v[2], display?.rotation ?: 0)
        } else if (event.sensor.type == Sensor.TYPE_GYROSCOPE) {
            val rate = if (gravity != null) neutral.toHead(v[0], v[1], v[2])
                else VrGyroAxes.toDisplay(v[0], v[1], v[2], display?.rotation ?: 0)
            if (rate != null) frames.gyro(rate[0], rate[1], rate[2], event.timestamp)
        }
    }
    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (!running || !isEnabled) return false
        val index = event.actionIndex
        if (event.actionMasked == MotionEvent.ACTION_DOWN || event.actionMasked == MotionEvent.ACTION_POINTER_DOWN) {
            touches[event.getPointerId(index)] = if (event.getX(index) < width/2f) 0 else 1
        }
        if (event.actionMasked == MotionEvent.ACTION_UP || event.actionMasked == MotionEvent.ACTION_POINTER_UP) {
            touches.remove(event.getPointerId(index))
            performClick()
        } else if (event.actionMasked == MotionEvent.ACTION_CANCEL) {
            touches.clear()
        }
        for (hand in 0..1) frames.axis(4+hand, if (touches.containsValue(hand)) 1f else 0f)
        return true
    }
    override fun performClick(): Boolean { super.performClick(); return true }
}
