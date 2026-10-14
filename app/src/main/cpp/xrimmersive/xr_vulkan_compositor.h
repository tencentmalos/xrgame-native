#pragma once

// Vulkan backend of XrImmersiveSession: owns the Turnip context and draws every swapchain the
// session submits (2D quad, loading interstitial, Windows VR projection).

#include "xr_frame_generation.h"
#include "xr_vulkan.h"
#include "xr_vulkan_projection.h"

#include "../winlator/xr_frame_exchange.h"

#include <EGL/egl.h>
#include <jni.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace xrimmersive::capture {
class XrCapture;
}

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

// How the big screen's game area appears to the player, for the foveation of its reconstruction:
// its half extents over the viewing distance, and where the eye gaze meets it in the game image
// (0..1, v down; the game may be flipped on the quad).
struct QuadView {
    float tanHalfWidth = 0.0f;
    float tanHalfHeight = 0.0f;
    bool gazeValid = false;
    float gazeU = 0.5f;
    float gazeV = 0.5f;
};

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
    // The session's capture producer (scrcpy capture SDK), or null; the quad, the interstitial and
    // the projection copy their images into it on captured frames.
    void setCapture(capture::XrCapture *capture) {
        capture_ = capture;
        projection_.setCapture(capture);
    }
    XrGraphicsBindingVulkan2KHR binding() const;
    // R8G8B8A8_SRGB when offered (the content is sRGB-encoded), else R8G8B8A8_UNORM.
    static int64_t ChooseFormat(const std::vector<int64_t> &formats, bool *srgb);

    bool attachSwapchain(Target target, XrSwapchain swapchain, int64_t format, uint32_t width, uint32_t height);
    void detachSwapchain(Target target);

    // The 2D quad: the game under the overlay, or the CPU-captured frame alone. The game is the X
    // server's scene ring (`scene`, the latest completed frame, or with frame generation an
    // interpolated one shown before it), else the shared game buffer (direct render). Null pixels
    // keep the previously uploaded overlay. When the quad's game area has more pixels than the
    // game and an upscale filter is set (UpscaleSettings, shared with the projection), the game is
    // reconstructed to that area first, foveated by `view`; generated frames take the same path.
    // `displayPeriodNs` paces generated frames.
    bool drawQuad(uint32_t imageIndex, xrgame::XrFrameExchange *scene, AHardwareBuffer *sharedGame,
                  const uint8_t *overlayPixels, uint32_t overlayWidth, uint32_t overlayHeight, float contentScaleX,
                  float contentScaleY, const QuadView &view, int64_t displayPeriodNs);
    bool drawInterstitial(uint32_t imageIndex, const uint8_t *pixels, uint32_t width, uint32_t height);
    // The game frame the last quad showed: the shared game buffer, else the captured frame.
    VkExtent2D quadSourceExtent() const { return quadSource_; }
    // Whether the last quad reconstructed the game into the layer, and the layer's size.
    bool quadReconstructing() const { return quadReconstructing_; }
    VkExtent2D quadLayerExtent() const { return {targets_[kQuad].width, targets_[kQuad].height}; }
    // Frame generation on the big screen: the output multiple in use (0 off or unavailable), and
    // game frames and shown game images per second over the last second.
    struct FrameGenStats {
        uint32_t multiplier = 0;
        float gameFps = 0.0f;
        float outputFps = 0.0f;
    };
    FrameGenStats frameGenStats() const { return frameGenStats_; }

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
    // Reconstructs `source` (`input` pixels, SHADER_READ_ONLY) into an image of `extent` in
    // `command`; returns its view, or null to blit the game as it is. `linear` tells whether that
    // view samples linear values.
    VkImageView reconstructQuad(VkCommandBuffer command, uint32_t slot, VkExtent2D extent,
                                const UpscaleSettings &settings, const QuadView &view, VkImageView source,
                                VkExtent2D input, bool *linear);
    void releaseQuadReconstruction();

    // The scene ring: imports its slots once per ring; holds the slots this side reads.
    bool bindScene(xrgame::XrFrameExchange *scene);
    void releaseScene();
    void releaseSlots(bool keepPrevious);
    // Takes the latest completed frame when it is newer than the current one; true if taken.
    bool takeLatestFrame(bool frameGeneration);
    void noteShown(int64_t nowNs, bool newImage);

    Context context_;
    ProjectionPresenter projection_;
    capture::XrCapture *capture_ = nullptr;
    std::array<SwapchainTarget, 2> targets_{};
    Image overlay_{};
    Image interstitial_{};
    Image sharedGame_{};
    AHardwareBuffer *sharedGameBuffer_ = nullptr;
    VkExtent2D quadSource_{};

    // The X server's scene ring and the frames taken from it (slot -1: none).
    struct SceneFrame {
        int slot = -1;
        uint64_t serial = 0;
        uint64_t content = 0;
        int64_t completedNs = 0;
    };
    xrgame::XrFrameExchange *scene_ = nullptr;
    bool sceneOk_ = false;
    std::array<Image, xrgame::XrFrameExchange::kSlots> sceneImages_{};
    std::array<uint64_t, xrgame::XrFrameExchange::kSlots> sceneUse_{}; // last submission reading a slot
    uint32_t sceneHeld_ = 0;
    SceneFrame current_{};
    SceneFrame previous_{};

    // Frame generation: the current game frame's sequence of generated images and itself.
    FrameGenerator generator_;
    bool fgEnabled_ = false;
    bool fgPendingShared_ = false;  // the current frame still needs its shared passes
    double fgIntervalNs_ = 0.0;     // smoothed game frame interval
    int64_t fgStartNs_ = 0;         // when the current frame was taken
    uint32_t fgGenerations_ = 0;    // generated images shown before it
    int fgShownItem_ = -1;          // index into that sequence, generated images first
    int fgComputed_ = -1;           // the generation whose output holds this sequence's image
    VkImageView fgComputedView_ = VK_NULL_HANDLE;
    uint64_t fgSharedFailures_ = 0;
    FrameGenStats frameGenStats_{};
    int64_t statsStartNs_ = 0;
    uint32_t statsGameFrames_ = 0;
    uint32_t statsShown_ = 0;

    // Big-screen reconstruction: two slots, each reused only once its last submission completed.
    static constexpr uint32_t kQuadSlots = 2;
    std::unique_ptr<spatial::upscale::VulkanUpscaler> quadUpscaler_;
    std::array<Image, kQuadSlots> quadImages_{};
    std::array<uint64_t, kQuadSlots> quadSerials_{};
    uint32_t quadSlot_ = 0;
    uint32_t quadFailures_ = 0;
    uint64_t quadFrames_ = 0;
    bool quadReconstructing_ = false;
};

}  // namespace xrimmersive::vulkan
