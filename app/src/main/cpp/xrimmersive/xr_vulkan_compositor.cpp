#include "xr_vulkan_compositor.h"

#include "xr_capture.h"

#include "xr_vulkan_trace.h"

#include "spatial/foveation/Foveation.h"

#include <android/log.h>
#include <sys/system_properties.h>
#include <time.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <mutex>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "xrimmersive", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "xrimmersive", __VA_ARGS__)

namespace xrimmersive::vulkan {

namespace {

std::mutex gConfigMutex;
CompositeConfig gConfig;

// The clock the X server's renderer stamps its frames with.
int64_t MonotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

}  // namespace

void SetCompositeConfig(const CompositeConfig &config) {
    std::lock_guard<std::mutex> lock(gConfigMutex);
    gConfig = config;
}

CompositeConfig GetCompositeConfig() {
    CompositeConfig config;
    {
        std::lock_guard<std::mutex> lock(gConfigMutex);
        config = gConfig;
    }
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.xrgame.xr.composite", value) > 0) {
        if (std::strcmp(value, "vulkan") == 0) config.vulkan = true;
        if (std::strcmp(value, "gles") == 0) config.vulkan = false;
    }
    return config;
}

bool Compositor::create(XrInstance instance, XrSystemId system, const CompositeConfig &config) {
    if (config.driverDir.empty() || config.libraryName.empty()) {
        LOGE("vulkan composite: no driver configured");
        return false;
    }
    const PFN_vkGetInstanceProcAddr getInstanceProcAddr =
        TraceWrapInstanceProcAddr(Context::LoadDriver(config.driverDir, config.libraryName, config.hookDir));
    return getInstanceProcAddr != nullptr && context_.create(instance, system, getInstanceProcAddr);
}

void Compositor::releaseResources() {
    if (context_.device() == VK_NULL_HANDLE) return;
    projection_.shutdown();
    context_.waitIdle();
    releaseScene();
    generator_.release();
    releaseQuadReconstruction();
    detachSwapchain(kQuad);
    detachSwapchain(kInterstitial);
    context_.destroyImage(&overlay_);
    context_.destroyImage(&interstitial_);
    context_.destroyImage(&sharedGame_);
    if (sharedGameBuffer_ != nullptr) {
        AHardwareBuffer_release(sharedGameBuffer_);
        sharedGameBuffer_ = nullptr;
    }
}

void Compositor::destroy() {
    releaseResources();
    context_.destroy();
}

XrGraphicsBindingVulkan2KHR Compositor::binding() const {
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = context_.instance();
    binding.physicalDevice = context_.physicalDevice();
    binding.device = context_.device();
    binding.queueFamilyIndex = context_.queueFamily();
    binding.queueIndex = 0;
    return binding;
}

int64_t Compositor::ChooseFormat(const std::vector<int64_t> &formats, bool *srgb) {
    *srgb = false;
    for (int64_t format : formats) {
        if (format == VK_FORMAT_R8G8B8A8_SRGB) {
            *srgb = true;
            return format;
        }
    }
    for (int64_t format : formats) {
        if (format == VK_FORMAT_R8G8B8A8_UNORM) return format;
    }
    if (!formats.empty()) {
        *srgb = formats[0] == VK_FORMAT_B8G8R8A8_SRGB;
        return formats[0];
    }
    *srgb = true;
    return VK_FORMAT_R8G8B8A8_SRGB;
}

bool Compositor::attachSwapchain(Target target, XrSwapchain swapchain, int64_t format, uint32_t width,
                                 uint32_t height) {
    detachSwapchain(target);
    SwapchainTarget &slot = targets_[target];
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr)) || count == 0) return false;
    slot.images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    if (XR_FAILED(xrEnumerateSwapchainImages(swapchain, count, &count,
                                             reinterpret_cast<XrSwapchainImageBaseHeader *>(slot.images.data())))) {
        slot.images.clear();
        return false;
    }
    slot.format = static_cast<VkFormat>(format);
    slot.srgb = slot.format == VK_FORMAT_R8G8B8A8_SRGB || slot.format == VK_FORMAT_B8G8R8A8_SRGB;
    for (const auto &image : slot.images) {
        slot.views.push_back(context_.createView(image.image, slot.format, 0));
        if (slot.views.back() == VK_NULL_HANDLE) return false;
        const VkMemoryRequirements requirements = context_.imageRequirements(image.image);
        LOGI("vulkan composite: swapchain %d image %p %ux%u format %d needs 0x%llx bytes (align 0x%llx)",
             static_cast<int>(target), reinterpret_cast<void *>(image.image), width, height, slot.format,
             static_cast<unsigned long long>(requirements.size),
             static_cast<unsigned long long>(requirements.alignment));
    }
    slot.swapchain = swapchain;
    slot.width = width;
    slot.height = height;
    return true;
}

void Compositor::detachSwapchain(Target target) {
    SwapchainTarget &slot = targets_[target];
    if (!slot.views.empty()) context_.waitIdle();
    for (VkImageView view : slot.views) context_.destroyView(view);
    slot = {};
}

bool Compositor::importSharedGame(AHardwareBuffer *buffer) {
    if (buffer == sharedGameBuffer_ && sharedGame_.image != VK_NULL_HANDLE) return true;
    if (sharedGame_.image != VK_NULL_HANDLE) {
        context_.waitIdle();
        context_.destroyImage(&sharedGame_);
    }
    if (sharedGameBuffer_ != nullptr) AHardwareBuffer_release(sharedGameBuffer_);
    sharedGameBuffer_ = nullptr;
    if (!context_.importHardwareBuffer(buffer, false, &sharedGame_)) return false;
    // Holding a reference keeps the address from being recycled, so identity is a sound test.
    AHardwareBuffer_acquire(buffer);
    sharedGameBuffer_ = buffer;
    LOGI("vulkan composite: shared game buffer imported %ux%u", sharedGame_.width, sharedGame_.height);
    return true;
}

void Compositor::releaseQuadReconstruction() {
    if (quadUpscaler_) quadUpscaler_->ReleaseTargets();
    for (Image &image : quadImages_) context_.destroyImage(&image);
    quadUpscaler_.reset();
    quadSerials_ = {};
}

VkImageView Compositor::reconstructQuad(VkCommandBuffer command, uint32_t slot, VkExtent2D extent,
                                        const UpscaleSettings &settings, const QuadView &view, VkImageView source,
                                        VkExtent2D input, bool *linear) {
    namespace fov = spatial::foveation;
    *linear = false;
    if (!quadUpscaler_) {
        spatial::upscale::Binding binding{};
        binding.vulkan.instance = context_.instance();
        binding.vulkan.physical_device = context_.physicalDevice();
        binding.vulkan.device = context_.device();
        binding.vulkan.vk_get_instance_proc_addr = context_.vk().vkGetInstanceProcAddr;
        binding.vulkan.vk_get_device_proc_addr = context_.vk().vkGetDeviceProcAddr;
        binding.vulkan.queue_submit_mutex = &context_.queueMutex();
        binding.enabled_fdm = context_.fdm();
        try {
            quadUpscaler_ = std::make_unique<spatial::upscale::VulkanUpscaler>(binding);
        } catch (const std::exception &error) {
            LOGE("vulkan composite: big screen upscaler creation failed: %s", error.what());
            ++quadFailures_;
            return VK_NULL_HANDLE;
        }
    }
    Image &image = quadImages_[slot];
    if (image.image != VK_NULL_HANDLE && (image.width != extent.width || image.height != extent.height)) {
        context_.waitIdle();
        quadUpscaler_->ReleaseTargets();
        context_.destroyImage(&image);
    }
    if (image.image == VK_NULL_HANDLE &&
        !context_.createImage(extent.width, extent.height, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &image)) {
        LOGE("vulkan composite: big screen reconstruction image %ux%u unavailable", extent.width, extent.height);
        ++quadFailures_;
        return VK_NULL_HANDLE;
    }

    spatial::upscale::Options options{};
    options.filter = static_cast<spatial::upscale::Filter>(settings.filter);
    options.sharpness = settings.sharpness;
    options.debug = settings.debug;
    spatial::upscale::FoveatedEye foveation{};
    if (settings.foveation != 0 && context_.fdm().UsableOnExternalTargets() && view.tanHalfWidth > 0.0f &&
        view.tanHalfHeight > 0.0f) {
        // The game area as an eye image seen from the player, so the fovea covers the same visual
        // angle as in the projection path; the eye gaze moves it to where the player looks.
        const fov::ViewFrustum frustum{-view.tanHalfWidth, view.tanHalfWidth, view.tanHalfHeight, -view.tanHalfHeight};
        foveation.profile = fov::ProfileForLevel(static_cast<fov::Level>(settings.level), frustum);
        foveation.enabled = true;
        if (settings.foveation == 2 && view.gazeValid) {
            foveation.center = {view.gazeU, view.gazeV};
            foveation.tracked = true;
        }
    }
    const std::array<float, 4> uv{1.0f, 1.0f, 0.0f, 0.0f};
    VkImage written = VK_NULL_HANDLE;
    VkImageView result = VK_NULL_HANDLE;
    try {
        // SGSR writes our image directly; FSR1 reconstructs into Foundation's own sRGB image.
        const spatial::upscale::Target layer{image.view, VK_FORMAT_R8G8B8A8_UNORM, extent,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (quadUpscaler_->RenderTo(command, slot, source, input, uv, layer, options, foveation, true)) {
            written = image.image;
            result = image.view;
        } else {
            const spatial::upscale::Output output =
                quadUpscaler_->Render(command, slot, source, input, uv, extent, options, foveation, true);
            written = output.image;
            result = output.view;
            *linear = true;
        }
    } catch (const std::exception &error) {
        LOGE("vulkan composite: big screen reconstruction failed, showing the game unscaled: %s", error.what());
        ++quadFailures_;
        return VK_NULL_HANDLE;
    }
    if (result == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    context_.barrier(command, written, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    if (++quadFrames_ == 1 || quadFrames_ % 900 == 0) {
        const auto stats = quadUpscaler_->Stats();
        LOGI("vulkan composite: big screen %ux%u -> %ux%u filter=%u fov=%u level=%u gaze=%d direct=%d frames=%llu "
             "fdm=%llu tracked=%llu",
             input.width, input.height, extent.width, extent.height, settings.filter, settings.foveation,
             settings.level, view.gazeValid ? 1 : 0, *linear ? 0 : 1, static_cast<unsigned long long>(quadFrames_),
             static_cast<unsigned long long>(stats.fdm_draws), static_cast<unsigned long long>(stats.tracked_draws));
    }
    return result;
}

bool Compositor::bindScene(xrgame::XrFrameExchange *scene) {
    if (scene == scene_) return sceneOk_;
    releaseScene();
    if (scene == nullptr) return false;
    xrgame::Retain(scene);
    scene_ = scene;
    sceneOk_ = scene->version == xrgame::XrFrameExchange::kVersion;
    for (uint32_t slot = 0; sceneOk_ && slot < xrgame::XrFrameExchange::kSlots; ++slot) {
        sceneOk_ = scene->buffers[slot] != nullptr &&
                   context_.importHardwareBuffer(scene->buffers[slot], false, &sceneImages_[slot]);
    }
    if (!sceneOk_) {
        LOGE("vulkan composite: scene ring import failed; the big screen keeps the shared game buffer");
        for (Image &image : sceneImages_) context_.destroyImage(&image);
        return false;
    }
    LOGI("vulkan composite: scene ring bound, %u slots %ux%u", xrgame::XrFrameExchange::kSlots, scene->width,
         scene->height);
    return true;
}

void Compositor::releaseScene() {
    if (scene_ == nullptr) return;
    context_.waitIdle();
    for (Image &image : sceneImages_) context_.destroyImage(&image);
    scene_->held.fetch_and(~sceneHeld_, std::memory_order_seq_cst);
    sceneHeld_ = 0;
    sceneUse_ = {};
    current_ = {};
    previous_ = {};
    xrgame::Release(scene_);
    scene_ = nullptr;
    sceneOk_ = false;
    generator_.reset();
    fgPendingShared_ = false;
    fgComputed_ = -1;
    fgShownItem_ = -1;
}

// Hands back the slots this side no longer shows or interpolates from, once the GPU is done with them.
void Compositor::releaseSlots(bool keepPrevious) {
    const uint64_t completed = context_.completedSerial();
    for (uint32_t slot = 0; slot < xrgame::XrFrameExchange::kSlots; ++slot) {
        const uint32_t bit = 1u << slot;
        if (!(sceneHeld_ & bit) || static_cast<int>(slot) == current_.slot ||
            (keepPrevious && static_cast<int>(slot) == previous_.slot) || sceneUse_[slot] > completed) {
            continue;
        }
        scene_->held.fetch_and(~bit, std::memory_order_seq_cst);
        sceneHeld_ &= ~bit;
    }
}

bool Compositor::takeLatestFrame(bool frameGeneration) {
    // Frames that only moved the cursor are skipped while the game keeps rendering, so frame
    // generation interpolates between game frames; a game that stops still shows the cursor.
    constexpr int64_t kStillGameNs = 100'000'000;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint64_t latest = scene_->latest.load(std::memory_order_seq_cst);
        if (latest == 0 || xrgame::XrFrameExchange::Serial(latest) <= current_.serial) return false;
        const uint32_t slot = xrgame::XrFrameExchange::Slot(latest);
        if (slot >= xrgame::XrFrameExchange::kSlots) return false;
        const uint32_t bit = 1u << slot;
        const bool held = (sceneHeld_ & bit) != 0;
        if (!held) {
            scene_->held.fetch_or(bit, std::memory_order_seq_cst);
            sceneHeld_ |= bit;
        }
        // Once the hold is visible the renderer leaves the slot alone; if it published again in
        // between it may already be writing this one, so take the newer frame instead.
        const bool stable = scene_->latest.load(std::memory_order_seq_cst) == latest;
        SceneFrame frame{static_cast<int>(slot), xrgame::XrFrameExchange::Serial(latest),
                         scene_->content[slot].load(std::memory_order_relaxed),
                         scene_->completedNs[slot].load(std::memory_order_relaxed)};
        const bool cursorOnly = frameGeneration && current_.slot >= 0 && frame.content == current_.content &&
                                frame.completedNs - current_.completedNs < kStillGameNs;
        if (!stable || cursorOnly) {
            if (!held) {
                scene_->held.fetch_and(~bit, std::memory_order_seq_cst);
                sceneHeld_ &= ~bit;
            }
            if (cursorOnly) return false;
            continue;
        }
        previous_ = frameGeneration ? current_ : SceneFrame{};
        current_ = frame;
        return true;
    }
    return false;
}

void Compositor::noteShown(int64_t nowNs, bool newImage) {
    if (statsStartNs_ == 0) statsStartNs_ = nowNs;
    if (newImage) ++statsShown_;
    const int64_t elapsed = nowNs - statsStartNs_;
    if (elapsed < 1'000'000'000) return;
    const float seconds = static_cast<float>(elapsed) / 1e9f;
    frameGenStats_.multiplier = fgEnabled_ ? fgGenerations_ + 1 : 0;
    frameGenStats_.gameFps = static_cast<float>(statsGameFrames_) / seconds;
    frameGenStats_.outputFps = static_cast<float>(statsShown_) / seconds;
    static uint32_t logged = 0;
    if (fgEnabled_ && logged++ % 10 == 0) {
        LOGI("vulkan composite: frame generation %ux, game %.1f fps, shown %.1f fps, interval %.1f ms",
             frameGenStats_.multiplier, frameGenStats_.gameFps, frameGenStats_.outputFps, fgIntervalNs_ / 1e6);
    }
    statsStartNs_ = nowNs;
    statsGameFrames_ = 0;
    statsShown_ = 0;
}

bool Compositor::drawQuad(uint32_t imageIndex, xrgame::XrFrameExchange *scene, AHardwareBuffer *sharedGame,
                          const uint8_t *overlayPixels, uint32_t overlayWidth, uint32_t overlayHeight, float contentScaleX,
                          float contentScaleY, const QuadView &view, int64_t displayPeriodNs) {
    SwapchainTarget &target = targets_[kQuad];
    if (imageIndex >= target.views.size()) return false;
    const UpscaleSettings settings = GetUpscaleSettings();
    const int64_t now = MonotonicNs();

    // The X server's scene ring first, the shared game buffer without one.
    const bool sceneBound = bindScene(scene);
    const bool frameGeneration = sceneBound && settings.frameGeneration > 1 && generator_.ensure(&context_);
    if (frameGeneration != fgEnabled_) {
        fgEnabled_ = frameGeneration;
        generator_.reset();
        previous_ = {};
        fgPendingShared_ = false;
        fgGenerations_ = 0;
        fgShownItem_ = -1;
        fgComputed_ = -1;
        fgIntervalNs_ = 0.0;
        LOGI("vulkan composite: big screen frame generation %s (up to %ux)", fgEnabled_ ? "on" : "off",
             settings.frameGeneration);
    }
    bool newFrame = false;
    if (sceneBound) {
        releaseSlots(fgEnabled_);
        newFrame = takeLatestFrame(fgEnabled_);
    }
    const bool sceneReady = sceneBound && current_.slot >= 0;
    if (newFrame) ++statsGameFrames_;
    if (newFrame && fgEnabled_) {
        // Pace by the game: generated images split its frame interval evenly before the frame itself.
        constexpr int64_t kGapNs = 200'000'000;
        const int64_t interval = previous_.slot >= 0 ? current_.completedNs - previous_.completedNs : 0;
        if (interval <= 0 || interval > kGapNs) {
            generator_.reset();
            fgIntervalNs_ = 0.0;
        } else {
            fgIntervalNs_ = fgIntervalNs_ == 0.0 ? static_cast<double>(interval)
                                                 : fgIntervalNs_ + 0.25 * (static_cast<double>(interval) - fgIntervalNs_);
        }
        const double periods = displayPeriodNs > 0 ? fgIntervalNs_ / static_cast<double>(displayPeriodNs) : 0.0;
        fgGenerations_ = static_cast<uint32_t>(std::clamp<long>(std::lround(periods) - 1, 0,
                                                                static_cast<long>(settings.frameGeneration) - 1));
        fgPendingShared_ = true;
        fgStartNs_ = now;
        fgComputed_ = -1;
    }

    const bool shared = !sceneReady && sharedGame != nullptr && importSharedGame(sharedGame);
    const Image *game = sceneReady ? &sceneImages_[current_.slot] : (shared ? &sharedGame_ : nullptr);
    const Image *previousGame = sceneReady && fgEnabled_ && previous_.slot >= 0 && previous_.slot != current_.slot
                                    ? &sceneImages_[previous_.slot]
                                    : nullptr;
    // The game area of the quad in swapchain pixels; reconstruct when it holds more than the game.
    // A negative content scale flips the game on the quad; the area is its magnitude.
    const VkExtent2D area{
        std::max(1u, static_cast<uint32_t>(std::lround(target.width * std::min(std::fabs(contentScaleX), 1.0f)))),
        std::max(1u, static_cast<uint32_t>(std::lround(target.height * std::min(std::fabs(contentScaleY), 1.0f))))};
    const bool reconstruct = game != nullptr && settings.filter != 0 && quadFailures_ == 0 &&
                             (area.width > game->width || area.height > game->height);
    const uint32_t slot = quadSlot_;
    if (reconstruct && quadSerials_[slot] != 0 && !context_.waitSerial(quadSerials_[slot])) return false;
    Submission *submission = context_.begin();
    if (submission == nullptr) return false;
    if (overlayPixels != nullptr) {
        context_.uploadRgba(submission, &overlay_, overlayPixels, overlayWidth, overlayHeight);
    }
    const uint32_t family = context_.queueFamily();
    BlitParams params{};
    params.flags = target.srgb ? kBlitDecodeSrgb : 0u;
    VkImageView source = VK_NULL_HANDLE;
    VkImageView overlay = VK_NULL_HANDLE;
    std::array<const Image *, 2> acquired{game, previousGame};
    if (game != nullptr) {
        // The X server's renderer writes these buffers; acquire them from the foreign queue per draw.
        for (const Image *image : acquired) {
            if (image == nullptr) continue;
            context_.barrier(submission->command, image->image, VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             VK_ACCESS_SHADER_READ_BIT, 0, 1, VK_QUEUE_FAMILY_FOREIGN_EXT, family);
        }
        params.mode = 1;
        params.contentScale[0] = contentScaleX;
        params.contentScale[1] = contentScaleY;
        source = game->view;
        overlay = overlay_.view;
        quadSource_ = {game->width, game->height};

        // Frame generation at the game's size, before the reconstruction both kinds of frame share.
        bool newImage = newFrame;
        if (fgEnabled_ && sceneReady) {
            const VkImageView previousView = previousGame != nullptr ? previousGame->view : VK_NULL_HANDLE;
            if (fgPendingShared_) {
                fgPendingShared_ = false;
                if (!generator_.recordShared(submission, {game->width, game->height},
                                             static_cast<float>(settings.flowPercent) / 100.0f, game->view, previousView)) {
                    if (fgSharedFailures_++ % 300 == 0) LOGE("vulkan composite: frame generation shared passes failed");
                    generator_.reset();
                    fgGenerations_ = 0;
                }
                if (!generator_.historyReady()) fgGenerations_ = 0;
                fgShownItem_ = -1;
            }
            int item = static_cast<int>(fgGenerations_);
            if (fgGenerations_ > 0 && fgIntervalNs_ > 0.0) {
                const double step = fgIntervalNs_ / static_cast<double>(fgGenerations_ + 1);
                const double due = static_cast<double>(now - fgStartNs_) + 0.5 * static_cast<double>(displayPeriodNs);
                item = std::clamp(static_cast<int>(due / step), 0, static_cast<int>(fgGenerations_));
            }
            if (item < static_cast<int>(fgGenerations_)) {
                if (fgComputed_ != item) {
                    fgComputedView_ = generator_.recordGeneration(submission, static_cast<uint32_t>(item), fgGenerations_,
                                                                  game->view, previousView);
                    fgComputed_ = fgComputedView_ != VK_NULL_HANDLE ? item : -1;
                }
                if (fgComputed_ == item) source = fgComputedView_;
            }
            newImage = item != fgShownItem_;
            fgShownItem_ = item;
        }
        noteShown(now, newImage);

        bool linear = false;
        const VkImageView reconstructed =
            reconstruct ? reconstructQuad(submission->command, slot, area, settings, view, source,
                                          {game->width, game->height}, &linear)
                        : VK_NULL_HANDLE;
        quadReconstructing_ = reconstructed != VK_NULL_HANDLE;
        if (quadReconstructing_) {
            source = reconstructed;
            if (linear) params.flags |= kBlitLinearSource;
        }
    } else if (overlay_.image != VK_NULL_HANDLE) {
        quadReconstructing_ = false;
        source = overlay_.view;
        quadSource_ = {overlay_.width, overlay_.height};
    }
    context_.blit(submission->command, target.views[imageIndex], {target.width, target.height}, target.format,
                  source, overlay, params);
    if (capture_ != nullptr) {
        capture_->copyScreen(submission->command, target.images[imageIndex].image, target.views[imageIndex],
                             target.format, target.width, target.height);
    }
    for (const Image *image : acquired) {
        if (game == nullptr || image == nullptr) continue;
        context_.barrier(submission->command, image->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 0, 1, family,
                         VK_QUEUE_FAMILY_FOREIGN_EXT);
    }
    const bool submitted = context_.submit(submission);
    if (submitted && sceneReady) {
        sceneUse_[current_.slot] = submission->serial;
        if (previousGame != nullptr) sceneUse_[previous_.slot] = submission->serial;
    }
    if (submitted && quadReconstructing_) {
        quadSerials_[slot] = submission->serial;
        quadSlot_ = (slot + 1) % kQuadSlots;
    }
    return submitted;
}

bool Compositor::drawInterstitial(uint32_t imageIndex, const uint8_t *pixels, uint32_t width, uint32_t height) {
    SwapchainTarget &target = targets_[kInterstitial];
    if (imageIndex >= target.views.size()) return false;
    Submission *submission = context_.begin();
    if (submission == nullptr) return false;
    if (!context_.uploadRgba(submission, &interstitial_, pixels, width, height)) {
        context_.submit(submission);
        return false;
    }
    BlitParams params{};
    params.flags = target.srgb ? kBlitDecodeSrgb : 0u;
    context_.blit(submission->command, target.views[imageIndex], {target.width, target.height}, target.format,
                  interstitial_.view, VK_NULL_HANDLE, params);
    if (capture_ != nullptr) {
        capture_->copyScreen(submission->command, target.images[imageIndex].image, target.views[imageIndex],
                             target.format, target.width, target.height);
    }
    return context_.submit(submission);
}

}  // namespace xrimmersive::vulkan
