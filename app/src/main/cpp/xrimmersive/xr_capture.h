#pragma once

// SteamPSP's use of Foundation's capture producers (foundation/modules/capture,
// docs/guides/emulator-capture.md): stills and H.264 recordings that tools fetch through DebugBus
// (capture_source, capture_screenshot, capture_video, capture_config; my_mcp_tools capturectl.py
// and liveview.py, the scrcpy MCP with the service
// com.tencentmalos.xrgamenative/app.gamenative.xrgame.DebugBusService). Producers:
//   the immersive session (Foundation XrLayerCapture): canvas is the 2D big screen's quad image
//     or the Windows VR projection's two eye images; xr is the submitted layers ray-cast per eye.
//   the launcher's 2D window (Foundation WindowCapture, drawn by XrPanelCapture.kt) while no
//     immersive session runs.
// Without the SDK (release and public builds) the commands answer `status: unavailable`.

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

// One DebugBus capture command with its arguments; files go to `capturesDir` (app-private).
// Returns `key: value` lines. Only changes request state; any thread.
std::string HandleCommand(const std::string &command, const std::vector<std::string> &args,
                          const std::string &capturesDir);

// The launcher's 2D window producer for XrPanelCapture.kt (Foundation WindowCapture.h). Main
// thread only.
namespace panel {
constexpr int kDraw = 1;
constexpr int kStill = 2;
constexpr int kActive = 4;
constexpr int kSurfaceChanged = 8;
bool Attach();
void Detach();
int Tick(uint32_t width, uint32_t height);
ANativeWindow *Window();
uint32_t EncodedWidth();
uint32_t EncodedHeight();
void Presented();
bool StillTarget(std::string *token, std::string *path);
void StillDone(const std::string &token, uint32_t width, uint32_t height, const std::string &error);
}  // namespace panel

// The images a submitted layer can show (Foundation capture::Source). The big screen session
// renders no environment scene, so the launch panel takes Foundation's Environment slot.
enum class Source : uint32_t { Screen = 0, Projection = 1, Hud = 2, Keyboard = 3, LaunchPanel = 4 };

// The immersive session as a producer (Foundation XrLayerCapture.h for the call order).
class XrCapture {
public:
    explicit XrCapture(vulkan::Context &context);
    ~XrCapture();
    XrCapture(const XrCapture &) = delete;
    XrCapture &operator=(const XrCapture &) = delete;

    bool begin(uint32_t eyeWidth, uint32_t eyeHeight, int64_t displayPeriodNs);
    bool active() const;
    void copyScreen(VkCommandBuffer command, VkImage image, VkImageView view, VkFormat format, uint32_t width,
                    uint32_t height);
    void copyProjection(VkCommandBuffer command, VkImage image, const std::array<VkImageView, 2> &views,
                        VkFormat format, uint32_t width, uint32_t height);
    void attach(Source source, spatial::xr::XrImguiVulkanLayer *layer);
    void note(const XrCompositionLayerBaseHeader *layer, Source source);
    void compose(XrSession session, XrSpace space, XrTime displayTime);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrimmersive::capture
