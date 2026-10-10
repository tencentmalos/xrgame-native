#include "xr_vulkan.h"

#include "xr_vulkan_trace.h"

#include <EGL/egl.h>
#include <jni.h>
#include <openxr/openxr_platform.h>

#include <adrenotools/driver.h>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "xrimmersive", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "xrimmersive", __VA_ARGS__)

namespace xrimmersive::vulkan {

namespace {

#include "xr_vulkan_shaders.inc"

std::mutex gDriverMutex;
bool gDriverAttempted = false;
std::string gDriverKey;
PFN_vkGetInstanceProcAddr gDriverEntry = nullptr;

std::string WithSlash(std::string path) {
    if (!path.empty() && path.back() != '/') path.push_back('/');
    return path;
}

bool HasExtension(const std::vector<VkExtensionProperties> &extensions, const char *name) {
    return std::any_of(extensions.begin(), extensions.end(),
                       [name](const VkExtensionProperties &e) { return std::strcmp(e.extensionName, name) == 0; });
}

template <typename T>
bool LoadXr(XrInstance instance, const char *name, T *out) {
    *out = nullptr;
    return XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction *>(out))) &&
           *out != nullptr;
}

}  // namespace

PFN_vkGetInstanceProcAddr Context::LoadDriver(const std::string &bundledDir, const std::string &libraryName,
                                              const std::string &hookDir) {
    std::lock_guard<std::mutex> lock(gDriverMutex);
    // Diagnostic override (debug.xrgame.xr.turnipdir): a driver directory the runtime check
    // before each launch does not restore, for testing a locally built Turnip.
    std::string driverDir = bundledDir;
    char overrideDir[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.xrgame.xr.turnipdir", overrideDir) > 0 &&
        access((WithSlash(overrideDir) + libraryName).c_str(), R_OK) == 0) {
        LOGI("vulkan composite: driver directory overridden by debug.xrgame.xr.turnipdir=%s", overrideDir);
        driverDir = overrideDir;
    }
    const std::string key = WithSlash(driverDir) + libraryName;
    if (gDriverAttempted) {
        if (key != gDriverKey) {
            LOGE("vulkan composite: driver %s requested after %s; a different driver needs an app restart",
                 key.c_str(), gDriverKey.c_str());
            return nullptr;
        }
        return gDriverEntry;
    }
    // The runtime installs the driver when a game launches; a session started before that falls
    // back to GLES without using up this process's one adrenotools load.
    if (access(key.c_str(), R_OK) != 0) {
        LOGE("vulkan composite: driver %s not installed yet", key.c_str());
        return nullptr;
    }
    gDriverAttempted = true;
    gDriverKey = key;
    const std::string driver = WithSlash(driverDir);
    const std::string hooks = WithSlash(hookDir);
    void *handle = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, nullptr,
                                              hooks.c_str(), driver.c_str(), libraryName.c_str(), nullptr,
                                              nullptr);
    if (handle == nullptr) {
        LOGE("vulkan composite: adrenotools could not load %s%s: %s", driver.c_str(), libraryName.c_str(),
             dlerror());
        return nullptr;
    }
    gDriverEntry = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
    LOGI("vulkan composite: loaded %s%s entry=%p", driver.c_str(), libraryName.c_str(),
         reinterpret_cast<void *>(gDriverEntry));
    return gDriverEntry;
}

bool Context::loadInstanceFunctions() {
#define XRIM_VK_LOAD(name)                                                                           \
    vk_.name = reinterpret_cast<PFN_##name>(vk_.vkGetInstanceProcAddr(instance_, #name));          \
    if (vk_.name == nullptr) {                                                                       \
        LOGE("vulkan composite: missing instance function %s", #name);                              \
        return false;                                                                                \
    }
    XRIM_VK_INSTANCE_FUNCTIONS(XRIM_VK_LOAD)
#undef XRIM_VK_LOAD
    return true;
}

bool Context::loadDeviceFunctions() {
#define XRIM_VK_LOAD(name)                                                                     \
    vk_.name = reinterpret_cast<PFN_##name>(vk_.vkGetDeviceProcAddr(device_, #name));        \
    if (vk_.name == nullptr) {                                                                 \
        LOGE("vulkan composite: missing device function %s", #name);                          \
        return false;                                                                          \
    }
    XRIM_VK_DEVICE_FUNCTIONS(XRIM_VK_LOAD)
#undef XRIM_VK_LOAD
    return true;
}

bool Context::create(XrInstance xrInstance, XrSystemId system, PFN_vkGetInstanceProcAddr getInstanceProcAddr) {
    vk_.vkGetInstanceProcAddr = getInstanceProcAddr;
    PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR createInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getDevice = nullptr;
    PFN_xrCreateVulkanDeviceKHR createDevice = nullptr;
    if (!LoadXr(xrInstance, "xrGetVulkanGraphicsRequirements2KHR", &getRequirements) ||
        !LoadXr(xrInstance, "xrCreateVulkanInstanceKHR", &createInstance) ||
        !LoadXr(xrInstance, "xrGetVulkanGraphicsDevice2KHR", &getDevice) ||
        !LoadXr(xrInstance, "xrCreateVulkanDeviceKHR", &createDevice)) {
        LOGE("vulkan composite: XR_KHR_vulkan_enable2 entry points missing");
        return false;
    }
    // The runtime requires this query before the session is created.
    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (XR_FAILED(getRequirements(xrInstance, system, &requirements))) return false;
    if (requirements.minApiVersionSupported > XR_MAKE_VERSION(1, 3, 0)) {
        LOGE("vulkan composite: runtime needs Vulkan %u.%u", XR_VERSION_MAJOR(requirements.minApiVersionSupported),
             XR_VERSION_MINOR(requirements.minApiVersionSupported));
        return false;
    }

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "SteamPSP XR composite";
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    const bool trace = TraceEnabled();
    const char *debugUtils = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (trace) {
        instanceInfo.enabledExtensionCount = 1;
        instanceInfo.ppEnabledExtensionNames = &debugUtils;
    }
    XrVulkanInstanceCreateInfoKHR xrInstanceInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xrInstanceInfo.systemId = system;
    xrInstanceInfo.pfnGetInstanceProcAddr = getInstanceProcAddr;
    xrInstanceInfo.vulkanCreateInfo = &instanceInfo;
    VkResult vkResult = VK_SUCCESS;
    if (XR_FAILED(createInstance(xrInstance, &xrInstanceInfo, &instance_, &vkResult)) || vkResult != VK_SUCCESS) {
        LOGE("vulkan composite: xrCreateVulkanInstanceKHR failed (vk %d)", vkResult);
        instance_ = VK_NULL_HANDLE;
        return false;
    }
    if (!loadInstanceFunctions()) return false;
    if (trace) addressReport_ = CreateAddressReport(instance_, getInstanceProcAddr);

    XrVulkanGraphicsDeviceGetInfoKHR deviceGet{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    deviceGet.systemId = system;
    deviceGet.vulkanInstance = instance_;
    if (XR_FAILED(getDevice(xrInstance, &deviceGet, &physicalDevice_))) return false;
    vk_.vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memory_);
    VkPhysicalDeviceProperties properties{};
    vk_.vkGetPhysicalDeviceProperties(physicalDevice_, &properties);

    uint32_t familyCount = 0;
    vk_.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vk_.vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &familyCount, families.data());
    queueFamily_ = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamily_ = i;
            break;
        }
    }
    if (queueFamily_ == UINT32_MAX) return false;

    uint32_t extensionCount = 0;
    vk_.vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> available(extensionCount);
    vk_.vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount, available.data());
    std::vector<const char *> extensions;
    for (const char *name : {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                             VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
                             VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME}) {
        if (!HasExtension(available, name)) {
            LOGE("vulkan composite: device lacks %s", name);
            return false;
        }
        extensions.push_back(name);
    }

    // FDM for the upscale passes: the device enables exactly what the probe reports.
    vk::detail::DispatchLoaderDynamic loader;
    loader.init(vk::Instance{instance_}, getInstanceProcAddr);
    const auto probed = spatial::foveation::vulkan::ProbeCapabilities(vk::PhysicalDevice{physicalDevice_}, loader);
    std::array<const char *, 3> fdmNames{};
    if (probed.UsableOnExternalTargets()) {
        fdm_ = probed;
        const uint32_t count = spatial::foveation::vulkan::RequiredDeviceExtensions(fdm_, fdmNames);
        extensions.insert(extensions.end(), fdmNames.begin(), fdmNames.begin() + count);
    }
    auto fdmFeatures = spatial::foveation::vulkan::MakeDeviceFeatures(fdm_);
    auto offsetFeatures = spatial::foveation::vulkan::MakeOffsetDeviceFeatures(fdm_);
    if (fdm_.qcom_offset) fdmFeatures.pNext = &offsetFeatures;

    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    if (fdm_.fragment_density_map) features13.pNext = &fdmFeatures;
    VkPhysicalDeviceFeatures2 enabled{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enabled.pNext = &features13;
    VkPhysicalDeviceAddressBindingReportFeaturesEXT addressFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT};
    if (addressReport_ != VK_NULL_HANDLE &&
        HasExtension(available, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME)) {
        extensions.push_back(VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);
        addressFeatures.reportAddressBinding = VK_TRUE;
        addressFeatures.pNext = enabled.pNext;
        enabled.pNext = &addressFeatures;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &enabled;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    deviceInfo.ppEnabledExtensionNames = extensions.data();
    XrVulkanDeviceCreateInfoKHR xrDeviceInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xrDeviceInfo.systemId = system;
    xrDeviceInfo.pfnGetInstanceProcAddr = getInstanceProcAddr;
    xrDeviceInfo.vulkanPhysicalDevice = physicalDevice_;
    xrDeviceInfo.vulkanCreateInfo = &deviceInfo;
    if (XR_FAILED(createDevice(xrInstance, &xrDeviceInfo, &device_, &vkResult)) || vkResult != VK_SUCCESS) {
        LOGE("vulkan composite: xrCreateVulkanDeviceKHR failed (vk %d)", vkResult);
        device_ = VK_NULL_HANDLE;
        return false;
    }
    if (!loadDeviceFunctions()) return false;
    vk_.vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    if (vk_.vkCreateCommandPool(device_, &poolInfo, nullptr, &pool_) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < kSubmissionCount; ++i) {
        Submission &submission = submissions_[i];
        submission.slot = i;
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = pool_;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        if (vk_.vkAllocateCommandBuffers(device_, &allocate, &submission.command) != VK_SUCCESS) return false;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vk_.vkCreateFence(device_, &fenceInfo, nullptr, &submission.fence) != VK_SUCCESS) return false;
    }
    stopping_ = false;
    completionThread_ = std::thread([this] { completionLoop(); });
    if (!createBlitResources()) return false;

    LOGI("vulkan composite: %s api %u.%u.%u (runtime tested %u.%u-%u.%u), FDM %s dynamic=%d offset=%d texel %u-%u",
         properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
         VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion),
         XR_VERSION_MAJOR(requirements.minApiVersionSupported), XR_VERSION_MINOR(requirements.minApiVersionSupported),
         XR_VERSION_MAJOR(requirements.maxApiVersionSupported), XR_VERSION_MINOR(requirements.maxApiVersionSupported),
         fdm_.UsableOnExternalTargets() ? "enabled" : (probed.extension_available ? "unusable" : "absent"),
         fdm_.fragment_density_map_dynamic ? 1 : 0, fdm_.qcom_offset ? 1 : 0, fdm_.MinTexel(), fdm_.MaxTexel());
    return true;
}

bool Context::createBlitResources() {
    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.0f;
    if (vk_.vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) return false;
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[i].pImmutableSamplers = &sampler_;
    }
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    setInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    setInfo.pBindings = bindings.data();
    if (vk_.vkCreateDescriptorSetLayout(device_, &setInfo, nullptr, &setLayout_) != VK_SUCCESS) return false;
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BlitParams)};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vk_.vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_) != VK_SUCCESS) return false;

    if (!createImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                     &placeholder_)) {
        return false;
    }
    Submission *init = begin();
    if (init == nullptr) return false;
    barrier(init->command, placeholder_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkClearColorValue transparent{{0.0f, 0.0f, 0.0f, 0.0f}};
    const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk_.vkCmdClearColorImage(init->command, placeholder_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &transparent,
                             1, &whole);
    barrier(init->command, placeholder_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    placeholder_.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return submit(init);
}

void Context::destroy() {
    if (device_ != VK_NULL_HANDLE) {
        waitIdle();
        {
            std::lock_guard<std::mutex> lock(completionMutex_);
            stopping_ = true;
        }
        completionCv_.notify_all();
        if (completionThread_.joinable()) completionThread_.join();
        destroyImage(&placeholder_);
        for (auto &staging : staging_) {
            if (staging.buffer != VK_NULL_HANDLE) vk_.vkDestroyBuffer(device_, staging.buffer, nullptr);
            if (staging.memory != VK_NULL_HANDLE) vk_.vkFreeMemory(device_, staging.memory, nullptr);
            staging = {};
        }
        for (auto &pipeline : pipelines_) {
            if (pipeline.pipeline != VK_NULL_HANDLE) vk_.vkDestroyPipeline(device_, pipeline.pipeline, nullptr);
            pipeline = {};
        }
        if (pipelineLayout_ != VK_NULL_HANDLE) vk_.vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        if (setLayout_ != VK_NULL_HANDLE) vk_.vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
        if (sampler_ != VK_NULL_HANDLE) vk_.vkDestroySampler(device_, sampler_, nullptr);
        for (auto &submission : submissions_) {
            if (submission.fence != VK_NULL_HANDLE) vk_.vkDestroyFence(device_, submission.fence, nullptr);
            submission = {};
        }
        if (pool_ != VK_NULL_HANDLE) vk_.vkDestroyCommandPool(device_, pool_, nullptr);
        vk_.vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE && vk_.vkGetInstanceProcAddr != nullptr) {
        DestroyAddressReport(instance_, vk_.vkGetInstanceProcAddr, addressReport_);
    }
    addressReport_ = VK_NULL_HANDLE;
    if (instance_ != VK_NULL_HANDLE && vk_.vkDestroyInstance != nullptr) vk_.vkDestroyInstance(instance_, nullptr);
    pipelineLayout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
}

uint32_t Context::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    return UINT32_MAX;
}

// Retires submissions in order. Turnip on KGSL cannot export a sync fd for the producer, so the
// game's eye images are handed back here, right after the fence signals, instead of at the next
// XR frame.
void Context::completionLoop() {
    for (;;) {
        Submission *submission = nullptr;
        {
            std::unique_lock<std::mutex> lock(completionMutex_);
            completionCv_.wait(lock, [this] { return stopping_ || !inFlight_.empty(); });
            if (inFlight_.empty()) return;
            submission = inFlight_.front();
        }
        VkResult result;
        do {
            result = vk_.vkWaitForFences(device_, 1, &submission->fence, VK_TRUE, 100000000ull);
        } while (result == VK_TIMEOUT && !lost_.load());
        if (result != VK_SUCCESS && !lost_.exchange(true)) {
            LOGE("vulkan composite: fence wait failed (%d); device treated as lost", result);
        }
        auto callbacks = std::move(submission->onComplete);
        submission->onComplete.clear();
        for (auto &callback : callbacks) callback();
        {
            std::lock_guard<std::mutex> lock(completionMutex_);
            inFlight_.pop_front();
            completedSerial_ = submission->serial;
            submission->pending = false;
        }
        completionCv_.notify_all();
    }
}

Submission *Context::begin() {
    if (device_ == VK_NULL_HANDLE || lost_.load()) return nullptr;
    Submission &submission = submissions_[next_];
    next_ = (next_ + 1) % kSubmissionCount;
    {
        std::unique_lock<std::mutex> lock(completionMutex_);
        if (!completionCv_.wait_for(lock, std::chrono::seconds(2), [&submission] { return !submission.pending; })) {
            LOGE("vulkan composite: submission %u still pending after 2 s", submission.slot);
            lost_.store(true);
            return nullptr;
        }
    }
    staging_[submission.slot].used = 0;
    vk_.vkResetFences(device_, 1, &submission.fence);
    vk_.vkResetCommandBuffer(submission.command, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vk_.vkBeginCommandBuffer(submission.command, &beginInfo) != VK_SUCCESS) return nullptr;
    return &submission;
}

bool Context::submit(Submission *submission) {
    if (submission == nullptr) return false;
    VkResult result = vk_.vkEndCommandBuffer(submission->command);
    if (result == VK_SUCCESS) {
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = 1;
        info.pCommandBuffers = &submission->command;
        std::lock_guard<std::mutex> lock(queueMutex_);
        result = vk_.vkQueueSubmit(queue_, 1, &info, submission->fence);
    }
    if (result != VK_SUCCESS) {
        LOGE("vulkan composite: submit failed (%d)", result);
        if (result == VK_ERROR_DEVICE_LOST) lost_.store(true);
        auto callbacks = std::move(submission->onComplete);
        submission->onComplete.clear();
        for (auto &callback : callbacks) callback();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(completionMutex_);
        submission->serial = ++submittedSerial_;
        submission->pending = true;
        inFlight_.push_back(submission);
    }
    completionCv_.notify_all();
    return true;
}

bool Context::waitSerial(uint64_t serial) {
    std::unique_lock<std::mutex> lock(completionMutex_);
    if (completionCv_.wait_for(lock, std::chrono::seconds(2), [this, serial] { return completedSerial_ >= serial; })) {
        return true;
    }
    LOGE("vulkan composite: submission %llu not complete after 2 s", static_cast<unsigned long long>(serial));
    lost_.store(true);
    return false;
}

void Context::waitIdle() {
    if (device_ == VK_NULL_HANDLE) return;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        vk_.vkDeviceWaitIdle(device_);
    }
    std::unique_lock<std::mutex> lock(completionMutex_);
    completionCv_.wait_for(lock, std::chrono::seconds(2), [this] { return inFlight_.empty(); });
}

bool Context::importHardwareBuffer(AHardwareBuffer *buffer, bool swapRedBlue, Image *out) {
    VkAndroidHardwareBufferFormatPropertiesANDROID formatProperties{
        VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID properties{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    properties.pNext = &formatProperties;
    if (vk_.vkGetAndroidHardwareBufferPropertiesANDROID(device_, buffer, &properties) != VK_SUCCESS ||
        formatProperties.format == VK_FORMAT_UNDEFINED) {
        LOGE("vulkan composite: AHB properties unavailable (format %d)", formatProperties.format);
        return false;
    }
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(buffer, &desc);
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.pNext = &external;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = formatProperties.format;
    imageInfo.extent = {desc.width, desc.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = std::max(1u, desc.layers);
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Image image{};
    if (vk_.vkCreateImage(device_, &imageInfo, nullptr, &image.image) != VK_SUCCESS) return false;
    VkImportAndroidHardwareBufferInfoANDROID import{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    import.buffer = buffer;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.pNext = &import;
    dedicated.image = image.image;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.pNext = &dedicated;
    allocate.allocationSize = properties.allocationSize;
    allocate.memoryTypeIndex = memoryType(properties.memoryTypeBits, 0);
    if (allocate.memoryTypeIndex == UINT32_MAX ||
        vk_.vkAllocateMemory(device_, &allocate, nullptr, &image.memory) != VK_SUCCESS ||
        vk_.vkBindImageMemory(device_, image.image, image.memory, 0) != VK_SUCCESS) {
        LOGE("vulkan composite: AHB import failed (%ux%u format %d)", desc.width, desc.height,
             formatProperties.format);
        destroyImage(&image);
        return false;
    }
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = formatProperties.format;
    if (swapRedBlue) {
        viewInfo.components = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
                               VK_COMPONENT_SWIZZLE_A};
    }
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vk_.vkCreateImageView(device_, &viewInfo, nullptr, &image.view) != VK_SUCCESS) {
        destroyImage(&image);
        return false;
    }
    image.format = formatProperties.format;
    image.width = desc.width;
    image.height = desc.height;
    *out = image;
    return true;
}

bool Context::createImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, Image *out) {
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Image image{};
    if (vk_.vkCreateImage(device_, &imageInfo, nullptr, &image.image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vk_.vkGetImageMemoryRequirements(device_, image.image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocate.memoryTypeIndex == UINT32_MAX ||
        vk_.vkAllocateMemory(device_, &allocate, nullptr, &image.memory) != VK_SUCCESS ||
        vk_.vkBindImageMemory(device_, image.image, image.memory, 0) != VK_SUCCESS) {
        destroyImage(&image);
        return false;
    }
    image.view = createView(image.image, format, 0);
    if (image.view == VK_NULL_HANDLE) {
        destroyImage(&image);
        return false;
    }
    image.format = format;
    image.width = width;
    image.height = height;
    *out = image;
    return true;
}

void Context::destroyImage(Image *image) {
    if (image == nullptr || device_ == VK_NULL_HANDLE) return;
    if (image->view != VK_NULL_HANDLE) vk_.vkDestroyImageView(device_, image->view, nullptr);
    if (image->image != VK_NULL_HANDLE) vk_.vkDestroyImage(device_, image->image, nullptr);
    if (image->memory != VK_NULL_HANDLE) vk_.vkFreeMemory(device_, image->memory, nullptr);
    *image = {};
}

VkImageView Context::createView(VkImage image, VkFormat format, uint32_t layer) {
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vk_.vkCreateImageView(device_, &viewInfo, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    return view;
}

void Context::destroyView(VkImageView view) {
    if (view != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) vk_.vkDestroyImageView(device_, view, nullptr);
}

VkMemoryRequirements Context::imageRequirements(VkImage image) {
    VkMemoryRequirements requirements{};
    if (image != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vk_.vkGetImageMemoryRequirements(device_, image, &requirements);
    }
    return requirements;
}

bool Context::uploadRgba(Submission *submission, Image *image, const uint8_t *pixels, uint32_t width,
                         uint32_t height) {
    if (submission == nullptr || pixels == nullptr || width == 0 || height == 0) return false;
    if (image->image == VK_NULL_HANDLE || image->width != width || image->height != height) {
        if (image->image != VK_NULL_HANDLE) {
            waitIdle();
            destroyImage(image);
        }
        if (!createImage(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, image)) {
            return false;
        }
    }
    Staging &staging = staging_[submission->slot];
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;
    const VkDeviceSize offset = (staging.used + 15) & ~VkDeviceSize{15};
    if (staging.size < offset + bytes) {
        if (staging.used != 0) {
            LOGE("vulkan composite: staging for one submission exceeded; upload skipped");
            return false;
        }
        if (staging.buffer != VK_NULL_HANDLE) vk_.vkDestroyBuffer(device_, staging.buffer, nullptr);
        if (staging.memory != VK_NULL_HANDLE) vk_.vkFreeMemory(device_, staging.memory, nullptr);
        staging = {};
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = std::max<VkDeviceSize>(bytes * 2, 4u << 20);
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vk_.vkCreateBuffer(device_, &bufferInfo, nullptr, &staging.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vk_.vkGetBufferMemoryRequirements(device_, staging.buffer, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memoryType(requirements.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (allocate.memoryTypeIndex == UINT32_MAX ||
            vk_.vkAllocateMemory(device_, &allocate, nullptr, &staging.memory) != VK_SUCCESS ||
            vk_.vkBindBufferMemory(device_, staging.buffer, staging.memory, 0) != VK_SUCCESS ||
            vk_.vkMapMemory(device_, staging.memory, 0, VK_WHOLE_SIZE, 0, &staging.mapped) != VK_SUCCESS) {
            return false;
        }
        staging.size = bufferInfo.size;
    }
    std::memcpy(static_cast<uint8_t *>(staging.mapped) + offset, pixels, bytes);
    staging.used = offset + bytes;
    VkCommandBuffer command = submission->command;
    barrier(command, image->image, image->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vk_.vkCmdCopyBufferToImage(command, staging.buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
    barrier(command, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
    image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

void Context::barrier(VkCommandBuffer command, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                      VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                      VkAccessFlags dstAccess, uint32_t baseLayer, uint32_t layerCount, uint32_t srcFamily,
                      uint32_t dstFamily) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = srcFamily;
    barrier.dstQueueFamilyIndex = dstFamily;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, baseLayer, layerCount};
    vk_.vkCmdPipelineBarrier(command, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool Context::ensureBlitPipeline(VkFormat colorFormat) {
    for (const auto &pipeline : pipelines_) {
        if (pipeline.pipeline != VK_NULL_HANDLE && pipeline.format == colorFormat) return true;
    }
    Pipeline *slot = nullptr;
    for (auto &pipeline : pipelines_) {
        if (pipeline.pipeline == VK_NULL_HANDLE) {
            slot = &pipeline;
            break;
        }
    }
    if (slot == nullptr) {
        LOGE("vulkan composite: no blit pipeline slot for format %d", colorFormat);
        return false;
    }
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = sizeof(kXrBlitVert);
    moduleInfo.pCode = kXrBlitVert;
    if (vk_.vkCreateShaderModule(device_, &moduleInfo, nullptr, &vertex) != VK_SUCCESS) return false;
    moduleInfo.codeSize = sizeof(kXrBlitFrag);
    moduleInfo.pCode = kXrBlitFrag;
    if (vk_.vkCreateShaderModule(device_, &moduleInfo, nullptr, &fragment) != VK_SUCCESS) {
        vk_.vkDestroyShaderModule(device_, vertex, nullptr);
        return false;
    }
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat;
    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = pipelineLayout_;
    const VkResult result =
        vk_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &slot->pipeline);
    vk_.vkDestroyShaderModule(device_, vertex, nullptr);
    vk_.vkDestroyShaderModule(device_, fragment, nullptr);
    if (result != VK_SUCCESS) {
        slot->pipeline = VK_NULL_HANDLE;
        return false;
    }
    slot->format = colorFormat;
    return true;
}

void Context::blit(VkCommandBuffer command, VkImageView target, VkExtent2D extent, VkFormat colorFormat,
                   VkImageView source, VkImageView overlay, const BlitParams &params) {
    if (!ensureBlitPipeline(colorFormat)) return;
    VkPipeline pipeline = VK_NULL_HANDLE;
    for (const auto &candidate : pipelines_) {
        if (candidate.format == colorFormat) pipeline = candidate.pipeline;
    }
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = target;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vk_.vkCmdBeginRendering(command, &rendering);
    if (source != VK_NULL_HANDLE) {
        vk_.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                                  0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, extent};
        vk_.vkCmdSetViewport(command, 0, 1, &viewport);
        vk_.vkCmdSetScissor(command, 0, 1, &scissor);
        std::array<VkDescriptorImageInfo, 2> images{};
        images[0] = {VK_NULL_HANDLE, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[1] = {VK_NULL_HANDLE, overlay != VK_NULL_HANDLE ? overlay : placeholder_.view,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
        }
        vk_.vkCmdPushDescriptorSetKHR(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0,
                                      static_cast<uint32_t>(writes.size()), writes.data());
        vk_.vkCmdPushConstants(command, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BlitParams),
                               &params);
        vk_.vkCmdDraw(command, 3, 1, 0, 0);
    }
    vk_.vkCmdEndRendering(command);
}

}  // namespace xrimmersive::vulkan
