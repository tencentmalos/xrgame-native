#pragma once

// Performance panel of the immersive session, modelled on shadPS4's XR status panel: its own Dear
// ImGui context, drawn by Foundation's XrImguiVulkanLayer into a separate OpenXR quad layer. The
// quad is world-locked in LOCAL space, up and to the left of where the user faced when the panel
// was shown (or LOCAL was recentred), and turned towards that head position; it does not follow
// the head. It redraws at most four times a second without waiting for the GPU; an XR frame only
// composites the last released image. Vulkan composite only. Game FPS counts the projection frames
// that brought a new game image, not the XR frames that reused one.

#include <jni.h>
#include <openxr/openxr.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace xrimmersive {

namespace vulkan {
class Context;
}

// The Performance HUD switch, for every session; any thread.
void SetPerfHudVisible(bool visible);
// The game's frame rate measured outside the composite (the X server's presented frames), for
// sessions without a stereo projection such as the 2D big screen; any thread.
void SetPerfHudGameFps(float fps);
// The input driving the game's pad, shown as INPUT; ASCII; any thread.
void SetPerfHudInput(const std::string &label);
bool PerfHudVisible();
// One-line JSON for DebugBus vr_hud; any thread.
std::string PerfHudStatusJson();
// Gives the device sampler an application context for BatteryManager; once per process.
void InitPerfHudJni(JNIEnv *env, jobject context);

struct PerfHudFrame {
    // Game eye and projection eye buffer of the last stereo frame, or on the 2D big screen (flat)
    // the Wine screen and the big screen's layer; zero when unknown.
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint32_t filter = 0;  // 0 none, 1 FSR1, 2 SGSR1
    bool reconstructing = false;
    bool flat = false;
    float displayHz = 0.0f;
    // Big-screen frame generation: the output multiple in use (0 off) and the images shown per second.
    uint32_t frameGenMultiplier = 0;
    float frameGenOutputFps = 0.0f;
};

class PerfHud {
public:
    PerfHud();
    ~PerfHud();
    PerfHud(const PerfHud &) = delete;
    PerfHud &operator=(const PerfHud &) = delete;

    void noteXrFrame(std::chrono::steady_clock::time_point now);
    void noteGameFrame(std::chrono::steady_clock::time_point now);
    // XR thread, after the eye layers were recorded: redraws the panel when due, creating its layer
    // the first time it is shown. True when a released panel image can be composited.
    bool update(XrSession session, vulkan::Context &context, const PerfHudFrame &frame);
    // The quad of the last released panel image, world-locked in `localSpace`. With `screen` (the
    // 2D big screen's quad in the same space) it sits above the screen's top-left corner, parallel
    // to it. Otherwise it is placed from the head pose (`viewSpace` located at `time`) when it has
    // no position yet; false while that pose is not tracked.
    bool fill(XrSpace localSpace, XrSpace viewSpace, XrTime time, XrCompositionLayerQuad *quad,
              const XrCompositionLayerQuad *screen = nullptr);
    // Places the panel again in front of the head on the next fill (LOCAL was recentred).
    void reanchor();
    // Before the Vulkan device and the session go away.
    void destroy();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrimmersive
