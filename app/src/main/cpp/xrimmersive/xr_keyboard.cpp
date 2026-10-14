#include "xr_keyboard.h"

#include "xr_capture.h"
#include "xr_immersive.h"
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

#define LOG_TAG "xrimmersive_keyboard"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace xrimmersive {
namespace {

namespace overlay = spatial::imgui::overlay;
using Clock = std::chrono::steady_clock;

constexpr int kCanvasWidth = 1600;
constexpr int kCanvasHeight = 620;
constexpr float kAspect = static_cast<float>(kCanvasHeight) / kCanvasWidth;
constexpr float kTopBarHeight = 96.0f;
constexpr float kHintHeight = 52.0f;
constexpr float kMargin = 18.0f;
constexpr float kKeyGap = 10.0f;
constexpr float kKeySize = 44.0f;
constexpr float kHintSize = 28.0f;
// The stereo panel: ahead of and below the head in its yaw frame, about 47 degrees wide.
constexpr float kStereoWidth = 1.0f;
constexpr float kStereoAhead = 1.1f;
constexpr float kStereoBelow = 0.32f;
// On the 2D big screen it hangs under the screen, a little over half its width.
constexpr float kScreenWidthShare = 0.62f;
constexpr float kScreenGap = 0.04f;
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

bool XrKeyboardRequested() {
    std::lock_guard<std::mutex> lock(gShared.mutex);
    return gShared.requested;
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
    char text[384];
    std::snprintf(text, sizeof(text),
                  "{\"requested\":%s,\"created\":%s,\"updates\":%llu,\"typed\":%llu,\"family\":\"%s\","
                  "\"anchorLocal\":%s,\"error\":\"%s\"}",
                  gShared.requested ? "true" : "false", gShared.created ? "true" : "false",
                  static_cast<unsigned long long>(gShared.updates), static_cast<unsigned long long>(gShared.typed),
                  gShared.family.c_str(), anchor, gShared.error.c_str());
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
    XrExtent2Df size{kStereoWidth, kStereoWidth * kAspect};

    overlay::KeyboardLayout layout = BuildLayout(overlay::KeyboardFamily::Alphabetic, overlay::KeyboardCase::Lower);
    overlay::KeyboardNavigationState focus{overlay::KeyboardFocusRegion::Keys, 1, 4};
    overlay::KeyRepeatState navRepeat;
    int navDirection = -1;
    uint32_t lastButtons = 0;
    // The trigger only presses once it was released while pointing at the panel.
    bool armed = false;
    bool mouseDown = false;
    std::string echo;

    bool create(XrSession session, vulkan::Context &context);
    void place(XrSpace localSpace, XrSpace viewSpace, XrTime time, const XrCompositionLayerQuad *screen);
    void applyPointer(const InputSnapshot &input, ImGuiIO &io);
    void applyPad(const InputSnapshot &input, float dt);
    void navigate(overlay::NavigationDirection direction);
    void pressFocused();
    void press(const overlay::KeyboardKey &key);
    void type(const std::string &text, bool echoText);
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
    ImFontConfig font;
    font.SizePixels = kKeySize;
    io.FontDefault = io.Fonts->AddFontDefault(&font);
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.FrameRounding = 10.0f;
    style.WindowRounding = 16.0f;
    style.WindowPadding = {kMargin, kMargin};

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
    if (screen != nullptr) {
        // Under the 2D big screen and parallel to it, following it when it moves.
        const float width = std::clamp(screen->size.width * kScreenWidthShare, 0.8f, 1.4f);
        size = {width, width * kAspect};
        const XrVector3f offset = Rotate(screen->pose.orientation,
                                         {0.0f, -(screen->size.height + size.height) * 0.5f - kScreenGap, 0.0f});
        XrPosef pose{};
        pose.orientation = screen->pose.orientation;
        pose.position = {screen->pose.position.x + offset.x, screen->pose.position.y + offset.y,
                         screen->pose.position.z + offset.z};
        anchor = pose;
        return;
    }
    if (anchor) return;
    // Stereo: placed once in front of the head, turned only about its yaw.
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    constexpr XrSpaceLocationFlags kTracked = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if (XR_FAILED(xrLocateSpace(viewSpace, localSpace, time, &head)) || (head.locationFlags & kTracked) != kTracked) return;
    const XrQuaternionf &q = head.pose.orientation;
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float yaw = std::atan2(-fx, -fz);
    size = {kStereoWidth, kStereoWidth * kAspect};
    XrPosef pose{};
    pose.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    pose.position = {head.pose.position.x - std::sin(yaw) * kStereoAhead, head.pose.position.y - kStereoBelow,
                     head.pose.position.z - std::cos(yaw) * kStereoAhead};
    anchor = pose;
    LOGI("XR keyboard placed in front of the head at (%.2f, %.2f, %.2f)", pose.position.x, pose.position.y,
         pose.position.z);
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
    if (pressed & kButtonB) type(kBackspace, false);
    if (pressed & kButtonY) type(" ", true);
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
        case kKeyboardPadBackspace: type(kBackspace, false); break;
        case kKeyboardPadSpace: type(" ", true); break;
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
        type(key.glyph, true);
        if (layout.case_state == overlay::KeyboardCase::Shift) overlay::applyKeyboardCase(layout, overlay::KeyboardCase::Lower);
        break;
    case Action::Space: type(" ", true); break;
    case Action::Backspace: type(kBackspace, false); break;
    case Action::Enter: type(kEnter, false); break;
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
    {
        std::lock_guard<std::mutex> lock(gShared.mutex);
        gShared.requested = false;
        gShared.output += kClosed;
    }
    LOGI("XR keyboard closed from the panel");
}

void XrKeyboard::Impl::draw(float dt) {
    ImGuiIO &io = ImGui::GetIO();
    io.DeltaTime = std::max(dt, 1.0f / 240.0f);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(18, 24, 36, 240));
    ImGui::Begin("xr_keyboard", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleColor();
    const ImU32 accent = IM_COL32(107, 200, 255, 255);
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    const float innerWidth = kCanvasWidth - 2.0f * kMargin;

    // Top bar: what was typed, and Close.
    const float closeWidth = 200.0f;
    const ImVec2 echoMin{kMargin, kMargin};
    const ImVec2 echoMax{kMargin + innerWidth - closeWidth - kKeyGap, kMargin + kTopBarHeight - 18.0f};
    drawList->AddRectFilled(echoMin, echoMax, IM_COL32(8, 12, 20, 255), 10.0f);
    drawList->AddRect(echoMin, echoMax, IM_COL32(70, 90, 120, 255), 10.0f, 0, 2.0f);
    drawList->PushClipRect(echoMin, echoMax, true);
    const ImVec2 echoSize = ImGui::CalcTextSize(echo.c_str());
    const float echoX = std::min(echoMin.x + 16.0f, echoMax.x - 16.0f - echoSize.x);
    drawList->AddText({echoX, echoMin.y + (echoMax.y - echoMin.y - echoSize.y) * 0.5f}, IM_COL32(235, 242, 250, 255),
                      echo.c_str());
    drawList->PopClipRect();
    const bool closeFocused = focus.region != overlay::KeyboardFocusRegion::Keys;
    ImGui::SetCursorPos({echoMax.x + kKeyGap, kMargin});
    if (ImGui::Button("Close", {closeWidth, echoMax.y - echoMin.y})) close();
    if (closeFocused) drawList->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), accent, 10.0f, 0, 5.0f);

    // Keys.
    const float gridTop = kMargin + kTopBarHeight;
    const float gridHeight = kCanvasHeight - gridTop - kHintHeight - kMargin;
    const float cellWidth = innerWidth / std::max<int>(layout.columns, 1);
    const float cellHeight = gridHeight / std::max<int>(layout.rows, 1);
    const overlay::KeyboardKey *focused =
        focus.region == overlay::KeyboardFocusRegion::Keys ? layout.keyAt(focus.row, focus.column) : nullptr;
    std::optional<overlay::KeyboardKey> pressed;
    for (size_t index = 0; index < layout.keys.size(); ++index) {
        const overlay::KeyboardKey &key = layout.keys[index];
        if (key.action == overlay::KeyboardKeyAction::Blank) continue;
        const bool usable = Usable(key);
        const bool active = key.action == overlay::KeyboardKeyAction::Shift &&
                            layout.case_state != overlay::KeyboardCase::Lower;
        ImGui::SetCursorPos({kMargin + key.column * cellWidth, gridTop + key.row * cellHeight});
        ImGui::PushID(static_cast<int>(index));
        if (!usable) ImGui::BeginDisabled();
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(40, 110, 170, 255));
        const std::string &label = key.action == overlay::KeyboardKeyAction::Character ? key.glyph : key.label.text();
        if (ImGui::Button(label.c_str(), {key.column_span * cellWidth - kKeyGap, cellHeight - kKeyGap})) pressed = key;
        if (active) ImGui::PopStyleColor();
        if (!usable) ImGui::EndDisabled();
        if (focused == &key) drawList->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), accent, 10.0f, 0, 5.0f);
        ImGui::PopID();
    }
    if (pressed) press(*pressed);

    // Hint line.
    const char *hint = "Trigger: press   A: press   B: delete   Y: space   X: shift   Menu: close";
    ImFont *font = ImGui::GetFont();
    const ImVec2 hintSize = font->CalcTextSizeA(kHintSize, FLT_MAX, 0.0f, hint);
    drawList->AddText(font, kHintSize,
                      {(kCanvasWidth - hintSize.x) * 0.5f, kCanvasHeight - kMargin - kHintHeight * 0.5f - hintSize.y * 0.5f},
                      IM_COL32(150, 170, 195, 255), hint);

    // Where the ray points.
    if (io.MousePos.x > -1.0f) {
        ImGui::GetForegroundDrawList()->AddCircleFilled(io.MousePos, mouseDown ? 14.0f : 10.0f,
                                                        mouseDown ? accent : IM_COL32(255, 255, 255, 220));
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
    }
    return kb.layer.HasReleasedImage();
}

void XrKeyboard::fill(XrSpace localSpace, XrCompositionLayerQuad *quad) {
    Impl &kb = *impl_;
    kb.layer.FillQuadLayer(localSpace, false, *quad);
    quad->pose = *kb.anchor;
    quad->size = kb.size;
}

void XrKeyboard::destroy() { impl_->destroy(); }

spatial::xr::XrImguiVulkanLayer *XrKeyboard::imguiLayer() {
    return impl_->layer.IsCreated() ? &impl_->layer : nullptr;
}

}  // namespace xrimmersive
