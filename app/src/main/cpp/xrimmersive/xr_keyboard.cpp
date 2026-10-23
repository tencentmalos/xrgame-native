#include "xr_keyboard.h"

#include "xr_capture.h"
#include "xr_imgui_fonts.h"
#include "xr_imgui_style.h"
#include "xr_immersive.h"
#include "xr_pinyin.h"
#include "xr_vulkan.h"

#include "imgui.h"
#include "spatial/imgui/overlay/KeyboardLayout.hpp"
#include "spatial/xr/XrImguiVulkanLayer.h"
#include "spatial/xr/XrMath.h"

#include <android/log.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#define LOG_TAG "xrimmersive_keyboard"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace xrimmersive {
namespace {

namespace overlay = spatial::imgui::overlay;
using Clock = std::chrono::steady_clock;

constexpr int kCanvasWidth = 1600;
constexpr int kCanvasHeight = 720;
constexpr float kAspect = static_cast<float>(kCanvasHeight) / kCanvasWidth;
// Two rows above the keys: what was typed (or the pinyin being composed), then the candidates.
constexpr float kTopRowHeight = 88.0f;
constexpr float kTopRowGap = 10.0f;
constexpr float kTopBarHeight = 2.0f * kTopRowHeight + kTopRowGap + 18.0f;
constexpr float kModeWidth = 170.0f;
constexpr float kPagerWidth = 84.0f;
constexpr float kHintHeight = 52.0f;
constexpr float kMargin = 18.0f;
constexpr float kKeyGap = 10.0f;
constexpr float kKeySize = 44.0f;
constexpr float kHintSize = 28.0f;
// Like a keyboard on a desk: ahead of and below the eyes in the head's yaw frame, turned up to face
// them, so a glance down finds it. About 45 degrees wide.
constexpr float kDegrees = 3.14159265f / 180.0f;
constexpr float kWidth = 0.62f;
constexpr float kDistance = 0.75f;  // from the eyes, along the line of sight
constexpr float kDown = 32.0f * kDegrees;
constexpr float kDownMax = 45.0f * kDegrees;
// On the 2D big screen its upper edge stays this far below the screen's lower edge.
constexpr float kScreenClearance = 2.0f * kDegrees;
constexpr float kTriggerPress = 0.55f;
constexpr float kTriggerRelease = 0.35f;
constexpr float kStickNavigate = 0.6f;
constexpr size_t kEchoLimit = 48;
// Bits of InputSnapshot::buttons (XrGamepadBridge's layout).
constexpr uint32_t kButtonA = 1u << 0;
constexpr uint32_t kButtonB = 1u << 1;
constexpr uint32_t kButtonX = 1u << 2;
constexpr uint32_t kButtonY = 1u << 3;
constexpr uint32_t kButtonStart = 1u << 7;

const char *const kBackspace = "\xEE\x80\x80";  // U+E000
const char *const kEnter = "\xEE\x80\x81";      // U+E001
const char *const kClosed = "\xEE\x80\x82";     // U+E002

struct Shared {
    std::mutex mutex;
    bool requested = false;
    // Pinyin first when the device speaks Chinese (Kotlin sets it).
    bool chineseDefault = false;
    bool chinese = false;
    bool composing = false;
    // A hand's ray is on the panel: the page under it leaves that ray to the keyboard.
    bool pointed = false;
    std::string output;
    std::deque<int> pad;
    // Status, without the typed text.
    bool created = false;
    uint64_t updates = 0;
    uint64_t typed = 0;
    std::string family = "alphabetic";
    std::optional<XrVector3f> anchor;
    std::string error;
};
Shared gShared;

struct RestoreImGuiContext {
    ImGuiContext *previous = ImGui::GetCurrentContext();
    ~RestoreImGuiContext() { ImGui::SetCurrentContext(previous); }
};

XrVector3f Rotate(const XrQuaternionf &q, const XrVector3f &v) {
    const XrVector3f t{2.0f * (q.y * v.z - q.z * v.y), 2.0f * (q.z * v.x - q.x * v.z),
                       2.0f * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
            v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}

const char *FamilyName(overlay::KeyboardFamily family) {
    switch (family) {
    case overlay::KeyboardFamily::Symbols: return "symbols";
    case overlay::KeyboardFamily::Specials: return "specials";
    case overlay::KeyboardFamily::Numeric: return "numeric";
    default: return "alphabetic";
    }
}

overlay::KeyboardLayout BuildLayout(overlay::KeyboardFamily family, overlay::KeyboardCase case_state) {
    return overlay::buildKeyboardLayout(family, case_state, overlay::LanguageTag{"en-US"}, overlay::ImeInputType::Text);
}

// Chinese punctuation for its keys while typing Chinese.
std::string ChinesePunctuation(const std::string &text) {
    if (text == ",") return "\xEF\xBC\x8C";  // U+FF0C
    if (text == ".") return "\xE3\x80\x82";  // U+3002
    if (text == "?") return "\xEF\xBC\x9F";  // U+FF1F
    if (text == "!") return "\xEF\xBC\x81";  // U+FF01
    if (text == ":") return "\xEF\xBC\x9A";  // U+FF1A
    if (text == ";") return "\xEF\xBC\x9B";  // U+FF1B
    return text;
}

// The specials page types characters outside the US layout the game receives keys through.
bool Usable(const overlay::KeyboardKey &key) {
    return key.enabled && key.action != overlay::KeyboardKeyAction::Blank &&
           !(key.action == overlay::KeyboardKeyAction::SwitchFamily &&
             key.target_family == overlay::KeyboardFamily::Specials);
}

}  // namespace

void SetXrKeyboardRequested(bool shown) {
    std::lock_guard<std::mutex> lock(gShared.mutex);
    if (gShared.requested == shown) return;
    gShared.requested = shown;
    if (shown) gShared.pad.clear();
    LOGI("XR keyboard %s", shown ? "requested" : "dismissed");
}

void SetXrKeyboardChinese(bool chinese) {
    std::lock_guard<std::mutex> lock(gShared.mutex);
    gShared.chineseDefault = chinese;
}

bool XrKeyboardRequested() {
    std::lock_guard<std::mutex> lock(gShared.mutex);
    return gShared.requested;
}

bool XrKeyboardPointed() {
    std::lock_guard<std::mutex> lock(gShared.mutex);
    return gShared.requested && gShared.pointed;
}

std::string DrainXrKeyboardOutput() {
    std::lock_guard<std::mutex> lock(gShared.mutex);
    std::string out;
    out.swap(gShared.output);
    return out;
}

void QueueXrKeyboardPad(int action) {
    std::lock_guard<std::mutex> lock(gShared.mutex);
    if (gShared.requested && gShared.pad.size() < 32) gShared.pad.push_back(action);
}

std::string XrKeyboardStatusJson() {
    std::lock_guard<std::mutex> lock(gShared.mutex);
    char anchor[96] = "null";
    if (gShared.anchor) {
        std::snprintf(anchor, sizeof(anchor), "[%.2f,%.2f,%.2f]", gShared.anchor->x, gShared.anchor->y,
                      gShared.anchor->z);
    }
    char text[448];
    std::snprintf(text, sizeof(text),
                  "{\"requested\":%s,\"created\":%s,\"updates\":%llu,\"typed\":%llu,\"family\":\"%s\","
                  "\"ime\":\"%s\",\"composing\":%s,\"anchorLocal\":%s,\"error\":\"%s\"}",
                  gShared.requested ? "true" : "false", gShared.created ? "true" : "false",
                  static_cast<unsigned long long>(gShared.updates), static_cast<unsigned long long>(gShared.typed),
                  gShared.family.c_str(), gShared.chinese ? "pinyin" : "latin", gShared.composing ? "true" : "false",
                  anchor, gShared.error.c_str());
    return text;
}

struct XrKeyboard::Impl {
    ImGuiContext *imgui = nullptr;
    spatial::xr::XrImguiLayerConfig config;
    spatial::xr::XrImguiVulkanLayer layer;
    bool failed = false;
    bool shown = false;
    Clock::time_point lastFrame{};
    std::optional<XrPosef> anchor;
    // On the 2D big screen: the panel's centre in the screen's yaw frame (x right, y up, z towards
    // the viewer, from the screen's centre) and its tilt, fixed when it is shown.
    std::optional<XrVector3f> screenOffset;
    float screenDown = 0.0f;
    XrExtent2Df size{kWidth, kWidth * kAspect};

    overlay::KeyboardLayout layout = BuildLayout(overlay::KeyboardFamily::Alphabetic, overlay::KeyboardCase::Lower);
    overlay::KeyboardNavigationState focus{overlay::KeyboardFocusRegion::Keys, 1, 4};
    overlay::KeyRepeatState navRepeat;
    int navDirection = -1;
    uint32_t lastButtons = 0;
    // The trigger only presses once it was released while pointing at the panel.
    bool armed = false;
    bool mouseDown = false;
    std::string echo;
    // Pinyin while `chinese`: letters compose, Space takes the first candidate, Enter types the letters.
    PinyinComposer pinyin;
    bool chinese = false;
    size_t candidateStart = 0;
    size_t candidateEnd = 0;
    std::vector<size_t> candidatePages;

    bool create(XrSession session, vulkan::Context &context);
    void place(XrSpace localSpace, XrSpace viewSpace, XrTime time, const XrCompositionLayerQuad *screen);
    void applyPointer(const InputSnapshot &input, ImGuiIO &io);
    void applyPad(const InputSnapshot &input, float dt);
    void navigate(overlay::NavigationDirection direction);
    void pressFocused();
    void press(const overlay::KeyboardKey &key);
    // A key's text, through the pinyin composer while it is on.
    void feed(const std::string &text);
    void choose(size_t index);
    void setChinese(bool on);
    void type(const std::string &text, bool echoText);
    void drawTopBar(ImDrawList *drawList, float innerWidth);
    void close();
    void draw(float dt);
    void destroy();
};

bool XrKeyboard::Impl::create(XrSession session, vulkan::Context &context) {
    config.canvas_width = kCanvasWidth;
    config.canvas_height = kCanvasHeight;
    config.space = spatial::xr::XrImguiLayerSpace::Local;
    config.width_meters = size.width;
    config.height_meters = size.height;
    config.input_enabled = false;
    // The capture producer copies the panel's images (TRANSFER_SRC swapchain).
    config.capture_source = capture::Available();

    RestoreImGuiContext restore;
    imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;
    io.DisplaySize = {static_cast<float>(kCanvasWidth), static_cast<float>(kCanvasHeight)};
    // Chinese candidates and echoes need CJK glyphs; Noto Sans CJK covers the Latin keys too.
    io.FontDefault = AddCjkFont(io.Fonts, kKeySize);
    // A local named `style` would hide the style namespace.
    ImGuiStyle &look = ImGui::GetStyle();
    style::ApplyKeyboardStyle(look);
    look.WindowPadding = {kMargin, kMargin};

    spatial::xr::XrImguiVulkanBinding binding;
    binding.api_version = VK_API_VERSION_1_3;
    binding.instance = context.instance();
    binding.physical_device = context.physicalDevice();
    binding.device = context.device();
    binding.queue = context.queue();
    binding.queue_family_index = context.queueFamily();
    binding.vk_get_instance_proc_addr = context.vk().vkGetInstanceProcAddr;
    binding.vk_get_device_proc_addr = context.vk().vkGetDeviceProcAddr;
    binding.queue_submit_mutex = &context.queueMutex();
    if (!layer.Create(session, xrCreateSwapchain, xrDestroySwapchain, xrEnumerateSwapchainFormats,
                      xrEnumerateSwapchainImages, xrAcquireSwapchainImage, xrWaitSwapchainImage,
                      xrReleaseSwapchainImage, binding, config)) {
        return false;
    }
    LOGI("XR keyboard layer created: %dx%d canvas", kCanvasWidth, kCanvasHeight);
    return true;
}

void XrKeyboard::Impl::place(XrSpace localSpace, XrSpace viewSpace, XrTime time, const XrCompositionLayerQuad *screen) {
    // On the 2D big screen the panel stays under the screen, wherever the head turns, and moves
    // with it; it is laid out once when shown, from where the eyes are then.
    if (screen != nullptr) {
        // The screen's yaw: its normal, level. Its own pitch (a raised screen leans back) stays out.
        const XrVector3f normal = Rotate(screen->pose.orientation, {0.0f, 0.0f, 1.0f});
        const float yaw = std::atan2(normal.x, normal.z);
        const float sy = std::sin(yaw), cy = std::cos(yaw);
        const XrVector3f &centre = screen->pose.position;
        if (!screenOffset) {
            XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
            constexpr XrSpaceLocationFlags kTracked = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if (XR_FAILED(xrLocateSpace(viewSpace, localSpace, time, &head)) || (head.locationFlags & kTracked) != kTracked) {
                return;
            }
            size = {kWidth, kWidth * kAspect};
            // The eyes in the screen's yaw frame, then centred under the screen.
            const float ex = head.pose.position.x - centre.x, ez = head.pose.position.z - centre.z;
            const float eyeUp = head.pose.position.y - centre.y;
            const float eyeAhead = sy * ex + cy * ez;
            // Low enough that the screen's lower edge stays in view above the keyboard.
            const XrVector3f lower = Rotate(screen->pose.orientation, {0.0f, -screen->size.height * 0.5f, 0.0f});
            const float edgeAhead = eyeAhead - (sy * lower.x + cy * lower.z);
            const float edgeDown = std::atan2(eyeUp - lower.y, edgeAhead);
            const float halfHeight = std::atan2(size.height * 0.5f, kDistance);
            screenDown = std::clamp(edgeDown + halfHeight + kScreenClearance, kDown, kDownMax);
            screenOffset = XrVector3f{0.0f, eyeUp - kDistance * std::sin(screenDown), eyeAhead - kDistance * std::cos(screenDown)};
            LOGI("XR keyboard placed under the screen, %.0f degrees below the eyes", screenDown / kDegrees);
        }
        // Turned about the screen's yaw, then pitched back by the tilt to face the eyes.
        const float hy = std::sin(yaw * 0.5f), hc = std::cos(yaw * 0.5f);
        const float px = std::sin(-screenDown * 0.5f), pc = std::cos(-screenDown * 0.5f);
        XrPosef pose{};
        pose.orientation = {hc * px, hy * pc, -hy * px, hc * pc};
        pose.position = {centre.x + cy * screenOffset->x + sy * screenOffset->z, centre.y + screenOffset->y,
                         centre.z - sy * screenOffset->x + cy * screenOffset->z};
        anchor = pose;
        return;
    }
    // Stereo: placed once when shown, where the head faces then, and fixed in the room.
    if (anchor) return;
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    constexpr XrSpaceLocationFlags kTracked = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (XR_FAILED(xrLocateSpace(viewSpace, localSpace, time, &head)) || (head.locationFlags & kTracked) != kTracked) return;
    const XrQuaternionf &q = head.pose.orientation;
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float yaw = std::atan2(-fx, -fz);
    const XrVector3f &eye = head.pose.position;
    size = {kWidth, kWidth * kAspect};
    const float down = kDown;
    // Turned about the yaw, then pitched back by `down` so the panel faces the eyes (top edge away).
    const float sy = std::sin(yaw * 0.5f), cy = std::cos(yaw * 0.5f);
    const float sx = std::sin(-down * 0.5f), cx = std::cos(-down * 0.5f);
    XrPosef pose{};
    pose.orientation = {cy * sx, sy * cx, -sy * sx, cy * cx};
    const float ahead = kDistance * std::cos(down);
    pose.position = {eye.x - std::sin(yaw) * ahead, eye.y - kDistance * std::sin(down), eye.z - std::cos(yaw) * ahead};
    anchor = pose;
    LOGI("XR keyboard placed %.0f degrees below the eyes at (%.2f, %.2f, %.2f)", down / kDegrees, pose.position.x,
         pose.position.y, pose.position.z);
}

void XrKeyboard::Impl::applyPointer(const InputSnapshot &input, ImGuiIO &io) {
    // The right hand points first, as in shadPS4's layer panel.
    std::optional<ImVec2> hit;
    float trigger = 0.0f;
    for (const int hand : {1, 0}) {
        if (!anchor || !input.aimPoseValid[hand]) continue;
        const auto result = spatial::xr::math::RayCastQuadLayer(
            spatial::xr::math::MakeRayFromPose(input.aimPoses[hand]), *anchor, size);
        if (!result.hit) continue;
        hit = ImVec2{(result.x_offset + 0.5f) * kCanvasWidth, (0.5f - result.y_offset) * kCanvasHeight};
        trigger = hand == 1 ? input.triggerR : input.triggerL;
        break;
    }
    {
        std::lock_guard<std::mutex> lock(gShared.mutex);
        gShared.pointed = hit.has_value();
    }
    if (!hit) {
        armed = false;
        mouseDown = false;
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(0, false);
        return;
    }
    if (trigger < kTriggerRelease) armed = true;
    if (mouseDown) {
        mouseDown = trigger > kTriggerRelease;
    } else {
        mouseDown = armed && trigger > kTriggerPress;
    }
    io.AddMousePosEvent(hit->x, hit->y);
    io.AddMouseButtonEvent(0, mouseDown);
}

void XrKeyboard::Impl::navigate(overlay::NavigationDirection direction) {
    focus = overlay::navigateKeyboard(layout, focus, direction, true);
}

void XrKeyboard::Impl::applyPad(const InputSnapshot &input, float dt) {
    const uint32_t pressed = input.buttons & ~lastButtons;
    lastButtons = input.buttons;
    if (pressed & kButtonA) pressFocused();
    if (pressed & kButtonB) feed(kBackspace);
    if (pressed & kButtonY) feed(" ");
    if (pressed & kButtonX) {
        const overlay::KeyboardKey shift{.action = overlay::KeyboardKeyAction::Shift};
        press(shift);
    }
    if (pressed & kButtonStart) close();

    // Left stick, with hold-to-repeat.
    int direction = -1;
    if (input.leftY > kStickNavigate) direction = static_cast<int>(overlay::NavigationDirection::Up);
    else if (input.leftY < -kStickNavigate) direction = static_cast<int>(overlay::NavigationDirection::Down);
    else if (input.leftX < -kStickNavigate) direction = static_cast<int>(overlay::NavigationDirection::Left);
    else if (input.leftX > kStickNavigate) direction = static_cast<int>(overlay::NavigationDirection::Right);
    if (direction != navDirection) {
        navRepeat.reset();
        navDirection = direction;
    }
    const uint32_t steps = navRepeat.advance(direction >= 0, dt);
    for (uint32_t i = 0; i < steps && direction >= 0; ++i) navigate(static_cast<overlay::NavigationDirection>(direction));

    std::deque<int> pad;
    {
        std::lock_guard<std::mutex> lock(gShared.mutex);
        pad.swap(gShared.pad);
    }
    for (const int action : pad) {
        switch (action) {
        case kKeyboardPadUp: navigate(overlay::NavigationDirection::Up); break;
        case kKeyboardPadDown: navigate(overlay::NavigationDirection::Down); break;
        case kKeyboardPadLeft: navigate(overlay::NavigationDirection::Left); break;
        case kKeyboardPadRight: navigate(overlay::NavigationDirection::Right); break;
        case kKeyboardPadPress: pressFocused(); break;
        case kKeyboardPadBackspace: feed(kBackspace); break;
        case kKeyboardPadSpace: feed(" "); break;
        case kKeyboardPadShift: {
            const overlay::KeyboardKey shift{.action = overlay::KeyboardKeyAction::Shift};
            press(shift);
            break;
        }
        case kKeyboardPadClose: close(); break;
        default: break;
        }
    }
}

void XrKeyboard::Impl::pressFocused() {
    if (focus.region != overlay::KeyboardFocusRegion::Keys) {
        // Above or below the keys sits the panel's own Close button.
        close();
        return;
    }
    if (const overlay::KeyboardKey *key = layout.keyAt(focus.row, focus.column); key != nullptr && Usable(*key)) press(*key);
}

void XrKeyboard::Impl::press(const overlay::KeyboardKey &key) {
    using Action = overlay::KeyboardKeyAction;
    switch (key.action) {
    case Action::Character:
        feed(key.glyph);
        if (layout.case_state == overlay::KeyboardCase::Shift) overlay::applyKeyboardCase(layout, overlay::KeyboardCase::Lower);
        break;
    case Action::Space: feed(" "); break;
    case Action::Backspace: feed(kBackspace); break;
    case Action::Enter: feed(kEnter); break;
    case Action::Shift:
    case Action::CapsLock: {
        const overlay::KeyboardCase next = layout.case_state == overlay::KeyboardCase::Lower ? overlay::KeyboardCase::Shift
                                           : layout.case_state == overlay::KeyboardCase::Shift ? overlay::KeyboardCase::CapsLock
                                                                                              : overlay::KeyboardCase::Lower;
        if (layout.family == overlay::KeyboardFamily::Alphabetic) overlay::applyKeyboardCase(layout, next);
        break;
    }
    case Action::SwitchFamily:
        if (key.target_family == overlay::KeyboardFamily::Specials) break;
        layout = BuildLayout(key.target_family, overlay::KeyboardCase::Lower);
        focus = overlay::clampKeyboardFocus(layout, focus);
        {
            std::lock_guard<std::mutex> lock(gShared.mutex);
            gShared.family = FamilyName(layout.family);
        }
        break;
    case Action::Confirm:
    case Action::Cancel: close(); break;
    default: break;
    }
}

void XrKeyboard::Impl::feed(const std::string &text) {
    if (!chinese) {
        type(text, text != kBackspace && text != kEnter);
        return;
    }
    if (text.size() == 1 && text[0] >= 'a' && text[0] <= 'z') {
        pinyin.addLetter(text[0]);
        candidateStart = 0;
        candidatePages.clear();
        return;
    }
    if (pinyin.composing()) {
        if (text == " ") {
            choose(0);
            return;
        }
        if (text == kBackspace) {
            pinyin.backspace();
            candidateStart = 0;
            candidatePages.clear();
            return;
        }
        if (text == kEnter) {
            type(pinyin.commitLetters(), true);
            return;
        }
        // Anything else ends the composition with the sentence candidate.
        choose(0);
        if (pinyin.composing()) type(pinyin.commitLetters(), true);
    }
    const std::string out = ChinesePunctuation(text);
    type(out, out != kBackspace && out != kEnter);
}

void XrKeyboard::Impl::choose(size_t index) {
    const std::string done = pinyin.choose(index);
    candidateStart = 0;
    candidatePages.clear();
    if (!done.empty()) type(done, true);
}

void XrKeyboard::Impl::setChinese(bool on) {
    if (on && !pinyin.available()) on = false;
    // Leaving pinyin types what was being composed as letters.
    if (!on && pinyin.composing()) type(pinyin.commitLetters(), true);
    chinese = on;
    std::lock_guard<std::mutex> lock(gShared.mutex);
    gShared.chinese = on;
}

void XrKeyboard::Impl::type(const std::string &text, bool echoText) {
    if (text == kBackspace) {
        // Drop one UTF-8 code point from the echo.
        while (!echo.empty()) {
            const auto byte = static_cast<unsigned char>(echo.back());
            echo.pop_back();
            if ((byte & 0xC0U) != 0x80U) break;
        }
    } else if (echoText) {
        echo += text;
        if (echo.size() > kEchoLimit) echo.erase(0, echo.size() - kEchoLimit);
    } else if (text == kEnter) {
        echo.clear();
    }
    std::lock_guard<std::mutex> lock(gShared.mutex);
    gShared.output += text;
    ++gShared.typed;
}

void XrKeyboard::Impl::close() {
    pinyin.reset();
    pinyin.flush();
    {
        std::lock_guard<std::mutex> lock(gShared.mutex);
        gShared.requested = false;
        gShared.output += kClosed;
    }
    LOGI("XR keyboard closed from the panel");
}

void XrKeyboard::Impl::drawTopBar(ImDrawList *drawList, float innerWidth) {
    const float closeWidth = kTopRowHeight * 1.4f;
    const bool canPinyin = pinyin.available();
    // Row 1: the input mode, what was typed (or the pinyin being composed), and Close.
    float left = kMargin;
    if (canPinyin) {
        ImGui::SetCursorPos({left, kMargin});
        if (style::IconButton("##mode", style::kKeyboard, chinese ? "\xE4\xB8\xAD" : "EN", {kModeWidth, kTopRowHeight},
                              chinese ? style::kKeySpecial : style::kKeyFunction)) {
            setChinese(!chinese);
        }
        left += kModeWidth + kKeyGap;
    }
    const ImVec2 boxMin{left, kMargin};
    const ImVec2 boxMax{kMargin + innerWidth - closeWidth - kKeyGap, kMargin + kTopRowHeight};
    drawList->AddRectFilled(boxMin, boxMax, style::kTextField, 10.0f);
    drawList->AddRect(boxMin, boxMax, style::kKeyboardLine, 10.0f, 0, 1.5f);
    drawList->PushClipRect(boxMin, boxMax, true);
    const bool composing = pinyin.composing();
    const std::string &shown = composing ? pinyin.preedit() : echo;
    const ImVec2 shownSize = ImGui::CalcTextSize(shown.c_str());
    const float textX = std::min(boxMin.x + 18.0f, boxMax.x - 18.0f - shownSize.x);
    const float textY = boxMin.y + (boxMax.y - boxMin.y - shownSize.y) * 0.5f;
    drawList->AddText({textX, textY}, composing ? style::kAccentLight : style::kKeyText, shown.c_str());
    // The composition is underlined, as an IME draws its preedit; a caret follows the text.
    if (composing) {
        drawList->AddLine({textX, textY + shownSize.y + 2.0f}, {textX + shownSize.x, textY + shownSize.y + 2.0f},
                          style::kAccentLight, 2.0f);
    }
    const float caretX = textX + shownSize.x + 3.0f;
    drawList->AddLine({caretX, textY + 4.0f}, {caretX, textY + shownSize.y - 4.0f}, style::kKeyText, 2.0f);
    drawList->PopClipRect();
    const bool closeFocused = focus.region != overlay::KeyboardFocusRegion::Keys;
    ImGui::SetCursorPos({boxMax.x + kKeyGap, kMargin});
    if (style::IconButton("##close", style::kClose, nullptr, {closeWidth, kTopRowHeight}, style::kKeyFunction)) close();
    if (closeFocused) style::DrawFocus(drawList, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), 10.0f);

    // Row 2: the candidates, a page at a time; the first is what Space takes.
    const float rowTop = kMargin + kTopRowHeight + kTopRowGap;
    if (!composing || pinyin.candidateCount() == 0) {
        if (chinese) {
            const char *hint = "\xE6\x8B\xBC\xE9\x9F\xB3";  // the mode's name, pinyin
            const ImVec2 size = ImGui::CalcTextSize(hint);
            drawList->AddText({kMargin + 18.0f, rowTop + (kTopRowHeight - size.y) * 0.5f}, IM_COL32(110, 110, 110, 255), hint);
        }
        return;
    }
    const float right = kMargin + innerWidth - 2.0f * (kPagerWidth + kKeyGap);
    float x = kMargin;
    size_t index = candidateStart;
    std::optional<size_t> picked;
    for (; index < pinyin.candidateCount(); ++index) {
        const std::string &text = pinyin.candidate(index);
        const float width = std::max(ImGui::CalcTextSize(text.c_str()).x + 44.0f, kTopRowHeight);
        if (x + width > right && index > candidateStart) break;
        ImGui::SetCursorPos({x, rowTop});
        ImGui::PushID(static_cast<int>(index));
        const bool first = index == candidateStart;
        if (style::IconButton("##candidate", nullptr, text.c_str(), {width, kTopRowHeight},
                              first ? style::kKeySpecial : style::kKey)) {
            picked = index;
        }
        ImGui::PopID();
        x += width + kKeyGap;
    }
    candidateEnd = index;
    ImGui::SetCursorPos({right + kKeyGap, rowTop});
    ImGui::BeginDisabled(candidatePages.empty());
    if (style::IconButton("##previous", style::kPrev, nullptr, {kPagerWidth, kTopRowHeight}, style::kKeyFunction)) {
        candidateStart = candidatePages.back();
        candidatePages.pop_back();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, kKeyGap);
    ImGui::BeginDisabled(candidateEnd >= pinyin.candidateCount());
    if (style::IconButton("##next", style::kPlay, nullptr, {kPagerWidth, kTopRowHeight}, style::kKeyFunction)) {
        candidatePages.push_back(candidateStart);
        candidateStart = candidateEnd;
    }
    ImGui::EndDisabled();
    if (picked) choose(*picked);
}

void XrKeyboard::Impl::draw(float dt) {
    ImGuiIO &io = ImGui::GetIO();
    io.DeltaTime = std::max(dt, 1.0f / 240.0f);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("xr_keyboard", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    const float innerWidth = kCanvasWidth - 2.0f * kMargin;

    drawTopBar(drawList, innerWidth);

    // Keys, in shadPS4's keyboard colours: letters dark, function keys lighter, Enter blue.
    using Action = overlay::KeyboardKeyAction;
    const float gridTop = kMargin + kTopBarHeight;
    const float gridHeight = kCanvasHeight - gridTop - kHintHeight - kMargin;
    const float cellWidth = innerWidth / std::max<int>(layout.columns, 1);
    const float cellHeight = gridHeight / std::max<int>(layout.rows, 1);
    const overlay::KeyboardKey *focused =
        focus.region == overlay::KeyboardFocusRegion::Keys ? layout.keyAt(focus.row, focus.column) : nullptr;
    std::optional<overlay::KeyboardKey> pressed;
    for (size_t index = 0; index < layout.keys.size(); ++index) {
        const overlay::KeyboardKey &key = layout.keys[index];
        if (key.action == Action::Blank) continue;
        const bool usable = Usable(key);
        const bool shifted = layout.case_state != overlay::KeyboardCase::Lower;
        const bool active = key.action == Action::Shift && shifted;
        const char *icon = nullptr;
        std::string label;
        ImU32 fill = style::kKey;
        switch (key.action) {
        case Action::Character: label = key.glyph; break;
        case Action::Space: icon = style::kSpace; break;
        case Action::Backspace: icon = style::kBackspace; fill = style::kKeyFunction; break;
        case Action::Enter: icon = style::kReturn; fill = style::kKeySpecial; break;
        case Action::Shift:
        case Action::CapsLock:
            icon = layout.case_state == overlay::KeyboardCase::CapsLock ? style::kCaps : style::kShift;
            fill = active ? style::kKeySpecial : style::kKeyFunction;
            break;
        default: label = key.label.text(); fill = style::kKeyFunction; break;
        }
        ImGui::SetCursorPos({kMargin + key.column * cellWidth, gridTop + key.row * cellHeight});
        ImGui::PushID(static_cast<int>(index));
        if (!usable) ImGui::BeginDisabled();
        if (style::IconButton("##key", icon, label.empty() ? nullptr : label.c_str(),
                              {key.column_span * cellWidth - kKeyGap, cellHeight - kKeyGap}, fill)) {
            pressed = key;
        }
        if (!usable) ImGui::EndDisabled();
        if (focused == &key) style::DrawFocus(drawList, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), 10.0f);
        ImGui::PopID();
    }
    if (pressed) press(*pressed);

    // Hint line: each controller button and what it does, without words.
    struct Hint {
        const char *badge;
        ImU32 color;
        const char *icon;
    };
    const Hint hints[] = {{"A", style::kBadgeA, style::kCheck},
                          {"B", style::kBadgeB, style::kBackspace},
                          {"Y", style::kBadgeY, style::kSpace},
                          {"X", style::kBadgeX, style::kShift}};
    const float badgeRadius = kHintSize * 0.55f;
    const float hintY = kCanvasHeight - kMargin - kHintHeight * 0.5f;
    ImFont *font = ImGui::GetFont();
    float hintWidth = 0.0f;
    for (const Hint &hint : hints) hintWidth += 2.0f * badgeRadius + 10.0f + font->CalcTextSizeA(kHintSize, FLT_MAX, 0.0f, hint.icon).x + 44.0f;
    const float menuWidth = 2.0f * badgeRadius + 10.0f + font->CalcTextSizeA(kHintSize, FLT_MAX, 0.0f, style::kClose).x;
    float hintX = (kCanvasWidth - hintWidth - menuWidth) * 0.5f;
    for (const Hint &hint : hints) {
        style::DrawButtonBadge(drawList, {hintX + badgeRadius, hintY}, badgeRadius, hint.badge, hint.color);
        hintX += 2.0f * badgeRadius + 10.0f;
        const ImVec2 size = font->CalcTextSizeA(kHintSize, FLT_MAX, 0.0f, hint.icon);
        drawList->AddText(font, kHintSize, {hintX, hintY - size.y * 0.5f}, IM_COL32(170, 170, 170, 255), hint.icon);
        hintX += size.x + 44.0f;
    }
    // The menu button closes: a rounded "menu" tag, as on the controller.
    const ImVec2 tagMin{hintX, hintY - badgeRadius};
    const ImVec2 tagMax{hintX + 2.0f * badgeRadius, hintY + badgeRadius};
    drawList->AddRectFilled(tagMin, tagMax, IM_COL32(200, 200, 200, 255), 6.0f);
    for (int line = -1; line <= 1; ++line) {
        const float y = hintY + line * badgeRadius * 0.45f;
        drawList->AddLine({tagMin.x + badgeRadius * 0.5f, y}, {tagMax.x - badgeRadius * 0.5f, y}, IM_COL32(16, 18, 22, 255), 2.0f);
    }
    hintX += 2.0f * badgeRadius + 10.0f;
    const ImVec2 closeSize = font->CalcTextSizeA(kHintSize, FLT_MAX, 0.0f, style::kClose);
    drawList->AddText(font, kHintSize, {hintX, hintY - closeSize.y * 0.5f}, IM_COL32(170, 170, 170, 255), style::kClose);

    // Where the ray points.
    if (io.MousePos.x > -1.0f) {
        ImDrawList *top = ImGui::GetForegroundDrawList();
        top->AddCircleFilled(io.MousePos, mouseDown ? 13.0f : 10.0f, mouseDown ? style::kAccentLight : IM_COL32(255, 255, 255, 230));
        top->AddCircle(io.MousePos, mouseDown ? 13.0f : 10.0f, IM_COL32(0, 0, 0, 120), 24, 2.0f);
    }
    ImGui::End();
    ImGui::Render();
}

void XrKeyboard::Impl::destroy() {
    anchor.reset();
    if (imgui != nullptr) {
        RestoreImGuiContext restore;
        ImGui::SetCurrentContext(imgui);
        layer.Destroy();
        ImGui::DestroyContext(imgui);
        imgui = nullptr;
    }
    std::lock_guard<std::mutex> lock(gShared.mutex);
    gShared.created = false;
}

XrKeyboard::XrKeyboard() : impl_(std::make_unique<Impl>()) {}

XrKeyboard::~XrKeyboard() { destroy(); }

bool XrKeyboard::update(XrSession session, vulkan::Context &context, const InputSnapshot &input, XrSpace localSpace,
                        XrSpace viewSpace, XrTime time, const XrCompositionLayerQuad *screen) {
    Impl &kb = *impl_;
    if (!XrKeyboardRequested()) {
        if (kb.shown) {
            // Shown again, the panel starts fresh and is placed where the user looks then.
            kb.shown = false;
            kb.anchor.reset();
            kb.screenOffset.reset();
            kb.echo.clear();
            kb.lastButtons = 0;
            kb.mouseDown = false;
            kb.armed = false;
        }
        return false;
    }
    if (kb.failed || context.lost()) return false;
    if (kb.imgui == nullptr) {
        if (!kb.create(session, context)) {
            kb.failed = true;
            LOGE("XR keyboard layer creation failed; the keyboard stays off for this session");
            kb.destroy();
            std::lock_guard<std::mutex> lock(gShared.mutex);
            gShared.error = "layer_create_failed";
            return false;
        }
        std::lock_guard<std::mutex> lock(gShared.mutex);
        gShared.created = true;
    }
    if (!kb.shown) {
        kb.shown = true;
        // Buttons already held when the keyboard opens (the menu's A) must not press a key.
        kb.lastButtons = input.buttons;
        kb.layout = BuildLayout(overlay::KeyboardFamily::Alphabetic, overlay::KeyboardCase::Lower);
        kb.focus = {overlay::KeyboardFocusRegion::Keys, 1, 4};
        bool chinese;
        {
            std::lock_guard<std::mutex> lock(gShared.mutex);
            chinese = gShared.chineseDefault;
        }
        kb.pinyin.reset();
        kb.setChinese(chinese);
    }
    kb.place(localSpace, viewSpace, time, screen);
    if (!kb.anchor) return false;

    const auto now = Clock::now();
    const float dt = kb.lastFrame == Clock::time_point{} ? 1.0f / 72.0f : std::chrono::duration<float>(now - kb.lastFrame).count();
    kb.lastFrame = now;
    RestoreImGuiContext restore;
    ImGui::SetCurrentContext(kb.imgui);
    kb.applyPointer(input, ImGui::GetIO());
    kb.applyPad(input, dt);
    if (!XrKeyboardRequested()) return false;
    kb.draw(dt);
    spatial::xr::XrImguiRenderedLayer rendered{"xr_keyboard", kb.config, kb.imgui, ImGui::GetDrawData(),
                                               ImGui::GetIO().Fonts};
    kb.layer.Render(rendered, true);
    {
        std::lock_guard<std::mutex> lock(gShared.mutex);
        ++gShared.updates;
        gShared.anchor = kb.anchor->position;
        gShared.composing = kb.pinyin.composing();
    }
    return kb.layer.HasReleasedImage();
}

void XrKeyboard::fill(XrSpace localSpace, XrCompositionLayerQuad *quad) {
    Impl &kb = *impl_;
    kb.layer.FillQuadLayer(localSpace, false, *quad);
    quad->pose = *kb.anchor;
    quad->size = kb.size;
}

void XrKeyboard::reanchor() {
    impl_->anchor.reset();
    impl_->screenOffset.reset();
}

void XrKeyboard::destroy() { impl_->destroy(); }

spatial::xr::XrImguiVulkanLayer *XrKeyboard::imguiLayer() {
    return impl_->layer.IsCreated() ? &impl_->layer : nullptr;
}

}  // namespace xrimmersive
