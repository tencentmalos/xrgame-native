#include "../xrgame_profiler.h"
#include "xr_vulkan_projection.h"
#include "xr_vulkan_probe.h"

#include "spatial/foveation/Foveation.h"

#include <android/log.h>
#include <poll.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <exception>
#include <mutex>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "xrimmersive", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "xrimmersive", __VA_ARGS__)

namespace xrimmersive::vulkan {

namespace {

// Diagnostic trigger (debug.xrgame.xr.ccuprobe=<variant + 1>): runs the guard probe once per new
// value. Returns the variant to run, or -1.
int GuardProbeRequest() {
    static uint32_t frames = 0;
    static int last = 0;
    if (frames++ % 72 != 0) return -1;
    char value[PROP_VALUE_MAX] = {};
    const int next = __system_property_get("debug.xrgame.xr.ccuprobe", value) > 0 ? std::atoi(value) : 0;
    if (next == last) return -1;
    last = next;
    return next > 0 ? next - 1 : -1;
}

// Diagnostic trigger (debug.xrgame.xr.upscaleprobe): runs the reconstruction equivalence probe
// once per new non-zero value.
bool UpscaleProbeRequest() {
    static uint32_t frames = 0;
    static int last = 0;
    if (frames++ % 72 != 0) return false;
    char value[PROP_VALUE_MAX] = {};
    const int next = __system_property_get("debug.xrgame.xr.upscaleprobe", value) > 0 ? std::atoi(value) : 0;
    if (next == last) return false;
    last = next;
    return next != 0;
}

// Diagnostic (debug.xrgame.xr.upscale.copy=1): reconstruct into Foundation's own image and copy
// that into the swapchain, as before direct rendering, for same-session A/B measurements.
bool UpscaleCopyRequested() {
    static uint32_t frames = 0;
    static bool requested = false;
    if (frames++ % 72 == 0) {
        char value[PROP_VALUE_MAX] = {};
        const bool next = __system_property_get("debug.xrgame.xr.upscale.copy", value) > 0 &&
                          std::strcmp(value, "1") == 0;
        if (next != requested) {
            LOGI("vulkan projection: reconstruction %s", next ? "copied into the swapchain (diagnostic)"
                                                                 : "rendered into the swapchain");
        }
        requested = next;
    }
    return requested;
}

using windowsvr::BufferKind;
using windowsvr::EyeFrame;
using windowsvr::WindowsFrameTransport;

std::mutex gSettingsMutex;
UpscaleSettings gSettings;

bool SameSettings(const UpscaleSettings &a, const UpscaleSettings &b) {
    return a.filter == b.filter && a.sharpness == b.sharpness && a.foveation == b.foveation && a.level == b.level &&
           a.outputPercent == b.outputPercent && a.debug == b.debug;
}

// Rotates v by the unit quaternion q.
std::array<float, 3> Rotate(const XrQuaternionf &q, const std::array<float, 3> &v) {
    const float tx = 2.0f * (q.y * v[2] - q.z * v[1]);
    const float ty = 2.0f * (q.z * v[0] - q.x * v[2]);
    const float tz = 2.0f * (q.x * v[1] - q.y * v[0]);
    return {v[0] + q.w * tx + (q.y * tz - q.z * ty), v[1] + q.w * ty + (q.z * tx - q.x * tz),
            v[2] + q.w * tz + (q.x * ty - q.y * tx)};
}

bool WaitAcquireFence(int fenceFd) {
    if (fenceFd < 0) return true;
    XrProfileScope profile("host.vr.acquire_fence", true);
    pollfd descriptor{fenceFd, POLLIN, 0};
    int result;
    do result = poll(&descriptor, 1, 5000); while (result < 0 && errno == EINTR);
    close(fenceFd);
    return result > 0;
}

}  // namespace

void SetUpscaleSettings(const UpscaleSettings &settings) {
    UpscaleSettings clamped = settings;
    clamped.filter = std::min(clamped.filter, 2u);
    clamped.sharpness = std::min(clamped.sharpness, 100u);
    clamped.foveation = std::min(clamped.foveation, 2u);
    clamped.level = std::min(clamped.level, 2u);
    clamped.outputPercent = std::clamp(clamped.outputPercent, 50u, 100u);
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    gSettings = clamped;
}

UpscaleSettings GetUpscaleSettings() {
    std::lock_guard<std::mutex> lock(gSettingsMutex);
    return gSettings;
}

bool ProjectionPresenter::initialize(Context *context, XrSession session, VkFormat format, bool srgb,
                                     uint32_t recommendedWidth, uint32_t recommendedHeight) {
    context_ = context;
    session_ = session;
    format_ = format;
    srgb_ = srgb;
    recommendedWidth_ = recommendedWidth;
    recommendedHeight_ = recommendedHeight;
    return ensureSwapchain(recommendedWidth, recommendedHeight);
}

void ProjectionPresenter::destroySwapchain() {
    if (swapchain_ == XR_NULL_HANDLE) return;
    context_->waitIdle();
    if (upscaler_) upscaler_->ReleaseTargets();
    for (auto &pair : views_) {
        for (VkImageView view : pair) context_->destroyView(view);
    }
    views_.clear();
    images_.clear();
    xrDestroySwapchain(swapchain_);
    swapchain_ = XR_NULL_HANDLE;
    releasedImageValid_ = false;
}

bool ProjectionPresenter::ensureSwapchain(uint32_t width, uint32_t height) {
    if (swapchain_ != XR_NULL_HANDLE && width == width_ && height == height_) return true;
    LOGI("vulkan projection swapchain %ux%u -> %ux%u (runtime recommendation %ux%u)", width_, height_, width, height,
         recommendedWidth_, recommendedHeight_);
    destroySwapchain();
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = format_;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 2;
    info.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(session_, &info, &swapchain_))) {
        swapchain_ = XR_NULL_HANDLE;
        return false;
    }
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateSwapchainImages(swapchain_, 0, &count, nullptr)) || count == 0) return false;
    images_.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    if (XR_FAILED(xrEnumerateSwapchainImages(swapchain_, count, &count,
                                             reinterpret_cast<XrSwapchainImageBaseHeader *>(images_.data())))) {
        return false;
    }
    views_.assign(count, {VK_NULL_HANDLE, VK_NULL_HANDLE});
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t eye = 0; eye < 2; ++eye) {
            views_[i][eye] = context_->createView(images_[i].image, format_, eye);
            if (views_[i][eye] == VK_NULL_HANDLE) return false;
        }
        const VkMemoryRequirements requirements = context_->imageRequirements(images_[i].image);
        LOGI("vulkan projection: swapchain image %p %ux%ux2 format %d needs 0x%llx bytes (align 0x%llx)",
             reinterpret_cast<void *>(images_[i].image), width, height, format_,
             static_cast<unsigned long long>(requirements.size),
             static_cast<unsigned long long>(requirements.alignment));
    }
    width_ = width;
    height_ = height;
    return true;
}

void ProjectionPresenter::releaseImported(Imported &imported) {
    context_->destroyImage(&imported.image);
    imported = {};
}

bool ProjectionPresenter::importEye(WindowsFrameTransport &transport, uint32_t eye, EyeFrame &frame, bool &fresh) {
    frame = transport.pollEye(static_cast<int>(eye));
    if (frame.kind == BufferKind::None) return false;
    fresh = frame.serial != renderedSerials_[eye];
    const bool acquired = WaitAcquireFence(frame.acquireFenceFd);
    frame.acquireFenceFd = -1;
    if (!acquired || frame.imageIndex < 0 || frame.imageIndex >= WindowsFrameTransport::kMaxImages) {
        transport.discardFrame(static_cast<int>(eye), frame.imageIndex, frame.serial);
        if (fresh) renderedSerials_[eye] = frame.serial;
        return false;
    }
    if (frame.kind != BufferKind::HardwareBuffer || frame.buffer == nullptr) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOGE("vulkan projection: eye frames arrive as dma-buf; only AHardwareBuffer eyes are imported");
        }
        transport.discardFrame(static_cast<int>(eye), frame.imageIndex, frame.serial);
        if (fresh) renderedSerials_[eye] = frame.serial;
        return false;
    }
    Imported &entry = imported_[eye][frame.imageIndex];
    if (entry.image.image != VK_NULL_HANDLE && entry.registration == frame.registrationSerial &&
        entry.buffer == frame.buffer) {
        return true;
    }
    if (entry.image.image != VK_NULL_HANDLE) {
        context_->waitIdle();
        releaseImported(entry);
    }
    if (!context_->importHardwareBuffer(frame.buffer, frame.swapRedBlue, &entry.image)) {
        transport.discardFrame(static_cast<int>(eye), frame.imageIndex, frame.serial);
        if (fresh) renderedSerials_[eye] = frame.serial;
        return false;
    }
    entry.registration = frame.registrationSerial;
    entry.buffer = frame.buffer;
    LOGI("vulkan projection: eye %u image %d imported %ux%u format %d", eye, frame.imageIndex, entry.image.width,
         entry.image.height, entry.image.format);
    return true;
}

void ProjectionPresenter::discardFresh(WindowsFrameTransport &transport, const std::array<EyeFrame, 2> &frames,
                                       const std::array<bool, 2> &fresh) {
    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (!fresh[eye]) continue;
        transport.discardFrame(static_cast<int>(eye), frames[eye].imageIndex, frames[eye].serial);
        renderedSerials_[eye] = frames[eye].serial;
    }
}

spatial::upscale::FoveatedEye ProjectionPresenter::foveate(const EyeFrame &frame, uint32_t width, uint32_t height,
                                                           const UpscaleSettings &settings) const {
    namespace fov = spatial::foveation;
    spatial::upscale::FoveatedEye eye{};
    if (settings.foveation == 0 || !context_->fdm().UsableOnExternalTargets()) return eye;
    const fov::ViewFrustum frustum{std::tan(frame.projectionFov[0]), std::tan(frame.projectionFov[1]),
                                   std::tan(frame.projectionFov[2]), std::tan(frame.projectionFov[3])};
    eye.profile = fov::ProfileForLevel(static_cast<fov::Level>(settings.level), frustum);
    eye.enabled = true;
    const fov::PixelRect rect{0, 0, width, height};
    eye.center = fov::RectCenterUv(rect, width, height);
    if (settings.foveation == 2 && gaze_.valid && frame.projectionValid) {
        // The game rendered this eye with its projection orientation; the gaze is located for the
        // XR frame that displays it, so the fovea lands where the eye looks in the rendered image.
        const std::array<float, 3> world = Rotate(gaze_.orientation, {0.0f, 0.0f, -1.0f});
        const XrQuaternionf inverse{-frame.projectionOrientation[0], -frame.projectionOrientation[1],
                                    -frame.projectionOrientation[2], frame.projectionOrientation[3]};
        const std::array<float, 3> view = Rotate(inverse, world);
        const fov::EyeGazeNdc ndc = fov::ProjectGazeToViewNdc(spatial::math::float3(view[0], view[1], view[2]), frustum);
        if (ndc.valid) {
            eye.center = fov::MapViewNdcToAtlasUv(ndc, rect, width, height);
            eye.tracked = true;
        }
    }
    return eye;
}

bool ProjectionPresenter::render(WindowsFrameTransport &transport, XrSpace space, XrCompositionLayerProjection *layer) {
    lastFreshSnap_ = -1;
    if (layer == nullptr || context_ == nullptr || context_->lost() || !transport.hasStereoContent()) return false;
    if (const int probe = GuardProbeRequest(); probe >= 0 && format_ != VK_FORMAT_UNDEFINED) {
        RunGuardProbe(*context_, format_, static_cast<uint32_t>(probe), 4);
    }
    if (UpscaleProbeRequest() && format_ != VK_FORMAT_UNDEFINED) RunUpscaleEquivalenceProbe(*context_, format_);
    std::array<EyeFrame, 2> frames{};
    std::array<bool, 2> fresh{false, false};
    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (!importEye(transport, eye, frames[eye], fresh[eye])) {
            discardFresh(transport, frames, fresh);
            return false;
        }
    }
    const UpscaleSettings settings = GetUpscaleSettings();
    const EyeFrame &first = frames[0];
    const uint32_t sourceWidth = static_cast<uint32_t>(first.sourceWidth > 0 ? first.sourceWidth : first.width);
    const uint32_t sourceHeight = static_cast<uint32_t>(first.sourceHeight > 0 ? first.sourceHeight : first.height);
    const uint32_t outputWidth = std::max(16u, recommendedWidth_ * settings.outputPercent / 100u);
    const uint32_t outputHeight = std::max(16u, recommendedHeight_ * settings.outputPercent / 100u);
    const bool reconstruct = settings.filter != 0 && upscaleFailures_ == 0 &&
                             (sourceWidth < outputWidth || sourceHeight < outputHeight);
    // Without reconstruction the swapchain matches the game's eye rect and the runtime's
    // compositor samples it 1:1, as on the GLES path.
    const uint32_t width = reconstruct ? outputWidth : std::min(sourceWidth, recommendedWidth_);
    const uint32_t height = reconstruct ? outputHeight : std::min(sourceHeight, recommendedHeight_);
    sourceWidth_ = sourceWidth;
    sourceHeight_ = sourceHeight;
    reconstructing_ = reconstruct;
    if (!ensureSwapchain(width, height)) {
        discardFresh(transport, frames, fresh);
        return false;
    }

    lastRenderReused_ = false;
    if (!fresh[0] && !fresh[1] && releasedImageValid_ && reuseReleased_ && SameSettings(settings, drawnSettings_)) {
        lastRenderReused_ = true;
        xrgame_profile_counter(kXrCounterPresentFresh, 0);
        xrgame_profile_counter(kXrCounterPresentFidLeft, static_cast<int64_t>(frames[0].frameId));
        xrgame_profile_counter(kXrCounterPresentFidRight, static_cast<int64_t>(frames[1].frameId));
        *layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer->space = space;
        layer->viewCount = 2;
        layer->views = layerViews_.data();
        return true;
    }

    const uint32_t parity = frameParity_;
    frameParity_ = (frameParity_ + 1) % kSlotsPerEye;
    if (reconstruct) {
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const uint64_t serial = slotSerials_[parity * 2 + eye];
            if (serial != 0 && !context_->waitSerial(serial)) {
                discardFresh(transport, frames, fresh);
                return false;
            }
        }
        if (!upscaler_) {
            spatial::upscale::Binding binding{};
            binding.vulkan.instance = context_->instance();
            binding.vulkan.physical_device = context_->physicalDevice();
            binding.vulkan.device = context_->device();
            binding.vulkan.vk_get_instance_proc_addr = context_->vk().vkGetInstanceProcAddr;
            binding.vulkan.vk_get_device_proc_addr = context_->vk().vkGetDeviceProcAddr;
            binding.vulkan.queue_submit_mutex = &context_->queueMutex();
            binding.enabled_fdm = context_->fdm();
            try {
                upscaler_ = std::make_unique<spatial::upscale::VulkanUpscaler>(binding);
            } catch (const std::exception &error) {
                LOGE("vulkan projection: upscaler creation failed: %s", error.what());
                ++upscaleFailures_;
                discardFresh(transport, frames, fresh);
                return false;
            }
        }
    }

    Submission *submission = context_->begin();
    if (submission == nullptr) {
        discardFresh(transport, frames, fresh);
        return false;
    }
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t imageIndex = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(swapchain_, &acquire, &imageIndex))) {
        context_->submit(submission);
        discardFresh(transport, frames, fresh);
        return false;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(swapchain_, &wait))) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(swapchain_, &release);
        context_->submit(submission);
        discardFresh(transport, frames, fresh);
        return false;
    }

    VkCommandBuffer command = submission->command;
    const VkImage target = images_[imageIndex].image;
    const uint32_t family = context_->queueFamily();
    const bool copyReconstruction = UpscaleCopyRequested();
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const EyeFrame &frame = frames[eye];
        const Image &source = imported_[eye][frame.imageIndex].image;
        // The producer is another process; take the image from the foreign queue for this pass
        // and hand it back afterwards.
        context_->barrier(command, source.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                          VK_ACCESS_SHADER_READ_BIT, 0, 1, VK_QUEUE_FAMILY_FOREIGN_EXT, family);
        const float frameWidth = static_cast<float>(frame.width);
        const float frameHeight = static_cast<float>(frame.height);
        const float cropWidth = static_cast<float>(frame.sourceWidth > 0 ? frame.sourceWidth : frame.width);
        const float cropHeight = static_cast<float>(frame.sourceHeight > 0 ? frame.sourceHeight : frame.height);
        const float u0 = static_cast<float>(frame.sourceX) / frameWidth;
        const float v0 = (frame.flipY ? static_cast<float>(frame.sourceY) + cropHeight
                                      : static_cast<float>(frame.sourceY)) / frameHeight;
        const float uScale = cropWidth / frameWidth;
        const float vScale = (frame.flipY ? -cropHeight : cropHeight) / frameHeight;
        bool drawn = false;
        if (reconstruct) {
            spatial::upscale::Options options{};
            options.filter = static_cast<spatial::upscale::Filter>(settings.filter);
            options.sharpness = settings.sharpness;
            options.debug = settings.debug;
            const spatial::upscale::FoveatedEye foveation = foveate(frame, width, height, settings);
            spatial::upscale::Output output{};
            try {
                // SGSR writes this eye's swapchain layer itself; other filters reconstruct into
                // Foundation's image, which is copied below.
                if (!copyReconstruction) {
                    const spatial::upscale::Target layer{views_[imageIndex][eye], format_, {width, height},
                                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
                    drawn = upscaler_->RenderTo(command, parity * 2 + eye, source.view,
                                                {source.width, source.height}, {uScale, vScale, u0, v0}, layer,
                                                options, foveation, true);
                }
                if (drawn) {
                    ++directEyes_;
                } else {
                    output = upscaler_->Render(command, parity * 2 + eye, source.view, {source.width, source.height},
                                               {uScale, vScale, u0, v0}, {width, height}, options, foveation, true);
                }
            } catch (const std::exception &error) {
                LOGE("vulkan projection: reconstruction failed, falling back to direct copies: %s", error.what());
                ++upscaleFailures_;
                drawn = false;
                output = {};
            }
            if (output.image != VK_NULL_HANDLE) {
                // The reconstructed bytes are already sRGB-encoded; copy them as they are.
                context_->barrier(command, output.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                context_->barrier(command, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                  VK_ACCESS_TRANSFER_WRITE_BIT, eye, 1);
                VkImageCopy copy{};
                copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye, 1};
                copy.extent = {width, height, 1};
                context_->vk().vkCmdCopyImage(command, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                context_->barrier(command, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, eye, 1);
                drawn = true;
            }
        }
        if (!drawn) {
            BlitParams params{};
            params.uv[0] = u0;
            params.uv[1] = v0;
            params.uv[2] = uScale;
            params.uv[3] = vScale;
            params.flags = kBlitOpaque | (srgb_ ? kBlitDecodeSrgb : 0u);
            context_->blit(command, views_[imageIndex][eye], {width, height}, format_, source.view, VK_NULL_HANDLE,
                           params);
        }
        context_->barrier(command, source.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                          VK_ACCESS_SHADER_READ_BIT, 0, 0, 1, family, VK_QUEUE_FAMILY_FOREIGN_EXT);

        XrCompositionLayerProjectionView &view = layerViews_[eye];
        view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        if (frame.projectionValid) {
            view.pose.orientation = {frame.projectionOrientation[0], frame.projectionOrientation[1],
                                     frame.projectionOrientation[2], frame.projectionOrientation[3]};
            view.pose.position = {frame.projectionPosition[0], frame.projectionPosition[1],
                                  frame.projectionPosition[2]};
            view.fov = {frame.projectionFov[0], frame.projectionFov[1], frame.projectionFov[2],
                        frame.projectionFov[3]};
        }
        view.subImage.swapchain = swapchain_;
        view.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(width), static_cast<int32_t>(height)}};
        view.subImage.imageArrayIndex = eye;
    }

    // Turnip on KGSL exports no sync fd, so the eyes go back to the producer when the fence
    // signals (completion thread) instead of with a release fence.
    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (!fresh[eye]) continue;
        const int index = frames[eye].imageIndex;
        submission->onComplete.push_back([&transport, eye, index] {
            transport.publishReleaseFence(static_cast<int>(eye), index, -1);
        });
        renderedSerials_[eye] = frames[eye].serial;
        if (frames[eye].snapSerial > lastFreshSnap_) lastFreshSnap_ = frames[eye].snapSerial;
    }
    const bool submitted = context_->submit(submission);
    if (submitted && reconstruct) {
        slotSerials_[parity * 2] = submission->serial;
        slotSerials_[parity * 2 + 1] = submission->serial;
    }
    xrgame_profile_counter(kXrCounterPresentFresh, static_cast<int64_t>(fresh[0]) + fresh[1]);
    xrgame_profile_counter(kXrCounterPresentFidLeft, static_cast<int64_t>(frames[0].frameId));
    xrgame_profile_counter(kXrCounterPresentFidRight, static_cast<int64_t>(frames[1].frameId));
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (XR_FAILED(xrReleaseSwapchainImage(swapchain_, &release)) || !submitted) return false;
    releasedImageValid_ = true;
    drawnSettings_ = settings;
    ++presentedFrames_;
    if (presentedFrames_ == 1 || presentedFrames_ % 600 == 0) {
        const auto stats = upscaler_ ? upscaler_->Stats() : spatial::upscale::Statistics{};
        LOGI("vulkan projection: frames=%llu source=%ux%u output=%ux%u filter=%u fov=%u level=%u gaze=%d "
             "draws=%llu direct=%llu fdm=%llu tracked=%llu uploads=%llu unchanged=%llu",
             static_cast<unsigned long long>(presentedFrames_), sourceWidth, sourceHeight, width, height,
             reconstruct ? settings.filter : 0u, settings.foveation, settings.level, gaze_.valid ? 1 : 0,
             static_cast<unsigned long long>(stats.draws), static_cast<unsigned long long>(directEyes_),
             static_cast<unsigned long long>(stats.fdm_draws),
             static_cast<unsigned long long>(stats.tracked_draws), static_cast<unsigned long long>(stats.uploads),
             static_cast<unsigned long long>(stats.unchanged));
    }
    *layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer->space = space;
    layer->viewCount = 2;
    layer->views = layerViews_.data();
    return true;
}

void ProjectionPresenter::shutdown() {
    if (context_ == nullptr) return;
    context_->waitIdle();
    upscaler_.reset();
    for (auto &eye : imported_) {
        for (auto &entry : eye) {
            if (entry.image.image != VK_NULL_HANDLE) releaseImported(entry);
        }
    }
    destroySwapchain();
    context_ = nullptr;
}

}  // namespace xrimmersive::vulkan
