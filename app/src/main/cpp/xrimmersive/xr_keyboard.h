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

// Kotlin asks for the keyboard (quick menu Keyboard, DebugBus vr_keyboard); any thread.
void SetXrKeyboardRequested(bool shown);
bool XrKeyboardRequested();
// Whether a hand's ray is on the open keyboard (as of its last frame); any thread.
bool XrKeyboardPointed();
// Whether the keyboard opens in pinyin (when the device speaks Chinese and the dictionary is set).
void SetXrKeyboardChinese(bool chinese);

// Typed output since the last call, UTF-8. Private-use code points stand for keys: U+E000
// Backspace, U+E001 Enter, U+E002 the keyboard closed. Any thread.
std::string DrainXrKeyboardOutput();

// A physical gamepad drives the open keyboard through these; any thread.
enum XrKeyboardPad : int {
    kKeyboardPadUp = 0,
    kKeyboardPadDown,
    kKeyboardPadLeft,
    kKeyboardPadRight,
    kKeyboardPadPress,
    kKeyboardPadBackspace,
    kKeyboardPadSpace,
    kKeyboardPadShift,
    kKeyboardPadClose,
};
void QueueXrKeyboardPad(int action);

// One-line JSON for DebugBus vr_keyboard; never contains typed text. Any thread.
std::string XrKeyboardStatusJson();

/**
 * The XR on-screen keyboard: shadPS4's layer-panel pattern (its own ImGui context in a quad,
 * controller rays as the mouse, placed once and hidden while not requested) around Foundation's
 * Latin keyboard model. Keys type straight into the game as they are pressed, like the Android IME
 * path; the panel echoes what was typed. In pinyin mode (the 中/EN button) letters compose instead:
 * the panel shows the underlined composition and a row of candidates, and a chosen sentence is
 * typed as one piece of text.
 *
 * Controllers: point and pull the trigger to press a key, or move focus with the left stick and
 * press A. B deletes, Y types a space, X is Shift, Menu closes. The trigger is only ever a click,
 * so it cannot also confirm or shift.
 */
class XrKeyboard {
public:
    XrKeyboard();
    ~XrKeyboard();
    XrKeyboard(const XrKeyboard &) = delete;
    XrKeyboard &operator=(const XrKeyboard &) = delete;

    // XR thread, each frame while requested: places the panel once (ahead of and below the eyes,
    // below `screen` on the 2D big screen), applies the input and redraws. True when a released
    // panel image can be composited.
    bool update(XrSession session, vulkan::Context &context, const InputSnapshot &input, XrSpace localSpace,
                XrSpace viewSpace, XrTime time, const XrCompositionLayerQuad *screen);
    void fill(XrSpace localSpace, XrCompositionLayerQuad *quad);
    // XR thread: places the panel again on the next update (LOCAL was recentred).
    void reanchor();
    // The panel's layer once created, for the capture producer; else null.
    spatial::xr::XrImguiVulkanLayer *imguiLayer();
    // Before the Vulkan device and the session go away.
    void destroy();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrimmersive
