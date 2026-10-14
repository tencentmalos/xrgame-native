// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from shadPS4's capture_recorder and openxr/xr_capture (GPL-2.0-or-later).
#pragma once

// The immersive session as a producer of the scrcpy capture SDK: lossless PNG stills and H.264
// recordings that tools fetch through DebugBus (capture_source, capture_screenshot, capture_video;
// my_mcp_tools capturectl.py and the scrcpy MCP capture_app_screenshot / record_app_video with the
// service com.tencentmalos.xrgamenative/app.gamenative.xrgame.DebugBusService). Two sources:
//   canvas  the image this session renders: the 2D big screen's quad image (mono), or the
//           Windows VR projection's two eye images side by side (stereo_sbs).
//   xr      what the wearer sees, without the lens distortion: the layers this session submits
//           (projection, big screen quad or cylinder, overlay, status layer, keyboard) ray-cast
//           per eye at half the runtime's eye size, side by side. Passthrough shows as black;
//           the Pico system's own layers are not in it.
// A layer is copied in its own command buffer before its swapchain image goes back to the runtime
// (the status layer and the keyboard through Foundation's XrImguiVulkanLayer capture target); the
// compose, the PNG readback and the encoder blit run after xrEndFrame on the same queue and never
// wait for the GPU on the XR thread. Only frames that are captured pay for the copies: a pending
// still, or every other XR frame while recording. Vulkan composite only; without the SDK (release
// and public builds) the commands answer `status: unavailable`.

#include "xr_vulkan.h"

#include <android/native_window.h>
#include <openxr/openxr.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace spatial::xr {
class XrImguiVulkanLayer;
}

namespace xrimmersive::capture {

// Whether this build links the capture SDK.
bool Available();

// One DebugBus command (capture_source, capture_screenshot or capture_video) with its arguments;
// files go to `capturesDir` (app-private). Returns `key: value` lines. Only changes request state;
// any thread.
std::string HandleCommand(const std::string &command, const std::vector<std::string> &args,
                          const std::string &capturesDir);

// The 2D window producer: SteamPSP's own window in front and no immersive session (the library,
// the launch panel, dialogs). Kotlin (XrPanelCapture) renders the resumed activity and its dialog
// windows into Window() on due frames, an image reader that a thread of ours feeds to the encoder
// (xr_panel_feed.h), and renders stills itself. It shares the commands and the state with the
// session; whichever is active holds the producer slot, and an immersive session always takes it
// over. Main thread only.
namespace panel {
constexpr int kDraw = 1;            // draw a frame into Window() now
constexpr int kStill = 2;           // a still is waiting: StillTarget()
constexpr int kActive = 4;          // a recording or still is wanted: tick every frame
constexpr int kSurfaceChanged = 8;  // Window() is a different Surface (or none) from now on
// Takes the producer slot unless an immersive session holds it.
bool Attach();
void Detach();
// Once per frame (or every 100 ms while idle) with the window's size in pixels.
int Tick(uint32_t width, uint32_t height);
// The window to render into while recording, else null; borrowed.
ANativeWindow *Window();
uint32_t EncodedWidth();
uint32_t EncodedHeight();
// After a frame was posted to Window().
void Presented();
bool StillTarget(std::string *token, std::string *path);
void StillDone(const std::string &token, uint32_t width, uint32_t height, const std::string &error);
}  // namespace panel

// The images a submitted layer can show.
enum class Source : uint32_t { Screen = 0, Projection = 1, Hud = 2, Keyboard = 3 };

class XrCapture {
public:
    explicit XrCapture(vulkan::Context &context);
    ~XrCapture();
    XrCapture(const XrCapture &) = delete;
    XrCapture &operator=(const XrCapture &) = delete;

    // XR thread, after xrBeginFrame, with the runtime's recommended eye size: picks up recording
    // starts and stops and a pending still. True when this frame is captured; then every layer
    // drawn this frame is copied and compose() must follow its xrEndFrame.
    bool begin(uint32_t eyeWidth, uint32_t eyeHeight, int64_t displayPeriodNs);
    bool active() const;

    // In the layer's own command buffer, before the release: copies the big screen's (or the
    // interstitial's) swapchain image `view`, in COLOR_ATTACHMENT_OPTIMAL and left there.
    void copyScreen(VkCommandBuffer command, VkImage image, VkImageView view, VkFormat format, uint32_t width,
                    uint32_t height);
    // The same for the projection's array-2 swapchain image, one view per eye layer.
    void copyProjection(VkCommandBuffer command, VkImage image, const std::array<VkImageView, 2> &views,
                        VkFormat format, uint32_t width, uint32_t height);
    // Before the status layer or the keyboard renders: routes its copies into our image for the
    // whole session. The layer redraws at most four times a second, so a still taken on any frame
    // finds its last image there; the copy is small and only as frequent as the redraws.
    void attach(Source source, spatial::xr::XrImguiVulkanLayer *layer);

    // A layer of this frame as it goes to xrEndFrame, bottom to top; `source` names its image.
    void note(const XrCompositionLayerBaseHeader *layer, Source source);

    // After xrEndFrame: locates the eyes and the noted layers' spaces in `space` at `displayTime`,
    // composes the selected source and hands it to the encoder or the still.
    void compose(XrSession session, XrSpace space, XrTime displayTime);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrimmersive::capture
