#include "../xrgame_profiler.h"
#include "xr_immersive.h"
#include "xr_vulkan_compositor.h"

#include "spatial/xr/XrEyeGazeTracker.h"

#include <android/log.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <unistd.h>
#include <vector>

#define LOG_TAG "xrimmersive"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace xrimmersive {

namespace {

// EGL_IMG_context_priority
constexpr EGLint kEglContextPriorityLevelImg = 0x3100;
constexpr EGLint kEglContextPriorityHighImg = 0x3101;

// Pico's per-thread KGSL priority (0 runtime/SurfaceFlinger, 4 xrshell, 8 focused app, 12
// unfocused app; four priorities per ringbuffer). The public system library libsysperftracker.so
// applies it with GPUOptimization::setPriority(pid, tid, prio); its spatial runtime lifts focused
// render threads to 4. At 4 our composite (ring 1) preempts the game, which Turnip runs at the
// default 8 (ring 2). At equal priority it queued ~23 ms behind the game's batches and the
// runtime paced the XR loop to that (Swan run19). EGL_IMG_context_priority left us at 8 (run20).
constexpr int kPicoCompositeGpuPriority = 4;

int ReadPicoGpuPriority(int tid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/gpu_procs/%d/%d/status", getpid(), tid);
    FILE *file = std::fopen(path, "r");
    if (file == nullptr) return -1;
    char line[96];
    int priority = -1;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::sscanf(line, "ctxt_prio: %d", &priority) == 1) break;
    }
    std::fclose(file);
    return priority;
}

// Returns setPriority's result, or INT_MIN when the interface is missing. A null `when` is the
// periodic re-request: it logs only the first call and changes of the result.
int RequestPicoGpuPriority(int priority, const char *when) {
    static void *library = dlopen("libsysperftracker.so", RTLD_NOW | RTLD_LOCAL);
    using GetInstance = void *(*)();
    using SetPriority = int (*)(void *, int, int, int);
    static auto getInstance = library == nullptr ? nullptr : reinterpret_cast<GetInstance>(
        dlsym(library, "_ZN15GPUOptimization11getInstanceEv"));
    static auto setPriority = library == nullptr ? nullptr : reinterpret_cast<SetPriority>(
        dlsym(library, "_ZN15GPUOptimization11setPriorityEiii"));
    const int tid = gettid();
    if (getInstance == nullptr || setPriority == nullptr) {
        if (when != nullptr) LOGI("Pico GPU priority unavailable (%s): library=%d", when, library != nullptr ? 1 : 0);
        return INT_MIN;
    }
    const int result = setPriority(getInstance(), getpid(), tid, priority);
    const int error = result < 0 ? errno : 0;
    static int lastPeriodicResult = INT_MIN;
    if (when == nullptr) {
        if (result == lastPeriodicResult) return result;
        lastPeriodicResult = result;
        when = "periodic";
    }
    const int applied = ReadPicoGpuPriority(tid);
    LOGI("Pico GPU priority %s: tid=%d requested=%d result=%d errno=%d ctxt_prio=%d", when, tid,
         priority, result, error, applied);
    return result;
}

std::atomic<bool> gWindowsPredictionExtended{false};

// Correction applied to the grip pose handed to Windows games (DebugBus vr_grip): a rigid
// transform in each hand's grip frame. The right hand holds the values as given; the left is
// mirrored across the YZ plane.
std::mutex gGripCorrectionMutex;
std::array<XrPosef, 2> gGripCorrection{};
bool gGripCorrectionEnabled = false;

XrQuaternionf QuatMultiply(const XrQuaternionf &a, const XrQuaternionf &b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

XrQuaternionf QuatConjugate(const XrQuaternionf &q) { return {-q.x, -q.y, -q.z, q.w}; }

XrVector3f QuatRotate(const XrQuaternionf &q, const XrVector3f &v) {
    const XrQuaternionf p{v.x, v.y, v.z, 0.0f};
    const XrQuaternionf r = QuatMultiply(QuatMultiply(q, p), QuatConjugate(q));
    return {r.x, r.y, r.z};
}

XrQuaternionf QuatAxisAngle(float x, float y, float z, float degrees) {
    const float half = degrees * static_cast<float>(M_PI) / 360.0f;
    const float s = std::sin(half);
    return {x * s, y * s, z * s, std::cos(half)};
}

// `local` expressed in `base`'s frame.
XrPosef ComposePose(const XrPosef &base, const XrPosef &local) {
    XrPosef out;
    out.orientation = QuatMultiply(base.orientation, local.orientation);
    const XrVector3f offset = QuatRotate(base.orientation, local.position);
    out.position = {base.position.x + offset.x, base.position.y + offset.y, base.position.z + offset.z};
    return out;
}

// `pose` expressed in `base`'s frame.
XrPosef RelativePose(const XrPosef &base, const XrPosef &pose) {
    const XrQuaternionf inverse = QuatConjugate(base.orientation);
    XrPosef out;
    out.orientation = QuatMultiply(inverse, pose.orientation);
    out.position = QuatRotate(inverse, {pose.position.x - base.position.x, pose.position.y - base.position.y,
                                        pose.position.z - base.position.z});
    return out;
}

bool XrCheck(XrResult result, const char *what) {
    if (XR_FAILED(result)) {
        LOGE("%s failed: %d", what, static_cast<int>(result));
        return false;
    }
    return true;
}

bool IsInstanceExtensionSupported(const char *name) {
    uint32_t count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    std::vector<XrExtensionProperties> properties(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, properties.data());
    for (const auto &property : properties) {
        if (std::strcmp(property.extensionName, name) == 0) return true;
    }
    return false;
}

XrPosef IdentityPose() {
    XrPosef pose;
    pose.orientation.x = 0.0f;
    pose.orientation.y = 0.0f;
    pose.orientation.z = 0.0f;
    pose.orientation.w = 1.0f;
    pose.position.x = 0.0f;
    pose.position.y = 0.0f;
    pose.position.z = 0.0f;
    return pose;
}

}  // namespace

void SetWindowsPredictionExtended(bool extended) {
    gWindowsPredictionExtended.store(extended, std::memory_order_relaxed);
}

void SetWindowsGripCorrection(float pitch, float yaw, float roll, float x, float y, float z) {
    std::array<XrPosef, 2> corrections{};
    for (uint32_t hand = 0; hand < 2; ++hand) {
        const float mirror = hand == 0 ? -1.0f : 1.0f;
        // Pitch about X, then yaw about Y, then roll about Z, all in the grip frame.
        corrections[hand].orientation =
            QuatMultiply(QuatMultiply(QuatAxisAngle(1, 0, 0, pitch), QuatAxisAngle(0, 1, 0, mirror * yaw)),
                         QuatAxisAngle(0, 0, 1, mirror * roll));
        corrections[hand].position = {mirror * x, y, z};
    }
    std::lock_guard<std::mutex> lock(gGripCorrectionMutex);
    gGripCorrection = corrections;
    gGripCorrectionEnabled = pitch != 0.0f || yaw != 0.0f || roll != 0.0f || x != 0.0f || y != 0.0f || z != 0.0f;
    LOGI("Windows VR grip correction: pitch=%.1f yaw=%.1f roll=%.1f offset=(%.1f, %.1f, %.1f) mm", pitch, yaw, roll,
         x * 1000.0f, y * 1000.0f, z * 1000.0f);
}

XrImmersiveSession::XrImmersiveSession() = default;
XrImmersiveSession::~XrImmersiveSession() = default;

bool XrImmersiveSession::initialize(JavaVM *vm, jobject activityRef) {
    vm_ = vm;
    activityRef_ = activityRef;

    thread_ = std::thread([this] { runLoop(); });
    return true;
}

void XrImmersiveSession::requestStop() {
    stopRequested_.store(true);
    windowsSnapshotCondition_.notify_all();
}

void XrImmersiveSession::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

InputSnapshot XrImmersiveSession::pollSnapshot() {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    InputSnapshot result = snapshot_;
    // Rising-edge flags are consumed on read.
    snapshot_.quickMenuClicked = false;
    snapshot_.pointerModeToggled = false;
    return result;
}

bool XrImmersiveSession::waitWindowsRuntimeSnapshot(uint64_t afterSerial, uint32_t timeoutMs,
                                                     WindowsRuntimeSnapshot *snapshot) {
    if (snapshot == nullptr) return false;
    std::unique_lock<std::mutex> lock(windowsSnapshotMutex_);
    windowsSnapshotCondition_.wait_for(
        lock, std::chrono::milliseconds(timeoutMs),
        [this, afterSerial] { return windowsSnapshot_.frameSerial > afterSerial || stopRequested_.load(); });
    if (windowsSnapshot_.frameSerial <= afterSerial) return false;
    *snapshot = windowsSnapshot_;
    return true;
}

bool XrImmersiveSession::windowsStereoActive() const {
    return stereoActive_.load();
}

bool XrImmersiveSession::applyWindowsHaptic(uint32_t hand, float amplitude, XrDuration duration,
                                            float frequency) {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    if (hand > 1 || session_ == XR_NULL_HANDLE || hapticAction_ == XR_NULL_HANDLE) return false;
    XrHapticActionInfo actionInfo{XR_TYPE_HAPTIC_ACTION_INFO};
    actionInfo.action = hapticAction_;
    actionInfo.subactionPath = handPaths_[hand];
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    vibration.duration = duration;
    vibration.frequency = frequency;
    return XR_SUCCEEDED(xrApplyHapticFeedback(
        session_, &actionInfo, reinterpret_cast<const XrHapticBaseHeader *>(&vibration)));
}

void XrImmersiveSession::setWindowsOverlayVisible(bool visible) {
    windowsOverlayVisible_.store(visible);
}

void XrImmersiveSession::submitFrame(const uint8_t *rgbaPixels, int32_t width, int32_t height,
                                      int32_t strideBytes) {
    if (width <= 0 || height <= 0) return;
    std::lock_guard<std::mutex> lock(frameMutex_);
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    pendingFramePixels_.resize(rowBytes * static_cast<size_t>(height));
    if (strideBytes <= 0 || static_cast<size_t>(strideBytes) == rowBytes) {
        std::memcpy(pendingFramePixels_.data(), rgbaPixels, rowBytes * static_cast<size_t>(height));
    } else {
        // pendingFramePixels_ is always tightly packed — uploadPendingGameFrameLocked() feeds it
        // straight to glTexImage2D/glTexSubImage2D with no unpack row length set.
        for (int32_t y = 0; y < height; ++y) {
            std::memcpy(pendingFramePixels_.data() + rowBytes * static_cast<size_t>(y),
                        rgbaPixels + static_cast<size_t>(strideBytes) * static_cast<size_t>(y),
                        rowBytes);
        }
    }
    pendingFrameWidth_ = width;
    pendingFrameHeight_ = height;
    hasPendingFrame_ = true;
}

void XrImmersiveSession::configure(int32_t quadWidth, int32_t quadHeight, float refreshRate) {
    if (quadWidth > 0 && quadHeight > 0) {
        swapchainWidth_ = quadWidth;
        swapchainHeight_ = quadHeight;
    }
    if (refreshRate > 0.0f) requestedRefreshRate_ = refreshRate;
}

void XrImmersiveSession::setSharedGameBuffer(AHardwareBuffer *buffer) {
    std::lock_guard<std::mutex> lock(sharedBufferMutex_);
    // The renderer republishes the same buffer every frame; re-importing it would destroy and
    // recreate the EGLImage/texture on the render thread for nothing. Holding a reference on
    // pendingSharedBuffer_ also keeps its address from being recycled, so pointer identity is a
    // sound "same buffer" test.
    if (buffer == pendingSharedBuffer_) return;
    if (pendingSharedBuffer_ != nullptr) {
        AHardwareBuffer_release(pendingSharedBuffer_);
    }
    if (buffer != nullptr) {
        AHardwareBuffer_acquire(buffer);
    }
    pendingSharedBuffer_ = buffer;
    sharedBufferChanged_ = true;
}

// Must run on the render thread (this thread) — glEGLImageTargetTexture2DOES needs this
// thread's EGL context current. AHardwareBuffer's underlying memory is what's actually shared
// across contexts/processes; the EGLImage/texture wrapping it is per-context, so importing
// again here (GLRenderer already imported the same buffer once, into its own context) is
// expected and correct, not a duplicate/wasted step.
void XrImmersiveSession::importSharedBufferIfNeeded() {
    AHardwareBuffer *buffer = nullptr;
    {
        std::lock_guard<std::mutex> lock(sharedBufferMutex_);
        if (!sharedBufferChanged_) return;
        buffer = pendingSharedBuffer_;
        sharedBufferChanged_ = false;
        // Take our own reference before dropping the mutex: a concurrent setSharedGameBuffer()
        // may release the session's reference while the import below is still running.
        if (buffer != nullptr) AHardwareBuffer_acquire(buffer);
    }
    if (buffer == nullptr) return;
    std::unique_ptr<AHardwareBuffer, void (*)(AHardwareBuffer *)> bufferRef(
        buffer, AHardwareBuffer_release);

    if (sharedGameImage_ != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR(eglDisplay_, sharedGameImage_);
        sharedGameImage_ = EGL_NO_IMAGE_KHR;
    }

    // setSharedGameBuffer() only re-arms sharedBufferChanged_ when the buffer identity changes,
    // so a failed import must re-arm it itself or this buffer would never be retried.
    auto retryNextFrame = [this, buffer] {
        std::lock_guard<std::mutex> lock(sharedBufferMutex_);
        if (pendingSharedBuffer_ == buffer) sharedBufferChanged_ = true;
    };

    EGLClientBuffer clientBuffer = eglGetNativeClientBufferANDROID(buffer);
    if (clientBuffer == nullptr) {
        LOGE("Immersive direct-render: eglGetNativeClientBufferANDROID failed");
        retryNextFrame();
        return;
    }

    const EGLint attrs[] = {EGL_NONE};
    sharedGameImage_ = eglCreateImageKHR(eglDisplay_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                          clientBuffer, attrs);
    if (sharedGameImage_ == EGL_NO_IMAGE_KHR) {
        LOGE("Immersive direct-render: eglCreateImageKHR failed (0x%x)", eglGetError());
        retryNextFrame();
        return;
    }

    if (sharedGameTexture_ == 0) {
        glGenTextures(1, &sharedGameTexture_);
    }
    glBindTexture(GL_TEXTURE_2D, sharedGameTexture_);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, sharedGameImage_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    hasSharedGameTexture_ = true;
    LOGI("Immersive direct-render: shared game buffer imported into XR thread's context (texture=%u)",
         sharedGameTexture_);
}

void XrImmersiveSession::runLoop() {
    if (!setupInstanceAndSession()) {
        LOGE("OpenXR setup failed, aborting immersive session thread");
        teardown();
        return;
    }

    // The Pico system puts the focused app's GPU contexts back to its default priority 8 after we
    // raised ours (Swan run26: requested 4 with result 0, read back 8 during play), and the
    // composite again queued behind the game. Re-request it once a second from this thread.
    auto lastPriorityRequest = std::chrono::steady_clock::now();
    while (!stopRequested_.load()) {
        pollXrEvents();
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPriorityRequest >= std::chrono::seconds(1)) {
            lastPriorityRequest = now;
            RequestPicoGpuPriority(kPicoCompositeGpuPriority, nullptr);
        }

        if (!sessionRunning_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        // A lost device must not reach xrEndFrame with its layers: the Pico runtime keeps the
        // rejected layers and later overflows.
        if (vulkan_ && vulkan_->context().lost()) {
            LOGE("Vulkan composite device lost — ending the immersive session");
            break;
        }

        XrProfileScope frameProfile("host.vr.xr.frame");
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        XrResult waited;
        {
            XrProfileScope waitProfile("host.vr.xr.wait_frame");
            waited = xrWaitFrame(session_, &waitInfo, &frameState);
        }
        if (!XrCheck(waited, "xrWaitFrame")) break;

        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        if (!XrCheck(xrBeginFrame(session_, &beginInfo), "xrBeginFrame")) break;

        XrProfileScope inputProfile("host.vr.xr.input_locate");
        applyPendingPassthroughState();
        syncControllerInputs(frameState.predictedDisplayTime);
        locateGaze(frameState.predictedDisplayTime);

        WindowsRuntimeSnapshot runtimeSnapshot;
        {
            std::lock_guard<std::mutex> lock(windowsSnapshotMutex_);
            runtimeSnapshot = windowsSnapshot_;
        }
        runtimeSnapshot.frameSerial += 1;
        // The game shows this snapshot's frame several XR periods after this one; with the
        // extended prediction its poses are located at that measured display time.
        XrTime poseTime = frameState.predictedDisplayTime;
        if (gWindowsPredictionExtended.load(std::memory_order_relaxed) && predictionLead_ > 0.0f) {
            poseTime += static_cast<XrTime>(std::min(predictionLead_, 4.0f) *
                                            static_cast<float>(frameState.predictedDisplayPeriod));
        }
        runtimeSnapshot.predictedDisplayTime = poseTime;
        runtimeSnapshot.predictedDisplayPeriod = frameState.predictedDisplayPeriod;
        runtimeSnapshot.sessionState = sessionState_;
        runtimeSnapshot.shouldRender = frameState.shouldRender == XR_TRUE;
        runtimeSnapshot.recenterSerial = recenterSerial_.load();
        XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        viewLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        viewLocateInfo.displayTime = poseTime;
        viewLocateInfo.space = windowsTrackingSpace_;
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t viewCount = 0;
        if (XR_SUCCEEDED(xrLocateViews(session_, &viewLocateInfo, &viewState, 2, &viewCount,
                                       runtimeSnapshot.views.data())) && viewCount == 2) {
            runtimeSnapshot.viewStateFlags = viewState.viewStateFlags;
        } else {
            runtimeSnapshot.viewStateFlags = 0;
        }
        {
            std::lock_guard<std::mutex> lock(snapshotMutex_);
            runtimeSnapshot.input = snapshot_;
        }
        syncWindowsTrackingPoses(&runtimeSnapshot.input, poseTime);
        {
            std::lock_guard<std::mutex> lock(windowsSnapshotMutex_);
            windowsSnapshot_ = runtimeSnapshot;
        }
        windowsSnapshotCondition_.notify_all();
        // FRAME_SYNC replies with this serial; the game's snap field joins against it.
        xrgame_profile_counter(kXrCounterAndroidSerial, static_cast<int64_t>(runtimeSnapshot.frameSerial));
        inputProfile.end();

        // Every xrBeginFrame must be matched by an xrEndFrame, but a layer may only reference a
        // swapchain image that was actually acquired — so a failed renderFrame() still ends the
        // frame, just with no layers.
        if (frameState.shouldRender &&
            submitWindowsInterstitial(frameState.predictedDisplayTime, runtimeSnapshot.views,
                                      runtimeSnapshot.viewStateFlags)) {
        } else if (frameState.shouldRender &&
                   submitWindowsProjection(frameState.predictedDisplayTime, runtimeSnapshot.frameSerial)) {
        } else if (frameState.shouldRender && renderFrame()) {
            submitQuadLayer(frameState.predictedDisplayTime, localSpace_, swapchain_,
                             swapchainWidth_, swapchainHeight_, true);
        } else {
            XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
            endInfo.displayTime = frameState.predictedDisplayTime;
            endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            endInfo.layerCount = 0;
            endInfo.layers = nullptr;
            XrProfileScope endProfile("host.vr.xr.end_frame");
            xrEndFrame(session_, &endInfo);
        }
    }

    teardown();
}

bool XrImmersiveSession::setupInstanceAndSession() {
    // Android requires the loader to be explicitly initialized before xrCreateInstance.
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          reinterpret_cast<PFN_xrVoidFunction *>(&initializeLoader));
    if (initializeLoader == nullptr) {
        LOGE("xrInitializeLoaderKHR not available");
        return false;
    }

    XrLoaderInitInfoAndroidKHR loaderInitInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInitInfo.applicationVM = vm_;
    loaderInitInfo.applicationContext = activityRef_;
    if (!XrCheck(initializeLoader(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR *>(&loaderInitInfo)),
                 "xrInitializeLoaderKHR")) {
        return false;
    }

    std::vector<const char *> extensions = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    };
    passthroughExtensionAvailable_ = IsInstanceExtensionSupported(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    if (passthroughExtensionAvailable_) {
        extensions.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
    } else {
        LOGI("XR_FB_passthrough not supported by this runtime — passthrough toggle will no-op");
    }

    // Perf/scheduling extensions GameNativeXR's libxr.so uses (confirmed via strings on the
    // binary — standard OpenXR extensions, nothing proprietary) that this module never
    // requested at all: without XR_EXT_performance_settings the runtime applies its own default
    // (often conservative) CPU/GPU clock levels for the session; without
    // XR_KHR_android_thread_settings, Horizon OS's scheduler has no hint which thread is the XR
    // render thread and may not prioritize it; XR_FB_display_refresh_rate lets us target a
    // lower refresh rate (less frame-budget pressure) instead of whatever the system default is.
    perfSettingsExtensionAvailable_ = IsInstanceExtensionSupported(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if (perfSettingsExtensionAvailable_) extensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    threadSettingsExtensionAvailable_ = IsInstanceExtensionSupported(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
    if (threadSettingsExtensionAvailable_) extensions.push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
    refreshRateExtensionAvailable_ = IsInstanceExtensionSupported(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (refreshRateExtensionAvailable_) extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    localFloorExtensionAvailable_ = IsInstanceExtensionSupported(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);
    if (localFloorExtensionAvailable_) extensions.push_back(XR_EXT_LOCAL_FLOOR_EXTENSION_NAME);
    const char *picoControllerExtension = "XR_BD_controller_interaction";
    const bool picoControllerExtensionAvailable = IsInstanceExtensionSupported(picoControllerExtension);
    if (picoControllerExtensionAvailable) extensions.push_back(picoControllerExtension);
    // The GLES binding stays enabled so a failed Vulkan device still leaves a working session.
    const vulkan::CompositeConfig compositeConfig = vulkan::GetCompositeConfig();
    const bool vulkanRequested =
        compositeConfig.vulkan && IsInstanceExtensionSupported(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    if (compositeConfig.vulkan && !vulkanRequested) LOGI("XR_KHR_vulkan_enable2 unavailable — GLES composite");
    if (vulkanRequested) extensions.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    eyeGazeExtensionAvailable_ =
        vulkanRequested && IsInstanceExtensionSupported(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    if (eyeGazeExtensionAvailable_) extensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);

    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = vm_;
    androidInfo.applicationActivity = activityRef_;

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &androidInfo;
    std::strncpy(createInfo.applicationInfo.applicationName, "GameNative",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    std::strncpy(createInfo.applicationInfo.engineName, "GameNative", XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.enabledExtensionNames = extensions.data();

    if (!XrCheck(xrCreateInstance(&createInfo, &instance_), "xrCreateInstance")) return false;

    XrSystemGetInfo systemGetInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!XrCheck(xrGetSystem(instance_, &systemGetInfo, &systemId_), "xrGetSystem")) return false;

    // Passthrough needs ALPHA_BLEND — but requesting a blend mode xrEndFrame doesn't list as
    // supported for this view config is a validation error (this runtime rejected every frame
    // with xrEndFrame failing -1 once passthrough was toggled on, until this check was added).
    uint32_t blendModeCount = 0;
    xrEnumerateEnvironmentBlendModes(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      0, &blendModeCount, nullptr);
    if (blendModeCount > 0) {
        std::vector<XrEnvironmentBlendMode> blendModes(blendModeCount);
        xrEnumerateEnvironmentBlendModes(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                          blendModeCount, &blendModeCount, blendModes.data());
        for (auto mode : blendModes) {
            if (mode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) alphaBlendSupported_ = true;
        }
    }
    if (!alphaBlendSupported_) {
        LOGI("XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND not supported by this runtime — passthrough toggle will no-op");
    }

    if (vulkanRequested) {
        // Turnip creates its KGSL context with the device, on this thread.
        RequestPicoGpuPriority(kPicoCompositeGpuPriority, "before vulkan device");
        auto compositor = std::make_unique<vulkan::Compositor>();
        if (compositor->create(instance_, systemId_, compositeConfig)) {
            vulkan_ = std::move(compositor);
            RequestPicoGpuPriority(kPicoCompositeGpuPriority, "after vulkan device");
        } else {
            compositor->destroy();
            eyeGazeExtensionAvailable_ = false;
            LOGE("Vulkan composite unavailable — falling back to GLES");
        }
    }

    if (!vulkan_) {
        PFN_xrGetOpenGLESGraphicsRequirementsKHR getGraphicsRequirements = nullptr;
        xrGetInstanceProcAddr(instance_, "xrGetOpenGLESGraphicsRequirementsKHR",
                              reinterpret_cast<PFN_xrVoidFunction *>(&getGraphicsRequirements));
        XrGraphicsRequirementsOpenGLESKHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        if (getGraphicsRequirements != nullptr) {
            getGraphicsRequirements(instance_, systemId_, &graphicsRequirements);
        }

        // --- EGL context, dedicated to this session (not yet shared with the app's own
        // GLRenderer/DXVK-facing surface — see the header comment / plan follow-ups). ---
        eglDisplay_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint eglMajor, eglMinor;
        eglInitialize(eglDisplay_, &eglMajor, &eglMinor);
        eglBindAPI(EGL_OPENGL_ES_API);

        const EGLint configAttribs[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        EGLint numConfigs = 0;
        eglChooseConfig(eglDisplay_, configAttribs, &eglConfig_, 1, &numConfigs);
        if (numConfigs == 0) {
            LOGE("eglChooseConfig found no matching config");
            return false;
        }

        // The Windows game renders through Turnip on the same GPU at the default priority, in
        // command batches of up to ~25 ms. At equal priority our small per-frame composite queued
        // behind them for ~23 ms (KGSL, Swan run19), and the runtime paced xrWaitFrame to that.
        // A higher-priority context gets its own ringbuffer and preempts the game like the
        // compositor does. Pico's per-thread interface works for apps; the standard EGL request is
        // kept for drivers that honour it. Both fall back to the default silently.
        RequestPicoGpuPriority(kPicoCompositeGpuPriority, "before context");
        const char *eglExtensions = eglQueryString(eglDisplay_, EGL_EXTENSIONS);
        const bool priorityExtension =
            eglExtensions != nullptr && std::strstr(eglExtensions, "EGL_IMG_context_priority") != nullptr;
        if (priorityExtension) {
            const EGLint highPriorityAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                                                  kEglContextPriorityLevelImg, kEglContextPriorityHighImg, EGL_NONE};
            eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, highPriorityAttribs);
        }
        if (eglContext_ == EGL_NO_CONTEXT) {
            const EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
            eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, contextAttribs);
        }
        EGLint contextPriority = 0;
        if (priorityExtension) {
            eglQueryContext(eglDisplay_, eglContext_, kEglContextPriorityLevelImg, &contextPriority);
        }
        LOGI("EGL context priority: extension=%d level=0x%x (high=0x%x)", priorityExtension ? 1 : 0,
             contextPriority, kEglContextPriorityHighImg);

        const EGLint pbufferAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        eglPbufferSurface_ = eglCreatePbufferSurface(eglDisplay_, eglConfig_, pbufferAttribs);
        eglMakeCurrent(eglDisplay_, eglPbufferSurface_, eglPbufferSurface_, eglContext_);
        RequestPicoGpuPriority(kPicoCompositeGpuPriority, "after context");
    }

    XrGraphicsBindingOpenGLESAndroidKHR graphicsBinding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    graphicsBinding.display = eglDisplay_;
    graphicsBinding.config = eglConfig_;
    graphicsBinding.context = eglContext_;

    XrGraphicsBindingVulkan2KHR vulkanBinding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    if (vulkan_) vulkanBinding = vulkan_->binding();

    XrSessionCreateInfo sessionCreateInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.next = vulkan_ ? static_cast<const void *>(&vulkanBinding) : &graphicsBinding;
    sessionCreateInfo.systemId = systemId_;
    XrSession createdSession = XR_NULL_HANDLE;
    if (!XrCheck(xrCreateSession(instance_, &sessionCreateInfo, &createdSession), "xrCreateSession")) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        session_ = createdSession;
    }

    // Perf levels: request BOOST for both CPU and GPU domains — this game is running Box64
    // (x86 emulation) + Wine + our own screen-scraping capture on top of everything else, so
    // there's no reason to accept the runtime's own default (often more conservative) clock
    // levels for an immersive session that specifically exists to maximize performance.
    if (perfSettingsExtensionAvailable_) {
        PFN_xrPerfSettingsSetPerformanceLevelEXT setPerfLevel = nullptr;
        xrGetInstanceProcAddr(instance_, "xrPerfSettingsSetPerformanceLevelEXT",
                              reinterpret_cast<PFN_xrVoidFunction *>(&setPerfLevel));
        if (setPerfLevel != nullptr) {
            // BOOST is documented as a short-burst level, not meant for continuous sustained
            // rendering — requesting it for an entire session risks hitting thermal limits
            // sooner, which the runtime then compensates for by throttling clocks back down,
            // net WORSE and less consistent than just requesting the sustained level up front
            // (confirmed by testing: frame pacing got worse after BOOST was added, not better).
            XrCheck(setPerfLevel(session_, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT),
                    "xrPerfSettingsSetPerformanceLevelEXT(CPU)");
            XrCheck(setPerfLevel(session_, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT),
                    "xrPerfSettingsSetPerformanceLevelEXT(GPU)");
        }
    }

    // Thread hint: tells Horizon OS's scheduler which OS thread is the XR render thread (this
    // one — setupInstanceAndSession() runs on the dedicated thread created in initialize(), so
    // gettid() here is correct) so it can prioritize it. Without this the scheduler has no
    // signal that this thread's timing matters for frame delivery.
    if (threadSettingsExtensionAvailable_) {
        PFN_xrSetAndroidApplicationThreadKHR setThread = nullptr;
        xrGetInstanceProcAddr(instance_, "xrSetAndroidApplicationThreadKHR",
                              reinterpret_cast<PFN_xrVoidFunction *>(&setThread));
        if (setThread != nullptr) {
            XrCheck(setThread(session_, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, gettid()),
                    "xrSetAndroidApplicationThreadKHR");
        }
    }

    if (refreshRateExtensionAvailable_) {
        PFN_xrEnumerateDisplayRefreshRatesFB enumerateRates = nullptr;
        PFN_xrRequestDisplayRefreshRateFB requestRate = nullptr;
        xrGetInstanceProcAddr(instance_, "xrEnumerateDisplayRefreshRatesFB",
                              reinterpret_cast<PFN_xrVoidFunction *>(&enumerateRates));
        xrGetInstanceProcAddr(instance_, "xrRequestDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction *>(&requestRate));
        if (enumerateRates != nullptr && requestRate != nullptr) {
            uint32_t count = 0;
            enumerateRates(session_, 0, &count, nullptr);
            std::vector<float> rates(count);
            if (count > 0 && XR_SUCCEEDED(enumerateRates(session_, count, &count, rates.data()))) {
                bool supported = false;
                for (float r : rates) {
                    if (std::fabs(r - requestedRefreshRate_) < 0.5f) { supported = true; break; }
                }
                if (supported) {
                    XrCheck(requestRate(session_, requestedRefreshRate_), "xrRequestDisplayRefreshRateFB");
                    LOGI("Requested %.0f Hz display refresh rate", requestedRefreshRate_);
                } else {
                    LOGI("Refresh rate %.0f Hz not offered by the runtime — keeping the system default",
                         requestedRefreshRate_);
                }
            }
        }
    }

    XrReferenceSpaceCreateInfo spaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceCreateInfo.poseInReferenceSpace = IdentityPose();
    if (!XrCheck(xrCreateReferenceSpace(session_, &spaceCreateInfo, &localSpace_),
                 "xrCreateReferenceSpace")) {
        return false;
    }
    windowsTrackingSpace_ = localSpace_;
    spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    const XrResult stageSpaceResult = xrCreateReferenceSpace(session_, &spaceCreateInfo, &stageSpace_);
    if (stageSpaceResult != XR_SUCCESS) stageSpace_ = XR_NULL_HANDLE;
    if (localFloorExtensionAvailable_) {
        spaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
        const XrResult floorResult = xrCreateReferenceSpace(session_, &spaceCreateInfo, &localFloorSpace_);
        if (floorResult != XR_SUCCESS) localFloorSpace_ = XR_NULL_HANDLE;
    }
    windowsTrackingSpaceType_ = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (localFloorSpace_ != XR_NULL_HANDLE) {
        windowsTrackingSpace_ = localFloorSpace_;
        windowsTrackingSpaceType_ = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
        LOGI("Windows VR tracking space: LOCAL_FLOOR (recenter follows the headset)");
    } else if (stageSpace_ != XR_NULL_HANDLE) {
        windowsTrackingSpace_ = stageSpace_;
        windowsTrackingSpaceType_ = XR_REFERENCE_SPACE_TYPE_STAGE;
        LOGI("Windows VR tracking space: STAGE");
    } else {
        LOGI("Windows VR tracking space: LOCAL fallback (xrCreateReferenceSpace STAGE=%d)",
             static_cast<int>(stageSpaceResult));
    }

    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats.data());
    // Prefer GL_SRGB8_ALPHA8: the compositor applies the display transfer per the swapchain
    // format, and the content is sRGB-encoded — submitting it as linear GL_RGBA8 double-applies
    // gamma (washed-out output).
    int64_t chosenFormat = formats.empty() ? 0x8C43 /* GL_SRGB8_ALPHA8 */ : formats[0];
    if (vulkan_) {
        chosenFormat = vulkan::Compositor::ChooseFormat(formats, &srgbSwapchain_);
    } else {
        for (int64_t f : formats) {
            if (f == 0x8C43) {
                chosenFormat = f;
                srgbSwapchain_ = true;
                break;
            }
        }
    }
    if (!vulkan_ && !srgbSwapchain_) {
        for (int64_t f : formats) {
            if (f == 0x8058 /* GL_RGBA8 */) {
                chosenFormat = f;
                break;
            }
        }
    }

    XrSwapchainCreateInfo swapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swapchainCreateInfo.format = chosenFormat;
    swapchainCreateInfo.sampleCount = 1;
    swapchainCreateInfo.width = swapchainWidth_;
    swapchainCreateInfo.height = swapchainHeight_;
    swapchainCreateInfo.faceCount = 1;
    swapchainCreateInfo.arraySize = 1;
    swapchainCreateInfo.mipCount = 1;
    if (!XrCheck(xrCreateSwapchain(session_, &swapchainCreateInfo, &swapchain_), "xrCreateSwapchain")) {
        return false;
    }
    swapchainFormat_ = chosenFormat;

    uint32_t imageCount = 0;
    xrEnumerateSwapchainImages(swapchain_, 0, &imageCount, nullptr);
    if (vulkan_) {
        if (!vulkan_->attachSwapchain(vulkan::Compositor::kQuad, swapchain_, chosenFormat, swapchainWidth_,
                                      swapchainHeight_)) {
            LOGE("Vulkan composite: quad swapchain images unavailable");
            return false;
        }
    } else {
        swapchainImages_.resize(imageCount);
        for (auto &image : swapchainImages_) image.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        xrEnumerateSwapchainImages(
            swapchain_, imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader *>(swapchainImages_.data()));
        glGenFramebuffers(1, &framebuffer_);
    }

    uint32_t projectionViewCount = 0;
    xrEnumerateViewConfigurationViews(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      0, &projectionViewCount, nullptr);
    std::vector<XrViewConfigurationView> projectionViews(
        projectionViewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (projectionViewCount >= 2 && XR_SUCCEEDED(xrEnumerateViewConfigurationViews(
            instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            projectionViewCount, &projectionViewCount, projectionViews.data()))) {
        {
            std::lock_guard<std::mutex> lock(windowsSnapshotMutex_);
            windowsSnapshot_.recommendedWidth = projectionViews[0].recommendedImageRectWidth;
            windowsSnapshot_.recommendedHeight = projectionViews[0].recommendedImageRectHeight;
            const XrResult boundsResult = xrGetReferenceSpaceBoundsRect(
                session_, XR_REFERENCE_SPACE_TYPE_STAGE, &windowsSnapshot_.stageBounds);
            windowsSnapshot_.stageSpaceActive = stageSpace_ != XR_NULL_HANDLE;
            windowsSnapshot_.stageAvailable = stageSpace_ != XR_NULL_HANDLE &&
                boundsResult == XR_SUCCESS &&
                windowsSnapshot_.stageBounds.width > 0.0f &&
                windowsSnapshot_.stageBounds.height > 0.0f;
            if (!windowsSnapshot_.stageAvailable) windowsSnapshot_.stageBounds = {0.0f, 0.0f};
            LOGI("Windows VR stage: active=%d bounds=%d %.3fx%.3f result=%d",
                 windowsSnapshot_.stageSpaceActive ? 1 : 0,
                 windowsSnapshot_.stageAvailable ? 1 : 0,
                 windowsSnapshot_.stageBounds.width,
                 windowsSnapshot_.stageBounds.height,
                 static_cast<int>(boundsResult));
        }
        if (vulkan_) {
            windowsProjectionReady_ = vulkan_->projection().initialize(
                &vulkan_->context(), session_, static_cast<VkFormat>(chosenFormat), srgbSwapchain_,
                projectionViews[0].recommendedImageRectWidth,
                projectionViews[0].recommendedImageRectHeight);
        } else {
            windowsProjectionReady_ = windowsProjection_.initialize(
                session_, chosenFormat,
                projectionViews[0].recommendedImageRectWidth,
                projectionViews[0].recommendedImageRectHeight,
                eglDisplay_);
        }
    }
        windowsTransport_.start("@gamenative-xr");

    // --- Input: action set + Meta Quest Touch controller bindings. ---
    XrActionSetCreateInfo actionSetInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(actionSetInfo.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(actionSetInfo.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    actionSetInfo.priority = 0;
    if (!XrCheck(xrCreateActionSet(instance_, &actionSetInfo, &actionSet_), "xrCreateActionSet")) {
        return false;
    }

    auto createAction = [this](const char *name, XrActionType type, XrAction *out) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
        std::strncpy(info.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
        info.actionType = type;
        info.countSubactionPaths = 0;
        XrCheck(xrCreateAction(actionSet_, &info, out), name);
    };

    createAction("button_x", XR_ACTION_TYPE_BOOLEAN_INPUT, &buttonXAction_);
    createAction("button_y", XR_ACTION_TYPE_BOOLEAN_INPUT, &buttonYAction_);
    createAction("button_a", XR_ACTION_TYPE_BOOLEAN_INPUT, &buttonAAction_);
    createAction("button_b", XR_ACTION_TYPE_BOOLEAN_INPUT, &buttonBAction_);
    createAction("squeeze_left", XR_ACTION_TYPE_FLOAT_INPUT, &squeezeLAction_);
    createAction("squeeze_right", XR_ACTION_TYPE_FLOAT_INPUT, &squeezeRAction_);
    createAction("trigger_left", XR_ACTION_TYPE_FLOAT_INPUT, &triggerLAction_);
    createAction("trigger_right", XR_ACTION_TYPE_FLOAT_INPUT, &triggerRAction_);
    createAction("thumbstick_left", XR_ACTION_TYPE_VECTOR2F_INPUT, &thumbstickLAction_);
    createAction("thumbstick_right", XR_ACTION_TYPE_VECTOR2F_INPUT, &thumbstickRAction_);
    createAction("thumbstick_left_click", XR_ACTION_TYPE_BOOLEAN_INPUT, &thumbstickLClickAction_);
    createAction("thumbstick_right_click", XR_ACTION_TYPE_BOOLEAN_INPUT, &thumbstickRClickAction_);
    createAction("menu_left", XR_ACTION_TYPE_BOOLEAN_INPUT, &menuLAction_);
    createAction("aim_pose_left", XR_ACTION_TYPE_POSE_INPUT, &aimPoseLeftAction_);
    createAction("aim_pose_right", XR_ACTION_TYPE_POSE_INPUT, &aimPoseRightAction_);
    createAction("grip_pose_left", XR_ACTION_TYPE_POSE_INPUT, &gripPoseLeftAction_);
    createAction("grip_pose_right", XR_ACTION_TYPE_POSE_INPUT, &gripPoseRightAction_);

    auto path = [this](const char *p) {
        XrPath result = XR_NULL_PATH;
        xrStringToPath(instance_, p, &result);
        return result;
    };

    handPaths_[0] = path("/user/hand/left");
    handPaths_[1] = path("/user/hand/right");
    XrActionCreateInfo hapticInfo{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(hapticInfo.actionName, "haptic", XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(hapticInfo.localizedActionName, "Haptic", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    hapticInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
    hapticInfo.countSubactionPaths = 2;
    hapticInfo.subactionPaths = handPaths_;
    XrAction hapticAction = XR_NULL_HANDLE;
    XrCheck(xrCreateAction(actionSet_, &hapticInfo, &hapticAction), "haptic");
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        hapticAction_ = hapticAction;
    }

    std::vector<XrActionSuggestedBinding> bindings = {
        {buttonXAction_, path("/user/hand/left/input/x/click")},
        {buttonYAction_, path("/user/hand/left/input/y/click")},
        {buttonAAction_, path("/user/hand/right/input/a/click")},
        {buttonBAction_, path("/user/hand/right/input/b/click")},
        {squeezeLAction_, path("/user/hand/left/input/squeeze/value")},
        {squeezeRAction_, path("/user/hand/right/input/squeeze/value")},
        {triggerLAction_, path("/user/hand/left/input/trigger/value")},
        {triggerRAction_, path("/user/hand/right/input/trigger/value")},
        {thumbstickLAction_, path("/user/hand/left/input/thumbstick")},
        {thumbstickRAction_, path("/user/hand/right/input/thumbstick")},
        {thumbstickLClickAction_, path("/user/hand/left/input/thumbstick/click")},
        {thumbstickRClickAction_, path("/user/hand/right/input/thumbstick/click")},
        {menuLAction_, path("/user/hand/left/input/menu/click")},
        {aimPoseLeftAction_, path("/user/hand/left/input/aim/pose")},
        {aimPoseRightAction_, path("/user/hand/right/input/aim/pose")},
        {gripPoseLeftAction_, path("/user/hand/left/input/grip/pose")},
        {gripPoseRightAction_, path("/user/hand/right/input/grip/pose")},
        {hapticAction_, path("/user/hand/left/output/haptic")},
        {hapticAction_, path("/user/hand/right/output/haptic")},
    };

    auto suggestBindings = [this, &path](const char *profile,
                                         const std::vector<XrActionSuggestedBinding> &profileBindings) {
        XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggestion.interactionProfile = path(profile);
        suggestion.countSuggestedBindings = static_cast<uint32_t>(profileBindings.size());
        suggestion.suggestedBindings = profileBindings.data();
        const XrResult result = xrSuggestInteractionProfileBindings(instance_, &suggestion);
        if (XR_SUCCEEDED(result)) {
            LOGI("OpenXR controller bindings enabled: %s", profile);
        } else {
            LOGI("OpenXR controller bindings unavailable: %s result=%d", profile,
                 static_cast<int>(result));
        }
    };

    suggestBindings("/interaction_profiles/oculus/touch_controller", bindings);

    std::vector<XrActionSuggestedBinding> picoLegacyBindings = {
        {buttonXAction_, path("/user/hand/left/input/x/click")},
        {buttonYAction_, path("/user/hand/left/input/y/click")},
        {buttonAAction_, path("/user/hand/right/input/a/click")},
        {buttonBAction_, path("/user/hand/right/input/b/click")},
        {squeezeLAction_, path("/user/hand/left/input/squeeze/value")},
        {squeezeRAction_, path("/user/hand/right/input/squeeze/value")},
        {triggerLAction_, path("/user/hand/left/input/trigger")},
        {triggerRAction_, path("/user/hand/right/input/trigger")},
        {thumbstickLAction_, path("/user/hand/left/input/thumbstick")},
        {thumbstickRAction_, path("/user/hand/right/input/thumbstick")},
        {thumbstickLClickAction_, path("/user/hand/left/input/thumbstick/click")},
        {thumbstickRClickAction_, path("/user/hand/right/input/thumbstick/click")},
        {menuLAction_, path("/user/hand/left/input/menu/click")},
        {aimPoseLeftAction_, path("/user/hand/left/input/aim/pose")},
        {aimPoseRightAction_, path("/user/hand/right/input/aim/pose")},
        {hapticAction_, path("/user/hand/left/output/haptic")},
        {hapticAction_, path("/user/hand/right/output/haptic")},
    };
    if (picoControllerExtensionAvailable) {
        suggestBindings("/interaction_profiles/pico/neo3_controller", picoLegacyBindings);
        suggestBindings("/interaction_profiles/bytedance/pico_neo3_controller", bindings);
        suggestBindings("/interaction_profiles/bytedance/pico4_controller", bindings);
    }

    // The gaze action set must be attached in the same call as the controller set.
    if (eyeGazeExtensionAvailable_) {
        spatial::xr::XrEyeGazeFunctions functions{};
        functions.createActionSet = xrCreateActionSet;
        functions.destroyActionSet = xrDestroyActionSet;
        functions.createAction = xrCreateAction;
        functions.destroyAction = xrDestroyAction;
        functions.stringToPath = xrStringToPath;
        functions.suggestInteractionProfileBindings = xrSuggestInteractionProfileBindings;
        functions.createActionSpace = xrCreateActionSpace;
        functions.destroySpace = xrDestroySpace;
        functions.getActionStatePose = xrGetActionStatePose;
        functions.locateSpace = xrLocateSpace;
        functions.getInstanceProcAddr = xrGetInstanceProcAddr;
        spatial::xr::XrEyeGazeOptions options{};
        options.platform_hook = spatial::xr::EyeGazePlatformHook::PicoTrackingMode;
        gaze_ = std::make_unique<spatial::xr::XrEyeGazeTracker>(instance_, session_, functions, options);
        if (!gaze_->IsValid()) {
            LOGI("Eye gaze unavailable: %s", gaze_->GetError().c_str());
            gaze_.reset();
        }
    }
    std::array<XrActionSet, 2> actionSets{actionSet_, gaze_ ? gaze_->ActionSet() : XR_NULL_HANDLE};
    XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = gaze_ ? 2 : 1;
    attachInfo.actionSets = actionSets.data();
    XrCheck(xrAttachSessionActionSets(session_, &attachInfo), "xrAttachSessionActionSets");

    XrActionSpaceCreateInfo aimSpaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    aimSpaceInfo.poseInActionSpace = IdentityPose();
    aimSpaceInfo.action = aimPoseLeftAction_;
    XrCheck(xrCreateActionSpace(session_, &aimSpaceInfo, &aimSpaceLeft_), "xrCreateActionSpace(left aim)");
    aimSpaceInfo.action = aimPoseRightAction_;
    XrCheck(xrCreateActionSpace(session_, &aimSpaceInfo, &aimSpaceRight_), "xrCreateActionSpace(right aim)");
    aimSpaceInfo.action = gripPoseLeftAction_;
    XrCheck(xrCreateActionSpace(session_, &aimSpaceInfo, &gripSpaceLeft_), "xrCreateActionSpace(left grip)");
    aimSpaceInfo.action = gripPoseRightAction_;
    XrCheck(xrCreateActionSpace(session_, &aimSpaceInfo, &gripSpaceRight_), "xrCreateActionSpace(right grip)");

    if (passthroughExtensionAvailable_) {
        setupPassthrough();
    }

    LOGI("OpenXR immersive session initialized (%dx%d quad, %u swapchain images)",
         swapchainWidth_, swapchainHeight_, imageCount);
    return true;
}

void XrImmersiveSession::pollXrEvents() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (instance_ != XR_NULL_HANDLE && xrPollEvent(instance_, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto *stateEvent = reinterpret_cast<XrEventDataSessionStateChanged *>(&event);
            sessionState_ = stateEvent->state;
            LOGI("OpenXR session state changed: %d", static_cast<int>(sessionState_));
            switch (sessionState_) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XrCheck(xrBeginSession(session_, &beginInfo), "xrBeginSession");
                    sessionRunning_ = true;
                    // Pico enables eye tracking for a running session (xrSetTrackingModePICO).
                    if (gaze_ && !gaze_->IsAttached()) {
                        gaze_->OnActionSetsAttached();
                        LOGI("Eye gaze attached: platform hook attempted=%d result=%d",
                             gaze_->PlatformHookAttempted() ? 1 : 0, static_cast<int>(gaze_->PlatformHookResult()));
                    }
                    break;
                }
                case XR_SESSION_STATE_STOPPING:
                    xrEndSession(session_);
                    sessionRunning_ = false;
                    break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    stopRequested_.store(true);
                    break;
                default:
                    break;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            const auto *change = reinterpret_cast<const XrEventDataReferenceSpaceChangePending *>(&event);
            const bool tracked = change->referenceSpaceType == windowsTrackingSpaceType_ && change->poseValid;
            const uint32_t serial = tracked ? recenterSerial_.fetch_add(1) + 1 : recenterSerial_.load();
            LOGI("OpenXR reference space change: type=%d poseValid=%d counted=%d serial=%u",
                 static_cast<int>(change->referenceSpaceType), change->poseValid ? 1 : 0, tracked ? 1 : 0, serial);
        } else if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            auto logProfile = [this](const char *hand, XrPath handPath) {
                XrInteractionProfileState profileState{XR_TYPE_INTERACTION_PROFILE_STATE};
                const XrResult result = xrGetCurrentInteractionProfile(session_, handPath, &profileState);
                if (XR_FAILED(result) || profileState.interactionProfile == XR_NULL_PATH) {
                    LOGI("OpenXR active controller profile: %s unavailable result=%d", hand,
                         static_cast<int>(result));
                    return;
                }
                std::array<char, XR_MAX_PATH_LENGTH> profile{};
                uint32_t length = 0;
                if (XR_SUCCEEDED(xrPathToString(instance_, profileState.interactionProfile,
                                                static_cast<uint32_t>(profile.size()), &length,
                                                profile.data()))) {
                    LOGI("OpenXR active controller profile: %s %s", hand, profile.data());
                }
            };
            logProfile("left", handPaths_[0]);
            logProfile("right", handPaths_[1]);
        }
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

namespace {
constexpr char kQuadVertexShader[] =
    "#version 300 es\n"
    "layout(location = 0) in vec2 aPosition;\n"
    "layout(location = 1) in vec2 aTexCoord;\n"
    "out vec2 vTexCoord;\n"
    "void main() {\n"
    "    vTexCoord = aTexCoord;\n"
    "    gl_Position = vec4(aPosition, 0.0, 1.0);\n"
    "}\n";

constexpr char kQuadFragmentShader[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 vTexCoord;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D uTexture;\n"
    "uniform float uLinearizeSrc;\n"
    "vec3 toLinear(vec3 c) { return mix(c, pow(c, vec3(2.2)), uLinearizeSrc); }\n"
    "void main() {\n"
    "    vec4 c = texture(uTexture, vTexCoord);\n"
    "    fragColor = vec4(toLinear(c.rgb), c.a);\n"
    "}\n";

// Direct-render path: uGameTexture is the shared-buffer texture GLRenderer writes into on its
// own thread (always opaque, drawn as the base); uOverlayTexture is the same CPU-uploaded
// menu/HUD layer as the non-direct path, alpha-blended on top — same visual result as the
// non-direct path's Kotlin-side Canvas composite, just with the base layer sourced from a
// shared GPU buffer instead of a PixelCopy'd CPU bitmap.
//
// uContentScale: the quad itself is grown by a margin band (pointer-mode resize/move handles
// live there — see POINTER_BORDER_MARGIN_METERS in ImmersiveXrActivity.kt), but uGameTexture is
// the raw game frame at its own native resolution with no margin baked in — sampling it at
// vTexCoord directly (spanning the WHOLE, margin-grown quad) would stretch it into the margin.
// uContentScale is content-half-extent / quad-half-extent per axis (< 1.0 whenever a margin is
// present); remapping vTexCoord by it recovers the sub-rectangle of the quad's UV space that is
// actually "the game", and anything outside that rectangle (the margin) is treated as if the
// game layer were fully transparent there — only uOverlayTexture (which DOES span the whole
// quad, including the margin — same bitmap the non-direct path already draws its handles/cursor
// into) shows through, at ITS OWN alpha, matching the non-direct path's compositing model.
constexpr char kDirectQuadFragmentShader[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 vTexCoord;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D uGameTexture;\n"
    "uniform sampler2D uOverlayTexture;\n"
    "uniform vec2 uContentScale;\n"
    "uniform float uLinearizeSrc;\n"
    "vec3 toLinear(vec3 c) { return mix(c, pow(c, vec3(2.2)), uLinearizeSrc); }\n"
    "void main() {\n"
    "    vec2 gameUv = (vTexCoord - 0.5) / uContentScale + 0.5;\n"
    "    bool insideGame = gameUv.x >= 0.0 && gameUv.x <= 1.0 && gameUv.y >= 0.0 && gameUv.y <= 1.0;\n"
    "    vec4 game = insideGame ? texture(uGameTexture, gameUv) : vec4(0.0);\n"
    "    vec4 overlay = texture(uOverlayTexture, vTexCoord);\n"
    "    vec3 rgb = mix(toLinear(game.rgb), toLinear(overlay.rgb), overlay.a);\n"
    "    float alpha = insideGame ? 1.0 : overlay.a;\n"
    "    fragColor = vec4(rgb, alpha);\n"
    "}\n";

GLuint CompileShader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        LOGE("Shader compile error: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
}  // namespace

void XrImmersiveSession::ensureQuadGeometryAndShader() {
    if (quadProgram_ != 0) return;

    GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, kQuadVertexShader);
    GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, kQuadFragmentShader);
    if (vertexShader == 0 || fragmentShader == 0) return;

    quadProgram_ = glCreateProgram();
    glAttachShader(quadProgram_, vertexShader);
    glAttachShader(quadProgram_, fragmentShader);
    glLinkProgram(quadProgram_);
    GLint linked = 0;
    glGetProgramiv(quadProgram_, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(quadProgram_, sizeof(log), nullptr, log);
        LOGE("Shader link error: %s", log);
    }
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    quadPositionLoc_ = 0;
    quadTexCoordLoc_ = 1;
    quadSamplerLoc_ = glGetUniformLocation(quadProgram_, "uTexture");
    quadLinearizeLoc_ = glGetUniformLocation(quadProgram_, "uLinearizeSrc");
    glUseProgram(quadProgram_);
    glUniform1f(quadLinearizeLoc_, srgbSwapchain_ ? 1.0f : 0.0f);
    glUseProgram(0);

    // Full-screen quad (2 triangles as a strip): xy in clip space, uv in texture space.
    // v=0 maps to the bitmap's first (top) row, v=1 to its last (bottom) row — Android
    // Bitmaps are stored top-down, so this should show right-side-up; flip here if a
    // device test shows it upside down.
    const float vertices[] = {
        // x,    y,    u,    v
        -1.0f, -1.0f, 0.0f, 1.0f,
        1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f, 1.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 0.0f,
    };
    glGenBuffers(1, &quadVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenTextures(1, &gameTexture_);
    glBindTexture(GL_TEXTURE_2D, gameTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    GLuint directVertexShader = CompileShader(GL_VERTEX_SHADER, kQuadVertexShader);
    GLuint directFragmentShader = CompileShader(GL_FRAGMENT_SHADER, kDirectQuadFragmentShader);
    if (directVertexShader != 0 && directFragmentShader != 0) {
        directQuadProgram_ = glCreateProgram();
        glAttachShader(directQuadProgram_, directVertexShader);
        glAttachShader(directQuadProgram_, directFragmentShader);
        glLinkProgram(directQuadProgram_);
        GLint directLinked = 0;
        glGetProgramiv(directQuadProgram_, GL_LINK_STATUS, &directLinked);
        if (!directLinked) {
            char log[512];
            glGetProgramInfoLog(directQuadProgram_, sizeof(log), nullptr, log);
            LOGE("Direct quad shader link error: %s", log);
        }
        directQuadPositionLoc_ = 0;
        directQuadTexCoordLoc_ = 1;
        directGameSamplerLoc_ = glGetUniformLocation(directQuadProgram_, "uGameTexture");
        directOverlaySamplerLoc_ = glGetUniformLocation(directQuadProgram_, "uOverlayTexture");
        directContentScaleLoc_ = glGetUniformLocation(directQuadProgram_, "uContentScale");
        directLinearizeLoc_ = glGetUniformLocation(directQuadProgram_, "uLinearizeSrc");
        glUseProgram(directQuadProgram_);
        glUniform1f(directLinearizeLoc_, srgbSwapchain_ ? 1.0f : 0.0f);
        glUseProgram(0);
    }
    glDeleteShader(directVertexShader);
    glDeleteShader(directFragmentShader);
}

void XrImmersiveSession::uploadPendingGameFrameLocked() {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (!hasPendingFrame_) return;

    glBindTexture(GL_TEXTURE_2D, gameTexture_);
    if (pendingFrameWidth_ != gameTextureWidth_ || pendingFrameHeight_ != gameTextureHeight_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pendingFrameWidth_, pendingFrameHeight_, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, pendingFramePixels_.data());
        gameTextureWidth_ = pendingFrameWidth_;
        gameTextureHeight_ = pendingFrameHeight_;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pendingFrameWidth_, pendingFrameHeight_, GL_RGBA,
                         GL_UNSIGNED_BYTE, pendingFramePixels_.data());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    hasPendingFrame_ = false;
}

bool XrImmersiveSession::renderFrame() {
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t imageIndex = 0;
    if (!XrCheck(xrAcquireSwapchainImage(swapchain_, &acquireInfo, &imageIndex),
                 "xrAcquireSwapchainImage")) {
        return false;
    }

    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(swapchain_, &waitInfo);

    if (vulkan_) {
        const bool drawn = renderQuadImage(imageIndex);
        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(swapchain_, &releaseInfo);
        return drawn;
    }

    ensureQuadGeometryAndShader();
    importSharedBufferIfNeeded();
    uploadPendingGameFrameLocked();

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            swapchainImages_[imageIndex].image, 0);
    glViewport(0, 0, swapchainWidth_, swapchainHeight_);

    if (hasSharedGameTexture_ && directQuadProgram_ != 0) {
        // GLRenderer wrote this frame's game image straight into sharedGameTexture_ on its own
        // thread (zero CPU copy) — gameTexture_ here now carries just the overlay (menu/HUD),
        // still fed the same way as the non-direct path (uploadPendingGameFrameLocked, above).
        static int directFrameLogCounter = 0;
        if (++directFrameLogCounter % 300 == 1) {
            LOGI("Immersive direct-render: compositing shared game texture + overlay (frame #%d)",
                 directFrameLogCounter);
        }
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(directQuadProgram_);
        glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
        glEnableVertexAttribArray(directQuadPositionLoc_);
        glVertexAttribPointer(directQuadPositionLoc_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                               reinterpret_cast<void *>(0));
        glEnableVertexAttribArray(directQuadTexCoordLoc_);
        glVertexAttribPointer(directQuadTexCoordLoc_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                               reinterpret_cast<void *>(2 * sizeof(float)));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sharedGameTexture_);
        glUniform1i(directGameSamplerLoc_, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gameTexture_);
        glUniform1i(directOverlaySamplerLoc_, 1);
        glUniform2f(directContentScaleLoc_, quadContentScaleX_.load(), quadContentScaleY_.load());
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    } else if (gameTextureWidth_ > 0 && quadProgram_ != 0) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(quadProgram_);
        glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
        glEnableVertexAttribArray(quadPositionLoc_);
        glVertexAttribPointer(quadPositionLoc_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                               reinterpret_cast<void *>(0));
        glEnableVertexAttribArray(quadTexCoordLoc_);
        glVertexAttribPointer(quadTexCoordLoc_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                               reinterpret_cast<void *>(2 * sizeof(float)));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gameTexture_);
        glUniform1i(quadSamplerLoc_, 0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
    } else {
        // No game frame captured yet (e.g. libxrimmersive.so present but the app hasn't
        // reached ImmersiveXrActivity's capture loop yet) — flat color instead of garbage.
        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchain_, &releaseInfo);
    return true;
}

bool XrImmersiveSession::renderQuadImage(uint32_t imageIndex) {
    AHardwareBuffer *shared = nullptr;
    {
        std::lock_guard<std::mutex> lock(sharedBufferMutex_);
        shared = pendingSharedBuffer_;
        sharedBufferChanged_ = false;
        if (shared != nullptr) AHardwareBuffer_acquire(shared);
    }
    std::unique_ptr<AHardwareBuffer, void (*)(AHardwareBuffer *)> sharedRef(shared, AHardwareBuffer_release);
    std::lock_guard<std::mutex> lock(frameMutex_);
    const uint8_t *pixels = hasPendingFrame_ ? pendingFramePixels_.data() : nullptr;
    hasPendingFrame_ = false;
    return vulkan_->drawQuad(imageIndex, shared, pixels, static_cast<uint32_t>(pendingFrameWidth_),
                             static_cast<uint32_t>(pendingFrameHeight_), quadContentScaleX_.load(),
                             quadContentScaleY_.load());
}

void XrImmersiveSession::locateGaze(XrTime predictedDisplayTime) {
    if (!gaze_ || !vulkan_) return;
    const spatial::xr::EyeGazeSample sample = gaze_->Locate(windowsTrackingSpace_, predictedDisplayTime);
    vulkan_->projection().setGaze({sample.valid, sample.pose.orientation});
    static bool announced = false;
    if (sample.valid && !announced) {
        announced = true;
        LOGI("Eye gaze: first valid sample");
    }
}

bool XrImmersiveSession::renderWindowsProjection(XrCompositionLayerProjection *layer) {
    XrProfileScope renderProfile("host.vr.projection.render");
    if (vulkan_) return vulkan_->projection().render(windowsTransport_, windowsTrackingSpace_, layer);
    return windowsProjection_.render(windowsTransport_, windowsTrackingSpace_, layer);
}

void XrImmersiveSession::setWindowsInterstitial(const uint8_t *rgbaPixels, int32_t width,
                                                int32_t height, int32_t strideBytes) {
    std::lock_guard<std::mutex> lock(interstitialMutex_);
    if (rgbaPixels == nullptr || width <= 0 || height <= 0 || strideBytes < width * 4) {
        interstitialVisible_.store(false);
        interstitialPixels_.clear();
        interstitialChanged_ = false;
        return;
    }
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    interstitialPixels_.resize(rowBytes * static_cast<size_t>(height));
    for (int32_t row = 0; row < height; ++row) {
        std::memcpy(interstitialPixels_.data() + rowBytes * static_cast<size_t>(row),
                    rgbaPixels + static_cast<size_t>(strideBytes) * static_cast<size_t>(row), rowBytes);
    }
    interstitialWidth_ = width;
    interstitialHeight_ = height;
    interstitialChanged_ = true;
    interstitialVisible_.store(true);
}

// Draws the pending interstitial pixels into a freshly acquired image of its own swapchain.
// The runtime keeps showing the last released image, so this runs only when the text changes.
bool XrImmersiveSession::uploadInterstitialLocked() {
    if (interstitialSwapchain_ == XR_NULL_HANDLE || interstitialSwapchainWidth_ != interstitialWidth_ ||
        interstitialSwapchainHeight_ != interstitialHeight_) {
        if (vulkan_) vulkan_->detachSwapchain(vulkan::Compositor::kInterstitial);
        if (interstitialSwapchain_ != XR_NULL_HANDLE) xrDestroySwapchain(interstitialSwapchain_);
        interstitialSwapchain_ = XR_NULL_HANDLE;
        interstitialImages_.clear();
        interstitialImageReleased_ = false;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = swapchainFormat_;
        info.sampleCount = 1;
        info.width = static_cast<uint32_t>(interstitialWidth_);
        info.height = static_cast<uint32_t>(interstitialHeight_);
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (!XrCheck(xrCreateSwapchain(session_, &info, &interstitialSwapchain_),
                     "xrCreateSwapchain(interstitial)")) {
            interstitialSwapchain_ = XR_NULL_HANDLE;
            return false;
        }
        if (vulkan_) {
            if (!vulkan_->attachSwapchain(vulkan::Compositor::kInterstitial, interstitialSwapchain_,
                                          swapchainFormat_, static_cast<uint32_t>(interstitialWidth_),
                                          static_cast<uint32_t>(interstitialHeight_))) {
                return false;
            }
        } else {
            uint32_t count = 0;
            xrEnumerateSwapchainImages(interstitialSwapchain_, 0, &count, nullptr);
            interstitialImages_.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
            xrEnumerateSwapchainImages(interstitialSwapchain_, count, &count,
                                       reinterpret_cast<XrSwapchainImageBaseHeader *>(interstitialImages_.data()));
        }
        interstitialSwapchainWidth_ = interstitialWidth_;
        interstitialSwapchainHeight_ = interstitialHeight_;
    }

    if (vulkan_) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t imageIndex = 0;
        if (!XrCheck(xrAcquireSwapchainImage(interstitialSwapchain_, &acquire, &imageIndex),
                     "xrAcquireSwapchainImage(interstitial)")) {
            return false;
        }
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(interstitialSwapchain_, &wait);
        const bool drawn = vulkan_->drawInterstitial(imageIndex, interstitialPixels_.data(),
                                                     static_cast<uint32_t>(interstitialWidth_),
                                                     static_cast<uint32_t>(interstitialHeight_));
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (!XrCheck(xrReleaseSwapchainImage(interstitialSwapchain_, &release),
                     "xrReleaseSwapchainImage(interstitial)") || !drawn) {
            return false;
        }
        interstitialImageReleased_ = true;
        interstitialChanged_ = false;
        return true;
    }

    ensureQuadGeometryAndShader();
    if (quadProgram_ == 0) return false;
    if (interstitialTexture_ == 0) glGenTextures(1, &interstitialTexture_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, interstitialTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, interstitialWidth_, interstitialHeight_, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, interstitialPixels_.data());

    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t imageIndex = 0;
    if (!XrCheck(xrAcquireSwapchainImage(interstitialSwapchain_, &acquire, &imageIndex),
                 "xrAcquireSwapchainImage(interstitial)")) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return false;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(interstitialSwapchain_, &wait);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           interstitialImages_[imageIndex].image, 0);
    glViewport(0, 0, interstitialWidth_, interstitialHeight_);
    glDisable(GL_BLEND);
    glUseProgram(quadProgram_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glEnableVertexAttribArray(quadPositionLoc_);
    glVertexAttribPointer(quadPositionLoc_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void *>(0));
    glEnableVertexAttribArray(quadTexCoordLoc_);
    glVertexAttribPointer(quadTexCoordLoc_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void *>(2 * sizeof(float)));
    glUniform1i(quadSamplerLoc_, 0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (!XrCheck(xrReleaseSwapchainImage(interstitialSwapchain_, &release),
                 "xrReleaseSwapchainImage(interstitial)")) {
        return false;
    }
    interstitialImageReleased_ = true;
    interstitialChanged_ = false;
    return true;
}

// The game's loading interstitial (Half-Life: Alyx draws it through SteamVR, which we replace)
// is shown as a world-locked panel instead of the game's projection. The game's frames are still
// consumed and released, so its swapchain waits never block on us while it loads or waits for the
// trigger press that ends the interstitial.
bool XrImmersiveSession::submitWindowsInterstitial(XrTime predictedDisplayTime,
                                                   const std::array<XrView, 2> &views,
                                                   XrViewStateFlags viewStateFlags) {
    if (!interstitialVisible_.load()) {
        interstitialPoseValid_ = false;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(interstitialMutex_);
        if (interstitialChanged_ && !uploadInterstitialLocked()) return false;
    }
    if (!interstitialImageReleased_) return false;

    if (!interstitialPoseValid_) {
        constexpr XrViewStateFlags kTracked =
            XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        constexpr float kDistance = 1.6f;
        float headX = 0.0f, headY = 1.6f, headZ = 0.0f, forwardX = 0.0f, forwardZ = -1.0f;
        if ((viewStateFlags & kTracked) == kTracked) {
            headX = 0.5f * (views[0].pose.position.x + views[1].pose.position.x);
            headY = 0.5f * (views[0].pose.position.y + views[1].pose.position.y);
            headZ = 0.5f * (views[0].pose.position.z + views[1].pose.position.z);
            const XrQuaternionf &q = views[0].pose.orientation;
            const float fx = -2.0f * (q.x * q.z + q.w * q.y);
            const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
            const float length = std::sqrt(fx * fx + fz * fz);
            if (length > 0.001f) {
                forwardX = fx / length;
                forwardZ = fz / length;
            }
        }
        const float yaw = std::atan2(-forwardX, -forwardZ);
        interstitialPose_.orientation = {0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)};
        interstitialPose_.position = {headX + forwardX * kDistance, headY, headZ + forwardZ * kDistance};
        interstitialPoseValid_ = true;
        LOGI("Windows VR interstitial shown %dx%d at (%.2f, %.2f, %.2f)", interstitialSwapchainWidth_,
             interstitialSwapchainHeight_, interstitialPose_.position.x, interstitialPose_.position.y,
             interstitialPose_.position.z);
    }

    if (windowsProjectionReady_ && windowsTransport_.hasStereoContent()) {
        XrCompositionLayerProjection unused{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        renderWindowsProjection(&unused);
    }

    constexpr float kWidth = 1.4f;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.space = windowsTrackingSpace_;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = interstitialSwapchain_;
    quad.subImage.imageRect = {{0, 0}, {interstitialSwapchainWidth_, interstitialSwapchainHeight_}};
    quad.subImage.imageArrayIndex = 0;
    quad.pose = interstitialPose_;
    quad.size = {kWidth, kWidth * static_cast<float>(interstitialSwapchainHeight_) /
                             static_cast<float>(interstitialSwapchainWidth_)};
    const XrCompositionLayerBaseHeader *layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad)};
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = 1;
    endInfo.layers = layers;
    XrProfileScope endProfile("host.vr.xr.end_frame");
    XrCheck(xrEndFrame(session_, &endInfo), "xrEndFrame(interstitial)");
    return true;
}

bool XrImmersiveSession::submitWindowsProjection(XrTime predictedDisplayTime, uint64_t xrSerial) {
    if (!windowsProjectionReady_ || !windowsTransport_.hasStereoContent()) {
        stereoActive_.store(false);
        stereoMisses_ = 0;
        return false;
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const bool rendered = renderWindowsProjection(&projection);
    if (!rendered) {
        if (++stereoMisses_ >= 8) stereoActive_.store(false);
        return false;
    }
    stereoMisses_ = 0;
    stereoActive_.store(true);
    const int64_t snap = vulkan_ ? vulkan_->projection().lastFreshSnap() : windowsProjection_.lastFreshSnap();
    if (snap >= 0 && xrSerial >= static_cast<uint64_t>(snap)) {
        const float lead = std::min(8.0f, static_cast<float>(xrSerial - static_cast<uint64_t>(snap)));
        predictionLead_ = predictionLead_ > 0.0f ? predictionLead_ + 0.1f * (lead - predictionLead_) : lead;
        xrgame_profile_counter(kXrCounterPredictLead, static_cast<int64_t>(predictionLead_ * 1000.0f));
    }
    XrCompositionLayerQuad overlay{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const bool overlayRendered = windowsOverlayVisible_.load() && renderFrame();
    if (overlayRendered) {
        overlay.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        overlay.space = localSpace_;
        overlay.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        overlay.subImage.swapchain = swapchain_;
        overlay.subImage.imageRect = {{0, 0}, {swapchainWidth_, swapchainHeight_}};
        const float distance = -quadPosZ_.load();
        const float yaw = distance > 0.0001f ? std::atan2(quadPosX_.load(), distance) : 0.0f;
        const float pitch = distance > 0.0001f ? std::atan2(quadPosY_.load(), distance) : 0.0f;
        const float cy = std::cos(yaw * 0.5f);
        const float sy = std::sin(yaw * 0.5f);
        const float cx = std::cos(pitch * 0.5f);
        const float sx = std::sin(pitch * 0.5f);
        overlay.pose = IdentityPose();
        overlay.pose.orientation = {cy * sx, -sy * cx, sy * sx, cy * cx};
        overlay.pose.position = {distance * std::sin(yaw) * std::cos(pitch), distance * std::sin(pitch),
                                 -distance * std::cos(yaw) * std::cos(pitch)};
        overlay.size = {quadWidth_.load(), quadHeight_.load()};
    }
    XrCompositionLayerPassthroughFB passthroughLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    passthroughLayer.space = XR_NULL_HANDLE;
    passthroughLayer.layerHandle = passthroughLayer_;
    std::array<const XrCompositionLayerBaseHeader *, 3> layers{};
    uint32_t layerCount = 0;
    if (passthroughActive_ && passthroughLayer_ != XR_NULL_HANDLE) {
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&passthroughLayer);
    }
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection);
    if (overlayRendered) layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&overlay);
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = predictedDisplayTime;
    endInfo.environmentBlendMode = passthroughActive_ ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
                                                       : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layers.data();
    XrResult ended;
    {
        XrProfileScope endProfile("host.vr.xr.end_frame");
        ended = xrEndFrame(session_, &endInfo);
    }
    if (!XrCheck(ended, "xrEndFrame(windows projection)")) {
        stereoActive_.store(false);
        const bool reused =
            vulkan_ ? vulkan_->projection().lastRenderReused() : windowsProjection_.lastRenderReused();
        if (reused) {
            LOGE("runtime rejected the reused projection image; redrawing every frame from now on");
            if (vulkan_) vulkan_->projection().disableReuse();
            windowsProjection_.disableReuse();
        }
    }
    return true;
}

void XrImmersiveSession::submitQuadLayer(XrTime predictedDisplayTime, XrSpace space,
                                          XrSwapchain swapchain, int32_t width, int32_t height,
                                          bool sessionActive) {
    if (!sessionActive) return;

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    // Without this flag the compositor ignores the texture's alpha channel entirely and treats
    // every pixel as fully opaque — the composited bitmap already has real alpha=0 in the
    // pointer-margin band (Kotlin side clears it to transparent before drawing the game frame
    // inset), but with no blend flag those pixels' RGB (0,0,0, i.e. black) still render at full
    // opacity, which is exactly the black border the passthrough toggle was supposed to see
    // through. This makes the quad respect its own alpha, so the passthrough layer behind it
    // (when active) shows through those pixels instead.
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = space;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = swapchain;
    quad.subImage.imageRect.offset.x = 0;
    quad.subImage.imageRect.offset.y = 0;
    quad.subImage.imageRect.extent.width = width;
    quad.subImage.imageRect.extent.height = height;
    quad.subImage.imageArrayIndex = 0;
    // Orbit around the player on BOTH axes instead of sliding on a flat plane: quadPosX_/quadPosY_
    // are tangential (left/right, up/down) offsets and quadPosZ_ is always -distance (Kotlin
    // never sends a raw Cartesian z), so distance = -quadPosZ_ is the orbit radius, yaw =
    // atan2(quadPosX_, distance) is the angle swept left/right, and pitch = atan2(quadPosY_,
    // distance) is the angle swept up/down. Position moves on the SPHERE of that radius (not a
    // flat plane), and orientation rotates to keep facing the player at every point on it —
    // moving the panel in any direction now feels like walking it around the player rather than
    // sliding a flat sign past them, including vertically (an earlier version only did this for
    // yaw/left-right, leaving up/down as a plain Cartesian translation that visibly turned away).
    //
    // Derivation: at yaw=pitch=0 (centered), identity orientation already faces the player (the
    // pre-existing, known-working baseline) — so the quad's local +Z axis is its "faces the
    // player" normal, and local +X/+Y are its right/up axes. Composing a yaw (about world Y) with
    // a pitch (about the quad's own, already-yawed local X) and solving for "local +Z ends up
    // pointing from the new position back to the player" gives position = distance *
    // (sin(yaw)cos(pitch), sin(pitch), -cos(yaw)cos(pitch)) and orientation quaternion (using
    // half-angles cy/sy for yaw, cx/sx for pitch, with the same yaw-sign convention as the
    // yaw-only case): (cy*sx, -sy*cx, sy*sx, cy*cx) — reduces exactly to the old yaw-only
    // quaternion (0, -sy, 0, cy) when pitch=0, confirming the two derivations agree.
    const float distance = -quadPosZ_.load();
    const float tangentialX = quadPosX_.load();
    const float tangentialY = quadPosY_.load();
    const float yaw = distance > 0.0001f ? std::atan2(tangentialX, distance) : 0.0f;
    const float pitch = distance > 0.0001f ? std::atan2(tangentialY, distance) : 0.0f;
    const float cy = std::cos(yaw * 0.5f);
    const float sy = std::sin(yaw * 0.5f);
    const float cx = std::cos(pitch * 0.5f);
    const float sx = std::sin(pitch * 0.5f);
    quad.pose = IdentityPose();
    quad.pose.orientation.x = cy * sx;
    quad.pose.orientation.y = -sy * cx;
    quad.pose.orientation.z = sy * sx;
    quad.pose.orientation.w = cy * cx;
    quad.pose.position.x = distance * std::sin(yaw) * std::cos(pitch);
    quad.pose.position.y = distance * std::sin(pitch);
    quad.pose.position.z = -distance * std::cos(yaw) * std::cos(pitch);
    quad.size.width = quadWidth_.load();
    quad.size.height = quadHeight_.load();

    // Unlike the quad, a passthrough layer isn't anchored to a location in the world — it
    // replaces the whole environment/background — so per Meta's own samples it takes
    // XR_NULL_HANDLE here, not a real reference space. Passing our local reference space (a
    // valid, non-null XrSpace meant for spatially-anchored layers like the quad) is a plausible
    // cause of the xrEndFrame validation failure (-1) seen on every frame once passthrough was
    // toggled on, previously misdiagnosed as an unsupported blend mode.
    XrCompositionLayerPassthroughFB passthroughLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    passthroughLayer.space = XR_NULL_HANDLE;
    passthroughLayer.layerHandle = passthroughLayer_;

    std::vector<const XrCompositionLayerBaseHeader *> layers;
    if (passthroughActive_ && passthroughLayer_ != XR_NULL_HANDLE) {
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&passthroughLayer));
    }
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad));

    // Passthrough shows the real world through gaps, so the "background" must be
    // ADDITIVE/ALPHA_BLEND rather than OPAQUE while it's active, or it'll just look black.
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = predictedDisplayTime;
    endInfo.environmentBlendMode = passthroughActive_ ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
                                                        : XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = static_cast<uint32_t>(layers.size());
    endInfo.layers = layers.data();
    XrProfileScope endProfile("host.vr.xr.end_frame");
    XrCheck(xrEndFrame(session_, &endInfo), "xrEndFrame");
}

void XrImmersiveSession::setQuadTransform(float x, float y, float z, float width, float height,
                                           float contentScaleX, float contentScaleY) {
    quadPosX_.store(x);
    quadPosY_.store(y);
    quadPosZ_.store(z);
    quadWidth_.store(width);
    quadHeight_.store(height);
    quadContentScaleX_.store(contentScaleX);
    quadContentScaleY_.store(contentScaleY);
}

void XrImmersiveSession::setPassthroughEnabled(bool enabled) {
    passthroughRequested_.store(enabled);
}

void XrImmersiveSession::setupPassthrough() {
    auto resolve = [this](const char *name, PFN_xrVoidFunction *out) {
        xrGetInstanceProcAddr(instance_, name, out);
    };
    resolve("xrCreatePassthroughFB", reinterpret_cast<PFN_xrVoidFunction *>(&xrCreatePassthroughFB_));
    resolve("xrDestroyPassthroughFB", reinterpret_cast<PFN_xrVoidFunction *>(&xrDestroyPassthroughFB_));
    resolve("xrPassthroughStartFB", reinterpret_cast<PFN_xrVoidFunction *>(&xrPassthroughStartFB_));
    resolve("xrPassthroughPauseFB", reinterpret_cast<PFN_xrVoidFunction *>(&xrPassthroughPauseFB_));
    resolve("xrCreatePassthroughLayerFB",
            reinterpret_cast<PFN_xrVoidFunction *>(&xrCreatePassthroughLayerFB_));
    resolve("xrDestroyPassthroughLayerFB",
            reinterpret_cast<PFN_xrVoidFunction *>(&xrDestroyPassthroughLayerFB_));
    resolve("xrPassthroughLayerResumeFB",
            reinterpret_cast<PFN_xrVoidFunction *>(&xrPassthroughLayerResumeFB_));
    resolve("xrPassthroughLayerPauseFB",
            reinterpret_cast<PFN_xrVoidFunction *>(&xrPassthroughLayerPauseFB_));

    if (xrCreatePassthroughFB_ == nullptr || xrCreatePassthroughLayerFB_ == nullptr) {
        LOGE("XR_FB_passthrough advertised but function pointers missing — disabling");
        passthroughExtensionAvailable_ = false;
        return;
    }

    XrPassthroughCreateInfoFB passthroughCreateInfo{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    if (!XrCheck(xrCreatePassthroughFB_(session_, &passthroughCreateInfo, &passthrough_),
                 "xrCreatePassthroughFB")) {
        passthroughExtensionAvailable_ = false;
        return;
    }

    XrPassthroughLayerCreateInfoFB layerCreateInfo{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    layerCreateInfo.passthrough = passthrough_;
    layerCreateInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    if (!XrCheck(xrCreatePassthroughLayerFB_(session_, &layerCreateInfo, &passthroughLayer_),
                 "xrCreatePassthroughLayerFB")) {
        passthroughExtensionAvailable_ = false;
        return;
    }

    LOGI("Passthrough initialized (inactive until toggled on)");
}

void XrImmersiveSession::teardownPassthrough() {
    if (passthroughLayer_ != XR_NULL_HANDLE && xrDestroyPassthroughLayerFB_ != nullptr) {
        xrDestroyPassthroughLayerFB_(passthroughLayer_);
        passthroughLayer_ = XR_NULL_HANDLE;
    }
    if (passthrough_ != XR_NULL_HANDLE && xrDestroyPassthroughFB_ != nullptr) {
        xrDestroyPassthroughFB_(passthrough_);
        passthrough_ = XR_NULL_HANDLE;
    }
}

void XrImmersiveSession::applyPendingPassthroughState() {
    if (!passthroughExtensionAvailable_) return;
    if (!alphaBlendSupported_) {
        passthroughRequested_.store(false);
        return;
    }
    const bool requested = passthroughRequested_.load();
    if (requested == passthroughActive_) return;

    if (requested) {
        // Only report "active" (and thus switch to ALPHA_BLEND + attach the passthrough layer in
        // submitQuadLayer) if the runtime actually started passthrough — otherwise we'd flip to
        // ALPHA_BLEND with no real passthrough image behind it, which just looks like a black
        // screen (nothing bridges the gap where OPAQUE mode's implicit skybox used to be).
        const bool started = XrCheck(xrPassthroughStartFB_(passthrough_), "xrPassthroughStartFB") &&
                              XrCheck(xrPassthroughLayerResumeFB_(passthroughLayer_), "xrPassthroughLayerResumeFB");
        if (!started) {
            LOGE("Passthrough failed to start — leaving environment opaque");
            passthroughRequested_.store(false);
        }
        passthroughActive_ = started;
    } else {
        XrCheck(xrPassthroughLayerPauseFB_(passthroughLayer_), "xrPassthroughLayerPauseFB");
        XrCheck(xrPassthroughPauseFB_(passthrough_), "xrPassthroughPauseFB");
        passthroughActive_ = false;
    }
}

void XrImmersiveSession::syncControllerInputs(XrTime predictedDisplayTime) {
    std::array<XrActiveActionSet, 2> activeActionSets{{{actionSet_, XR_NULL_PATH}, {XR_NULL_HANDLE, XR_NULL_PATH}}};
    if (gaze_) activeActionSets[1].actionSet = gaze_->ActionSet();
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = gaze_ ? 2 : 1;
    syncInfo.activeActionSets = activeActionSets.data();
    if (!XrCheck(xrSyncActions(session_, &syncInfo), "xrSyncActions")) return;

    auto getBool = [this](XrAction action) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        xrGetActionStateBoolean(session_, &info, &state);
        return state.isActive && state.currentState;
    };
    auto getFloat = [this](XrAction action) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        xrGetActionStateFloat(session_, &info, &state);
        return state.isActive ? state.currentState : 0.0f;
    };
    auto getVector2 = [this](XrAction action, float *x, float *y) {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        xrGetActionStateVector2f(session_, &info, &state);
        if (state.isActive) {
            *x = state.currentState.x;
            *y = state.currentState.y;
        } else {
            *x = *y = 0.0f;
        }
    };

    InputSnapshot next;
    next.buttons = 0;
    if (getBool(buttonXAction_)) next.buttons |= (1u << BUTTON_X);
    if (getBool(buttonYAction_)) next.buttons |= (1u << BUTTON_Y);
    if (getBool(buttonAAction_)) next.buttons |= (1u << BUTTON_A);
    if (getBool(buttonBAction_)) next.buttons |= (1u << BUTTON_B);
    next.squeezeL = getFloat(squeezeLAction_);
    next.squeezeR = getFloat(squeezeRAction_);
    if (next.squeezeL > 0.5f) next.buttons |= (1u << BUTTON_LB);
    if (next.squeezeR > 0.5f) next.buttons |= (1u << BUTTON_RB);

    // Menu/Start button: a short press still forwards a normal Start press to the game (for
    // the game's own pause/settings menu); holding it for 600ms instead opens the immersive
    // quick menu. The raw OpenXR click can bounce (several false->true transitions within a
    // single physical press), so lastMenuDebouncedPressed_ applies a short bounce-tolerance
    // window before measuring hold duration — a millisecond-scale dropout mid-hold must not
    // reset the 600ms timer.
    const auto nowMenu = std::chrono::steady_clock::now();
    const bool rawMenuPressed = getBool(menuLAction_);
    if (rawMenuPressed) lastMenuRawTrueTime_ = nowMenu;
    const bool debouncedMenuPressed = rawMenuPressed ||
        (nowMenu - lastMenuRawTrueTime_) < std::chrono::milliseconds(50);
    if (debouncedMenuPressed && !lastMenuDebouncedPressed_) {
        menuHoldStartTime_ = nowMenu;
        menuLongPressTriggered_ = false;
    }
    if (debouncedMenuPressed) {
        if (!menuLongPressTriggered_ &&
            (nowMenu - menuHoldStartTime_) >= std::chrono::milliseconds(600)) {
            next.quickMenuClicked = true;
            menuLongPressTriggered_ = true;
        }
    } else if (lastMenuDebouncedPressed_ && !menuLongPressTriggered_) {
        // Released before reaching the long-press threshold: forward a clean momentary Start
        // press to the game, same as a real controller's Menu button.
        menuDebouncedActive_ = true;
        menuPressStartTime_ = nowMenu;
    }
    lastMenuDebouncedPressed_ = debouncedMenuPressed;
    next.menuButtonHeld = debouncedMenuPressed;
    if (menuDebouncedActive_) {
        if ((nowMenu - menuPressStartTime_) < std::chrono::milliseconds(150)) {
            next.buttons |= (1u << BUTTON_START);
        } else {
            menuDebouncedActive_ = false;
        }
    }

    const bool l3Pressed = getBool(thumbstickLClickAction_);
    const bool r3Pressed = getBool(thumbstickRClickAction_);
    if (l3Pressed) next.buttons |= (1u << BUTTON_L3);
    if (r3Pressed) next.buttons |= (1u << BUTTON_R3);

    next.triggerL = getFloat(triggerLAction_);
    next.triggerR = getFloat(triggerRAction_);
    getVector2(thumbstickLAction_, &next.leftX, &next.leftY);
    getVector2(thumbstickRAction_, &next.rightX, &next.rightY);
    // Confirmed by testing: this OpenXR runtime's thumbstick Y is inverted relative to what
    // WinHandler's shared-memory XInput buffer expects (positive Y = down here, but the buffer
    // wants positive Y = up, same as a real Xbox controller) — up/down were swapped in-game.
    // Only the left stick was confirmed broken; negating both since they're read the same way
    // and almost certainly share the same convention.
    next.leftY = -next.leftY;
    next.rightY = -next.rightY;
    // No synthetic dpad: on a real Xbox controller the left stick and the d-pad are two
    // separate physical controls, and Touch controllers have no physical d-pad at all. Faking
    // one out of the stick's direction fired alongside the real analog values, which is why
    // stick movement felt like the d-pad instead of a proper analog Xbox stick. leftX/leftY/
    // rightX/rightY carry the real analog values; dpadUp/Down/Left/Right stay false.

    // XR pointer-mode toggle: double-click of either thumbstick (L3 or R3) — checked
    // independently rather than requiring both at once, since it's easier to double-click one
    // stick with one hand than to coordinate a multi-button chord. Reserved for grabbing the
    // quad's corner (resize) / top-bottom bar (reposition), and later a real VR game's own
    // pointer needs.
    const auto now = std::chrono::steady_clock::now();
    auto detectDoubleClick = [&](bool pressed, bool &lastPressed,
                                  std::chrono::steady_clock::time_point &lastClickTime,
                                  int &clickCount) {
        bool triggered = false;
        if (pressed && !lastPressed) {
            clickCount = (now - lastClickTime) < std::chrono::milliseconds(400) ? clickCount + 1 : 1;
            lastClickTime = now;
            if (clickCount >= 2) {
                triggered = true;
                clickCount = 0;
            }
        }
        lastPressed = pressed;
        return triggered;
    };
    const bool leftDoubleClick = detectDoubleClick(l3Pressed, lastL3Pressed_, lastL3ClickTime_, l3ClickCount_);
    const bool rightDoubleClick = detectDoubleClick(r3Pressed, lastR3Pressed_, lastR3ClickTime_, r3ClickCount_);
    next.pointerModeToggled = leftDoubleClick || rightDoubleClick;

    // Aim-pose ray for each hand, in the same LOCAL space the quad transform lives in — lets
    // Kotlin do a simple ray/plane intersection against the quad's known rectangle instead of
    // needing any OpenXR types on that side.
    auto rotateForward = [](const XrQuaternionf &q) {
        // Standard quaternion-rotate-vector for v=(0,0,-1) (OpenXR's forward), expanded by hand
        // since -1 in one component simplifies most of the cross-product terms away.
        const float vx = 0.0f, vy = 0.0f, vz = -1.0f;
        const float tx = 2.0f * (q.y * vz - q.z * vy);
        const float ty = 2.0f * (q.z * vx - q.x * vz);
        const float tz = 2.0f * (q.x * vy - q.y * vx);
        const float rx = vx + q.w * tx + (q.y * tz - q.z * ty);
        const float ry = vy + q.w * ty + (q.z * tx - q.x * tz);
        const float rz = vz + q.w * tz + (q.x * ty - q.y * tx);
        return std::array<float, 3>{rx, ry, rz};
    };

    XrSpaceLocation leftLoc{XR_TYPE_SPACE_LOCATION};
    XrSpaceLocation rightLoc{XR_TYPE_SPACE_LOCATION};
    constexpr XrSpaceLocationFlags kPoseValidBits =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    const bool haveLeft = aimSpaceLeft_ != XR_NULL_HANDLE &&
        XrCheck(xrLocateSpace(aimSpaceLeft_, localSpace_, predictedDisplayTime, &leftLoc), "xrLocateSpace(left)") &&
        (leftLoc.locationFlags & kPoseValidBits) == kPoseValidBits;
    const bool haveRight = aimSpaceRight_ != XR_NULL_HANDLE &&
        XrCheck(xrLocateSpace(aimSpaceRight_, localSpace_, predictedDisplayTime, &rightLoc), "xrLocateSpace(right)") &&
        (rightLoc.locationFlags & kPoseValidBits) == kPoseValidBits;
    next.handPosesValid = haveLeft && haveRight;
    next.aimPoseValid[0] = haveLeft;
    next.aimPoseValid[1] = haveRight;
    if (haveLeft) {
        next.aimPoses[0] = leftLoc.pose;
        next.leftHandPosX = leftLoc.pose.position.x;
        next.leftHandPosY = leftLoc.pose.position.y;
        next.leftHandPosZ = leftLoc.pose.position.z;
        const auto fwd = rotateForward(leftLoc.pose.orientation);
        next.leftHandFwdX = fwd[0];
        next.leftHandFwdY = fwd[1];
        next.leftHandFwdZ = fwd[2];
    }
    if (haveRight) {
        next.aimPoses[1] = rightLoc.pose;
        next.rightHandPosX = rightLoc.pose.position.x;
        next.rightHandPosY = rightLoc.pose.position.y;
        next.rightHandPosZ = rightLoc.pose.position.z;
        const auto fwd = rotateForward(rightLoc.pose.orientation);
        next.rightHandFwdX = fwd[0];
        next.rightHandFwdY = fwd[1];
        next.rightHandFwdZ = fwd[2];
    }
    XrSpaceLocation leftGrip{XR_TYPE_SPACE_LOCATION};
    XrSpaceLocation rightGrip{XR_TYPE_SPACE_LOCATION};
    next.gripPoseValid[0] = gripSpaceLeft_ != XR_NULL_HANDLE &&
        XrCheck(xrLocateSpace(gripSpaceLeft_, localSpace_, predictedDisplayTime, &leftGrip), "xrLocateSpace(left grip)") &&
        (leftGrip.locationFlags & kPoseValidBits) == kPoseValidBits;
    next.gripPoseValid[1] = gripSpaceRight_ != XR_NULL_HANDLE &&
        XrCheck(xrLocateSpace(gripSpaceRight_, localSpace_, predictedDisplayTime, &rightGrip), "xrLocateSpace(right grip)") &&
        (rightGrip.locationFlags & kPoseValidBits) == kPoseValidBits;
    if (next.gripPoseValid[0]) next.gripPoses[0] = leftGrip.pose;
    if (next.gripPoseValid[1]) next.gripPoses[1] = rightGrip.pose;

    // Rate-limited raw-value dump (~every 0.5s at 90Hz) — temporary, for diagnosing controller
    // mapping reports; safe to remove once the analog stick mapping is confirmed solid.
    if ((syncFrameCounter_++ % 45) == 0) {
        LOGI("controller raw: leftX=%.2f leftY=%.2f rightX=%.2f rightY=%.2f buttons=0x%03x",
             next.leftX, next.leftY, next.rightX, next.rightY, next.buttons);
    }

    std::lock_guard<std::mutex> lock(snapshotMutex_);
    // Preserve rising-edge flags that a concurrent pollSnapshot() hasn't consumed yet.
    next.quickMenuClicked = next.quickMenuClicked || snapshot_.quickMenuClicked;
    next.pointerModeToggled = next.pointerModeToggled || snapshot_.pointerModeToggled;
    snapshot_ = next;
}

void XrImmersiveSession::syncWindowsTrackingPoses(InputSnapshot *snapshot,
                                                  XrTime predictedDisplayTime) {
    if (snapshot == nullptr || windowsTrackingSpace_ == XR_NULL_HANDLE ||
        windowsTrackingSpace_ == localSpace_) return;
    constexpr XrSpaceLocationFlags validBits =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    XrSpaceLocation aim[2]{{XR_TYPE_SPACE_LOCATION}, {XR_TYPE_SPACE_LOCATION}};
    XrSpaceLocation grip[2]{{XR_TYPE_SPACE_LOCATION}, {XR_TYPE_SPACE_LOCATION}};
    const XrSpace aimSpaces[2]{aimSpaceLeft_, aimSpaceRight_};
    const XrSpace gripSpaces[2]{gripSpaceLeft_, gripSpaceRight_};
    for (uint32_t hand = 0; hand < 2; ++hand) {
        snapshot->aimPoseValid[hand] = aimSpaces[hand] != XR_NULL_HANDLE &&
            XR_SUCCEEDED(xrLocateSpace(aimSpaces[hand], windowsTrackingSpace_,
                                       predictedDisplayTime, &aim[hand])) &&
            (aim[hand].locationFlags & validBits) == validBits;
        snapshot->gripPoseValid[hand] = gripSpaces[hand] != XR_NULL_HANDLE &&
            XR_SUCCEEDED(xrLocateSpace(gripSpaces[hand], windowsTrackingSpace_,
                                       predictedDisplayTime, &grip[hand])) &&
            (grip[hand].locationFlags & validBits) == validBits;
        if (snapshot->aimPoseValid[hand]) snapshot->aimPoses[hand] = aim[hand].pose;
        if (snapshot->gripPoseValid[hand]) snapshot->gripPoses[hand] = grip[hand].pose;
        if (snapshot->aimPoseValid[hand] && snapshot->gripPoseValid[hand] && !gripRelationLogged_[hand]) {
            // The runtime's own aim in its grip frame: what the game's controller model has to
            // reproduce for its ray to follow the physical controller.
            gripRelationLogged_[hand] = true;
            const XrPosef relation = RelativePose(grip[hand].pose, aim[hand].pose);
            const XrVector3f forward = QuatRotate(relation.orientation, {0.0f, 0.0f, -1.0f});
            const XrVector3f up = QuatRotate(relation.orientation, {0.0f, 1.0f, 0.0f});
            constexpr float kDegrees = 180.0f / static_cast<float>(M_PI);
            LOGI("Windows VR controller %s: aim in grip frame pitch=%.1f yaw=%.1f roll=%.1f offset=(%.1f, %.1f, %.1f) mm "
                 "q=(%.4f, %.4f, %.4f, %.4f)",
                 hand == 0 ? "left" : "right", std::asin(std::clamp(forward.y, -1.0f, 1.0f)) * kDegrees,
                 std::atan2(-forward.x, -forward.z) * kDegrees, std::atan2(-up.x, up.y) * kDegrees,
                 relation.position.x * 1000.0f, relation.position.y * 1000.0f, relation.position.z * 1000.0f,
                 relation.orientation.x, relation.orientation.y, relation.orientation.z, relation.orientation.w);
        }
    }
    {
        std::lock_guard<std::mutex> lock(gGripCorrectionMutex);
        if (gGripCorrectionEnabled) {
            for (uint32_t hand = 0; hand < 2; ++hand) {
                if (snapshot->gripPoseValid[hand]) {
                    snapshot->gripPoses[hand] = ComposePose(snapshot->gripPoses[hand], gGripCorrection[hand]);
                }
            }
        }
    }
    snapshot->handPosesValid = snapshot->aimPoseValid[0] && snapshot->aimPoseValid[1];
}

void XrImmersiveSession::teardown() {
    stereoActive_.store(false);
    stereoMisses_ = 0;
    windowsTransport_.stop();
    if (vulkan_) vulkan_->releaseResources();
    windowsProjection_.shutdown();
    gaze_.reset();
    teardownPassthrough();
    if (gameTexture_ != 0) {
        glDeleteTextures(1, &gameTexture_);
        gameTexture_ = 0;
    }
    if (quadVbo_ != 0) {
        glDeleteBuffers(1, &quadVbo_);
        quadVbo_ = 0;
    }
    if (quadProgram_ != 0) {
        glDeleteProgram(quadProgram_);
        quadProgram_ = 0;
    }
    if (directQuadProgram_ != 0) {
        glDeleteProgram(directQuadProgram_);
        directQuadProgram_ = 0;
    }
    if (sharedGameTexture_ != 0) {
        glDeleteTextures(1, &sharedGameTexture_);
        sharedGameTexture_ = 0;
    }
    if (sharedGameImage_ != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR(eglDisplay_, sharedGameImage_);
        sharedGameImage_ = EGL_NO_IMAGE_KHR;
    }
    {
        std::lock_guard<std::mutex> lock(sharedBufferMutex_);
        if (pendingSharedBuffer_ != nullptr) {
            AHardwareBuffer_release(pendingSharedBuffer_);
            pendingSharedBuffer_ = nullptr;
        }
    }
    if (framebuffer_ != 0) {
        glDeleteFramebuffers(1, &framebuffer_);
        framebuffer_ = 0;
    }
    if (swapchain_ != XR_NULL_HANDLE) {
        xrDestroySwapchain(swapchain_);
        swapchain_ = XR_NULL_HANDLE;
    }
    if (interstitialSwapchain_ != XR_NULL_HANDLE) {
        xrDestroySwapchain(interstitialSwapchain_);
        interstitialSwapchain_ = XR_NULL_HANDLE;
        interstitialImages_.clear();
        interstitialImageReleased_ = false;
    }
    if (interstitialTexture_ != 0) {
        glDeleteTextures(1, &interstitialTexture_);
        interstitialTexture_ = 0;
    }
    if (localSpace_ != XR_NULL_HANDLE) {
        xrDestroySpace(localSpace_);
        localSpace_ = XR_NULL_HANDLE;
    }
    if (stageSpace_ != XR_NULL_HANDLE) {
        xrDestroySpace(stageSpace_);
        stageSpace_ = XR_NULL_HANDLE;
    }
    if (localFloorSpace_ != XR_NULL_HANDLE) {
        xrDestroySpace(localFloorSpace_);
        localFloorSpace_ = XR_NULL_HANDLE;
    }
    windowsTrackingSpace_ = XR_NULL_HANDLE;
    if (aimSpaceLeft_ != XR_NULL_HANDLE) {
        xrDestroySpace(aimSpaceLeft_);
        aimSpaceLeft_ = XR_NULL_HANDLE;
    }
    if (aimSpaceRight_ != XR_NULL_HANDLE) {
        xrDestroySpace(aimSpaceRight_);
        aimSpaceRight_ = XR_NULL_HANDLE;
    }
    if (gripSpaceLeft_ != XR_NULL_HANDLE) {
        xrDestroySpace(gripSpaceLeft_);
        gripSpaceLeft_ = XR_NULL_HANDLE;
    }
    if (gripSpaceRight_ != XR_NULL_HANDLE) {
        xrDestroySpace(gripSpaceRight_);
        gripSpaceRight_ = XR_NULL_HANDLE;
    }
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (actionSet_ != XR_NULL_HANDLE) {
            xrDestroyActionSet(actionSet_);
            actionSet_ = XR_NULL_HANDLE;
        }
        hapticAction_ = XR_NULL_HANDLE;
        if (session_ != XR_NULL_HANDLE) {
            xrDestroySession(session_);
            session_ = XR_NULL_HANDLE;
        }
    }
    // The runtime created the Vulkan device for this session; it goes after the session.
    if (vulkan_) {
        vulkan_->destroy();
        vulkan_.reset();
    }
    if (instance_ != XR_NULL_HANDLE) {
        xrDestroyInstance(instance_);
        instance_ = XR_NULL_HANDLE;
    }
    if (eglContext_ != EGL_NO_CONTEXT) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(eglDisplay_, eglContext_);
        eglContext_ = EGL_NO_CONTEXT;
    }
    if (eglPbufferSurface_ != EGL_NO_SURFACE) {
        eglDestroySurface(eglDisplay_, eglPbufferSurface_);
        eglPbufferSurface_ = EGL_NO_SURFACE;
    }
}

}  // namespace xrimmersive
