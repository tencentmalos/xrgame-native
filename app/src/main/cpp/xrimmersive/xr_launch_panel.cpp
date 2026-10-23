#include "xr_launch_panel.h"

#include "xr_capture.h"
#include "xr_imgui_fonts.h"
#include "xr_imgui_style.h"
#include "xr_immersive.h"
#include "xr_vulkan.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "spatial/xr/XrImguiVulkanLayer.h"
#include "spatial/xr/XrMath.h"

#include <android/log.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <mutex>
#include <cstdlib>
#include <optional>
#include <vector>

#define LOG_TAG "xrimmersive_launch"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace xrimmersive {
namespace {

using Clock = std::chrono::steady_clock;

// 16:9 like the big screen, whose size and place the panel takes.
constexpr int kCanvasWidth = 1600;
constexpr int kCanvasHeight = 900;
constexpr float kMargin = 32.0f;
constexpr float kFontSize = 30.0f;
constexpr float kSmallSize = 22.0f;
constexpr float kTitleSize = 44.0f;
constexpr float kHeaderHeight = 128.0f;
constexpr float kLabelWidth = 236.0f;
// More choices than this drop down, as the 2D panel's do.
constexpr size_t kMaxPills = 3;
constexpr float kFooterHeight = 96.0f;
constexpr float kChoicePadX = 20.0f;
constexpr float kChoicePadY = 8.0f;
constexpr float kChoiceGap = 10.0f;
constexpr float kRowGap = 20.0f;
constexpr float kTriggerPress = 0.55f;
constexpr float kTriggerRelease = 0.35f;
constexpr float kScrollDeadzone = 0.25f;
// Bits of InputSnapshot::buttons (XrGamepadBridge's layout).
constexpr uint32_t kButtonA = 1u << 0;
constexpr uint32_t kButtonB = 1u << 1;


struct Shared {
    std::mutex mutex;
    std::string model;
    uint64_t version = 0;
    std::string events;
};
Shared gShared;

struct RestoreImGuiContext {
    ImGuiContext *previous = ImGui::GetCurrentContext();
    ~RestoreImGuiContext() { ImGui::SetCurrentContext(previous); }
};

struct Row {
    int section = 0;
    int index = 0;
    std::string label;
    int selected = -1;
    std::vector<std::string> choices;
    std::string disabled;  // '1' per disabled choice
    std::string note;
};

struct Model {
    std::string header, title, summary;
    std::string footer, cancel, start, advanced, ok;
    std::string view = "options";
    std::string progress;
    std::string messageTitle, message;
    bool advancedOpen = false;
    std::vector<Row> rows;
};

std::vector<std::string> Split(const std::string &text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t end = text.find(separator, start);
        parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return parts;
}

// Kotlin writes line breaks inside a field as U+001E.
std::string Lines(std::string text) {
    std::replace(text.begin(), text.end(), '\x1e', '\n');
    return text;
}

Model Parse(const std::string &text) {
    Model model;
    for (const std::string &line : Split(text, '\n')) {
        if (line.empty()) continue;
        const std::vector<std::string> f = Split(line, '\t');
        auto at = [&](size_t i) { return i < f.size() ? Lines(f[i]) : std::string(); };
        const std::string &kind = f[0];
        if (kind == "T") {
            model.header = at(1);
            model.title = at(2);
            model.summary = at(3);
        } else if (kind == "F") {
            model.footer = at(1);
            model.cancel = at(2);
            model.start = at(3);
            model.advanced = at(4);
            model.ok = at(5);
        } else if (kind == "V") {
            model.view = at(1);
        } else if (kind == "P") {
            model.progress = at(1);
        } else if (kind == "G") {
            model.messageTitle = at(1);
            model.message = at(2);
        } else if (kind == "A") {
            model.advancedOpen = at(1) == "1";
        } else if (kind == "R" && f.size() >= 8) {
            Row row;
            row.section = std::atoi(f[1].c_str());
            row.index = std::atoi(f[2].c_str());
            row.label = at(3);
            row.selected = std::atoi(f[4].c_str());
            row.choices = Split(f[5], '\x1f');
            row.disabled = f[6];
            row.note = at(7);
            model.rows.push_back(std::move(row));
        }
    }
    return model;
}

void Emit(const std::string &event) {
    std::lock_guard<std::mutex> lock(gShared.mutex);
    gShared.events += event;
    gShared.events += '\n';
}

}  // namespace

void SetLaunchPanelModel(std::string model) {
    std::lock_guard<std::mutex> lock(gShared.mutex);
    if (model == gShared.model) return;
    if (gShared.model.empty() != model.empty()) LOGI("launch panel %s", model.empty() ? "hidden" : "shown");
    gShared.model = std::move(model);
    ++gShared.version;
}

bool LaunchPanelShown() {
    std::lock_guard<std::mutex> lock(gShared.mutex);
    return !gShared.model.empty();
}

std::string DrainLaunchPanelEvents() {
    std::lock_guard<std::mutex> lock(gShared.mutex);
    std::string out;
    out.swap(gShared.events);
    return out;
}

struct XrLaunchPanel::Impl {
    ImGuiContext *imgui = nullptr;
    spatial::xr::XrImguiLayerConfig config;
    spatial::xr::XrImguiVulkanLayer layer;
    bool failed = false;
    bool shown = false;
    uint64_t version = 0;
    Model model;
    Clock::time_point lastFrame{};
    XrPosef anchor{{0, 0, 0, 1}, {0, 0, 0}};
    XrExtent2Df size{1.6f, 0.9f};
    uint32_t lastButtons = 0;
    bool armed = false;
    bool mouseDown = false;
    float spinner = 0.0f;
    float fade = 0.0f;
    ImGuiID loggedConflict = 0;

    bool create(XrSession session, vulkan::Context &context);
    void applyPointer(const InputSnapshot &input, ImGuiIO &io, float dt);
    void applyPad(const InputSnapshot &input);
    void draw(float dt);
    void drawHeader();
    void drawRows(int section, float width);
    void drawRow(const Row &row, float width);
    void drawOptions();
    void drawProgress();
    void drawMessage();
    void destroy();
};

bool XrLaunchPanel::Impl::create(XrSession session, vulkan::Context &context) {
    config.canvas_width = kCanvasWidth;
    config.canvas_height = kCanvasHeight;
    config.space = spatial::xr::XrImguiLayerSpace::Local;
    config.width_meters = size.width;
    config.height_meters = size.height;
    config.input_enabled = false;
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
    io.FontDefault = AddCjkFont(io.Fonts, kFontSize);
    // A local named `style` would hide the style namespace.
    ImGuiStyle &look = ImGui::GetStyle();
    style::ApplyPanelStyle(look);
    look.FontSizeBase = kFontSize;

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
    LOGI("launch panel layer created: %dx%d canvas", kCanvasWidth, kCanvasHeight);
    return true;
}

void XrLaunchPanel::Impl::applyPointer(const InputSnapshot &input, ImGuiIO &io, float dt) {
    std::optional<ImVec2> hit;
    float trigger = 0.0f;
    for (const int hand : {1, 0}) {
        if (!input.aimPoseValid[hand]) continue;
        const auto result = spatial::xr::math::RayCastQuadLayer(
            spatial::xr::math::MakeRayFromPose(input.aimPoses[hand]), anchor, size);
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
    // The trigger that pressed Play in the client may still be held when the panel appears.
    if (trigger < kTriggerRelease) armed = true;
    mouseDown = mouseDown ? trigger > kTriggerRelease : armed && trigger > kTriggerPress;
    io.AddMousePosEvent(hit->x, hit->y);
    io.AddMouseButtonEvent(0, mouseDown);
    // The right stick scrolls the options under the ray.
    if (std::fabs(input.rightY) > kScrollDeadzone) io.AddMouseWheelEvent(0.0f, input.rightY * dt * 6.0f);
}

void XrLaunchPanel::Impl::applyPad(const InputSnapshot &input) {
    const uint32_t pressed = input.buttons & ~lastButtons;
    lastButtons = input.buttons;
    if (model.view == "options") {
        if (pressed & kButtonA) Emit("start");
        if (pressed & kButtonB) Emit("cancel");
    } else if (model.view == "message") {
        if (pressed & (kButtonA | kButtonB)) Emit("ok");
    }
}

void XrLaunchPanel::Impl::drawHeader() {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const float rounding = ImGui::GetStyle().WindowRounding;
    // A blue band across the top, darkening downwards, with a faint light edge (shadPS4's card header).
    const ImVec2 bandMax{origin.x + kCanvasWidth, origin.y + kHeaderHeight};
    draw->AddRectFilled(origin, {bandMax.x, origin.y + 2.0f * rounding}, style::kHeaderTop, rounding,
                        ImDrawFlags_RoundCornersTop);
    draw->AddRectFilledMultiColor({origin.x, origin.y + rounding}, bandMax, style::kHeaderTop, style::kHeaderTop,
                                  style::kHeaderBottom, style::kHeaderBottom);
    draw->AddLine({origin.x, bandMax.y}, bandMax, IM_COL32(255, 255, 255, 40), 1.0f);

    ImGui::SetCursorPos({kMargin, 18.0f});
    ImGui::PushFont(nullptr, kSmallSize);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 225, 245, 255));
    ImGui::TextUnformatted((std::string(style::kPlay) + "  " + model.header).c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetCursorPos({kMargin, 50.0f});
    ImGui::PushFont(nullptr, kTitleSize);
    ImGui::TextUnformatted(model.title.c_str());
    ImGui::PopFont();
    const float titleMid = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
    if (!model.summary.empty()) {
        // How the game will show, as a pill on the right.
        const ImVec2 text = ImGui::CalcTextSize(model.summary.c_str());
        const ImVec2 max{origin.x + kCanvasWidth - kMargin, titleMid + text.y * 0.5f + 10.0f};
        const ImVec2 min{max.x - text.x - 40.0f, titleMid - text.y * 0.5f - 10.0f};
        draw->AddRectFilled(min, max, IM_COL32(255, 255, 255, 36), (max.y - min.y) * 0.5f);
        draw->AddRect(min, max, IM_COL32(255, 255, 255, 80), (max.y - min.y) * 0.5f, 0, 1.5f);
        draw->AddText({min.x + 20.0f, min.y + 10.0f}, style::kText, model.summary.c_str());
    }
    ImGui::SetCursorPos({kMargin, kHeaderHeight + 22.0f});
}

void XrLaunchPanel::Impl::drawRow(const Row &row, float width) {
    // Rows repeat the same choices (Off/On, say): each row is its own ID scope.
    ImGui::PushID(row.section * 1000 + row.index);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float right = origin.x + width;
    ImGui::PushStyleColor(ImGuiCol_Text, style::kTextDim);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + kLabelWidth - 14.0f);
    ImGui::TextUnformatted(row.label.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    float bottom = ImGui::GetItemRectMax().y;

    const float left = origin.x + kLabelWidth;
    float x = left;
    float y = origin.y - kChoicePadY;
    float height = 0.0f;
    if (row.choices.size() > kMaxPills) {
        // A drop-down: the selected choice, and the list on a click.
        const bool valid = row.selected >= 0 && row.selected < static_cast<int>(row.choices.size());
        const std::string preview = valid ? row.choices[row.selected] : std::string();
        float widest = 0.0f;
        for (const std::string &choice : row.choices) widest = std::max(widest, ImGui::CalcTextSize(choice.c_str()).x);
        ImGui::SetCursorScreenPos({left, y});
        ImGui::SetNextItemWidth(std::min(right - left, widest + 2.0f * kChoicePadX + ImGui::GetFrameHeight() + 40.0f));
        if (ImGui::BeginCombo("##choices", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (size_t i = 0; i < row.choices.size(); ++i) {
                const bool selected = static_cast<int>(i) == row.selected;
                const bool disabled = i < row.disabled.size() && row.disabled[i] == '1';
                const std::string label = (selected ? std::string(style::kCheck) + " " : std::string("    ")) + row.choices[i];
                if (disabled) ImGui::BeginDisabled();
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Selectable(label.c_str(), selected, 0, {0.0f, ImGui::GetFrameHeight()}) && !selected) {
                    Emit("select " + std::to_string(row.section) + " " + std::to_string(row.index) + " " + std::to_string(i));
                }
                if (selected) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
                if (disabled) ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        height = ImGui::GetItemRectSize().y;
    }
    for (size_t i = 0; row.choices.size() <= kMaxPills && i < row.choices.size(); ++i) {
        const bool selected = static_cast<int>(i) == row.selected;
        const bool disabled = i < row.disabled.size() && row.disabled[i] == '1';
        // The selected choice carries a check mark, as a ticked pill.
        const std::string label = selected ? std::string(style::kCheck) + " " + row.choices[i] : row.choices[i];
        const ImVec2 text = ImGui::CalcTextSize(label.c_str());
        const float w = text.x + 2.0f * kChoicePadX;
        height = text.y + 2.0f * kChoicePadY;
        if (x + w > right && x > left) {
            x = left;
            y += height + kChoiceGap;
        }
        ImGui::SetCursorScreenPos({x, y});
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, style::kSelected);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, style::Lighter(style::kSelected, 24));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 150));
        }
        if (disabled) ImGui::BeginDisabled();
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button(label.c_str(), {w, height}) && !selected) {
            Emit("select " + std::to_string(row.section) + " " + std::to_string(row.index) + " " + std::to_string(i));
        }
        ImGui::PopID();
        if (disabled) ImGui::EndDisabled();
        if (selected) ImGui::PopStyleColor(3);
        x += w + kChoiceGap;
    }
    bottom = std::max(bottom, y + height);
    if (!row.note.empty()) {
        ImGui::SetCursorScreenPos({left, bottom + 6.0f});
        ImGui::PushFont(nullptr, kSmallSize);
        ImGui::PushStyleColor(ImGuiCol_Text, style::kTextDim);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (right - left));
        ImGui::TextUnformatted(row.note.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
        bottom = ImGui::GetItemRectMax().y;
    }
    // A hairline between rows.
    const float line = bottom + kRowGap * 0.5f;
    draw->AddLine({origin.x, line}, {right, line}, style::kSeparator, 1.0f);
    ImGui::SetCursorScreenPos({origin.x, bottom + kRowGap});
    ImGui::Dummy({0.0f, 0.0f});
    ImGui::PopID();
}

void XrLaunchPanel::Impl::drawRows(int section, float width) {
    std::vector<const Row *> rows;
    for (const Row &row : model.rows) {
        if (row.section == section) rows.push_back(&row);
    }
    if (rows.empty()) return;
    // Two columns, filled top to bottom on the left first, as the 2D panel does.
    const size_t split = (rows.size() + 1) / 2;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {18.0f, 0.0f});
    if (ImGui::BeginTable(section == 0 ? "main" : "extra", 2, ImGuiTableFlags_SizingStretchSame, {width, 0.0f})) {
        ImGui::TableNextColumn();
        const float column = ImGui::GetContentRegionAvail().x;
        for (size_t i = 0; i < split; ++i) drawRow(*rows[i], column);
        ImGui::TableNextColumn();
        for (size_t i = split; i < rows.size(); ++i) drawRow(*rows[i], ImGui::GetContentRegionAvail().x);
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

void XrLaunchPanel::Impl::drawOptions() {
    const float listHeight = ImGui::GetContentRegionAvail().y - kFooterHeight;
    ImGui::BeginChild("options", {0.0f, listHeight}, ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    // Room above the first row's choices, which sit a little above their label.
    ImGui::Dummy({0.0f, kChoicePadY + 4.0f});
    const float width = ImGui::GetContentRegionAvail().x;
    drawRows(0, width);
    if (std::any_of(model.rows.begin(), model.rows.end(), [](const Row &row) { return row.section == 1; })) {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, style::kAccentLight);
        const std::string label = std::string(style::kGear) + "  " + model.advanced + "  " +
                                  (model.advancedOpen ? style::kCollapse : style::kExpand);
        if (ImGui::Button(label.c_str())) Emit("advanced");
        ImGui::PopStyleColor(3);
        if (model.advancedOpen) drawRows(1, width);
    }
    ImGui::EndChild();

    // The bottom bar: the hint, then Cancel (B) and Start (A) with their controller glyphs.
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const float barTop = kCanvasHeight - kFooterHeight;
    draw->AddLine({origin.x + kMargin, origin.y + barTop}, {origin.x + kCanvasWidth - kMargin, origin.y + barTop},
                  style::kSeparator, 1.0f);
    const float buttonHeight = kFooterHeight - 30.0f;
    const float buttonTop = barTop + 15.0f;
    const float startWidth = 260.0f;
    const float cancelWidth = 220.0f;
    ImGui::SetCursorPos({kMargin, buttonTop + (buttonHeight - kSmallSize) * 0.5f});
    ImGui::PushFont(nullptr, kSmallSize);
    ImGui::PushStyleColor(ImGuiCol_Text, style::kTextDim);
    ImGui::PushTextWrapPos(kCanvasWidth - kMargin - startWidth - cancelWidth - 40.0f);
    ImGui::TextUnformatted(model.footer.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetCursorPos({kCanvasWidth - kMargin - startWidth - 16.0f - cancelWidth, buttonTop});
    if (style::IconButton("##cancel", style::kClose, model.cancel.c_str(), {cancelWidth, buttonHeight}, style::kButton,
                          "B", style::kBadgeB)) {
        Emit("cancel");
    }
    ImGui::SameLine(0.0f, 16.0f);
    if (style::IconButton("##start", style::kPlay, model.start.c_str(), {startWidth, buttonHeight}, style::kSelected,
                          "A", style::kBadgeA)) {
        Emit("start");
    }
}

void XrLaunchPanel::Impl::drawProgress() {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 center{origin.x + kCanvasWidth * 0.5f, origin.y + kHeaderHeight + (kCanvasHeight - kHeaderHeight) * 0.42f};
    // A faint ring with a turning arc on it.
    const float radius = 40.0f;
    draw->AddCircle(center, radius, IM_COL32(255, 255, 255, 30), 48, 7.0f);
    draw->PathArcTo(center, radius, spinner, spinner + 2.2f, 32);
    draw->PathStroke(style::kAccentLight, 0, 7.0f);
    const ImVec2 size = ImGui::CalcTextSize(model.progress.c_str(), nullptr, false, kCanvasWidth * 0.8f);
    ImGui::SetCursorPos({(kCanvasWidth - size.x) * 0.5f, center.y - origin.y + radius + 36.0f});
    ImGui::PushTextWrapPos((kCanvasWidth + size.x) * 0.5f + 1.0f);
    ImGui::TextUnformatted(model.progress.c_str());
    ImGui::PopTextWrapPos();
}

void XrLaunchPanel::Impl::drawMessage() {
    const float top = kHeaderHeight + 44.0f;
    // A warning sign beside the title.
    ImGui::SetCursorPos({kMargin, top - 8.0f});
    ImGui::PushFont(nullptr, 64.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, style::kWarningColor);
    ImGui::TextUnformatted(style::kWarning);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetCursorPos({kMargin + 92.0f, top});
    ImGui::PushFont(nullptr, kTitleSize * 0.85f);
    ImGui::TextUnformatted(model.messageTitle.c_str());
    ImGui::PopFont();
    ImGui::SetCursorPos({kMargin + 92.0f, ImGui::GetCursorPosY() + 12.0f});
    ImGui::PushStyleColor(ImGuiCol_Text, style::kTextDim);
    ImGui::PushTextWrapPos(kCanvasWidth - kMargin);
    ImGui::TextUnformatted(model.message.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    const float buttonHeight = kFooterHeight - 30.0f;
    const float width = 240.0f;
    ImGui::SetCursorPos({kCanvasWidth - kMargin - width, kCanvasHeight - kFooterHeight + 15.0f});
    if (style::IconButton("##ok", style::kCheck, model.ok.c_str(), {width, buttonHeight}, style::kSelected, "A",
                          style::kBadgeA)) {
        Emit("ok");
    }
}

void XrLaunchPanel::Impl::draw(float dt) {
    ImGuiIO &io = ImGui::GetIO();
    io.DeltaTime = std::max(dt, 1.0f / 240.0f);
    spinner = std::fmod(spinner + dt * 5.0f, 6.2831853f);
    // Fades in when it appears, as shadPS4's dialogs do.
    fade = std::min(1.0f, fade + dt * 6.0f);
    ImGui::GetStyle().Alpha = fade;
    ImGui::NewFrame();
    // ImGui notices a duplicate ID only on the item under the pointer; log each one it finds.
    const ImGuiID conflict = ImGui::GetCurrentContext()->DebugDrawIdConflictsId;
    if (conflict != 0 && conflict != loggedConflict) LOGW("ImGui ID conflict on the pointed item (0x%08x)", conflict);
    loggedConflict = conflict;
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("launch_panel", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav);
    drawHeader();
    if (model.view == "progress") {
        drawProgress();
    } else if (model.view == "message") {
        drawMessage();
    } else {
        drawOptions();
    }
    if (io.MousePos.x > -1.0f) {
        ImDrawList *top = ImGui::GetForegroundDrawList();
        top->AddCircleFilled(io.MousePos, mouseDown ? 13.0f : 10.0f, mouseDown ? style::kAccentLight : IM_COL32(255, 255, 255, 230));
        top->AddCircle(io.MousePos, mouseDown ? 13.0f : 10.0f, IM_COL32(0, 0, 0, 120), 24, 2.0f);
    }
    ImGui::End();
    ImGui::Render();
}

void XrLaunchPanel::Impl::destroy() {
    if (imgui != nullptr) {
        RestoreImGuiContext restore;
        ImGui::SetCurrentContext(imgui);
        layer.Destroy();
        ImGui::DestroyContext(imgui);
        imgui = nullptr;
    }
}

XrLaunchPanel::XrLaunchPanel() : impl_(std::make_unique<Impl>()) {}

XrLaunchPanel::~XrLaunchPanel() { destroy(); }

bool XrLaunchPanel::update(XrSession session, vulkan::Context &context, const InputSnapshot &input,
                           const XrCompositionLayerQuad &screen) {
    Impl &panel = *impl_;
    std::string text;
    uint64_t version;
    {
        std::lock_guard<std::mutex> lock(gShared.mutex);
        text = gShared.model;
        version = gShared.version;
    }
    if (text.empty()) {
        panel.shown = false;
        return false;
    }
    if (panel.failed || context.lost()) return false;
    if (panel.imgui == nullptr) {
        if (!panel.create(session, context)) {
            panel.failed = true;
            LOGE("launch panel layer creation failed; it stays off for this session");
            panel.destroy();
            return false;
        }
    }
    if (version != panel.version) {
        panel.version = version;
        panel.model = Parse(text);
    }
    if (!panel.shown) {
        panel.shown = true;
        // Buttons already held when the panel opens (the client's A on Play) press nothing.
        panel.lastButtons = input.buttons;
        panel.armed = false;
        panel.mouseDown = false;
        panel.fade = 0.0f;
    }
    panel.anchor = screen.pose;
    panel.size = screen.size;

    const auto now = Clock::now();
    const float dt =
        panel.lastFrame == Clock::time_point{} ? 1.0f / 72.0f : std::chrono::duration<float>(now - panel.lastFrame).count();
    panel.lastFrame = now;
    RestoreImGuiContext restore;
    ImGui::SetCurrentContext(panel.imgui);
    panel.applyPointer(input, ImGui::GetIO(), dt);
    panel.applyPad(input);
    panel.draw(dt);
    spatial::xr::XrImguiRenderedLayer rendered{"launch_panel", panel.config, panel.imgui, ImGui::GetDrawData(),
                                               ImGui::GetIO().Fonts};
    panel.layer.Render(rendered, true);
    return panel.layer.HasReleasedImage();
}

void XrLaunchPanel::fill(XrSpace localSpace, XrCompositionLayerQuad *quad) {
    Impl &panel = *impl_;
    panel.layer.FillQuadLayer(localSpace, false, *quad);
    quad->pose = panel.anchor;
    quad->size = panel.size;
}

void XrLaunchPanel::destroy() { impl_->destroy(); }

spatial::xr::XrImguiVulkanLayer *XrLaunchPanel::imguiLayer() {
    return impl_->layer.IsCreated() ? &impl_->layer : nullptr;
}

}  // namespace xrimmersive
