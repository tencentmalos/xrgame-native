#include "xr_vulkan_probe.h"

#include "spatial/upscale/VulkanUpscaler.h"

#include <android/log.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <string>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "xrprobe", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "xrprobe", __VA_ARGS__)

namespace xrimmersive::vulkan {

namespace {

constexpr uint32_t kGuardWord = 0xcdcdcdcdu;
// Rows past the end of the image the guard covers; the writes seen in the field reach row ~3100.
constexpr uint32_t kGuardRows = 4096;

struct GuardedImage {
    Image image{};
    VkDeviceSize imageSize = 0;
    VkDeviceSize guardSize = 0;
    uint32_t pitch = 0;
    uint8_t *mapped = nullptr;
};

// Pitch of a TILE6_3 RGBA8 level 0 in Turnip's layout (64-texel alignment).
uint32_t TiledPitch(uint32_t width) {
    return ((width + 63u) & ~63u) * 4u;
}

bool CreateGuarded(Context &context, uint32_t width, uint32_t height, VkFormat format, bool mutableFormat,
                   GuardedImage *out) {
    const Dispatch &vk = context.vk();
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = mutableFormat ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    GuardedImage guarded{};
    if (vk.vkCreateImage(context.device(), &info, nullptr, &guarded.image.image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vk.vkGetImageMemoryRequirements(context.device(), guarded.image.image, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vk.vkGetPhysicalDeviceMemoryProperties(context.physicalDevice(), &properties);
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount && type == UINT32_MAX; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            type = i;
        }
    }
    guarded.pitch = TiledPitch(width);
    guarded.imageSize = requirements.size;
    guarded.guardSize = static_cast<VkDeviceSize>(guarded.pitch) * kGuardRows;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = guarded.imageSize + guarded.guardSize;
    allocate.memoryTypeIndex = type;
    void *mapped = nullptr;
    if (type == UINT32_MAX ||
        vk.vkAllocateMemory(context.device(), &allocate, nullptr, &guarded.image.memory) != VK_SUCCESS ||
        vk.vkBindImageMemory(context.device(), guarded.image.image, guarded.image.memory, 0) != VK_SUCCESS ||
        vk.vkMapMemory(context.device(), guarded.image.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        LOGE("guard probe: %ux%u image setup failed (memory type %u)", width, height, type);
        context.destroyImage(&guarded.image);
        return false;
    }
    guarded.mapped = static_cast<uint8_t *>(mapped);
    std::memset(guarded.mapped + guarded.imageSize, 0xcd, guarded.guardSize);
    guarded.image.view = context.createView(guarded.image.image, format, 0);
    guarded.image.format = format;
    guarded.image.width = width;
    guarded.image.height = height;
    *out = guarded;
    return guarded.image.view != VK_NULL_HANDLE;
}

// Logs the changed guard words of `image` and restores the fill.
void ReportGuard(const char *name, uint32_t run, uint32_t variant, GuardedImage &image) {
    auto *words = reinterpret_cast<uint32_t *>(image.mapped + image.imageSize);
    const size_t count = image.guardSize / 4;
    size_t changed = 0;
    size_t first = 0;
    size_t last = 0;
    uint32_t minColumn = UINT32_MAX;
    uint32_t maxColumn = 0;
    std::map<uint32_t, size_t> values;
    for (size_t i = 0; i < count; ++i) {
        if (words[i] == kGuardWord) continue;
        if (changed == 0) first = i;
        last = i;
        ++changed;
        const VkDeviceSize offset = image.imageSize + i * 4;
        const uint32_t column = static_cast<uint32_t>((offset % image.pitch) / 4);
        minColumn = std::min(minColumn, column);
        maxColumn = std::max(maxColumn, column);
        if (values.size() < 6 || values.count(words[i]) != 0) ++values[words[i]];
        words[i] = kGuardWord;
    }
    if (changed == 0) {
        LOGI("guard probe run=%u variant=%u %s %ux%u: guard intact", run, variant, name, image.image.width,
             image.image.height);
        return;
    }
    const VkDeviceSize firstOffset = image.imageSize + first * 4;
    const VkDeviceSize lastOffset = image.imageSize + last * 4;
    std::string top;
    for (const auto &[value, hits] : values) {
        char item[40];
        std::snprintf(item, sizeof(item), " %08x:%zu", value, hits);
        top += item;
    }
    LOGE("guard probe run=%u variant=%u %s %ux%u pitch=%u size=0x%" PRIx64 ": %zu words changed, rows %" PRIu64
         "..%" PRIu64 " (offsets 0x%" PRIx64 "..0x%" PRIx64 "), columns %u..%u, values%s",
         run, variant, name, image.image.width, image.image.height, image.pitch, image.imageSize, changed,
         firstOffset / image.pitch, lastOffset / image.pitch, firstOffset, lastOffset, minColumn, maxColumn,
         top.c_str());
}

}  // namespace

void RunGuardProbe(Context &context, VkFormat format, uint32_t variant, uint32_t runs) {
    const bool large = (variant & kProbeLarge) != 0;
    GuardedImage first{};
    GuardedImage second{};
    Image pattern{};
    if (!CreateGuarded(context, large ? 2592 : 1296, large ? 2400 : 1200, format, (variant & kProbeMutable) != 0,
                       &first) ||
        !CreateGuarded(context, 2592, 2400, format, false, &second)) {
        context.destroyImage(&first.image);
        context.destroyImage(&second.image);
        return;
    }
    LOGI("guard probe: variant=%u first=%ux%u size=0x%" PRIx64 " second=%ux%u size=0x%" PRIx64 " format=%d", variant,
         first.image.width, first.image.height, first.imageSize, second.image.width, second.image.height,
         second.imageSize, format);
    std::array<uint8_t, 4 * 4 * 4> pixels{};
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i] = 0x11;
        pixels[i + 1] = 0x22;
        pixels[i + 2] = 0x33;
        pixels[i + 3] = 0xff;
    }
    for (uint32_t run = 0; run < runs; ++run) {
        Submission *submission = context.begin();
        if (submission == nullptr || !context.uploadRgba(submission, &pattern, pixels.data(), 4, 4)) {
            if (submission != nullptr) context.submit(submission);
            break;
        }
        VkCommandBuffer command = submission->command;
        const VkImageLayout firstFinal = (variant & kProbeCopy) ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        context.barrier(command, first.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        context.blit(command, first.image.view, {first.image.width, first.image.height}, format, pattern.view,
                     VK_NULL_HANDLE, BlitParams{});
        context.barrier(command, first.image.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, firstFinal,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        (variant & kProbeCopy) ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        (variant & kProbeCopy) ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_READ_BIT);
        if ((variant & kProbeRenderOnly) == 0) {
            if (variant & kProbeCopy) {
                context.barrier(command, second.image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
                VkImageCopy copy{};
                copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.extent = {first.image.width, first.image.height, 1};
                context.vk().vkCmdCopyImage(command, first.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                            second.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            } else {
                context.barrier(command, second.image.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                BlitParams decode{};
                decode.flags = kBlitDecodeSrgb;
                context.blit(command, second.image.view, {second.image.width, second.image.height}, format,
                             first.image.view, VK_NULL_HANDLE, decode);
            }
        }
        if (!context.submit(submission) || !context.waitSerial(submission->serial)) break;
        ReportGuard("first", run, variant, first);
        ReportGuard("second", run, variant, second);
    }
    context.waitIdle();
    context.destroyImage(&pattern);
    context.destroyImage(&first.image);
    context.destroyImage(&second.image);
}

namespace {

struct Readback {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const uint8_t *bytes = nullptr;
};

bool CreateReadback(Context &context, VkDeviceSize size, Readback *out) {
    const Dispatch &vk = context.vk();
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk.vkCreateBuffer(context.device(), &info, nullptr, &out->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vk.vkGetBufferMemoryRequirements(context.device(), out->buffer, &requirements);
    VkPhysicalDeviceMemoryProperties properties{};
    vk.vkGetPhysicalDeviceMemoryProperties(context.physicalDevice(), &properties);
    const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount && type == UINT32_MAX; ++i) {
        if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            type = i;
        }
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    void *mapped = nullptr;
    if (type == UINT32_MAX || vk.vkAllocateMemory(context.device(), &allocate, nullptr, &out->memory) != VK_SUCCESS ||
        vk.vkBindBufferMemory(context.device(), out->buffer, out->memory, 0) != VK_SUCCESS ||
        vk.vkMapMemory(context.device(), out->memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        return false;
    }
    out->bytes = static_cast<const uint8_t *>(mapped);
    return true;
}

void DestroyReadback(Context &context, Readback *readback) {
    if (readback->buffer != VK_NULL_HANDLE) context.vk().vkDestroyBuffer(context.device(), readback->buffer, nullptr);
    if (readback->memory != VK_NULL_HANDLE) context.vk().vkFreeMemory(context.device(), readback->memory, nullptr);
    *readback = {};
}

void CopyToReadback(Context &context, VkCommandBuffer command, VkImage image, VkImageLayout layout,
                    VkPipelineStageFlags stage, VkAccessFlags access, VkExtent2D extent, const Readback &readback) {
    context.barrier(command, image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    access, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {extent.width, extent.height, 1};
    context.vk().vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
                                        &copy);
}

}  // namespace

void RunUpscaleEquivalenceProbe(Context &context, VkFormat format) {
    constexpr uint32_t kAtlasWidth = 3670, kAtlasHeight = 1699, kEyeWidth = 1296, kEyeHeight = 1200;
    constexpr uint32_t kOutWidth = 2592, kOutHeight = 2400;
    // Detail that drives SGSR's edge path inside both eyes, and a colour outside them that must
    // never reach a reconstructed eye (the other eye is a third distinct pattern).
    std::vector<uint8_t> pixels(size_t(kAtlasWidth) * kAtlasHeight * 4);
    for (uint32_t y = 0; y < kAtlasHeight; ++y) {
        for (uint32_t x = 0; x < kAtlasWidth; ++x) {
            uint8_t *p = &pixels[(size_t(y) * kAtlasWidth + x) * 4];
            const uint32_t hash = (x * 73856093u) ^ (y * 19349663u);
            if (x >= 2 * kEyeWidth || y >= kEyeHeight) {
                p[0] = 255, p[1] = 0, p[2] = 255;
            } else if (x < kEyeWidth) {
                p[0] = uint8_t(x / 6), p[1] = ((x / 9 + y / 7) & 1) ? 210 : 30, p[2] = uint8_t(hash >> 24);
            } else {
                p[0] = uint8_t(hash >> 16), p[1] = ((x / 5 + y / 11) & 1) ? 40 : 190, p[2] = uint8_t(y / 5);
            }
            p[3] = 255;
        }
    }
    spatial::upscale::Binding binding{};
    binding.vulkan.instance = context.instance();
    binding.vulkan.physical_device = context.physicalDevice();
    binding.vulkan.device = context.device();
    binding.vulkan.vk_get_instance_proc_addr = context.vk().vkGetInstanceProcAddr;
    binding.vulkan.vk_get_device_proc_addr = context.vk().vkGetDeviceProcAddr;
    binding.vulkan.queue_submit_mutex = &context.queueMutex();
    binding.enabled_fdm = context.fdm();
    Image atlas{};
    Image target{};
    Readback reference{};
    Readback direct{};
    const VkDeviceSize bytes = VkDeviceSize(kOutWidth) * kOutHeight * 4;
    try {
        spatial::upscale::VulkanUpscaler upscaler(binding);
        if (!context.createImage(kOutWidth, kOutHeight, format,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &target) ||
            !CreateReadback(context, bytes, &reference) || !CreateReadback(context, bytes, &direct)) {
            LOGE("upscale probe: setup failed");
        } else {
            for (uint32_t eye = 0; eye < 2; ++eye) {
                for (const bool foveate : {false, true}) {
                    Submission *submission = context.begin();
                    if (submission == nullptr) break;
                    if (atlas.image == VK_NULL_HANDLE &&
                        !context.uploadRgba(submission, &atlas, pixels.data(), kAtlasWidth, kAtlasHeight)) {
                        context.submit(submission);
                        break;
                    }
                    VkCommandBuffer command = submission->command;
                    const std::array<float, 4> uv{float(kEyeWidth) / kAtlasWidth, float(kEyeHeight) / kAtlasHeight,
                                                  float(eye * kEyeWidth) / kAtlasWidth, 0.0f};
                    spatial::upscale::FoveatedEye fov{};
                    fov.center = {0.5f, 0.5f};
                    fov.profile = {.inner_x = .3f, .inner_y = .3f, .outer_x = .7f, .outer_y = .7f,
                                   .mid_rate = 4, .outer_rate = 16};
                    fov.enabled = fov.tracked = foveate;
                    const spatial::upscale::Options options{spatial::upscale::Filter::Sgsr1, 50, false};
                    const auto out = upscaler.Render(command, 0, atlas.view, {kAtlasWidth, kAtlasHeight}, uv,
                                                     {kOutWidth, kOutHeight}, options, fov, true);
                    const spatial::upscale::Target layer{target.view, format, {kOutWidth, kOutHeight},
                                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
                    const bool drawn = upscaler.RenderTo(command, 1, atlas.view, {kAtlasWidth, kAtlasHeight}, uv,
                                                         layer, options, fov, true);
                    if (out.image == VK_NULL_HANDLE || !drawn) {
                        LOGE("upscale probe: eye=%u foveate=%d render=%d direct=%d", eye, foveate,
                             out.image != VK_NULL_HANDLE, drawn);
                        context.submit(submission);
                        break;
                    }
                    CopyToReadback(context, command, out.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, {kOutWidth, kOutHeight}, reference);
                    CopyToReadback(context, command, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                   {kOutWidth, kOutHeight}, direct);
                    if (!context.submit(submission) || !context.waitSerial(submission->serial)) break;
                    uint64_t differing = 0, beyondOne = 0, leaked = 0;
                    int maxDiff = 0;
                    int64_t first = -1;
                    for (VkDeviceSize i = 0; i < bytes; ++i) {
                        const int diff = std::abs(int(reference.bytes[i]) - int(direct.bytes[i]));
                        if (diff == 0) continue;
                        ++differing;
                        if (diff > 1) ++beyondOne;
                        if (first < 0) first = int64_t(i);
                        maxDiff = std::max(maxDiff, diff);
                    }
                    for (VkDeviceSize i = 0; i < bytes; i += 4) {
                        leaked += direct.bytes[i] > 240 && direct.bytes[i + 1] < 15 && direct.bytes[i + 2] > 240;
                    }
                    const auto stats = upscaler.Stats();
                    LOGI("upscale probe eye=%u foveate=%d format=%d: %" PRIu64 " of %" PRIu64
                         " bytes differ (%" PRIu64 " by more than 1, max %d, first at pixel %" PRId64
                         "), outside-colour pixels %" PRIu64 ", fdm draws %" PRIu64,
                         eye, foveate, format, differing, uint64_t(bytes), beyondOne, maxDiff,
                         first < 0 ? int64_t(-1) : first / 4, leaked, stats.fdm_draws);
                }
            }
        }
        context.waitIdle();
        upscaler.ReleaseTargets();
    } catch (const std::exception &error) {
        LOGE("upscale probe: %s", error.what());
        context.waitIdle();
    }
    DestroyReadback(context, &reference);
    DestroyReadback(context, &direct);
    context.destroyImage(&target);
    context.destroyImage(&atlas);
}

}  // namespace xrimmersive::vulkan
