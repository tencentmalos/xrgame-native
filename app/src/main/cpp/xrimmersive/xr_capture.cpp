// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from shadPS4's capture_recorder and openxr/xr_capture (GPL-2.0-or-later).
#include "xr_capture.h"

#include <android/log.h>

#include <string>
#include <vector>

#if XRGAME_CAPTURE
// openxr_platform.h (through the Foundation layer) needs the JNI and EGL types first.
#include <EGL/egl.h>
#include <jni.h>

#include "scrcpy_capture/encoder_session.h"
#include "scrcpy_capture/snapshot_session.h"
#include "spatial/xr/XrImguiVulkanLayer.h"

#include <android/bitmap.h>
#include <android/data_space.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#endif

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "xrimmersive", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "xrimmersive", __VA_ARGS__)

namespace xrimmersive::capture {

#if !XRGAME_CAPTURE

bool Available() { return false; }

std::string HandleCommand(const std::string &, const std::vector<std::string> &, const std::string &) {
    return "status: unavailable\nerror: this build has no scrcpy capture SDK\n";
}

struct XrCapture::Impl {};
XrCapture::XrCapture(vulkan::Context &) {}
XrCapture::~XrCapture() = default;
bool XrCapture::begin(uint32_t, uint32_t, int64_t) { return false; }
bool XrCapture::active() const { return false; }
void XrCapture::copyScreen(VkCommandBuffer, VkImage, VkImageView, VkFormat, uint32_t, uint32_t) {}
void XrCapture::copyProjection(VkCommandBuffer, VkImage, const std::array<VkImageView, 2> &, VkFormat, uint32_t,
                               uint32_t) {}
void XrCapture::attach(Source, spatial::xr::XrImguiVulkanLayer *) {}
void XrCapture::note(const XrCompositionLayerBaseHeader *, Source) {}
void XrCapture::compose(XrSession, XrSpace, XrTime) {}

#else

namespace {

#include "xr_vulkan_shaders.inc"

using scrcpy::capture::EncoderConfig;
using scrcpy::capture::EncoderSession;
using scrcpy::capture::EncoderStats;
using scrcpy::capture::EyeSpan;
using scrcpy::capture::SnapshotRequest;
using scrcpy::capture::SnapshotSession;

constexpr int kCanvas = 0;
constexpr int kXr = 1;
constexpr uint32_t kSourceCount = 4;
constexpr uint32_t kMaxLayers = 6;

uint64_t NowNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// What stills and recordings show (capture_source); any thread.
std::atomic<int> gSelected{kCanvas};
// The running session's generation, 0 without one; stills belong to one generation.
std::atomic<uint64_t> gGeneration{0};
std::atomic<uint64_t> gLastGeneration{0};

// Recording requests (capture_video) and what the session reports back.
struct Control {
    std::mutex mutex;
    std::atomic<uint64_t> revision{0};
    bool wanted = false;
    bool live = false;
    std::string token;
    std::string eye = "both";
    std::string directory;
    std::string layout;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string phase = "idle";
    std::string path;
    std::string error;
    uint64_t submitted = 0;
    uint64_t skipped = 0;
    // Why encoder frames were skipped: no free encoder image, or the acquire semaphore's previous
    // submission had not completed yet.
    uint64_t skippedBusy = 0;
    uint64_t skippedPending = 0;
    // Submission to completion of the composes that fed the encoder.
    uint64_t composes = 0;
    uint64_t composeNsTotal = 0;
    uint64_t composeNsMax = 0;
    bool legacy = false;
    uint32_t images = 0;
    bool realtime = false;
    // capture_config, as scrcpy's options: 0 keeps the default. They apply to recordings and live
    // streams started afterwards; stills stay lossless at the source size.
    uint32_t videoBitRate = 0;   // bits per second (default 12M)
    uint32_t maxSize = 0;        // longest side of the encoded picture (default: the source's)
    uint32_t maxFps = 0;         // encoded frames per second (default: half the XR rate)
    uint32_t iFrameInterval = 0; // seconds between key frames (default 2)
    uint32_t encodedFps = 0;     // the cap in effect for the running recording
    EncoderStats encoder{};
};

Control &State() {
    static Control state;
    return state;
}

SnapshotSession &Shots() {
    static SnapshotSession session;
    return session;
}

const char *SourceKey(int source) { return source == kXr ? "xr" : "canvas"; }
const char *SourceName(int source) { return source == kXr ? "steampsp.xr_composite" : "steampsp.canvas"; }
const char *ViewName(int source) { return source == kXr ? "xr_eyes" : "canvas"; }

bool EnsureDirectory(const std::string &directory) {
    return !directory.empty() && (mkdir(directory.c_str(), 0700) == 0 || errno == EEXIST);
}

std::string SourceCommand(const std::vector<std::string> &args) {
    if (args.size() > 1 || (args.size() == 1 && args[0] != "canvas" && args[0] != "xr" && args[0] != "status")) {
        return "status: bad-arguments\nusage: capture_source [canvas|xr|status]\n";
    }
    if (args.size() == 1 && args[0] != "status") {
        const int wanted = args[0] == "xr" ? kXr : kCanvas;
        if (wanted != gSelected.load()) {
            auto &state = State();
            std::lock_guard<std::mutex> lock(state.mutex);
            const std::string shot = Shots().Status().phase;
            // A capture in flight keeps the source it started with.
            if (state.wanted || state.phase == "recording" || shot == "pending" || shot == "capturing") {
                return std::string("status: busy\nerror: stop the recording or wait for the screenshot first\n"
                                   "source: ") +
                       SourceKey(gSelected.load()) + "\n";
            }
            gSelected.store(wanted);
        }
    }
    const int source = gSelected.load();
    return std::string("status: ok\nsource: ") + SourceKey(source) + "\nproducer: " + SourceName(source) +
           "\nview: " + ViewName(source) + "\nsources: canvas xr\n";
}

// "8M", "800K", "8000000": bits per second; 0 when not a number.
uint32_t ParseBitRate(const std::string &text) {
    char *end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || value <= 0.0) return 0;
    double scale = 1.0;
    if (*end == 'K' || *end == 'k') scale = 1e3, ++end;
    else if (*end == 'M' || *end == 'm') scale = 1e6, ++end;
    if (*end != '\0') return 0;
    return static_cast<uint32_t>(std::min(value * scale, 200e6));
}

std::string ConfigCommand(const std::vector<std::string> &args) {
    auto &state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    const char *usage = "status: bad-arguments\nusage: capture_config [video_bit_rate=8M] [max_size=1024] "
                        "[max_fps=30] [i_frame_interval=2] | reset | status (0 = default)\n";
    if (args.size() == 1 && args[0] == "reset") {
        state.videoBitRate = state.maxSize = state.maxFps = state.iFrameInterval = 0;
    } else if (!(args.empty() || (args.size() == 1 && args[0] == "status"))) {
        for (const std::string &arg : args) {
            const size_t equals = arg.find('=');
            if (equals == std::string::npos) return usage;
            const std::string key = arg.substr(0, equals);
            const std::string value = arg.substr(equals + 1);
            char *end = nullptr;
            const long number = std::strtol(value.c_str(), &end, 10);
            const bool integer = end != value.c_str() && *end == '\0' && number >= 0;
            if (key == "video_bit_rate") {
                const uint32_t rate = value == "0" ? 0 : ParseBitRate(value);
                if (rate == 0 && value != "0") return usage;
                state.videoBitRate = rate;
            } else if (key == "max_size" && integer && (number == 0 || (number >= 64 && number <= 4096))) {
                state.maxSize = static_cast<uint32_t>(number);
            } else if (key == "max_fps" && integer && number <= 120) {
                state.maxFps = static_cast<uint32_t>(number);
            } else if (key == "i_frame_interval" && integer && number <= 60) {
                state.iFrameInterval = static_cast<uint32_t>(number);
            } else {
                return usage;
            }
        }
    }
    std::ostringstream out;
    out << "status: ok\nvideo_bit_rate: " << state.videoBitRate << "\nmax_size: " << state.maxSize
        << "\nmax_fps: " << state.maxFps << "\ni_frame_interval: " << state.iFrameInterval
        << "\napplies_to: recordings and live streams started next (0 = default: 12000000, source size, "
           "half the XR rate, 2 s)\n";
    return out.str();
}

std::string ScreenshotCommand(const std::vector<std::string> &args, const std::string &directory) {
    const bool withEye = args.size() == 3 && args[0] == "request";
    if ((args.size() != 2 && !withEye) || !SnapshotSession::ValidToken(args[1]) ||
        (withEye && !scrcpy::capture::ValidEye(args[2]))) {
        return "status: bad-arguments\nusage: capture_screenshot request TOKEN [left|right|both] | "
               "status|cancel TOKEN\n";
    }
    auto &session = Shots();
    if (args[0] == "request") {
        const uint64_t generation = gGeneration.load();
        if (generation == 0) return "status: failed\nerror: no immersive Vulkan composite session\n";
        if (!EnsureDirectory(directory)) return "status: failed\nerror: captures directory unavailable\n";
        const std::string error = session.Request(
            {args[1], directory + "/snapshot-" + args[1] + ".png", generation, withEye ? args[2] : "both"});
        if (!error.empty()) return "status: rejected\nerror: " + error + "\n";
    } else if (args[0] == "cancel") {
        session.Cancel(args[1]);
    } else if (args[0] != "status") {
        return "status: bad-arguments\n";
    }
    const auto state = session.Status();
    if (state.request.token != args[1]) return "status: unknown-request\n";
    const int source = gSelected.load();
    std::ostringstream out;
    out << "status: " << state.phase << "\nrequest_id: " << state.request.token
        << "\ngeneration: " << state.request.generation << "\nsource: " << SourceName(source)
        << "\nview: " << ViewName(source) << "\nencoding: png\neye: " << state.request.eye
        << "\nlayout: " << (state.layout.empty() ? "unknown" : state.layout) << "\nwidth: " << state.width
        << "\nheight: " << state.height << "\nproducer_frame: " << state.producer_frame
        << "\nrecorded_monotonic_ns: " << state.recorded_ns << '\n';
    if (state.phase == "ready") out << "file: " << state.request.path << '\n';
    if (!state.error.empty()) out << "error: " << state.error << '\n';
    return out.str();
}

std::string VideoCommand(const std::vector<std::string> &args, const std::string &directory) {
    auto &state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    const std::string action = args.empty() ? "status" : args.front();
    // Optional trailing eye (left|right|both) for a stereo source.
    std::vector<std::string> rest = args;
    std::string eye = "both";
    if (rest.size() >= 2 && scrcpy::capture::ValidEye(rest.back()) &&
        (action == "start" || (action == "start_live" && rest.size() == 3))) {
        eye = rest.back();
        rest.pop_back();
    }
    const bool startFile = action == "start" && rest.size() == 1;
    const bool startLive = action == "start_live" && rest.size() == 2 && SnapshotSession::ValidToken(rest[1]);
    if (startFile || startLive) {
        if (gGeneration.load() == 0) return "status: failed\nerror: no immersive Vulkan composite session\n";
        if (startFile && !EnsureDirectory(directory)) return "status: failed\nerror: captures directory unavailable\n";
        if (!state.wanted) {
            state.wanted = true;
            state.live = startLive;
            state.token = startLive ? rest[1] : std::string{};
            state.eye = eye;
            state.directory = directory;
            ++state.revision;
            state.phase = "pending";
            state.layout.clear();
            state.width = 0;
            state.height = 0;
            state.error.clear();
            state.path.clear();
            state.submitted = 0;
            state.skipped = 0;
            state.skippedBusy = 0;
            state.skippedPending = 0;
            state.composes = 0;
            state.composeNsTotal = 0;
            state.composeNsMax = 0;
            state.encoder = {};
        }
    } else if (action == "stop" && args.size() == 1) {
        if (state.wanted) {
            state.wanted = false;
            ++state.revision;
            state.phase = "stopping";
        }
    } else if (action != "status" || args.size() > 1) {
        return "status: bad-arguments\nusage: capture_video [start [EYE]|start_live TOKEN [EYE]|stop|status] "
               "(EYE: left|right|both)\n";
    }
    const int source = gSelected.load();
    std::ostringstream out;
    out << "status: " << state.phase << "\nrevision: " << state.revision.load() << "\nsource: " << SourceName(source)
        << "\nmode: " << (state.live ? "live" : "file") << "\nview: " << ViewName(source) << "\neye: " << state.eye
        << "\nlayout: " << (state.layout.empty() ? "unknown" : state.layout) << "\nwidth: " << state.width
        << "\nheight: " << state.height << "\nsubmitted: " << state.submitted << "\nskipped: " << state.skipped
        << "\nskipped_busy: " << state.skippedBusy << "\nskipped_pending: " << state.skippedPending
        << "\ncompose_latency_ms_avg: "
        << (state.composes ? static_cast<double>(state.composeNsTotal) / state.composes / 1e6 : 0.0)
        << "\ncompose_latency_ms_max: " << static_cast<double>(state.composeNsMax) / 1e6
        << "\npipeline: " << (state.legacy ? "legacy" : "retry") << "\nencoder_images: " << state.images
        << "\nrealtime: " << (state.realtime ? "true" : "false") << "\nencoded_fps_cap: " << state.encodedFps
        << "\nencoded_samples: " << state.encoder.encoded_samples << "\nencoded_bytes: " << state.encoder.encoded_bytes
        << "\nfirst_codec_pts_us: " << state.encoder.first_pts_us
        << "\nlast_codec_pts_us: " << state.encoder.last_pts_us << "\nmetadata_association: codec_pts_only\neos: "
        << (state.encoder.eos ? "true" : "false") << '\n';
    if (!state.path.empty()) out << "file: " << state.path << "\nmetadata_file: " << state.path << ".frames.csv\n";
    if (!state.error.empty()) out << "error: " << state.error << '\n';
    return out.str();
}

// debug.xrgame.capture.legacy=1 restores the first pipeline for same-session A/B: encode on even XR
// frames only, one spare encoder image, both eyes composed.
bool LegacyPipeline() {
    char value[PROP_VALUE_MAX] = {};
    return __system_property_get("debug.xrgame.capture.legacy", value) > 0 && value[0] == '1';
}

// An integer system property, or `fallback` when unset or not a number.
int PropertyInt(const char *name, int fallback) {
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get(name, value) <= 0) return fallback;
    char *end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return end != value ? static_cast<int>(parsed) : fallback;
}

uint32_t EyeBits(const std::string &eye) { return eye == "left" ? 1u : eye == "right" ? 2u : 3u; }

void SetPhase(const std::string &phase, const std::string &error) {
    auto &state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.phase = phase;
    state.error = error;
}

// PNG through the platform encoder (libjnigraphics, API 30), looked up at run time: the app's
// minimum is API 29.
using CompressWrite = bool (*)(void *, const void *, size_t);
using CompressFunction = int (*)(const AndroidBitmapInfo *, int32_t, const void *, int32_t, int32_t, void *,
                                 CompressWrite);

std::string WritePng(const std::string &path, const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height) {
    static const CompressFunction compress = [] {
        void *library = dlopen("libjnigraphics.so", RTLD_NOW | RTLD_LOCAL);
        return library != nullptr ? reinterpret_cast<CompressFunction>(dlsym(library, "AndroidBitmap_compress"))
                                  : nullptr;
    }();
    if (compress == nullptr) return "AndroidBitmap_compress unavailable";
    const std::string partial = path + ".partial";
    const int fd = open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return "cannot create " + partial;
    struct Output {
        int fd;
        bool ok;
    } output{fd, true};
    AndroidBitmapInfo info{};
    info.width = width;
    info.height = height;
    info.stride = width * 4;
    info.format = ANDROID_BITMAP_FORMAT_RGBA_8888;
    info.flags = ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE;
    const int result = compress(&info, ADATASPACE_SRGB, rgba.data(), ANDROID_BITMAP_COMPRESS_FORMAT_PNG, 100, &output,
                                [](void *context, const void *data, size_t size) {
                                    auto &out = *static_cast<Output *>(context);
                                    const auto *bytes = static_cast<const uint8_t *>(data);
                                    while (out.ok && size > 0) {
                                        const ssize_t written = write(out.fd, bytes, size);
                                        if (written <= 0) {
                                            out.ok = false;
                                            break;
                                        }
                                        bytes += written;
                                        size -= static_cast<size_t>(written);
                                    }
                                    return out.ok;
                                });
    const bool closed = close(fd) == 0;
    if (result != ANDROID_BITMAP_RESULT_SUCCESS || !output.ok || !closed || rename(partial.c_str(), path.c_str()) != 0) {
        unlink(partial.c_str());
        return "PNG write failed (" + std::to_string(result) + ")";
    }
    return {};
}

VkFormat Unorm(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_UNORM;
    default:
        return format;
    }
}

// std140 mirror of xr_capture.comp's Constants.
struct Vec4 {
    float v[4]{};
};
struct LayerUbo {
    Vec4 toLayer[3];
    Vec4 center;
    Vec4 shape;
    Vec4 rect;
    uint32_t info[4]{};
};
struct EyeUbo {
    Vec4 toWorld[3];
    Vec4 tanFov;
    Vec4 position;
    LayerUbo layers[kMaxLayers];
};
struct Constants {
    EyeUbo eyes[2];
    int32_t extent[4];
};
static_assert(sizeof(LayerUbo) == 112 && sizeof(EyeUbo) == 80 + 6 * 112 && sizeof(Constants) == 2 * 752 + 16);

constexpr uint32_t kOff = 0;
constexpr uint32_t kQuad = 1;
constexpr uint32_t kCylinder = 2;
constexpr uint32_t kProjection = 3;
constexpr uint32_t kOpaque = 0;
constexpr uint32_t kPremultiplied = 1;
constexpr uint32_t kUnpremultiplied = 2;

XrQuaternionf Multiply(const XrQuaternionf &a, const XrQuaternionf &b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

using Matrix = std::array<std::array<float, 3>, 3>;
Matrix Rotation(const XrQuaternionf &q) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    return {{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
             {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
             {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}}};
}

XrVector3f Rotate(const XrQuaternionf &q, const XrVector3f &v) {
    const Matrix r = Rotation(q);
    return {r[0][0] * v.x + r[0][1] * v.y + r[0][2] * v.z, r[1][0] * v.x + r[1][1] * v.y + r[1][2] * v.z,
            r[2][0] * v.x + r[2][1] * v.y + r[2][2] * v.z};
}

// `inner` given in a space whose pose in the target space is `outer`.
XrPosef Chain(const XrPosef &outer, const XrPosef &inner) {
    XrPosef pose{};
    pose.orientation = Multiply(outer.orientation, inner.orientation);
    const XrVector3f offset = Rotate(outer.orientation, inner.position);
    pose.position = {outer.position.x + offset.x, outer.position.y + offset.y, outer.position.z + offset.z};
    return pose;
}

// Rows of R (local -> world) or of R^T (world -> local).
void Rows(Vec4 (&out)[3], const XrQuaternionf &q, bool inverse) {
    const Matrix r = Rotation(q);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[i].v[j] = inverse ? r[j][i] : r[i][j];
    }
}

Vec4 TanFov(const XrFovf &fov) {
    return {{std::tan(fov.angleLeft), std::tan(fov.angleRight), std::tan(fov.angleDown), std::tan(fov.angleUp)}};
}

Vec4 Rect(const XrRect2Di &rect, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return {{0.0f, 0.0f, 1.0f, 1.0f}};
    return {{static_cast<float>(rect.offset.x) / static_cast<float>(width),
             static_cast<float>(rect.offset.y) / static_cast<float>(height),
             static_cast<float>(rect.extent.width) / static_cast<float>(width),
             static_cast<float>(rect.extent.height) / static_cast<float>(height)}};
}

uint32_t Blend(XrCompositionLayerFlags flags) {
    if ((flags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) == 0) return kOpaque;
    return (flags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) != 0 ? kUnpremultiplied : kPremultiplied;
}

bool Visible(XrEyeVisibility visibility, uint32_t eye) {
    return visibility == XR_EYE_VISIBILITY_BOTH || (visibility == XR_EYE_VISIBILITY_LEFT && eye == 0) ||
           (visibility == XR_EYE_VISIBILITY_RIGHT && eye == 1);
}

}  // namespace

bool Available() { return true; }

std::string HandleCommand(const std::string &command, const std::vector<std::string> &args,
                          const std::string &capturesDir) {
    if (command == "capture_source") return SourceCommand(args);
    if (command == "capture_screenshot") return ScreenshotCommand(args, capturesDir);
    if (command == "capture_video") return VideoCommand(args, capturesDir);
    if (command == "capture_config") return ConfigCommand(args);
    return "status: bad-arguments\nerror: unknown capture command\n";
}

struct XrCapture::Impl {
    // A copy of one layer's image: what the compose samples.
    struct Slot {
        vulkan::Image image;                      // view: layer 0 in `image.format`
        VkImageView sampled = VK_NULL_HANDLE;     // UNORM array view of every layer
        VkImageView second = VK_NULL_HANDLE;      // layer 1 in `image.format` (projection)
        uint32_t layers = 0;
        uint32_t sourceWidth = 0;                 // the swapchain the copy came from
        uint32_t sourceHeight = 0;
        uint64_t copiedFrame = 0;                 // screen / projection: the frame of the last copy
        spatial::xr::XrImguiVulkanLayer *attached = nullptr;
        uint64_t serialAtAttach = 0;
    };

    // A layer as submitted, with its image.
    struct Noted {
        Source source = Source::Screen;
        XrStructureType type = XR_TYPE_UNKNOWN;
        XrCompositionLayerFlags flags = 0;
        XrSpace space = XR_NULL_HANDLE;
        XrEyeVisibility visibility = XR_EYE_VISIBILITY_BOTH;
        XrPosef pose{};
        XrExtent2Df size{};
        float radius = 0.0f;
        float centralAngle = 0.0f;
        float aspectRatio = 1.0f;
        XrSwapchainSubImage subImage{};
        std::array<XrCompositionLayerProjectionView, 2> views{};
    };

    struct Wsi {
        PFN_vkCreateAndroidSurfaceKHR createSurface = nullptr;
        PFN_vkDestroySurfaceKHR destroySurface = nullptr;
        PFN_vkGetPhysicalDeviceSurfaceSupportKHR surfaceSupport = nullptr;
        PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR surfaceCapabilities = nullptr;
        PFN_vkGetPhysicalDeviceSurfaceFormatsKHR surfaceFormats = nullptr;
        PFN_vkCreateSwapchainKHR createSwapchain = nullptr;
        PFN_vkDestroySwapchainKHR destroySwapchain = nullptr;
        PFN_vkGetSwapchainImagesKHR swapchainImages = nullptr;
        PFN_vkAcquireNextImageKHR acquire = nullptr;
        PFN_vkQueuePresentKHR present = nullptr;
        PFN_vkCreateSemaphore createSemaphore = nullptr;
        PFN_vkDestroySemaphore destroySemaphore = nullptr;
        PFN_vkCmdBlitImage blitImage = nullptr;
    };

    explicit Impl(vulkan::Context &context) : ctx(context), vk(context.vk()) {
        generation = gLastGeneration.fetch_add(1) + 1;
        gGeneration.store(generation);
        const auto instanceFunction = [this](const char *name) {
            return vk.vkGetInstanceProcAddr(ctx.instance(), name);
        };
        const auto deviceFunction = [this](const char *name) { return vk.vkGetDeviceProcAddr(ctx.device(), name); };
        wsi.createSemaphore = reinterpret_cast<PFN_vkCreateSemaphore>(deviceFunction("vkCreateSemaphore"));
        wsi.destroySemaphore = reinterpret_cast<PFN_vkDestroySemaphore>(deviceFunction("vkDestroySemaphore"));
        wsi.blitImage = reinterpret_cast<PFN_vkCmdBlitImage>(deviceFunction("vkCmdBlitImage"));
        if (ctx.surfaces()) {
            wsi.createSurface = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(instanceFunction("vkCreateAndroidSurfaceKHR"));
            wsi.destroySurface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(instanceFunction("vkDestroySurfaceKHR"));
            wsi.surfaceSupport = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
                instanceFunction("vkGetPhysicalDeviceSurfaceSupportKHR"));
            wsi.surfaceCapabilities = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
                instanceFunction("vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
            wsi.surfaceFormats = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
                instanceFunction("vkGetPhysicalDeviceSurfaceFormatsKHR"));
            wsi.createSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(deviceFunction("vkCreateSwapchainKHR"));
            wsi.destroySwapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(deviceFunction("vkDestroySwapchainKHR"));
            wsi.swapchainImages = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(deviceFunction("vkGetSwapchainImagesKHR"));
            wsi.acquire = reinterpret_cast<PFN_vkAcquireNextImageKHR>(deviceFunction("vkAcquireNextImageKHR"));
            wsi.present = reinterpret_cast<PFN_vkQueuePresentKHR>(deviceFunction("vkQueuePresentKHR"));
        }
        encoderReady = wsi.createSurface && wsi.destroySurface && wsi.surfaceSupport && wsi.surfaceCapabilities &&
                       wsi.surfaceFormats && wsi.createSwapchain && wsi.destroySwapchain && wsi.swapchainImages &&
                       wsi.acquire && wsi.present && wsi.createSemaphore && wsi.destroySemaphore && wsi.blitImage;
        LOGI("capture: session generation %llu, encoder surfaces %s", static_cast<unsigned long long>(generation),
             encoderReady ? "available" : "unavailable");
    }

    ~Impl() {
        gGeneration.store(0);
        Shots().Close(generation);
        {
            // The session's end ends its recording.
            auto &state = State();
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.wanted) {
                state.wanted = false;
                ++state.revision;
            }
        }
        closeRecorder();
        {
            auto &state = State();
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.phase == "recording" || state.phase == "pending" || state.phase == "stopping") {
                state.phase = encoder.Stats().failed ? "failed" : "stopped";
                state.error = encoder.Error();
            }
        }
        ctx.waitIdle();
        for (Slot &slot : slots) destroySlot(slot);
        destroySlot(projectionStill);
        ctx.destroyImage(&output);
        ctx.destroyImage(&stillOutput);
        ctx.destroyImage(&placeholder);
        if (placeholderView != VK_NULL_HANDLE) ctx.destroyView(placeholderView);
        for (auto &buffer : constants) ctx.destroyHostBuffer(&buffer);
        ctx.destroyHostBuffer(&readback);
        const VkDevice device = ctx.device();
        if (pipeline != VK_NULL_HANDLE) vk.vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout != VK_NULL_HANDLE) vk.vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (setLayout != VK_NULL_HANDLE) vk.vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        if (sampler != VK_NULL_HANDLE) vk.vkDestroySampler(device, sampler, nullptr);
    }

    // --- capture images ---------------------------------------------------------------------------

    void destroySlot(Slot &slot) {
        if (slot.sampled != VK_NULL_HANDLE) ctx.destroyView(slot.sampled);
        if (slot.second != VK_NULL_HANDLE) ctx.destroyView(slot.second);
        ctx.destroyImage(&slot.image);
        slot = {};
    }

    // (Re)creates `slot` for this shape; true when it holds a new image.
    bool ensureSlot(Slot &slot, VkFormat format, uint32_t width, uint32_t height, uint32_t layers,
                    VkImageUsageFlags usage) {
        if (slot.image.image != VK_NULL_HANDLE && slot.image.format == format && slot.image.width == width &&
            slot.image.height == height && slot.layers == layers) {
            return false;
        }
        // Earlier copies and composes on the queue may still use the old image.
        if (slot.image.image != VK_NULL_HANDLE) ctx.waitIdle();
        spatial::xr::XrImguiVulkanLayer *attached = slot.attached;
        destroySlot(slot);
        slot.attached = attached;
        const VkImageCreateFlags flags = Unorm(format) != format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
        if (!ctx.createImage(width, height, format, usage, &slot.image, layers, flags)) {
            LOGE("capture: image %ux%ux%u format %d failed", width, height, layers, format);
            slot = {};
            return true;
        }
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = slot.image.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view.format = Unorm(format);
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
        if (vk.vkCreateImageView(ctx.device(), &view, nullptr, &slot.sampled) != VK_SUCCESS) slot.sampled = VK_NULL_HANDLE;
        if (layers > 1) slot.second = ctx.createView(slot.image.image, format, 1);
        slot.layers = layers;
        if (slot.sampled == VK_NULL_HANDLE || (layers > 1 && slot.second == VK_NULL_HANDLE)) {
            LOGE("capture: views of the %ux%u image failed", width, height);
            destroySlot(slot);
        }
        return true;
    }

    // A sampling copy of a swapchain layer (COLOR_ATTACHMENT_OPTIMAL before and after) into `target`.
    void copyLayer(VkCommandBuffer command, VkImage image, VkImageView view, uint32_t layer, VkImageView target,
                   VkImage targetImage, VkFormat format, VkExtent2D extent) {
        ctx.barrier(command, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    layer, 1);
        ctx.barrier(command, targetImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, layer, 1);
        // Sampled and stored in the swapchain's own format: the bytes stay as they are.
        vulkan::BlitParams params{};
        ctx.blit(command, target, extent, format, view, VK_NULL_HANDLE, params);
        ctx.barrier(command, targetImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, layer, 1);
        ctx.barrier(command, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, layer, 1);
    }

    // --- compose pipeline -------------------------------------------------------------------------

    bool initPipeline() {
        if (pipeline != VK_NULL_HANDLE) return true;
        const VkDevice device = ctx.device();
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW =
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vk.vkCreateSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) return false;
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSourceCount, VK_SHADER_STAGE_COMPUTE_BIT,
                       nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        setInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        setInfo.pBindings = bindings.data();
        if (vk.vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout) != VK_SUCCESS) return false;
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &setLayout;
        if (vk.vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) return false;
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = sizeof(kXrCaptureComp);
        moduleInfo.pCode = kXrCaptureComp;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vk.vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = "main";
        pipelineInfo.layout = pipelineLayout;
        // One eye of a stereo output can be composed alone (vkCmdDispatchBase, Vulkan 1.1).
        dispatchBase = reinterpret_cast<PFN_vkCmdDispatchBase>(vk.vkGetDeviceProcAddr(ctx.device(), "vkCmdDispatchBase"));
        if (dispatchBase != nullptr) pipelineInfo.flags = VK_PIPELINE_CREATE_DISPATCH_BASE_BIT;
        const VkResult result = vk.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
        vk.vkDestroyShaderModule(device, module, nullptr);
        if (result != VK_SUCCESS) {
            pipeline = VK_NULL_HANDLE;
            return false;
        }
        // Stand-in for absent images: never sampled (its layers are off), but every binding must
        // name an image in a valid layout.
        if (!ctx.createImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             &placeholder)) {
            return false;
        }
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = placeholder.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return vk.vkCreateImageView(device, &view, nullptr, &placeholderView) == VK_SUCCESS;
    }

    // --- recorder ---------------------------------------------------------------------------------

    void closeRecorder() {
        if (surface == VK_NULL_HANDLE && swapchain == VK_NULL_HANDLE && encoder.InputWindow() == nullptr) {
            encoder.Stop();
            recActive = false;
            return;
        }
        // Submitted blits and binary present semaphores must retire before the swapchain goes.
        ctx.waitIdle();
        for (VkSemaphore semaphore : acquireSemaphores) wsi.destroySemaphore(ctx.device(), semaphore, nullptr);
        for (VkSemaphore semaphore : readySemaphores) wsi.destroySemaphore(ctx.device(), semaphore, nullptr);
        acquireSemaphores.clear();
        acquireUse.clear();
        readySemaphores.clear();
        encoderImages.clear();
        if (swapchain != VK_NULL_HANDLE) wsi.destroySwapchain(ctx.device(), swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
        if (surface != VK_NULL_HANDLE) wsi.destroySurface(ctx.instance(), surface, nullptr);
        surface = VK_NULL_HANDLE;
        encoder.Stop();
        {
            auto &state = State();
            std::lock_guard<std::mutex> lock(state.mutex);
            state.encoder = encoder.Stats();
        }
        recActive = false;
        acquired = false;
    }

    // Picks up starts and stops; the source is `width` x `height` with `views` side-by-side views.
    void syncRecorder(uint32_t width, uint32_t height, uint32_t views, int fps) {
        auto &state = State();
        const uint64_t revision = state.revision.load();
        const bool sameShape = width == recSourceWidth && height == recHeight && views == recViews;
        if (revision == recRevision && (!recActive || sameShape)) return;
        bool wanted = false;
        bool live = false;
        std::string token;
        std::string eye;
        std::string directory;
        uint32_t bitRate = 0;
        uint32_t maxSize = 0;
        uint32_t maxFps = 0;
        uint32_t iFrameInterval = 0;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            wanted = state.wanted;
            live = state.live;
            token = state.token;
            eye = state.eye;
            directory = state.directory;
            bitRate = state.videoBitRate;
            maxSize = state.maxSize;
            maxFps = state.maxFps;
            iFrameInterval = state.iFrameInterval;
        }
        // A canvas recording waits until a canvas was composed once (its shape is known).
        if (wanted && (width < 2 || height < 2)) return;
        closeRecorder();
        recRevision = revision;
        if (!wanted) {
            SetPhase(encoder.Stats().failed ? "failed" : "stopped", encoder.Error());
            return;
        }
        if (!encoderReady) {
            SetPhase("failed", "the composite device has no Android surface support");
            return;
        }
        recSourceWidth = width;
        recHeight = height;
        recViews = views;
        recEyes = views == 2 ? EyeBits(eye) : 3u;
        nextEncodeFrame = 0;
        span = scrcpy::capture::SelectEye(width, views, eye);
        uint32_t encodedWidth = span.width & ~1u;
        uint32_t encodedHeight = height & ~1u;
        // max_size scales the encoded picture down (the encoder blit filters it), as scrcpy does.
        const uint32_t longest = std::max(encodedWidth, encodedHeight);
        if (maxSize != 0 && longest > maxSize) {
            const double scale = static_cast<double>(maxSize) / longest;
            encodedWidth = std::max(16u, static_cast<uint32_t>(std::lround(encodedWidth * scale)) & ~1u);
            encodedHeight = std::max(16u, static_cast<uint32_t>(std::lround(encodedHeight * scale)) & ~1u);
        }
        encWidth = encodedWidth;
        encHeight = encodedHeight;
        // max_fps below the default cadence (every other XR frame) spaces the encoded frames in time.
        encodeIntervalNs = maxFps != 0 && static_cast<int>(maxFps) < fps ? 1'000'000'000ull / maxFps : 0;
        nextEncodeNs = 0;
        const int encodedFps = encodeIntervalNs != 0 ? static_cast<int>(maxFps) : fps;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.layout = scrcpy::capture::LayoutName(views);
            state.width = encodedWidth;
            state.height = encodedHeight;
            state.encodedFps = static_cast<uint32_t>(encodedFps);
        }
        std::string error;
        const std::string path = live ? std::string{} : directory + "/embedded-" + std::to_string(NowNs()) + ".h264";
        EncoderConfig config;
        config.width = static_cast<int>(encodedWidth);
        config.height = static_cast<int>(encodedHeight);
        config.fps = encodedFps;
        config.bitrate = bitRate != 0 ? static_cast<int>(bitRate) : 12'000'000;
        config.i_frame_interval = iFrameInterval != 0 ? static_cast<int>(iFrameInterval) : 2;
        config.output_path = path;
        config.live_token = live ? token : std::string{};
        // Real-time codec scheduling (debug.xrgame.capture.realtime=0 turns it off for A/B).
        realtime = !legacy && PropertyInt("debug.xrgame.capture.realtime", 1) != 0;
        config.realtime = realtime;
        if (!encoder.Start(config, &error)) {
            closeRecorder();
            SetPhase("failed", error);
            return;
        }
        error = createEncoderSwapchain(encodedWidth, encodedHeight);
        if (!error.empty()) {
            closeRecorder();
            SetPhase("failed", error);
            return;
        }
        recActive = true;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            state.images = static_cast<uint32_t>(encoderImages.size());
            state.realtime = realtime;
            state.path = path;
            state.submitted = 0;
            state.skipped = 0;
            state.encoder = {};
        }
        SetPhase("recording", {});
        LOGI("capture: recording %ux%u (%s, %s) from a %ux%u source", encodedWidth, encodedHeight,
             SourceKey(gSelected.load()), live ? "live" : "file", width, height);
    }

    std::string createEncoderSwapchain(uint32_t width, uint32_t height) {
        VkAndroidSurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.window = encoder.InputWindow();
        VkResult result = wsi.createSurface(ctx.instance(), &surfaceInfo, nullptr, &surface);
        if (result != VK_SUCCESS) {
            surface = VK_NULL_HANDLE;
            return "encoder VkSurface: " + std::to_string(result);
        }
        VkBool32 supported = VK_FALSE;
        result = wsi.surfaceSupport(ctx.physicalDevice(), ctx.queueFamily(), surface, &supported);
        if (result != VK_SUCCESS || !supported) return "encoder Surface not presentable by the composite queue";
        VkSurfaceCapabilitiesKHR caps{};
        result = wsi.surfaceCapabilities(ctx.physicalDevice(), surface, &caps);
        if (result != VK_SUCCESS || !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            return "encoder Surface lacks transfer-dst usage";
        }
        uint32_t formatCount = 0;
        wsi.surfaceFormats(ctx.physicalDevice(), surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        wsi.surfaceFormats(ctx.physicalDevice(), surface, &formatCount, formats.data());
        const auto selected = std::find_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR &format) {
            return format.format == VK_FORMAT_R8G8B8A8_UNORM || format.format == VK_FORMAT_B8G8R8A8_UNORM;
        });
        if (selected == formats.end()) return "encoder Surface has no 8-bit UNORM format";
        const VkExtent2D requested{width, height};
        if (caps.currentExtent.width != UINT32_MAX &&
            (caps.currentExtent.width != width || caps.currentExtent.height != height)) {
            return "encoder Surface extent differs from the capture";
        }
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface = surface;
        // Spare images give the encoder time to hand them back while it is busy
        // (debug.xrgame.capture.spare, 1-6, default 2).
        const uint32_t spare = legacy ? 1u : static_cast<uint32_t>(std::clamp(PropertyInt("debug.xrgame.capture.spare", 2), 1, 6));
        info.minImageCount = caps.maxImageCount == 0 ? caps.minImageCount + spare
                                                     : std::min(caps.minImageCount + spare, caps.maxImageCount);
        info.imageFormat = selected->format;
        info.imageColorSpace = selected->colorSpace;
        info.imageExtent = requested;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                  ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                  : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        result = wsi.createSwapchain(ctx.device(), &info, nullptr, &swapchain);
        if (result != VK_SUCCESS) {
            swapchain = VK_NULL_HANDLE;
            return "encoder swapchain: " + std::to_string(result);
        }
        uint32_t count = 0;
        wsi.swapchainImages(ctx.device(), swapchain, &count, nullptr);
        encoderImages.resize(count);
        if (count == 0 || wsi.swapchainImages(ctx.device(), swapchain, &count, encoderImages.data()) != VK_SUCCESS) {
            return "encoder swapchain has no images";
        }
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        // One acquire semaphore more than images, each reused once the submission that waited on it
        // completed; one present semaphore per image.
        acquireSemaphores.assign(count + 1, VK_NULL_HANDLE);
        acquireUse.assign(count + 1, 0);
        readySemaphores.assign(count, VK_NULL_HANDLE);
        for (auto *list : {&acquireSemaphores, &readySemaphores}) {
            for (VkSemaphore &semaphore : *list) {
                if (wsi.createSemaphore(ctx.device(), &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) {
                    semaphore = VK_NULL_HANDLE;
                    return "encoder semaphore";
                }
            }
        }
        encoderFormat = selected->format;
        LOGI("capture: encoder swapchain %u images (surface min %u max %u), %s codec scheduling", count,
             caps.minImageCount, caps.maxImageCount, realtime ? "real-time" : "default");
        return {};
    }

    // Zero timeout: a busy encoder skips the frame instead of holding up the XR thread.
    bool acquireEncoderImage() {
        if (!recActive) return false;
        const uint32_t index = nextAcquire % static_cast<uint32_t>(acquireSemaphores.size());
        const auto skip = [](bool pending) {
            auto &state = State();
            std::lock_guard<std::mutex> lock(state.mutex);
            ++state.skipped;
            ++(pending ? state.skippedPending : state.skippedBusy);
            return false;
        };
        if (acquireUse[index] > ctx.completedSerial()) return skip(true);
        uint32_t image = 0;
        const VkResult result = wsi.acquire(ctx.device(), swapchain, 0, acquireSemaphores[index], VK_NULL_HANDLE, &image);
        if (result == VK_NOT_READY || result == VK_TIMEOUT) return skip(false);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            SetPhase("failed", "encoder acquire: " + std::to_string(result));
            return false;
        }
        nextAcquire = index + 1;
        acquireIndex = index;
        imageIndex = image;
        acquired = true;
        return true;
    }

    // The encoded eye span of `output` (TRANSFER_SRC_OPTIMAL) into the acquired encoder image.
    void recordEncoderBlit(VkCommandBuffer command) {
        const VkImage target = encoderImages[imageIndex];
        ctx.barrier(command, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {static_cast<int32_t>(span.x), 0, 0};
        blit.srcOffsets[1] = {static_cast<int32_t>(span.x + span.width), static_cast<int32_t>(recHeight), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {static_cast<int32_t>(encWidth), static_cast<int32_t>(encHeight), 1};
        const bool scaled = encWidth != (span.width & ~1u) || encHeight != (recHeight & ~1u);
        wsi.blitImage(command, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, scaled ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        ctx.barrier(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    }

    // With the queue lock held, right after the submission that blitted into the image.
    void presentEncoderImage(VkQueue queue) {
        VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &readySemaphores[imageIndex];
        info.swapchainCount = 1;
        info.pSwapchains = &swapchain;
        info.pImageIndices = &imageIndex;
        const uint64_t start = NowNs();
        const VkResult result = wsi.present(queue, &info);
        const uint64_t took = NowNs() - start;
        if (took > 2'000'000 && slowPresents++ % 100 == 0) {
            LOGI("capture: encoder present took %.2f ms (%llu slow so far)", static_cast<double>(took) / 1e6,
                 static_cast<unsigned long long>(slowPresents));
        }
        presentResult = result;
    }

    // An acquired encoder image whose frame was not composed still goes back to the encoder: with
    // the last composed frame when it has the recording's shape, else black. Its acquire semaphore
    // is consumed and the swapchain keeps all of its images.
    void releaseEncoderImage() {
        if (!acquired) return;
        acquired = false;
        vulkan::Submission *submission = ctx.begin();
        if (submission == nullptr) return;  // device lost: the recorder closes with the session
        const VkCommandBuffer command = submission->command;
        if (outputComposed && output.width == recSourceWidth && output.height == recHeight) {
            recordEncoderBlit(command);  // a compose leaves `output` in TRANSFER_SRC_OPTIMAL
        } else {
            const VkImage target = encoderImages[imageIndex];
            ctx.barrier(command, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT);
            const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vk.vkCmdClearColorImage(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            ctx.barrier(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        }
        vulkan::SubmitSync sync;
        sync.wait = acquireSemaphores[acquireIndex];
        sync.waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        sync.signal = readySemaphores[imageIndex];
        sync.afterSubmit = [this](VkQueue queue) { presentEncoderImage(queue); };
        const bool submitted = ctx.submit(submission, sync);
        acquireUse[acquireIndex] = submitted ? submission->serial : 0;
        if (submitted) notePresented();
    }

    void notePresented() {
        const EncoderStats stats = encoder.Stats();
        if (presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR) {
            {
                auto &state = State();
                std::lock_guard<std::mutex> lock(state.mutex);
                ++state.submitted;
                state.encoder = stats;
            }
            if (stats.failed) SetPhase("failed", encoder.Error());
        } else {
            SetPhase("failed", "encoder present: " + std::to_string(presentResult));
        }
    }

    // --- state ------------------------------------------------------------------------------------

    vulkan::Context &ctx;
    const vulkan::Dispatch &vk;
    Wsi wsi;
    bool encoderReady = false;
    uint64_t generation = 0;
    bool failed = false;

    std::array<Slot, kSourceCount> slots{};

    // A source's image this frame: stills use their own full-size projection copy.
    Slot &slot(Source source) {
        return still && source == Source::Projection ? projectionStill : slots[static_cast<uint32_t>(source)];
    }

    void releaseStill() {
        if (stillUse == 0 || ctx.completedSerial() < stillUse) return;
        destroySlot(projectionStill);
        ctx.destroyImage(&stillOutput);
        if (readbackUse <= ctx.completedSerial()) ctx.destroyHostBuffer(&readback);
        stillUse = 0;
    }
    std::vector<Noted> noted;
    uint64_t frames = 0;
    bool active = false;
    bool encoding = false;
    std::optional<SnapshotRequest> shot;
    uint32_t eyeWidth = 0;
    uint32_t eyeHeight = 0;
    // The canvas composed last: its size and view count, for the recorder.
    uint32_t canvasWidth = 0;
    uint32_t canvasHeight = 0;
    uint32_t canvasViews = 0;

    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    vulkan::Image placeholder{};
    VkImageView placeholderView = VK_NULL_HANDLE;
    bool placeholderReady = false;
    vulkan::Image output{};
    bool outputComposed = false;  // `output` holds a composed frame
    // Stills are full size: their own projection copy and output, released once the still is
    // written so they do not stay allocated during play.
    bool still = false;
    Slot projectionStill{};
    vulkan::Image stillOutput{};
    uint64_t stillUse = 0;  // the last submission using them
    std::array<vulkan::HostBuffer, vulkan::Context::kSubmissionCount> constants{};
    vulkan::HostBuffer readback{};
    uint64_t readbackUse = 0;

    EncoderSession encoder;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat encoderFormat = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> encoderImages;
    std::vector<VkSemaphore> acquireSemaphores;
    std::vector<uint64_t> acquireUse;
    std::vector<VkSemaphore> readySemaphores;
    uint32_t nextAcquire = 0;
    uint32_t acquireIndex = 0;
    uint32_t imageIndex = 0;
    bool acquired = false;
    bool recActive = false;
    uint64_t recRevision = 0;
    uint32_t recSourceWidth = 0;
    uint32_t recHeight = 0;
    uint32_t recViews = 0;
    EyeSpan span{};
    VkResult presentResult = VK_SUCCESS;
    uint64_t slowPresents = 0;
    // The first encoder frame at or after which the next one is due; a failed acquire leaves it so
    // the next XR frame tries again.
    uint64_t nextEncodeFrame = 0;
    uint32_t recEyes = 3;  // eyes of a stereo source the recording uses (bit 0 left, bit 1 right)
    uint32_t encWidth = 0;  // the encoded picture (max_size applied)
    uint32_t encHeight = 0;
    uint64_t encodeIntervalNs = 0;  // max_fps spacing, 0 for every other XR frame
    uint64_t nextEncodeNs = 0;
    uint32_t eyes = 3;     // eyes this frame needs
    bool legacy = false;
    bool realtime = false;
    PFN_vkCmdDispatchBase dispatchBase = nullptr;
};

XrCapture::XrCapture(vulkan::Context &context) : impl_(std::make_unique<Impl>(context)) {}
XrCapture::~XrCapture() = default;

bool XrCapture::active() const { return impl_->active; }

bool XrCapture::begin(uint32_t eyeWidth, uint32_t eyeHeight, int64_t displayPeriodNs) {
    Impl &p = *impl_;
    p.active = false;
    p.encoding = false;
    p.acquired = false;
    p.shot.reset();
    p.noted.clear();
    if (p.failed) return false;
    // Video: half the runtime's eye size (its eye buffer) unless capture_config's max_size scales it
    // further; stills: the full eye size.
    p.eyeWidth = std::max(64u, (std::min(eyeWidth, 4096u) / 2) & ~15u);
    p.eyeHeight = std::max(64u, (std::min(eyeHeight, 4096u) / 2) & ~15u);
    const uint32_t stillEyeWidth = std::max(64u, std::min(eyeWidth, 4096u) & ~15u);
    const uint32_t stillEyeHeight = std::max(64u, std::min(eyeHeight, 4096u) & ~15u);
    const int selected = gSelected.load();
    const double hz = displayPeriodNs > 0 ? 1e9 / static_cast<double>(displayPeriodNs) : 72.0;
    // Every other XR frame while recording (36-45 fps at 72-90 Hz).
    const int fps = std::max(15, static_cast<int>(std::lround(hz / 2.0)));
    if (selected == kXr) {
        p.syncRecorder(p.eyeWidth * 2, p.eyeHeight, 2, fps);
    } else {
        p.syncRecorder(p.canvasWidth, p.canvasHeight, p.canvasViews, fps);
    }
    ++p.frames;
    p.shot = Shots().Take(p.generation, p.frames, NowNs());
    p.still = p.shot.has_value();
    if (p.still) {
        p.eyeWidth = stillEyeWidth;
        p.eyeHeight = stillEyeHeight;
    } else {
        p.releaseStill();
    }
    bool wanted = false;
    {
        auto &state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        wanted = state.wanted;
    }
    if (p.frames % 72 == 1) {
        p.legacy = LegacyPipeline();
        auto &state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.legacy = p.legacy;
    }
    // Every other XR frame; a frame whose acquire failed retries on the next one.
    const uint64_t now = NowNs();
    const bool due = (p.legacy ? p.frames % 2 == 0 : p.frames >= p.nextEncodeFrame) && now >= p.nextEncodeNs;
    // A canvas recording learns the canvas's shape from one compose before its encoder starts.
    const bool learn = wanted && !p.recActive && selected == kCanvas && p.canvasWidth == 0 && due;
    // A still frame composes at full size; the recording skips it (one frame).
    if (p.recActive && due && !p.still) p.encoding = p.acquireEncoderImage();
    p.active = p.shot.has_value() || p.encoding || learn;
    // The eyes this frame's copies and compose need: a mono recording or one-eye still uses one.
    p.eyes = 0;
    if (p.shot) p.eyes |= EyeBits(p.shot->eye);
    if (p.encoding) p.eyes |= p.recEyes;
    if (learn || p.legacy || p.eyes == 0) p.eyes = 3;
    if (p.active && !p.initPipeline()) {
        LOGE("capture: compose pipeline unavailable; capture disabled for this session");
        if (p.shot) Shots().Complete(p.shot->token, 0, 0, "capture compose pipeline unavailable");
        p.shot.reset();
        p.failed = true;
        p.active = false;
    }
    return p.active;
}

void XrCapture::copyScreen(VkCommandBuffer command, VkImage image, VkImageView view, VkFormat format, uint32_t width,
                           uint32_t height) {
    Impl &p = *impl_;
    if (!p.active || width == 0 || height == 0) return;
    Impl::Slot &slot = p.slots[static_cast<uint32_t>(Source::Screen)];
    p.ensureSlot(slot, format, width, height, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (slot.sampled == VK_NULL_HANDLE) return;
    p.copyLayer(command, image, view, 0, slot.image.view, slot.image.image, format, {width, height});
    slot.sourceWidth = width;
    slot.sourceHeight = height;
    slot.copiedFrame = p.frames;
}

void XrCapture::copyProjection(VkCommandBuffer command, VkImage image, const std::array<VkImageView, 2> &views,
                               VkFormat format, uint32_t width, uint32_t height) {
    Impl &p = *impl_;
    if (!p.active || width == 0 || height == 0) return;
    Impl::Slot &slot = p.slot(Source::Projection);
    // At most the capture's eye size: both the xr compose and the canvas use it at that size.
    const uint32_t copyWidth = std::max(16u, std::min(width, p.eyeWidth) & ~1u);
    const uint32_t copyHeight = std::max(16u, std::min(height, p.eyeHeight) & ~1u);
    p.ensureSlot(slot, format, copyWidth, copyHeight, 2,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (slot.sampled == VK_NULL_HANDLE) return;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (!(p.eyes & (1u << eye))) continue;
        p.copyLayer(command, image, views[eye], eye, eye == 0 ? slot.image.view : slot.second, slot.image.image,
                    format, {copyWidth, copyHeight});
    }
    slot.sourceWidth = width;
    slot.sourceHeight = height;
    slot.copiedFrame = p.frames;
}

void XrCapture::attach(Source source, spatial::xr::XrImguiVulkanLayer *layer) {
    Impl &p = *impl_;
    if (layer == nullptr) return;
    Impl::Slot &slot = p.slots[static_cast<uint32_t>(source)];
    // Always attached: the panel redraws at most four times a second, so a copy taken only once a
    // still was requested would miss it in that still. Its copy is small and as rare as its redraws.
    if (p.failed || !layer->IsCreated() || layer->Format() == VK_FORMAT_UNDEFINED) {
        layer->SetCaptureTarget(VK_NULL_HANDLE);
        slot.attached = nullptr;
        return;
    }
    const bool recreated = p.ensureSlot(slot, layer->Format(), layer->CanvasWidth(), layer->CanvasHeight(), 1,
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (slot.sampled == VK_NULL_HANDLE) {
        layer->SetCaptureTarget(VK_NULL_HANDLE);
        slot.attached = nullptr;
        return;
    }
    // A serial below the recorded one: a new layer object at the same address.
    if (recreated || slot.attached != layer || layer->CaptureSerial() < slot.serialAtAttach) {
        slot.attached = layer;
        slot.serialAtAttach = layer->CaptureSerial();
    }
    slot.sourceWidth = layer->CanvasWidth();
    slot.sourceHeight = layer->CanvasHeight();
    layer->SetCaptureTarget(slot.image.image);
}

void XrCapture::note(const XrCompositionLayerBaseHeader *layer, Source source) {
    Impl &p = *impl_;
    if (!p.active || layer == nullptr || p.noted.size() >= kMaxLayers) return;
    Impl::Noted entry;
    entry.source = source;
    entry.type = layer->type;
    entry.flags = layer->layerFlags;
    entry.space = layer->space;
    switch (layer->type) {
    case XR_TYPE_COMPOSITION_LAYER_QUAD: {
        const auto &quad = *reinterpret_cast<const XrCompositionLayerQuad *>(layer);
        entry.visibility = quad.eyeVisibility;
        entry.pose = quad.pose;
        entry.size = quad.size;
        entry.subImage = quad.subImage;
        break;
    }
    case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: {
        const auto &cylinder = *reinterpret_cast<const XrCompositionLayerCylinderKHR *>(layer);
        entry.visibility = cylinder.eyeVisibility;
        entry.pose = cylinder.pose;
        entry.radius = cylinder.radius;
        entry.centralAngle = cylinder.centralAngle;
        entry.aspectRatio = cylinder.aspectRatio;
        entry.subImage = cylinder.subImage;
        break;
    }
    case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
        const auto &projection = *reinterpret_cast<const XrCompositionLayerProjection *>(layer);
        if (projection.viewCount < 2) return;
        entry.views = {projection.views[0], projection.views[1]};
        break;
    }
    default:
        return;
    }
    p.noted.push_back(entry);
}

void XrCapture::compose(XrSession session, XrSpace space, XrTime displayTime) {
    Impl &p = *impl_;
    if (!p.active) return;
    p.active = false;
    const int selected = gSelected.load();
    // Nothing composed this frame: the still fails, an acquired encoder image still goes back.
    const auto finishShot = [&p](const std::string &why) {
        if (p.shot) Shots().Complete(p.shot->token, 0, 0, why);
        p.shot.reset();
        p.releaseEncoderImage();
        p.encoding = false;
    };

    // Which images hold a usable copy this frame.
    const auto usable = [&p](Source source) {
        const Impl::Slot &slot = p.slot(source);
        if (slot.sampled == VK_NULL_HANDLE) return false;
        if (source == Source::Screen || source == Source::Projection) return slot.copiedFrame == p.frames;
        return slot.attached != nullptr && slot.attached->CaptureSerial() != slot.serialAtAttach;
    };

    Constants constants{};
    uint32_t outWidth = 0;
    uint32_t outHeight = 0;
    uint32_t views = 2;
    if (selected == kCanvas) {
        // The projection when the frame has one, else the big screen (or the loading interstitial).
        const bool projection = std::any_of(p.noted.begin(), p.noted.end(), [](const Impl::Noted &n) {
            return n.source == Source::Projection;
        }) && usable(Source::Projection);
        const bool screen = !projection && std::any_of(p.noted.begin(), p.noted.end(), [](const Impl::Noted &n) {
            return n.source == Source::Screen;
        }) && usable(Source::Screen);
        if (!projection && !screen) {
            finishShot("no canvas layer was drawn this frame");
            return;
        }
        const Impl::Slot &slot = p.slot(projection ? Source::Projection : Source::Screen);
        outWidth = slot.image.width;
        outHeight = slot.image.height;
        views = projection ? 2 : 1;
        for (uint32_t eye = 0; eye < views; ++eye) {
            LayerUbo &layer = constants.eyes[eye].layers[0];
            layer.rect = {{0.0f, 0.0f, 1.0f, 1.0f}};
            layer.info[0] = kQuad;
            layer.info[1] = static_cast<uint32_t>(projection ? Source::Projection : Source::Screen);
            layer.info[2] = projection ? eye : 0u;
        }
        constants.extent[3] = projection ? 2 : 1;
        if (!p.still) {  // the recording's canvas shape; stills are larger
            p.canvasWidth = outWidth * views;
            p.canvasHeight = outHeight;
            p.canvasViews = views;
        }
    } else {
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = displayTime;
        locate.space = space;
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        std::array<XrView, 2> eyes{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
        uint32_t count = 0;
        if (XR_FAILED(xrLocateViews(session, &locate, &viewState, 2, &count, eyes.data())) || count != 2 ||
            !(viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            finishShot("eye views not located");
            return;
        }
        outWidth = p.eyeWidth;
        outHeight = p.eyeHeight;
        if (p.shot) {
            // Where the still looks, for reading layer positions off it (LOCAL is gravity aligned).
            const XrVector3f forward = Rotate(eyes[0].pose.orientation, {0.0f, 0.0f, -1.0f});
            LOGI("capture: still view pitch %.1f deg, FOV up %.1f down %.1f deg",
                 std::asin(std::clamp(forward.y, -1.0f, 1.0f)) * 57.2958f, eyes[0].fov.angleUp * 57.2958f,
                 eyes[0].fov.angleDown * 57.2958f);
        }
        for (uint32_t eye = 0; eye < 2; ++eye) {
            EyeUbo &out = constants.eyes[eye];
            Rows(out.toWorld, eyes[eye].pose.orientation, false);
            out.tanFov = TanFov(eyes[eye].fov);
            out.position = {{eyes[eye].pose.position.x, eyes[eye].pose.position.y, eyes[eye].pose.position.z, 0.0f}};
        }
        uint32_t index = 0;
        for (const Impl::Noted &layer : p.noted) {
            if (!usable(layer.source)) continue;
            // The layer's space in the eyes' space.
            XrPosef spacePose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
            if (layer.space != space) {
                XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
                constexpr XrSpaceLocationFlags kValid =
                    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
                if (XR_FAILED(xrLocateSpace(layer.space, space, displayTime, &location)) ||
                    (location.locationFlags & kValid) != kValid) {
                    continue;
                }
                spacePose = location.pose;
            }
            const Impl::Slot &slot = p.slot(layer.source);
            for (uint32_t eye = 0; eye < 2; ++eye) {
                LayerUbo &out = constants.eyes[eye].layers[index];
                out.info[1] = static_cast<uint32_t>(layer.source);
                out.info[3] = Blend(layer.flags);
                if (layer.type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    const XrCompositionLayerProjectionView &view = layer.views[eye];
                    const XrPosef pose = Chain(spacePose, view.pose);
                    Rows(out.toLayer, pose.orientation, true);
                    out.shape = TanFov(view.fov);
                    out.rect = Rect(view.subImage.imageRect, slot.sourceWidth, slot.sourceHeight);
                    out.info[0] = kProjection;
                    out.info[2] = std::min(view.subImage.imageArrayIndex, slot.layers - 1);
                    continue;
                }
                if (!Visible(layer.visibility, eye)) continue;
                const XrPosef pose = Chain(spacePose, layer.pose);
                Rows(out.toLayer, pose.orientation, true);
                out.center = {{pose.position.x, pose.position.y, pose.position.z, 0.0f}};
                out.rect = Rect(layer.subImage.imageRect, slot.sourceWidth, slot.sourceHeight);
                out.info[2] = std::min(layer.subImage.imageArrayIndex, slot.layers - 1);
                if (layer.type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                    out.shape = {{layer.size.width, layer.size.height, 0.0f, 0.0f}};
                    out.info[0] = kQuad;
                } else {
                    const float aspect = layer.aspectRatio > 0.0f ? layer.aspectRatio : 1.0f;
                    out.shape = {{layer.radius, layer.centralAngle, layer.radius * layer.centralAngle / aspect, 0.0f}};
                    out.info[0] = layer.radius > 0.0f && layer.centralAngle > 0.0f ? kCylinder : kOff;
                }
            }
            ++index;
        }
    }
    constants.extent[0] = static_cast<int32_t>(outWidth);
    constants.extent[1] = static_cast<int32_t>(outHeight);
    constants.extent[2] = static_cast<int32_t>(views);
    const uint32_t fullWidth = outWidth * views;

    // The recorder was started for another shape: it restarts on the next frame.
    const bool encodeNow = p.encoding && fullWidth == p.recSourceWidth && outHeight == p.recHeight;

    vulkan::Image &composed = p.still ? p.stillOutput : p.output;
    if (composed.image == VK_NULL_HANDLE || composed.width != fullWidth || composed.height != outHeight) {
        if (composed.image != VK_NULL_HANDLE) p.ctx.waitIdle();
        p.ctx.destroyImage(&composed);
        if (!p.still) p.outputComposed = false;
        if (!p.ctx.createImage(fullWidth, outHeight, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &composed)) {
            finishShot("capture output image unavailable");
            return;
        }
    }
    vulkan::Submission *submission = p.ctx.begin();
    if (submission == nullptr) {
        p.acquired = false;  // device lost: nothing more is submitted
        finishShot("composite device unavailable");
        return;
    }
    vulkan::HostBuffer &ubo = p.constants[submission->slot];
    if (ubo.buffer == VK_NULL_HANDLE &&
        !p.ctx.createHostBuffer(sizeof(Constants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ubo)) {
        p.ctx.submit(submission);
        finishShot("capture constants unavailable");
        return;
    }
    std::memcpy(ubo.mapped, &constants, sizeof(constants));
    const VkCommandBuffer command = submission->command;
    if (!p.placeholderReady) {
        p.ctx.barrier(command, p.placeholder.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                      VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        p.vk.vkCmdClearColorImage(command, p.placeholder.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        p.ctx.barrier(command, p.placeholder.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        p.placeholderReady = true;
    }
    p.ctx.barrier(command, composed.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                  VK_ACCESS_SHADER_WRITE_BIT);
    std::array<VkDescriptorImageInfo, kSourceCount> images{};
    for (uint32_t i = 0; i < kSourceCount; ++i) {
        const Impl::Slot &slot = p.slot(static_cast<Source>(i));
        const bool use = usable(static_cast<Source>(i));
        images[i] = {p.sampler, use ? slot.sampled : p.placeholderView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    const VkDescriptorBufferInfo buffer{ubo.buffer, 0, sizeof(Constants)};
    const VkDescriptorImageInfo target{VK_NULL_HANDLE, composed.view, VK_IMAGE_LAYOUT_GENERAL};
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstBinding = i;
    }
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &buffer;
    writes[1].descriptorCount = kSourceCount;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = images.data();
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = &target;
    p.vk.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    p.vk.vkCmdPushDescriptorSetKHR(command, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipelineLayout, 0,
                                   static_cast<uint32_t>(writes.size()), writes.data());
    const uint32_t groupsY = (outHeight + 7) / 8;
    if (views == 2 && p.eyes != 3 && p.dispatchBase != nullptr && outWidth % 8 == 0) {
        // One eye of the side-by-side output; the other half is left undefined and never read.
        const uint32_t eyeGroups = outWidth / 8;
        p.dispatchBase(command, p.eyes == 1 ? 0 : eyeGroups, 0, 0, eyeGroups, groupsY, 1);
    } else {
        p.vk.vkCmdDispatch(command, (fullWidth + 7) / 8, groupsY, 1);
    }
    p.ctx.barrier(command, composed.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT);

    std::optional<SnapshotRequest> shot = std::move(p.shot);
    p.shot.reset();
    EyeSpan shotSpan{};
    if (shot) {
        shotSpan = scrcpy::capture::SelectEye(fullWidth, views, shot->eye);
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(shotSpan.width) * outHeight * 4;
        if (p.readbackUse > p.ctx.completedSerial()) p.ctx.waitSerial(p.readbackUse);
        if (p.readback.size < bytes) {
            p.ctx.destroyHostBuffer(&p.readback);
            if (!p.ctx.createHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &p.readback)) {
                Shots().Complete(shot->token, 0, 0, "capture readback buffer unavailable");
                shot.reset();
            }
        }
        if (shot) {
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageOffset = {static_cast<int32_t>(shotSpan.x), 0, 0};
            copy.imageExtent = {shotSpan.width, outHeight, 1};
            p.vk.vkCmdCopyImageToBuffer(command, composed.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        p.readback.buffer, 1, &copy);
            VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.buffer = p.readback.buffer;
            toHost.size = VK_WHOLE_SIZE;
            p.vk.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                                      nullptr, 1, &toHost, 0, nullptr);
            // Read on the completion thread right after the fence; the PNG is written on its own
            // thread, so neither the XR thread nor the completion thread waits for the encoder.
            const void *mapped = p.readback.mapped;
            const uint32_t width = shotSpan.width;
            const uint32_t height = outHeight;
            submission->onComplete.push_back([request = *shot, mapped, width, height, views] {
                std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
                std::memcpy(rgba.data(), mapped, rgba.size());
                std::thread([request, rgba = std::move(rgba), width, height, views] {
                    const std::string written = WritePng(request.path, rgba, width, height);
                    Shots().Complete(request.token, width, height, written, views);
                    LOGI("capture: still %ux%u %s", width, height, written.empty() ? "written" : written.c_str());
                }).detach();
            });
        }
    }
    vulkan::SubmitSync sync;
    if (encodeNow) {
        const uint64_t submittedNs = NowNs();
        submission->onComplete.push_back([submittedNs] {
            const uint64_t latency = NowNs() - submittedNs;
            auto &state = State();
            std::lock_guard<std::mutex> lock(state.mutex);
            ++state.composes;
            state.composeNsTotal += latency;
            state.composeNsMax = std::max(state.composeNsMax, latency);
        });
        p.recordEncoderBlit(command);
        sync.wait = p.acquireSemaphores[p.acquireIndex];
        sync.waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        sync.signal = p.readySemaphores[p.imageIndex];
        sync.afterSubmit = [&p](VkQueue queue) { p.presentEncoderImage(queue); };
    }
    const bool submitted = p.ctx.submit(submission, sync);
    if (submitted && !p.still) p.outputComposed = true;
    if (submitted && p.still) p.stillUse = submission->serial;
    if (shot && submitted) p.readbackUse = submission->serial;
    if (encodeNow) {
        p.acquireUse[p.acquireIndex] = submitted ? submission->serial : 0;
        p.acquired = false;
        if (submitted) {
            p.notePresented();
            p.nextEncodeFrame = p.frames + 2;
            // A few milliseconds early, so a frame at the interval is not pushed to the next XR frame.
            if (p.encodeIntervalNs != 0) p.nextEncodeNs = NowNs() + p.encodeIntervalNs - 5'000'000ull;
        }
    }
    // Acquired for a recording of another shape (it restarts next frame), or the submit failed:
    // the image goes back with black.
    p.releaseEncoderImage();
    if (!submitted && shot) Shots().Complete(shot->token, 0, 0, "capture submit failed");
    p.encoding = false;
}

#endif  // XRGAME_CAPTURE

}  // namespace xrimmersive::capture
