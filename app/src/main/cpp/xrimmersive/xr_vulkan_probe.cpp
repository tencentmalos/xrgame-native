#include "xr_vulkan_probe.h"

#include <android/log.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

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

}  // namespace xrimmersive::vulkan
