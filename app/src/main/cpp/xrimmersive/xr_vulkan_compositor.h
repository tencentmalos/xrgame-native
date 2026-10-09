#pragma once

// Vulkan backend of XrImmersiveSession: owns the Turnip context and draws every swapchain the
// session submits (2D quad, loading interstitial, Windows VR projection).

#include "xr_vulkan.h"
#include "xr_vulkan_projection.h"

#include <EGL/egl.h>
#include <jni.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <string>
#include <vector>

namespace xrimmersive::vulkan {

struct CompositeConfig {
    bool vulkan = false;
    std::string driverDir;
    std::string libraryName;
    std::string hookDir;
};
void SetCompositeConfig(const CompositeConfig &config);
// The configured backend; the system property debug.xrgame.xr.composite (vulkan|gles) overrides it.
CompositeConfig GetCompositeConfig();

class Compositor {
public:
    enum Target : uint32_t { kQuad = 0, kInterstitial = 1 };

    bool create(XrInstance instance, XrSystemId system, const CompositeConfig &config);
    // Views, images and the projection swapchain; before the session's swapchains and session go.
    void releaseResources();
    // The device and instance; after xrDestroySession.
    void destroy();

    Context &context() { return context_; }
    ProjectionPresenter &projection() { return projection_; }
    XrGraphicsBindingVulkan2KHR binding() const;
    // R8G8B8A8_SRGB when offered (the content is sRGB-encoded), else R8G8B8A8_UNORM.
    static int64_t ChooseFormat(const std::vector<int64_t> &formats, bool *srgb);

    bool attachSwapchain(Target target, XrSwapchain swapchain, int64_t format, uint32_t width, uint32_t height);
    void detachSwapchain(Target target);

    // The 2D quad: the shared game buffer (direct render) under the overlay, or the CPU-captured
    // frame alone. Null pixels keep the previously uploaded overlay.
    bool drawQuad(uint32_t imageIndex, AHardwareBuffer *sharedGame, const uint8_t *overlayPixels,
                  uint32_t overlayWidth, uint32_t overlayHeight, float contentScaleX, float contentScaleY);
    bool drawInterstitial(uint32_t imageIndex, const uint8_t *pixels, uint32_t width, uint32_t height);

private:
    struct SwapchainTarget {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        bool srgb = false;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<XrSwapchainImageVulkan2KHR> images;
        std::vector<VkImageView> views;
    };

    bool importSharedGame(AHardwareBuffer *buffer);

    Context context_;
    ProjectionPresenter projection_;
    std::array<SwapchainTarget, 2> targets_{};
    Image overlay_{};
    Image interstitial_{};
    Image sharedGame_{};
    AHardwareBuffer *sharedGameBuffer_ = nullptr;
};

}  // namespace xrimmersive::vulkan
