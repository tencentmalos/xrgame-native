#pragma once

#include <openxr/openxr.h>

#include <memory>
#include <string>

namespace spatial::xr {
class XrImguiVulkanLayer;
}

namespace xrimmersive {

namespace vulkan {
class Context;
}
struct InputSnapshot;

// The panel Kotlin wants shown, as lines of tab-separated records (XrLaunchPanelModel.kt writes
// them); empty hides it. Any thread.
void SetLaunchPanelModel(std::string model);
bool LaunchPanelShown();

// What was chosen since the last call, one event per line: "select <section> <row> <choice>",
// "start", "cancel", "advanced", "ok". Any thread.
std::string DrainLaunchPanelEvents();

/**
 * A game's launch panel drawn on the 2D big screen itself, the size of the screen and in its place,
 * so starting a game from a hosted client (Steam on the big screen) never leaves XR. Kotlin owns
 * the options and their logic; the panel draws rows of choices from the model and reports what
 * was picked. It also shows the launch's progress and messages.
 *
 * Controllers: point and pull the trigger to pick a choice or a button, the right stick scrolls
 * the options, A starts (or confirms a message), B cancels.
 */
class XrLaunchPanel {
public:
    XrLaunchPanel();
    ~XrLaunchPanel();
    XrLaunchPanel(const XrLaunchPanel &) = delete;
    XrLaunchPanel &operator=(const XrLaunchPanel &) = delete;

    // XR thread, each frame: applies the input and redraws while a model is set. True when a
    // released image can be composited over `screen`.
    bool update(XrSession session, vulkan::Context &context, const InputSnapshot &input,
                const XrCompositionLayerQuad &screen);
    void fill(XrSpace localSpace, XrCompositionLayerQuad *quad);
    // The panel's layer once created, for the capture producer; else null.
    spatial::xr::XrImguiVulkanLayer *imguiLayer();
    // Before the Vulkan device and the session go away.
    void destroy();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrimmersive
