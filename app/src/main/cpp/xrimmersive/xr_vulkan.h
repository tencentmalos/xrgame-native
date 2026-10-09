#pragma once

// Vulkan backend of the immersive session (Windows VR): the app-process Turnip is loaded through
// adrenotools and handed to the OpenXR runtime with XR_KHR_vulkan_enable2, so the runtime creates
// the instance and device. Recording and submission happen on the XR thread; a completion thread
// retires submissions as soon as their fences signal.

#include "spatial/foveation/vulkan/FragmentDensityMap.h"

#include <android/hardware_buffer.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <openxr/openxr.h>

namespace xrimmersive::vulkan {

#define XRIM_VK_INSTANCE_FUNCTIONS(X)            \
    X(vkDestroyInstance)                         \
    X(vkGetDeviceProcAddr)                       \
    X(vkGetPhysicalDeviceProperties)             \
    X(vkGetPhysicalDeviceMemoryProperties)       \
    X(vkGetPhysicalDeviceQueueFamilyProperties)  \
    X(vkEnumerateDeviceExtensionProperties)

#define XRIM_VK_DEVICE_FUNCTIONS(X)                   \
    X(vkDestroyDevice)                                \
    X(vkGetDeviceQueue)                               \
    X(vkDeviceWaitIdle)                               \
    X(vkCreateCommandPool)                            \
    X(vkDestroyCommandPool)                           \
    X(vkAllocateCommandBuffers)                       \
    X(vkBeginCommandBuffer)                           \
    X(vkEndCommandBuffer)                             \
    X(vkResetCommandBuffer)                           \
    X(vkQueueSubmit)                                  \
    X(vkCreateFence)                                  \
    X(vkDestroyFence)                                 \
    X(vkWaitForFences)                                \
    X(vkResetFences)                                  \
    X(vkCreateImage)                                  \
    X(vkDestroyImage)                                 \
    X(vkCreateImageView)                              \
    X(vkDestroyImageView)                             \
    X(vkGetImageMemoryRequirements)                   \
    X(vkAllocateMemory)                               \
    X(vkFreeMemory)                                   \
    X(vkBindImageMemory)                              \
    X(vkCreateBuffer)                                 \
    X(vkDestroyBuffer)                                \
    X(vkGetBufferMemoryRequirements)                  \
    X(vkBindBufferMemory)                             \
    X(vkMapMemory)                                    \
    X(vkCreateSampler)                                \
    X(vkDestroySampler)                               \
    X(vkCreateShaderModule)                           \
    X(vkDestroyShaderModule)                          \
    X(vkCreateDescriptorSetLayout)                    \
    X(vkDestroyDescriptorSetLayout)                   \
    X(vkCreatePipelineLayout)                         \
    X(vkDestroyPipelineLayout)                        \
    X(vkCreateGraphicsPipelines)                      \
    X(vkDestroyPipeline)                              \
    X(vkCmdPipelineBarrier)                           \
    X(vkCmdCopyBufferToImage)                         \
    X(vkCmdCopyImage)                                 \
    X(vkCmdClearColorImage)                           \
    X(vkCmdBeginRendering)                            \
    X(vkCmdEndRendering)                              \
    X(vkCmdBindPipeline)                              \
    X(vkCmdPushConstants)                             \
    X(vkCmdPushDescriptorSetKHR)                      \
    X(vkCmdSetViewport)                               \
    X(vkCmdSetScissor)                                \
    X(vkCmdDraw)                                      \
    X(vkGetAndroidHardwareBufferPropertiesANDROID)

struct Dispatch {
#define XRIM_VK_DECLARE(name) PFN_##name name = nullptr;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    XRIM_VK_INSTANCE_FUNCTIONS(XRIM_VK_DECLARE)
    XRIM_VK_DEVICE_FUNCTIONS(XRIM_VK_DECLARE)
#undef XRIM_VK_DECLARE
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// Push constants of shaders/xr_blit.frag.
struct BlitParams {
    float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    float contentScale[2] = {1.0f, 1.0f};
    uint32_t mode = 0;
    uint32_t flags = 0;
};
constexpr uint32_t kBlitDecodeSrgb = 1u;
constexpr uint32_t kBlitOpaque = 2u;

// One command buffer per submission, recycled once its fence has signalled. Work that must wait
// for the GPU (returning game images to the producer) is attached to the submission and runs on
// the completion thread.
struct Submission {
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    uint32_t slot = 0;
    uint64_t serial = 0;
    bool pending = false;
    std::vector<std::function<void()>> onComplete;
};

class Context {
public:
    // Loads libraryName from driverDir through adrenotools (hooks from hookDir). Once per process:
    // adrenotools keeps its namespace for the process lifetime. Returns null on failure.
    static PFN_vkGetInstanceProcAddr LoadDriver(const std::string &driverDir, const std::string &libraryName,
                                                const std::string &hookDir);

    bool create(XrInstance instance, XrSystemId system, PFN_vkGetInstanceProcAddr getInstanceProcAddr);
    void destroy();

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }
    uint32_t queueFamily() const { return queueFamily_; }
    const Dispatch &vk() const { return vk_; }
    std::mutex &queueMutex() { return queueMutex_; }
    // FDM features enabled on the device (empty when the device cannot foveate external targets).
    const spatial::foveation::vulkan::Capabilities &fdm() const { return fdm_; }
    bool lost() const { return lost_.load(); }

    // Starts a command buffer, waiting only when every submission in the ring is still in flight.
    Submission *begin();
    bool submit(Submission *submission);
    // Blocks until the submission that received `serial` has completed (submissions retire in order).
    bool waitSerial(uint64_t serial);
    void waitIdle();
    static constexpr uint32_t kSubmissionCount = 6;

    bool importHardwareBuffer(AHardwareBuffer *buffer, bool swapRedBlue, Image *out);
    bool createImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, Image *out);
    void destroyImage(Image *image);
    VkImageView createView(VkImage image, VkFormat format, uint32_t layer);
    void destroyView(VkImageView view);
    // Memory the driver expects behind an image it did not allocate (runtime swapchain images).
    VkMemoryRequirements imageRequirements(VkImage image);
    // Records a staged copy of tightly packed RGBA8 pixels into `image` and leaves it
    // SHADER_READ_ONLY_OPTIMAL. The staging memory belongs to `submission`.
    bool uploadRgba(Submission *submission, Image *image, const uint8_t *pixels, uint32_t width,
                    uint32_t height);

    void barrier(VkCommandBuffer command, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                 VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage, VkAccessFlags srcAccess,
                 VkAccessFlags dstAccess, uint32_t baseLayer = 0, uint32_t layerCount = 1,
                 uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED, uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED);

    // Draws a fullscreen pass of shaders/xr_blit into `target` (a color attachment view in
    // COLOR_ATTACHMENT_OPTIMAL). A null source only clears.
    void blit(VkCommandBuffer command, VkImageView target, VkExtent2D extent, VkFormat colorFormat,
              VkImageView source, VkImageView overlay, const BlitParams &params);

private:
    bool loadInstanceFunctions();
    bool loadDeviceFunctions();
    bool createBlitResources();
    bool ensureBlitPipeline(VkFormat colorFormat);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const;
    void completionLoop();

    Dispatch vk_{};
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT addressReport_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::array<Submission, kSubmissionCount> submissions_{};
    uint32_t next_ = 0;
    std::mutex queueMutex_;
    spatial::foveation::vulkan::Capabilities fdm_{};
    std::atomic<bool> lost_{false};

    std::mutex completionMutex_;
    std::condition_variable completionCv_;
    std::deque<Submission *> inFlight_;
    uint64_t submittedSerial_ = 0;
    uint64_t completedSerial_ = 0;
    std::thread completionThread_;
    bool stopping_ = false;

    struct Staging {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void *mapped = nullptr;
        VkDeviceSize size = 0;
        VkDeviceSize used = 0;
    };
    std::array<Staging, kSubmissionCount> staging_{};

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    struct Pipeline {
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };
    std::array<Pipeline, 2> pipelines_{};
    Image placeholder_{};
};

}  // namespace xrimmersive::vulkan
