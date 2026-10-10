#pragma once

// Windows VR stereo presentation on the Vulkan composite: the game's eye images (AHardwareBuffers
// from the Wine-side runtime) are reconstructed with Foundation's FSR1/SGSR1 upscaler into an
// array-2 projection swapchain at the runtime's recommended size. With VK_EXT_fragment_density_map
// the reconstruction pass is foveated around a fixed centre or the tracked gaze (ETFR); the game's
// own render resolution, poses and FOV are untouched.

#include "xr_vulkan.h"
#include "xr_windows_transport.h"

#include "spatial/upscale/VulkanUpscaler.h"

#include <EGL/egl.h>
#include <jni.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace xrimmersive::vulkan {

// Runtime-adjustable reconstruction settings (DebugBus vr_upscale, per-container defaults).
struct UpscaleSettings {
    uint32_t filter = 0;          // spatial::upscale::Filter: 0 off, 1 FSR1, 2 SGSR1
    uint32_t sharpness = 50;      // 0..100
    uint32_t foveation = 0;       // spatial::foveation::Mode: 0 off, 1 fixed, 2 eye tracked
    uint32_t level = 1;           // spatial::foveation::Level: 0 low, 1 balanced, 2 high
    uint32_t outputPercent = 100; // output size relative to the runtime recommendation, 50..100
    bool debug = false;           // tint by actual fragment density
};
void SetUpscaleSettings(const UpscaleSettings &settings);
UpscaleSettings GetUpscaleSettings();

// Gaze orientation in the projection layer's space at the XR frame's display time.
struct GazeSample {
    bool valid = false;
    XrQuaternionf orientation{0.0f, 0.0f, 0.0f, 1.0f};
};

class ProjectionPresenter {
public:
    bool initialize(Context *context, XrSession session, VkFormat format, bool srgb, uint32_t recommendedWidth,
                    uint32_t recommendedHeight);
    bool render(windowsvr::WindowsFrameTransport &transport, XrSpace space, XrCompositionLayerProjection *layer);
    void setGaze(const GazeSample &gaze) { gaze_ = gaze; }
    bool lastRenderReused() const { return lastRenderReused_; }
    int64_t lastFreshSnap() const { return lastFreshSnap_; }
    // The game's eye size and the projection eye size of the last rendered frame, and whether it
    // was reconstructed (for the performance HUD).
    VkExtent2D sourceExtent() const { return {sourceWidth_, sourceHeight_}; }
    VkExtent2D outputExtent() const { return {width_, height_}; }
    bool reconstructing() const { return reconstructing_; }
    void disableReuse() { reuseReleased_ = false; }
    void shutdown();

private:
    static constexpr uint32_t kSlotsPerEye = 2;

    struct Imported {
        Image image;
        uint64_t registration = 0;
        AHardwareBuffer *buffer = nullptr;
    };

    bool ensureSwapchain(uint32_t width, uint32_t height);
    void destroySwapchain();
    bool importEye(windowsvr::WindowsFrameTransport &transport, uint32_t eye, windowsvr::EyeFrame &frame,
                   bool &fresh);
    void discardFresh(windowsvr::WindowsFrameTransport &transport, const std::array<windowsvr::EyeFrame, 2> &frames,
                      const std::array<bool, 2> &fresh);
    spatial::upscale::FoveatedEye foveate(const windowsvr::EyeFrame &frame, uint32_t width, uint32_t height,
                                          const UpscaleSettings &settings) const;
    void releaseImported(Imported &imported);

    Context *context_ = nullptr;
    XrSession session_ = XR_NULL_HANDLE;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    bool srgb_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t recommendedWidth_ = 0;
    uint32_t recommendedHeight_ = 0;
    std::vector<XrSwapchainImageVulkan2KHR> images_;
    std::vector<std::array<VkImageView, 2>> views_;
    std::array<XrCompositionLayerProjectionView, 2> layerViews_{};
    std::array<std::array<Imported, windowsvr::WindowsFrameTransport::kMaxImages>, 2> imported_{};
    std::array<uint64_t, 2> renderedSerials_{0, 0};
    std::unique_ptr<spatial::upscale::VulkanUpscaler> upscaler_;
    std::array<uint64_t, 2 * kSlotsPerEye> slotSerials_{};
    uint32_t frameParity_ = 0;
    uint32_t upscaleFailures_ = 0;
    GazeSample gaze_{};
    UpscaleSettings drawnSettings_{};
    bool releasedImageValid_ = false;
    bool reuseReleased_ = true;
    bool lastRenderReused_ = false;
    int64_t lastFreshSnap_ = -1;
    uint32_t sourceWidth_ = 0;
    uint32_t sourceHeight_ = 0;
    bool reconstructing_ = false;
    uint64_t presentedFrames_ = 0;
    uint64_t directEyes_ = 0;  // eyes reconstructed straight into the swapchain
};

}  // namespace xrimmersive::vulkan
