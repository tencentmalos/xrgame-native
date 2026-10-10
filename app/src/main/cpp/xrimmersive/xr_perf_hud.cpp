#include "xr_perf_hud.h"

#include "xr_vulkan.h"

// xrimmersive enables the OpenGL ES platform structs of openxr_platform.h.
#include <EGL/egl.h>

#include "imgui.h"
#include "spatial/perf/PerfMetrics.hpp"
#include "spatial/platform/android/JniHelper.h"
#include "spatial/xr/XrImguiVulkanLayer.h"

#include <android/log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>

#define LOG_TAG "xrimmersive_hud"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace xrimmersive {
namespace {

using Clock = std::chrono::steady_clock;

// 4 x 2 cells of 384 x 96 px on a 1.6 x 0.2 m quad. Its centre sits 2.5 m ahead, 0.9 m left and
// 1.0 m up from the head (the top-left corner where shadPS4's head-locked panel has it), measured
// in the head's yaw frame, so pitch and roll at placement do not tilt the panel away.
constexpr int kColumns = 4;
constexpr int kCellWidth = 384;
constexpr int kCellHeight = 96;
constexpr int kCanvasWidth = kColumns * kCellWidth;
constexpr int kCanvasHeight = 2 * kCellHeight;
constexpr float kWidthMeters = 1.6f;
constexpr float kHeightMeters = kWidthMeters * kCanvasHeight / kCanvasWidth;
constexpr float kLabelSize = 26.0f;
constexpr float kValueSize = 36.0f;
constexpr XrVector3f kAnchorOffset{-0.9f, 1.0f, -2.5f};
constexpr auto kUpdateInterval = std::chrono::milliseconds(250);
constexpr auto kStaleAfter = std::chrono::seconds(2);

std::atomic<bool> gVisible{false};

struct Status {
    bool created = false;
    std::string error;
    uint64_t updates = 0;
    uint64_t reused = 0;
    int64_t lastUpdateUs = 0;
    int64_t maxUpdateUs = 0;
    std::optional<double> gameFps;
    std::optional<double> xrFps;
    std::optional<XrVector3f> anchor;
};
std::mutex gStatusMutex;
Status gStatus;

void SetError(const char *error) {
    std::lock_guard<std::mutex> lock(gStatusMutex);
    gStatus.error = error;
}

struct RestoreImGuiContext {
    ImGuiContext *previous = ImGui::GetCurrentContext();
    ~RestoreImGuiContext() { ImGui::SetCurrentContext(previous); }
};

// The local filesystem, minus two Swan problems. SELinux logs a denial for every read of a node the
// app may not read (kgsl gpuclk/clock_mhz, devfreq: several a second), so a /sys node that failed
// once is not read again. Thermal zones named after a trip point report the threshold
// (cpu-hw-trip-* is always 105 C), not a temperature, so their type is hidden and they are skipped.
class HudFileSource final : public spatial::perf::FileSource {
public:
    std::optional<std::string> read(const std::string &path) const override {
        if (refused(path)) return std::nullopt;
        auto text = local_->read(path);
        if (!text) {
            refuse(path);
            return std::nullopt;
        }
        if (path.ends_with("/type") && text->find("trip") != std::string::npos) return std::nullopt;
        return text;
    }
    std::vector<std::string> list(const std::string &directory) const override {
        if (refused(directory)) return {};
        auto entries = local_->list(directory);
        if (entries.empty()) refuse(directory);
        return entries;
    }

private:
    bool refused(const std::string &path) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return refused_.count(path) != 0;
    }
    // Processes come and go under /proc; only fixed sysfs nodes are remembered.
    void refuse(const std::string &path) const {
        if (!path.starts_with("/sys/")) return;
        std::lock_guard<std::mutex> lock(mutex_);
        refused_.insert(path);
    }
    std::shared_ptr<const spatial::perf::FileSource> local_ = spatial::perf::localFileSource();
    mutable std::mutex mutex_;
    mutable std::unordered_set<std::string> refused_;
};

// DeviceMetricsSampler's one-second thread around a reader that keeps Android's BatteryManager
// source with the file source above (the sampler only takes a file source, and a custom one drops
// the platform battery).
class HudSampler {
public:
    HudSampler() : reader_(std::make_shared<HudFileSource>(), {}, spatial::perf::platformBatterySource()) {
        thread_ = std::thread([this] { run(); });
    }
    ~HudSampler() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    std::optional<spatial::perf::DeviceMetrics> latest() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return latest_;
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            lock.unlock();
            auto sample = reader_.sample(Clock::now());
            lock.lock();
            latest_ = std::move(sample);
            wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stop_; });
        }
    }
    spatial::perf::DeviceMetricsReader reader_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<spatial::perf::DeviceMetrics> latest_;
    bool stop_ = false;
    std::thread thread_;
};

std::optional<double> Fresh(const spatial::perf::FrameRateCounter &counter, Clock::time_point now) {
    const auto published = counter.publishedAt();
    if (!published || now - *published > kStaleAfter) return std::nullopt;
    return counter.fps();
}

std::string Format(const char *format, double a, double b = 0.0) {
    char text[64];
    std::snprintf(text, sizeof(text), format, a, b);
    return text;
}

// Panel pose in the space `head` is located in: kAnchorOffset in the head's yaw frame, the quad's
// front (+Z) turned towards the head.
XrPosef PlacePanel(const XrPosef &head) {
    const XrQuaternionf &q = head.orientation;
    // Head forward is the rotated -Z axis.
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float yaw = std::atan2(-fx, -fz);
    const float s = std::sin(yaw);
    const float c = std::cos(yaw);
    const XrVector3f &o = kAnchorOffset;
    XrPosef pose{};
    pose.position = {head.position.x + c * o.x + s * o.z, head.position.y + o.y,
                     head.position.z - s * o.x + c * o.z};
    float dx = head.position.x - pose.position.x;
    float dy = head.position.y - pose.position.y;
    float dz = head.position.z - pose.position.z;
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    dx /= length;
    dy /= length;
    dz /= length;
    // Yaw a about +Y, then pitch b about +X: +Z maps to (sin a cos b, -sin b, cos a cos b).
    const float a = std::atan2(dx, dz);
    const float b = -std::asin(std::clamp(dy, -1.0f, 1.0f));
    const float ca = std::cos(a * 0.5f);
    const float sa = std::sin(a * 0.5f);
    const float cb = std::cos(b * 0.5f);
    const float sb = std::sin(b * 0.5f);
    pose.orientation = {ca * sb, sa * cb, -sa * sb, ca * cb};
    return pose;
}

}  // namespace

void SetPerfHudVisible(bool visible) {
    if (gVisible.exchange(visible) != visible) LOGI("Performance HUD %s", visible ? "shown" : "hidden");
}

bool PerfHudVisible() { return gVisible.load(); }

std::string PerfHudStatusJson() {
    std::lock_guard<std::mutex> lock(gStatusMutex);
    auto number = [](const std::optional<double> &value) {
        return value ? Format("%.1f", *value) : std::string("null");
    };
    const std::string anchor = gStatus.anchor ? Format("[%.2f,%.2f,", gStatus.anchor->x, gStatus.anchor->y) +
                                                     Format("%.2f]", gStatus.anchor->z)
                                              : std::string("null");
    char text[640];
    std::snprintf(text, sizeof(text),
                  "{\"visible\":%s,\"created\":%s,\"updates\":%llu,\"reused\":%llu,\"lastUpdateUs\":%lld,"
                  "\"maxUpdateUs\":%lld,\"gameFps\":%s,\"xrFps\":%s,\"anchorLocal\":%s,\"error\":\"%s\"}",
                  gVisible.load() ? "true" : "false", gStatus.created ? "true" : "false",
                  static_cast<unsigned long long>(gStatus.updates), static_cast<unsigned long long>(gStatus.reused),
                  static_cast<long long>(gStatus.lastUpdateUs), static_cast<long long>(gStatus.maxUpdateUs),
                  number(gStatus.gameFps).c_str(), number(gStatus.xrFps).c_str(), anchor.c_str(),
                  gStatus.error.c_str());
    return text;
}

void InitPerfHudJni(JNIEnv *env, jobject context) {
    if (!spatial::platform::JniHelper::initJniEnviroment(env, context)) {
        LOGE("Performance HUD: no JNI context, battery falls back to sysfs");
    }
}

struct PerfHud::Impl {
    ImGuiContext *imgui = nullptr;
    spatial::xr::XrImguiLayerConfig config;
    spatial::xr::XrImguiVulkanLayer layer;
    bool failed = false;
    bool baked = false;
    spatial::perf::FrameRateCounter gameFps{std::chrono::milliseconds(1000)};
    spatial::perf::FrameRateCounter xrFps{std::chrono::milliseconds(500)};
    std::unique_ptr<HudSampler> device;
    Clock::time_point lastUpdate{};
    // World pose of the panel in LOCAL space; empty until the next fill places it.
    std::optional<XrPosef> anchor;

    bool create(XrSession session, vulkan::Context &context);
    void draw(const PerfHudFrame &frame, Clock::time_point now);
    void destroy();
};

bool PerfHud::Impl::create(XrSession session, vulkan::Context &context) {
    config.canvas_width = kCanvasWidth;
    config.canvas_height = kCanvasHeight;
    config.space = spatial::xr::XrImguiLayerSpace::Local;
    config.width_meters = kWidthMeters;
    config.height_meters = kHeightMeters;
    config.input_enabled = false;

    RestoreImGuiContext restore;
    imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
    io.DisplaySize = {static_cast<float>(kCanvasWidth), static_cast<float>(kCanvasHeight)};
    ImFontConfig font;
    font.SizePixels = kValueSize;
    io.FontDefault = io.Fonts->AddFontDefault(&font);
    ImGui::StyleColorsDark();

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
    LOGI("Performance HUD layer created: %dx%d canvas, %.2fx%.2f m, world-locked in LOCAL space", kCanvasWidth, kCanvasHeight,
         kWidthMeters, kHeightMeters);
    return true;
}

void PerfHud::Impl::draw(const PerfHudFrame &frame, Clock::time_point now) {
    ImGuiIO &io = ImGui::GetIO();
    io.DeltaTime = lastUpdate == Clock::time_point{} ? 0.25f : std::chrono::duration<float>(now - lastUpdate).count();
    ImGui::NewFrame();
    ImFont *font = io.FontDefault;
    if (!baked) {
        // Every glyph the panel can show, so no new digit uploads font pixels mid-game.
        for (const float size : {kLabelSize, kValueSize}) {
            ImFontBaked *glyphs = font->GetFontBaked(size);
            for (ImWchar c = 32; c < 127; ++c) glyphs->FindGlyph(c);
            glyphs->FindGlyph(0xB0);
        }
        baked = true;
    }

    const auto device_sample = device ? device->latest() : std::nullopt;
    const bool deviceFresh = device_sample && now - device_sample->sampled_at < kStaleAfter;
    const auto game = Fresh(gameFps, now);
    const auto xr = Fresh(xrFps, now);
    {
        std::lock_guard<std::mutex> lock(gStatusMutex);
        gStatus.gameFps = game;
        gStatus.xrFps = xr;
    }

    const char *dash = "--";
    std::array<std::string, 8> values;
    // In stereo a game that stopped presenting is at 0 fps, not unknown.
    values[0] = game ? Format("%.1f", *game) : frame.sourceWidth > 0 ? std::string("0.0") : dash;
    values[1] = xr ? (frame.displayHz > 0.0f ? Format("%.1f / %.0f", *xr, frame.displayHz) : Format("%.1f", *xr)) : dash;
    values[2] = values[3] = values[4] = values[5] = values[6] = dash;
    if (deviceFresh) {
        const spatial::perf::DeviceMetrics &m = *device_sample;
        if (m.cpu.usage_percent) {
            values[2] = m.cpu.frequency_hz ? Format("%.0f%%  %.1f GHz", *m.cpu.usage_percent, *m.cpu.frequency_hz / 1e9)
                                           : Format("%.0f%%", *m.cpu.usage_percent);
        }
        if (m.gpu.usage_percent) {
            values[3] = m.gpu.frequency_hz ? Format("%.0f%%  %.0f MHz", *m.gpu.usage_percent, *m.gpu.frequency_hz / 1e6)
                                           : Format("%.0f%%", *m.gpu.usage_percent);
        }
        if (m.memory_used_bytes && m.memory_total_bytes) {
            values[4] = Format("%.1f / %.1f GB", *m.memory_used_bytes / 1073741824.0, *m.memory_total_bytes / 1073741824.0);
        }
        if (m.battery.level_percent) {
            values[5] = m.battery.charging ? Format("%.0f%%  charging", *m.battery.level_percent)
                        : m.battery.power_watts ? Format("%.0f%%  %.1f W", *m.battery.level_percent, *m.battery.power_watts)
                                                : Format("%.0f%%", *m.battery.level_percent);
        }
        if (m.cpu_temperature_celsius || m.gpu_temperature_celsius) {
            auto temperature = [](const std::optional<float> &value) {
                return value ? Format("%.0f", *value) : std::string("--");
            };
            values[6] = temperature(m.cpu_temperature_celsius) + " / " + temperature(m.gpu_temperature_celsius) + " \xC2\xB0" "C";
        }
    }
    static constexpr std::array<const char *, 3> kFilters{"", " FSR1", " SGSR"};
    values[7] = frame.sourceWidth == 0 ? std::string(dash)
                : Format("%.0fx%.0f", frame.sourceWidth, frame.sourceHeight) +
                      (frame.reconstructing ? kFilters[std::min<uint32_t>(frame.filter, 2)] : "");
    static constexpr std::array<const char *, 8> kLabels{"GAME FPS", "XR FPS / HZ", "CPU", "GPU",
                                                         "MEMORY", "BATTERY", "TEMP CPU / GPU", "GAME EYE"};

    const ImU32 accent = IM_COL32(107, 200, 255, 255);
    ImDrawList *drawList = ImGui::GetBackgroundDrawList();
    drawList->AddRectFilled({0.0f, 0.0f}, io.DisplaySize, IM_COL32(10, 17, 28, 235), 14.0f);
    drawList->AddRect({1.0f, 1.0f}, {io.DisplaySize.x - 1.0f, io.DisplaySize.y - 1.0f}, accent, 14.0f, 0, 2.0f);
    for (int i = 0; i < 8; ++i) {
        const float x = 20.0f + static_cast<float>((i % kColumns) * kCellWidth);
        const float y = 8.0f + static_cast<float>((i / kColumns) * kCellHeight);
        drawList->PushClipRect({x, y}, {x + kCellWidth - 28.0f, y + kCellHeight}, true);
        drawList->AddText(font, kLabelSize, {x, y}, accent, kLabels[i]);
        drawList->AddText(font, kValueSize, {x, y + 36.0f}, IM_COL32(235, 242, 250, 255), values[i].c_str());
        drawList->PopClipRect();
    }
    ImGui::Render();
}

void PerfHud::Impl::destroy() {
    device.reset();
    anchor.reset();
    if (imgui != nullptr) {
        RestoreImGuiContext restore;
        ImGui::SetCurrentContext(imgui);
        layer.Destroy();
        ImGui::DestroyContext(imgui);
        imgui = nullptr;
    }
    baked = false;
    std::lock_guard<std::mutex> lock(gStatusMutex);
    gStatus.created = false;
}

PerfHud::PerfHud() : impl_(std::make_unique<Impl>()) {}

PerfHud::~PerfHud() { destroy(); }

void PerfHud::noteXrFrame(std::chrono::steady_clock::time_point now) { impl_->xrFps.onFrame(now); }

void PerfHud::noteGameFrame(std::chrono::steady_clock::time_point now) { impl_->gameFps.onFrame(now); }

bool PerfHud::update(XrSession session, vulkan::Context &context, const PerfHudFrame &frame) {
    Impl &hud = *impl_;
    if (!gVisible.load()) {
        // The sampler thread reads sysfs every second; it only runs while the panel is shown.
        hud.device.reset();
        // Shown again, the panel is placed in front of the head anew.
        hud.anchor.reset();
        return false;
    }
    if (hud.failed || context.lost()) return false;
    if (hud.imgui == nullptr) {
        if (!hud.create(session, context)) {
            hud.failed = true;
            SetError("layer_create_failed");
            LOGE("Performance HUD layer creation failed; the HUD stays off for this session");
            hud.destroy();
            return false;
        }
        std::lock_guard<std::mutex> lock(gStatusMutex);
        gStatus = {};
        gStatus.created = true;
    }
    if (!hud.device) {
        hud.device = std::make_unique<HudSampler>();
    }
    const auto now = Clock::now();
    if (now - hud.lastUpdate >= kUpdateInterval) {
        RestoreImGuiContext restore;
        ImGui::SetCurrentContext(hud.imgui);
        hud.draw(frame, now);
        hud.lastUpdate = now;
        spatial::xr::XrImguiRenderedLayer rendered{"perf_hud", hud.config, hud.imgui, ImGui::GetDrawData(),
                                                   ImGui::GetIO().Fonts};
        const bool ok = hud.layer.Render(rendered, true);
        const int64_t micros = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - now).count();
        std::lock_guard<std::mutex> lock(gStatusMutex);
        if (!ok && !hud.layer.LastRenderReused()) {
            gStatus.error = "render_failed";
        } else if (hud.layer.LastRenderReused()) {
            ++gStatus.reused;
        } else {
            ++gStatus.updates;
        }
        gStatus.lastUpdateUs = micros;
        gStatus.maxUpdateUs = std::max(gStatus.maxUpdateUs, micros);
    }
    return hud.layer.HasReleasedImage();
}

bool PerfHud::fill(XrSpace localSpace, XrSpace viewSpace, XrTime time, XrCompositionLayerQuad *quad) {
    Impl &hud = *impl_;
    if (!hud.anchor) {
        // Once per placement, not per frame; xrLocateSpace does not wait for the GPU.
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags kTracked =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_FAILED(xrLocateSpace(viewSpace, localSpace, time, &head)) ||
            (head.locationFlags & kTracked) != kTracked) {
            return false;
        }
        hud.anchor = PlacePanel(head.pose);
        const XrVector3f &p = hud.anchor->position;
        LOGI("Performance HUD placed in LOCAL space at (%.2f, %.2f, %.2f)", p.x, p.y, p.z);
        std::lock_guard<std::mutex> lock(gStatusMutex);
        gStatus.anchor = p;
    }
    hud.layer.FillQuadLayer(localSpace, false, *quad);
    quad->pose = *hud.anchor;
    return true;
}

void PerfHud::reanchor() { impl_->anchor.reset(); }

void PerfHud::destroy() { impl_->destroy(); }

}  // namespace xrimmersive
