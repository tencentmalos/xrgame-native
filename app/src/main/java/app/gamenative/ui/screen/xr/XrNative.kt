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
     * Composite backend for sessions created afterwards: the app-side Turnip (adrenotools,
     * [libraryName] in [driverDir], hooks in [hookDir]) through XR_KHR_vulkan_enable2 when
     * [vulkan], else GLES. The system property debug.xrgame.xr.composite (vulkan|gles) overrides.
     */
    external fun nativeSetComposite(vulkan: Boolean, driverDir: String, libraryName: String, hookDir: String)

    /**
     * Windows VR reconstruction on the Vulkan composite, applied from the next frame:
     * [filter] 0 off / 1 FSR1 / 2 SGSR1, [sharpness] 0..100, [foveation] 0 off / 1 fixed /
     * 2 eye tracked, [level] 0 low / 1 balanced / 2 high, [outputPercent] 50..100 of the runtime's
     * recommended eye size, [debug] tints by fragment density.
     */
    external fun nativeSetUpscale(filter: Int, sharpness: Int, foveation: Int, level: Int, outputPercent: Int, debug: Boolean)

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
     * involved at all. See DirectVulkanBridge/VulkanXrFrameBridge.
     */
    external fun nativeSetSharedGameBufferPtr(handle: Long, ahbPtr: Long)
}
