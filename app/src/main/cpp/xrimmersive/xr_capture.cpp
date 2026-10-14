#include "xr_capture.h"

#if XRGAME_CAPTURE
// openxr_platform.h (through the Foundation layer) needs the JNI and EGL types first.
#include <EGL/egl.h>
#include <jni.h>

#include "spatial/capture/CaptureHub.h"
#include "spatial/capture/WindowCapture.h"
#include "spatial/capture/XrLayerCapture.h"
#include "spatial/xr/XrImguiVulkanLayer.h"

#include <mutex>
#endif

namespace xrimmersive::capture {

#if !XRGAME_CAPTURE

bool Available() { return false; }

std::string HandleCommand(const std::string &, const std::vector<std::string> &, const std::string &) {
    return "status: unavailable\nerror: this build has no scrcpy capture SDK\n";
}

namespace panel {
bool Attach() { return false; }
void Detach() {}
int Tick(uint32_t, uint32_t) { return 0; }
ANativeWindow *Window() { return nullptr; }
uint32_t EncodedWidth() { return 0; }
uint32_t EncodedHeight() { return 0; }
void Presented() {}
bool StillTarget(std::string *, std::string *) { return false; }
void StillDone(const std::string &, uint32_t, uint32_t, const std::string &) {}
}  // namespace panel

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

namespace fc = spatial::capture;

// SteamPSP's names in answers, the A/B properties it has always used, and its log tag.
void ConfigureOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        fc::Configure({.product = "steampsp", .propertyPrefix = "debug.xrgame.capture", .logTag = "xrimmersive"});
    });
}

fc::VulkanBinding Binding(vulkan::Context &context) {
    fc::VulkanBinding binding;
    binding.instance = context.instance();
    binding.physicalDevice = context.physicalDevice();
    binding.device = context.device();
    binding.queue = context.queue();
    binding.queueFamily = context.queueFamily();
    binding.getInstanceProcAddr = context.vk().vkGetInstanceProcAddr;
    binding.queueMutex = &context.queueMutex();
    binding.surfaces = context.surfaces();
    return binding;
}

}  // namespace

bool Available() { return true; }

std::string HandleCommand(const std::string &command, const std::vector<std::string> &args,
                          const std::string &capturesDir) {
    ConfigureOnce();
    return fc::HandleCommand(command, args, capturesDir);
}

namespace panel {
bool Attach() {
    ConfigureOnce();
    return fc::window::Attach();
}
void Detach() { fc::window::Detach(); }
int Tick(uint32_t width, uint32_t height) { return fc::window::Tick(width, height); }
ANativeWindow *Window() { return fc::window::Window(); }
uint32_t EncodedWidth() { return fc::window::EncodedWidth(); }
uint32_t EncodedHeight() { return fc::window::EncodedHeight(); }
void Presented() { fc::window::Presented(); }
bool StillTarget(std::string *token, std::string *path) { return fc::window::StillTarget(token, path); }
void StillDone(const std::string &token, uint32_t width, uint32_t height, const std::string &error) {
    fc::window::StillDone(token, width, height, error);
}
}  // namespace panel

struct XrCapture::Impl {
    explicit Impl(vulkan::Context &context) : capture((ConfigureOnce(), Binding(context)), {xrLocateViews, xrLocateSpace}) {}
    fc::XrLayerCapture capture;
};

XrCapture::XrCapture(vulkan::Context &context) : impl_(std::make_unique<Impl>(context)) {}
XrCapture::~XrCapture() = default;

bool XrCapture::begin(uint32_t eyeWidth, uint32_t eyeHeight, int64_t displayPeriodNs) {
    return impl_->capture.begin(eyeWidth, eyeHeight, displayPeriodNs);
}

bool XrCapture::active() const { return impl_->capture.active(); }

void XrCapture::copyScreen(VkCommandBuffer command, VkImage image, VkImageView view, VkFormat format, uint32_t width,
                           uint32_t height) {
    impl_->capture.copyScreen(command, image, view, format, width, height);
}

void XrCapture::copyProjection(VkCommandBuffer command, VkImage image, const std::array<VkImageView, 2> &views,
                               VkFormat format, uint32_t width, uint32_t height) {
    impl_->capture.copyProjection(command, image, views, format, width, height);
}

void XrCapture::attach(Source source, spatial::xr::XrImguiVulkanLayer *layer) {
    impl_->capture.attach(static_cast<fc::Source>(source), fc::OverlayHook::Of(layer));
}

void XrCapture::note(const XrCompositionLayerBaseHeader *layer, Source source) {
    impl_->capture.note(layer, static_cast<fc::Source>(source));
}

void XrCapture::compose(XrSession session, XrSpace space, XrTime displayTime) {
    impl_->capture.compose(session, space, displayTime);
}

#endif  // XRGAME_CAPTURE

}  // namespace xrimmersive::capture
