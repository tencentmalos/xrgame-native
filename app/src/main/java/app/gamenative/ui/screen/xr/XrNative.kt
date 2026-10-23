package app.gamenative.ui.screen.xr

import android.content.Context
import android.graphics.Bitmap
import android.hardware.HardwareBuffer

/**
 * Thin JNI wrapper around the native OpenXR session (app/src/main/cpp/xrimmersive).
 *
 * Not wired into the Gradle build by default — see the comment at the top of
 * app/src/main/cpp/xrimmersive/CMakeLists.txt. Until libxrimmersive.so is built and placed in
 * jniLibs/, loading this class will throw UnsatisfiedLinkError.
 */
object XrNative {
    init {
        System.loadLibrary("xrimmersive")
    }

    /** Starts the OpenXR session on its own native thread. Returns an opaque session handle. */
    external fun nativeCreate(activity: Context, quadWidth: Int, quadHeight: Int, refreshRate: Float): Long

    /** Signals the native frame-loop thread to stop; does not block. */
    external fun nativeRequestStop(handle: Long)

    /** Blocks until the native thread has exited, then frees native resources. */
    external fun nativeJoinAndDestroy(handle: Long)

    /**
     * Polls the latest controller snapshot.
     * outButtons must be an IntArray(1): [0] = Xbox-layout button bitmask.
     * outAxes must be a FloatArray(6): [leftX, leftY, rightX, rightY, triggerL, triggerR].
     * outHandPoses must be a FloatArray(12): [leftX,leftY,leftZ, leftFwdX,leftFwdY,leftFwdZ,
     * rightX,rightY,rightZ, rightFwdX,rightFwdY,rightFwdZ] — aim-pose ray per hand in the quad's
     * local space, meaningful only when outFlags[0] is true.
     * outFlags must be a BooleanArray(3): [0] = hand poses valid, [1] = the XR-pointer-mode toggle
     * (double-click of either thumbstick) fired since the last poll, [2] = Menu/Start button is
     * currently physically held (level, not edge — true for the whole hold).
     * Returns true if the quick-menu trigger (Start held 600ms) fired since the last poll.
     */
    external fun nativePollSnapshot(
        handle: Long,
        outButtons: IntArray,
        outAxes: FloatArray,
        outHandPoses: FloatArray,
        outFlags: BooleanArray,
    ): Boolean

    external fun nativeWaitWindowsFrame(
        handle: Long,
        afterSerial: Long,
        timeoutMs: Int,
        outTiming: LongArray,
        outViews: FloatArray,
        outInput: FloatArray,
        outFlags: IntArray,
    ): Boolean

    external fun nativeIsWindowsStereoActive(handle: Long): Boolean

    external fun nativeApplyWindowsHaptic(
        handle: Long,
        hand: Int,
        amplitude: Float,
        duration: Long,
        frequency: Float,
    ): Boolean

    external fun nativeSetWindowsOverlayVisible(handle: Long, visible: Boolean)

    /**
     * While the quick menu or the pause screen has the controllers, Windows VR games keep the
     * controller poses but get no buttons, triggers or sticks.
     */
    external fun nativeSetWindowsInputBlocked(handle: Long, blocked: Boolean)

    /**
     * Shows an RGBA_8888 [bitmap] as a world-locked panel in place of the Windows game's frames
     * (game loading interstitials); null hides it. The pixels are copied during the call.
     */
    external fun nativeSetWindowsInterstitial(handle: Long, bitmap: Bitmap?)

    /** Locates the Windows game's poses at the measured display time of its frames (DebugBus vr_tuning). */
    external fun nativeSetWindowsPrediction(extended: Boolean)

    /**
     * Rigid correction of the grip pose handed to Windows games (DebugBus vr_grip), in the right
     * hand's grip frame: [pitch], [yaw], [roll] in degrees about X, Y, Z (in that order) and an
     * offset in metres. The left hand is mirrored.
     */
    external fun nativeSetGripCorrection(pitch: Float, yaw: Float, roll: Float, x: Float, y: Float, z: Float)

    /**
     * The performance HUD of immersive sessions (the Performance HUD switch), world-locked in LOCAL
     * space up and to the left of where the user faced when it was shown. It needs the Vulkan
     * composite; the GLES backend has none.
     */
    external fun nativeSetPerfHudVisible(visible: Boolean)

    /** The game's presented frame rate for sessions without a stereo projection (2D big screen). */
    external fun nativeSetPerfHudGameFps(fps: Float)

    /** The input driving the game's pad (XR controllers or a physical gamepad), shown as INPUT. */
    external fun nativeSetPerfHudInput(label: String)

    /** Shows or hides the XR keyboard (xr_keyboard.cpp); the panel can also close itself. */
    external fun nativeSetXrKeyboardVisible(visible: Boolean)

    /** Whether a hand's ray is on the open XR keyboard, as of its last frame. */
    external fun nativeXrKeyboardPointed(): Boolean

    /**
     * The XR keyboard's pinyin (xr_pinyin.cpp): the unpacked system dictionary, a writable user
     * dictionary, and whether the keyboard opens in pinyin.
     */
    external fun nativeSetPinyin(systemDictionary: String?, userDictionary: String?, chinese: Boolean)

    /** The launch panel on the big screen (xr_launch_panel.cpp, model from XrLaunchPanel.kt); null hides it. */
    external fun nativeSetLaunchPanel(model: String?)

    /** What was picked on the launch panel since the last call, one event per line, or null. */
    external fun nativeDrainLaunchPanel(): String?

    /**
     * Text typed on the XR keyboard since the last call, or null. U+E000 is Backspace, U+E001
     * Enter and U+E002 means the keyboard closed.
     */
    external fun nativeDrainXrKeyboard(): String?

    /** A physical gamepad's input for the open XR keyboard (XrKeyboardPad in xr_keyboard.h). */
    external fun nativeXrKeyboardPad(action: Int)

    /** DebugBus vr_keyboard status JSON; never contains typed text. */
    external fun nativeXrKeyboardStatus(): String

    /** One-line JSON state of the performance HUD (DebugBus vr_hud). */
    external fun nativePerfHudStatus(): String

    /**
     * scrcpy capture SDK commands of the immersive session (DebugBus capture_source,
     * capture_screenshot, capture_video): `key: value` lines; files go to [capturesDir].
     */
    external fun nativeCaptureCommand(command: String, args: Array<String>, capturesDir: String): String

    /** Whether this build links the scrcpy capture SDK (internal debug APKs). */
    external fun nativeCaptureAvailable(): Boolean

    // The capture SDK's 2D window producer (app.gamenative.xrgame.XrPanelCapture); main thread.
    external fun nativePanelAttach(): Boolean
    external fun nativePanelDetach()
    /** Flags: 1 draw a frame now, 2 a still is waiting, 4 tick every frame, 8 the Surface changed. */
    external fun nativePanelTick(width: Int, height: Int): Int
    external fun nativePanelSurface(): android.view.Surface?
    /** Encoded width in the high 32 bits, height in the low. */
    external fun nativePanelEncodedSize(): Long
    external fun nativePanelPresented()
    /** Token and file path of the waiting still, or null. */
    external fun nativePanelStillTarget(): Array<String>?
    external fun nativePanelStillDone(token: String, width: Int, height: Int, error: String)

    /**
     * Synthetic controller state (DebugBus input) merged into the real controllers for
     * [durationMs] (0 releases): [buttons] in XrGamepadBridge's layout plus bit 16 for Menu, and
     * each [axes] entry whose [axisMask] bit is set replaces leftX, leftY, rightX, rightY (snapshot
     * values, as nativePollSnapshot returns them), triggerL, triggerR, squeezeL, squeezeR. It reaches the game, the quick menu and Windows VR.
     */
    external fun nativeSetDebugInput(buttons: Int, axisMask: Int, axes: FloatArray, durationMs: Int)

    /**
     * Composite backend for sessions created afterwards: the app-side Turnip (adrenotools,
     * [libraryName] in [driverDir], hooks in [hookDir]) through XR_KHR_vulkan_enable2 when
     * [vulkan], else GLES. The system property debug.xrgame.xr.composite (vulkan|gles) overrides.
     */
    external fun nativeSetComposite(vulkan: Boolean, driverDir: String, libraryName: String, hookDir: String)

    /**
     * Windows VR reconstruction on the Vulkan composite, applied from the next frame:
     * [filter] 0 off / 1 FSR1 / 2 SGSR1, [sharpness] 0..100, [foveation] 0 off / 1 fixed /
     * 2 eye tracked, [level] 0 low / 1 balanced / 2 high, [outputPercent] 50..100 of the eye size of
     * [eyeBuffer] (0 recommended / 1 halfway to the limit / 2 the limit), [debug] tints by
     * fragment density. [frameGeneration] is the big screen's largest output multiple of the game
     * frame rate (1 off, 2..4), [flowPercent] the optical flow resolution (25..100).
     */
    external fun nativeSetUpscale(
        filter: Int,
        sharpness: Int,
        foveation: Int,
        level: Int,
        outputPercent: Int,
        eyeBuffer: Int,
        frameGeneration: Int,
        flowPercent: Int,
        debug: Boolean,
    )

    /**
     * The headset's eye views once a session located them: recommended width/height, largest
     * width/height, left eye FOV tangents left/right/up/down; null before that.
     */
    external fun nativeXrDisplayInfo(): FloatArray?

    /**
     * Hands off one PixelCopy'd frame of the game's actual rendered output (ARGB_8888) to be
     * drawn into the immersive quad layer. See ImmersiveXrActivity's capture loop.
     */
    external fun nativeSubmitFrame(handle: Long, bitmap: Bitmap)

    /**
     * Repositions/resizes the virtual screen quad. x/y are tangential (left/right, up/down)
     * offsets and z is always -distance — native reinterprets these as orbiting the player on
     * both axes at that distance (not plain Cartesian coordinates), so the quad stays facing the
     * player as x and/or y move away from 0 (see xr_immersive.cpp's submitQuadLayer). width/
     * height are the quad's real-world size in meters. contentScaleX/Y are content-half-extent /
     * quad-half-extent per axis (1.0 if there's no margin band beyond the content) — only
     * consumed by the direct-render shader, see kDirectQuadFragmentShader.
     */
    external fun nativeSetQuadTransform(
        handle: Long,
        x: Float,
        y: Float,
        z: Float,
        width: Float,
        height: Float,
        contentScaleX: Float,
        contentScaleY: Float,
    )

    /**
     * Toggles the XR_FB_passthrough layer behind the quad. No-op if the runtime/manifest
     * doesn't support it (logged from native, see xr_immersive.cpp's setupPassthrough()).
     */
    external fun nativeSetPassthroughEnabled(handle: Long, enabled: Boolean)

    /** The 2D big screen as a cylinder around the player; flat when the runtime lacks the layer. */
    external fun nativeSetCurvedScreen(handle: Long, curved: Boolean)

    /**
     * Direct-render path (see DirectGLBridge) — hands the native session the same
     * [HardwareBuffer] GLRenderer is writing the game's frame into on its own thread, so it can
     * be imported and sampled directly instead of going through PixelCopy. Pass the same buffer
     * object every frame; only needs calling again if the buffer identity itself changes.
     */
    external fun nativeSetSharedGameBuffer(handle: Long, hardwareBuffer: HardwareBuffer)

    /**
     * Same direct-render path, for VulkanRenderer's zero-copy scanout output — ahbPtr is the raw
     * AHardwareBuffer* (as a long) VulkanRenderer already has, with no HardwareBuffer Java object
     * involved at all. See DirectVulkanBridge/VulkanXrFrameBridge. 0 drops the shared frame.
     */
    external fun nativeSetSharedGameBufferPtr(handle: Long, ahbPtr: Long)

    /**
     * VulkanRenderer's scene target for the big screen: exchangePtr is its native
     * xrgame::XrFrameExchange* (a ring of AHardwareBuffers with the latest completed frame, see
     * xr_frame_exchange.h); the session keeps its own reference. 0 drops it.
     */
    external fun nativeSetSharedGameExchange(handle: Long, exchangePtr: Long)
}
