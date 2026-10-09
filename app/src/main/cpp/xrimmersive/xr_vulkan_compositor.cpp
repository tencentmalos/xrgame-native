#include "xr_vulkan_compositor.h"

#include "xr_vulkan_trace.h"

#include <android/log.h>
#include <sys/system_properties.h>

#include <cstring>
#include <mutex>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "xrimmersive", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "xrimmersive", __VA_ARGS__)

namespace xrimmersive::vulkan {

namespace {

std::mutex gConfigMutex;
CompositeConfig gConfig;

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

bool Compositor::drawQuad(uint32_t imageIndex, AHardwareBuffer *sharedGame, const uint8_t *overlayPixels,
                          uint32_t overlayWidth, uint32_t overlayHeight, float contentScaleX, float contentScaleY) {
    SwapchainTarget &target = targets_[kQuad];
    if (imageIndex >= target.views.size()) return false;
    Submission *submission = context_.begin();
    if (submission == nullptr) return false;
    if (overlayPixels != nullptr) {
        context_.uploadRgba(submission, &overlay_, overlayPixels, overlayWidth, overlayHeight);
    }
    const bool shared = sharedGame != nullptr && importSharedGame(sharedGame);
    const uint32_t family = context_.queueFamily();
    BlitParams params{};
    params.flags = target.srgb ? kBlitDecodeSrgb : 0u;
    VkImageView source = VK_NULL_HANDLE;
    VkImageView overlay = VK_NULL_HANDLE;
    if (shared) {
        // The app's GL renderer writes this buffer; acquire it from the foreign queue per draw.
        context_.barrier(submission->command, sharedGame_.image, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_ACCESS_SHADER_READ_BIT, 0, 1,
                         VK_QUEUE_FAMILY_FOREIGN_EXT, family);
        params.mode = 1;
        params.contentScale[0] = contentScaleX;
        params.contentScale[1] = contentScaleY;
        source = sharedGame_.view;
        overlay = overlay_.view;
    } else if (overlay_.image != VK_NULL_HANDLE) {
        source = overlay_.view;
    }
    context_.blit(submission->command, target.views[imageIndex], {target.width, target.height}, target.format,
                  source, overlay, params);
    if (shared) {
        context_.barrier(submission->command, sharedGame_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_SHADER_READ_BIT, 0, 0, 1, family,
                         VK_QUEUE_FAMILY_FOREIGN_EXT);
    }
    return context_.submit(submission);
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
    return context_.submit(submission);
}

}  // namespace xrimmersive::vulkan
