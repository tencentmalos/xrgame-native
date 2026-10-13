#include "xr_frame_generation.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <set>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "xrimmersive", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "xrimmersive", __VA_ARGS__)

namespace xrimmersive::vulkan {

namespace fg = spatial::lossless_fg;

namespace {

VkDescriptorType DescriptorType(fg::DescriptorKind kind) {
    switch (kind) {
    case fg::DescriptorKind::UniformBuffer: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case fg::DescriptorKind::Sampler: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case fg::DescriptorKind::SampledImage: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case fg::DescriptorKind::StorageImage: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

VkFormat ImageFormat(fg::ImageFormat format) {
    switch (format) {
    case fg::ImageFormat::R8Unorm: return VK_FORMAT_R8_UNORM;
    case fg::ImageFormat::Rgba16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case fg::ImageFormat::ExternalColor:
    case fg::ImageFormat::Rgba8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

// Any hazard between the dispatches of one list: a read or write of something written since the
// last barrier, or a write of something read since then.
void MarkBarriers(const std::vector<fg::Dispatch> &source, std::vector<bool> *barriers) {
    std::set<std::string> written;
    std::set<std::string> read;
    barriers->assign(source.size(), false);
    for (size_t index = 0; index < source.size(); ++index) {
        bool hazard = false;
        for (const fg::DescriptorBinding &binding : source[index].bindings) {
            if (binding.resource.empty()) continue;
            if (binding.access == fg::ResourceAccess::SampledRead && written.count(binding.resource)) hazard = true;
            if (binding.access == fg::ResourceAccess::StorageWrite &&
                (written.count(binding.resource) || read.count(binding.resource))) {
                hazard = true;
            }
        }
        if (hazard) {
            (*barriers)[index] = true;
            written.clear();
            read.clear();
        }
        for (const fg::DescriptorBinding &binding : source[index].bindings) {
            if (binding.resource.empty()) continue;
            if (binding.access == fg::ResourceAccess::SampledRead) read.insert(binding.resource);
            if (binding.access == fg::ResourceAccess::StorageWrite) written.insert(binding.resource);
        }
    }
}

}  // namespace

bool FrameGenerator::ensure(Context *context) {
    if (ready_) return true;
    if (unsupported_) return false;
    context_ = context;
    const ComputeFeatures &compute = context->compute();
    fg::BackendCapabilities capabilities{};
    capabilities.storage_image_write_without_format = compute.storageWriteWithoutFormat;
    capabilities.storage_image_extended_formats = compute.storageExtendedFormats;
    capabilities.shader_float16 = compute.float16;
    // Nullable bindings get a cleared 1x1 image.
    capabilities.nullable_sampled_image_binding = true;
    capabilities.r8_unorm_storage_image = context->formatSupports(VK_FORMAT_R8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    capabilities.rgba8_unorm_storage_image =
        context->formatSupports(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    capabilities.rgba16_float_storage_image =
        context->formatSupports(VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    capabilities.external_color_sampled_image =
        context->formatSupports(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    capabilities.external_color_storage_image = capabilities.rgba8_unorm_storage_image;
    capabilities.max_compute_work_group_invocations = compute.maxWorkGroupInvocations;
    capabilities.max_compute_work_group_size = {compute.maxWorkGroupSize[0], compute.maxWorkGroupSize[1],
                                                compute.maxWorkGroupSize[2]};
    // FP16 first (Adreno runs half precision at twice the rate), FP32 when the device lacks it.
    variant_ = fg::ShaderVariant::NativeFp32;
    if (const auto preferred = fg::SelectBuiltinShaderVariant({.allow_fp16 = compute.float16, .prefer_fp16 = true});
        preferred && fg::SupportsFrameGeneration(*preferred, capabilities)) {
        variant_ = *preferred;
    }
    const auto missing = fg::MissingBackendRequirements(variant_, capabilities);
    if (!missing.empty()) {
        std::string names;
        for (const auto requirement : missing) {
            if (!names.empty()) names += ",";
            names += fg::ToString(requirement);
        }
        LOGE("frame generation unavailable: device lacks %s", names.c_str());
        unsupported_ = true;
        return false;
    }
    if (!createPipelines()) {
        release();
        unsupported_ = true;
        return false;
    }
    ready_ = true;
    LOGI("frame generation ready: %s shaders %.*s, %zu pipelines", fp16() ? "FP16" : "FP32",
         static_cast<int>(fg::BuiltinShaderFingerprint(variant_).size()), fg::BuiltinShaderFingerprint(variant_).data(),
         pipelines_.size());
    return true;
}

bool FrameGenerator::createPipelines() {
    const Dispatch &vk = context_->vk();
    const VkDevice device = context_->device();
    fg::FrameGenerationPlan schema;
    try {
        schema = fg::BuildFrameGenerationPlan(
            {.input_extent = {400, 240}, .flow_scale = 0.5f, .frame_index = 3, .generation_count = fg::MaximumGenerations});
    } catch (const std::exception &error) {
        LOGE("frame generation: plan unavailable: %s", error.what());
        return false;
    }
    std::map<uint32_t, std::vector<VkDescriptorType>> layouts;
    for (const auto *list : {&schema.shared_dispatches, &schema.generation_dispatches}) {
        for (const fg::Dispatch &dispatch : *list) {
            std::vector<VkDescriptorType> types;
            for (const fg::DescriptorBinding &binding : dispatch.bindings) types.push_back(DescriptorType(binding.kind));
            const auto [it, inserted] = layouts.try_emplace(dispatch.shader_id, types);
            if (!inserted && it->second != types) {
                LOGE("frame generation: shader %u has inconsistent bindings", dispatch.shader_id);
                return false;
            }
        }
    }
    fg::ShaderModuleViews modules{};
    if (fg::GetBuiltinShaderModules(variant_, modules) != fg::Status::Ok) {
        LOGE("frame generation: builtin shaders unavailable");
        return false;
    }
    for (const fg::ShaderModuleView &module : modules) {
        const auto layout = layouts.find(module.logical_id);
        if (layout == layouts.end() || layout->second.size() != module.descriptor_count) {
            LOGE("frame generation: shader %u does not match its plan bindings", module.logical_id);
            return false;
        }
        Pipeline pipeline;
        pipeline.bindings = layout->second;
        std::vector<VkDescriptorSetLayoutBinding> bindings(pipeline.bindings.size());
        for (uint32_t index = 0; index < bindings.size(); ++index) {
            bindings[index].binding = index;
            bindings[index].descriptorType = pipeline.bindings[index];
            bindings[index].descriptorCount = 1;
            bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = module.words.size_bytes();
        moduleInfo.pCode = module.words.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        bool created = vk.vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &pipeline.layout) == VK_SUCCESS;
        if (created) {
            VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pipelineLayoutInfo.setLayoutCount = 1;
            pipelineLayoutInfo.pSetLayouts = &pipeline.layout;
            created = vk.vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipeline.pipelineLayout) == VK_SUCCESS &&
                      vk.vkCreateShaderModule(device, &moduleInfo, nullptr, &shader) == VK_SUCCESS;
        }
        if (created) {
            VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            pipelineInfo.stage.module = shader;
            pipelineInfo.stage.pName = "main";
            pipelineInfo.layout = pipeline.pipelineLayout;
            created = vk.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline.pipeline) ==
                      VK_SUCCESS;
        }
        if (shader != VK_NULL_HANDLE) vk.vkDestroyShaderModule(device, shader, nullptr);
        pipelineIndex_[module.logical_id] = static_cast<uint32_t>(pipelines_.size());
        pipelines_.push_back(pipeline);
        if (!created) {
            LOGE("frame generation: pipeline for shader %u failed", module.logical_id);
            return false;
        }
    }

    const auto makeSampler = [&](VkSamplerAddressMode mode, VkBorderColor border, VkSampler *out) {
        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = info.addressModeV = info.addressModeW = mode;
        info.maxLod = 0.0f;
        info.borderColor = border;
        return vk.vkCreateSampler(device, &info, nullptr, out) == VK_SUCCESS;
    };
    if (!makeSampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                     &samplers_[static_cast<int>(fg::SamplerKind::LinearBorderBlack)]) ||
        !makeSampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
                     &samplers_[static_cast<int>(fg::SamplerKind::LinearBorderWhite)]) ||
        !makeSampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                     &samplers_[static_cast<int>(fg::SamplerKind::LinearClampEdge)])) {
        LOGE("frame generation: samplers failed");
        return false;
    }

    // One submission holds at most the shared passes and every generation of a frame.
    poolSizes_.clear();
    poolSets_ = 0;
    const auto count = [&](const std::vector<fg::Dispatch> &list, uint32_t times) {
        for (const fg::Dispatch &dispatch : list) {
            poolSets_ += times;
            for (const fg::DescriptorBinding &binding : dispatch.bindings) poolSizes_[DescriptorType(binding.kind)] += times;
        }
    };
    count(schema.shared_dispatches, 1);
    count(schema.generation_dispatches, fg::MaximumGenerations);
    uniformBytes_ = static_cast<VkDeviceSize>(schema.constants.size()) * (1 + fg::MaximumGenerations) *
                    std::max<VkDeviceSize>(context_->compute().uniformAlignment, sizeof(fg::PackedUniformConstants));
    std::vector<VkDescriptorPoolSize> sizes;
    for (const auto &[type, descriptors] : poolSizes_) sizes.push_back({type, descriptors});
    for (PerSubmission &slot : perSubmission_) {
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = poolSets_;
        poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
        poolInfo.pPoolSizes = sizes.data();
        if (vk.vkCreateDescriptorPool(device, &poolInfo, nullptr, &slot.pool) != VK_SUCCESS ||
            !context_->createHostBuffer(uniformBytes_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &slot.uniforms)) {
            LOGE("frame generation: descriptor pool or constants unavailable");
            return false;
        }
        slot.serial = ~0ull;
    }
    return true;
}

void FrameGenerator::releaseImages() {
    if (context_ == nullptr) return;
    for (Image &image : images_) context_->destroyImage(&image);
    images_.clear();
    imageIndex_.clear();
    context_->destroyImage(&dummy_);
    outputs_.fill(-1);
    outputLayouts_.fill(VK_IMAGE_LAYOUT_UNDEFINED);
    imagesCleared_ = false;
    extent_ = {};
    flowMilli_ = 0;
    plans_.clear();
}

void FrameGenerator::release() {
    if (context_ == nullptr || context_->device() == VK_NULL_HANDLE) return;
    const Dispatch &vk = context_->vk();
    const VkDevice device = context_->device();
    releaseImages();
    for (Pipeline &pipeline : pipelines_) {
        if (pipeline.pipeline != VK_NULL_HANDLE) vk.vkDestroyPipeline(device, pipeline.pipeline, nullptr);
        if (pipeline.pipelineLayout != VK_NULL_HANDLE) vk.vkDestroyPipelineLayout(device, pipeline.pipelineLayout, nullptr);
        if (pipeline.layout != VK_NULL_HANDLE) vk.vkDestroyDescriptorSetLayout(device, pipeline.layout, nullptr);
    }
    pipelines_.clear();
    pipelineIndex_.clear();
    for (VkSampler &sampler : samplers_) {
        if (sampler != VK_NULL_HANDLE) vk.vkDestroySampler(device, sampler, nullptr);
        sampler = VK_NULL_HANDLE;
    }
    for (PerSubmission &slot : perSubmission_) {
        if (slot.pool != VK_NULL_HANDLE) vk.vkDestroyDescriptorPool(device, slot.pool, nullptr);
        context_->destroyHostBuffer(&slot.uniforms);
        slot = {};
    }
    ready_ = false;
    reset();
}

bool FrameGenerator::configure(VkExtent2D extent, float flowScale) {
    const uint32_t flowMilli = std::clamp(static_cast<uint32_t>(std::lround(flowScale * 1000.0f)), 250u, 1000u);
    if (extent.width == extent_.width && extent.height == extent_.height && flowMilli == flowMilli_ && !images_.empty()) {
        return true;
    }
    context_->waitIdle();
    releaseImages();
    reset();
    fg::FrameGenerationPlan plan;
    try {
        plan = fg::BuildFrameGenerationPlan({.input_extent = {extent.width, extent.height},
                                             .flow_scale = static_cast<float>(flowMilli) / 1000.0f,
                                             .generation_count = fg::MaximumGenerations});
    } catch (const std::exception &error) {
        LOGE("frame generation: plan for %ux%u failed: %s", extent.width, extent.height, error.what());
        return false;
    }
    VkDeviceSize bytes = 0;
    for (const fg::ImageResource &declaration : plan.images) {
        // The game frames are the scene slots themselves.
        if (declaration.name.rfind("frame.", 0) == 0) continue;
        const VkFormat format = ImageFormat(declaration.format);
        Image image{};
        if (!context_->createImage(declaration.extent.width, declaration.extent.height, format,
                                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   &image)) {
            LOGE("frame generation: image %s %ux%u unavailable", declaration.name.c_str(), declaration.extent.width,
                 declaration.extent.height);
            releaseImages();
            return false;
        }
        bytes += context_->imageRequirements(image.image).size;
        imageIndex_[declaration.name] = static_cast<int>(images_.size());
        if (declaration.name.rfind("output.", 0) == 0) {
            const uint32_t generation = static_cast<uint32_t>(std::stoul(declaration.name.substr(7)));
            if (generation < outputs_.size()) outputs_[generation] = static_cast<int>(images_.size());
        }
        images_.push_back(image);
    }
    if (!context_->createImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               &dummy_)) {
        releaseImages();
        return false;
    }
    extent_ = extent;
    flowMilli_ = flowMilli;
    LOGI("frame generation: %ux%u flow %.2f, %zu images, %.1f MiB", extent.width, extent.height, flowMilli / 1000.0f,
         images_.size(), static_cast<double>(bytes) / (1024.0 * 1024.0));
    return true;
}

FrameGenerator::Plan FrameGenerator::buildPlan(const fg::FrameGenerationPlan &source) const {
    Plan plan;
    std::map<std::string, int> constants;
    for (const fg::UniformConstants &entry : source.constants) {
        constants[entry.name] = static_cast<int>(plan.constants.size());
        plan.constants.push_back(fg::PackUniformConstants(entry));
    }
    const std::string current = "frame." + std::to_string(source.frame_index % 2);
    const auto resolve = [&](const std::vector<fg::Dispatch> &list, std::vector<Pass> *out) {
        std::vector<bool> barriers;
        MarkBarriers(list, &barriers);
        for (size_t index = 0; index < list.size(); ++index) {
            const fg::Dispatch &dispatch = list[index];
            Pass resolved;
            resolved.pipeline = pipelineIndex_.at(dispatch.shader_id);
            resolved.groups = dispatch.group_count;
            resolved.barrier = barriers[index];
            for (const fg::DescriptorBinding &binding : dispatch.bindings) {
                Binding bound;
                bound.type = DescriptorType(binding.kind);
                if (binding.kind == fg::DescriptorKind::UniformBuffer) {
                    bound.constants = constants.at(binding.resource);
                } else if (binding.kind == fg::DescriptorKind::Sampler) {
                    bound.sampler = static_cast<int>(binding.sampler);
                } else if (binding.resource.empty()) {
                    bound.image = kDummy;
                } else if (binding.resource.rfind("frame.", 0) == 0) {
                    bound.image = binding.resource == current ? kCurrent : kPrevious;
                } else {
                    bound.image = imageIndex_.at(binding.resource);
                }
                resolved.bindings.push_back(bound);
            }
            out->push_back(std::move(resolved));
        }
    };
    resolve(source.shared_dispatches, &plan.shared);
    resolve(source.generation_dispatches, &plan.generation);
    plan.output = imageIndex_.at(source.output_resource);
    return plan;
}

const FrameGenerator::Plan &FrameGenerator::plan(uint32_t count, uint32_t generation) {
    const auto key = std::make_tuple(static_cast<uint32_t>(frameIndex_ % 6), fg::IsFrameGenerationHistoryReady(frameIndex_),
                                     count, generation);
    auto it = plans_.find(key);
    if (it == plans_.end()) {
        // frame_index only selects the frame and history slots (modulo 2 and 3) and readiness.
        const uint64_t index = std::get<1>(key) ? std::get<0>(key) + 6 : std::get<0>(key);
        const fg::FrameGenerationPlan source =
            fg::BuildFrameGenerationPlan({.input_extent = {extent_.width, extent_.height},
                                          .flow_scale = static_cast<float>(flowMilli_) / 1000.0f,
                                          .frame_index = index,
                                          .generation_count = count,
                                          .generation = generation});
        it = plans_.emplace(key, buildPlan(source)).first;
    }
    return it->second;
}

bool FrameGenerator::prepareSubmission(Submission *submission) {
    PerSubmission &slot = perSubmission_[submission->slot];
    // Within one recording the submission keeps its previous serial; a new one means the slot's
    // last use completed (Context::begin waits for that).
    if (slot.serial != submission->serial) {
        if (context_->vk().vkResetDescriptorPool(context_->device(), slot.pool, 0) != VK_SUCCESS) return false;
        slot.used = 0;
        slot.serial = submission->serial;
    }
    return true;
}

bool FrameGenerator::record(Submission *submission, const Plan &plan, const std::vector<Pass> &dispatches,
                            VkImageView current, VkImageView previous) {
    if (!prepareSubmission(submission)) return false;
    const Dispatch &vk = context_->vk();
    const VkDevice device = context_->device();
    PerSubmission &slot = perSubmission_[submission->slot];
    const VkDeviceSize stride = std::max<VkDeviceSize>(context_->compute().uniformAlignment, sizeof(fg::PackedUniformConstants));
    const VkDeviceSize base = (slot.used + stride - 1) / stride * stride;
    if (base + stride * plan.constants.size() > slot.uniforms.size) {
        LOGE("frame generation: constants exceed the submission's buffer");
        return false;
    }
    for (size_t index = 0; index < plan.constants.size(); ++index) {
        std::memcpy(static_cast<uint8_t *>(slot.uniforms.mapped) + base + stride * index, &plan.constants[index],
                    sizeof(fg::PackedUniformConstants));
    }
    slot.used = base + stride * plan.constants.size();

    std::vector<VkDescriptorSetLayout> layouts;
    layouts.reserve(dispatches.size());
    for (const Pass &dispatch : dispatches) layouts.push_back(pipelines_[dispatch.pipeline].layout);
    std::vector<VkDescriptorSet> sets(dispatches.size());
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = slot.pool;
    allocate.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocate.pSetLayouts = layouts.data();
    if (vk.vkAllocateDescriptorSets(device, &allocate, sets.data()) != VK_SUCCESS) {
        LOGE("frame generation: descriptor sets exhausted");
        return false;
    }

    size_t bindingCount = 0;
    for (const Pass &dispatch : dispatches) bindingCount += dispatch.bindings.size();
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorImageInfo> imageInfos;
    std::vector<VkDescriptorBufferInfo> bufferInfos;
    writes.reserve(bindingCount);
    imageInfos.reserve(bindingCount);
    bufferInfos.reserve(bindingCount);
    for (size_t index = 0; index < dispatches.size(); ++index) {
        const Pass &dispatch = dispatches[index];
        for (uint32_t binding = 0; binding < dispatch.bindings.size(); ++binding) {
            const Binding &bound = dispatch.bindings[binding];
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = sets[index];
            write.dstBinding = binding;
            write.descriptorCount = 1;
            write.descriptorType = bound.type;
            if (bound.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                bufferInfos.push_back({slot.uniforms.buffer, base + stride * static_cast<VkDeviceSize>(bound.constants),
                                       sizeof(fg::PackedUniformConstants)});
                write.pBufferInfo = &bufferInfos.back();
            } else if (bound.type == VK_DESCRIPTOR_TYPE_SAMPLER) {
                imageInfos.push_back({samplers_[bound.sampler], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
                write.pImageInfo = &imageInfos.back();
            } else {
                VkImageView view = VK_NULL_HANDLE;
                VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
                if (bound.image == kCurrent || bound.image == kPrevious) {
                    view = bound.image == kCurrent ? current : previous;
                    layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    if (view == VK_NULL_HANDLE) {
                        view = dummy_.view;
                        layout = VK_IMAGE_LAYOUT_GENERAL;
                    }
                } else if (bound.image == kDummy) {
                    view = dummy_.view;
                } else {
                    view = images_[bound.image].view;
                }
                imageInfos.push_back({VK_NULL_HANDLE, view, layout});
                write.pImageInfo = &imageInfos.back();
            }
            writes.push_back(write);
        }
    }
    vk.vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    VkCommandBuffer command = submission->command;
    // Against earlier submissions and lists: their compute writes, and reads of what this list writes.
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vk.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                            &memory, 0, nullptr, 0, nullptr);
    memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    uint32_t bound = UINT32_MAX;
    for (size_t index = 0; index < dispatches.size(); ++index) {
        const Pass &dispatch = dispatches[index];
        if (dispatch.barrier) {
            vk.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                    &memory, 0, nullptr, 0, nullptr);
        }
        const Pipeline &pipeline = pipelines_[dispatch.pipeline];
        if (bound != dispatch.pipeline) {
            vk.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
            bound = dispatch.pipeline;
        }
        vk.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipelineLayout, 0, 1, &sets[index], 0,
                                   nullptr);
        vk.vkCmdDispatch(command, dispatch.groups[0], dispatch.groups[1], dispatch.groups[2]);
    }
    return true;
}

bool FrameGenerator::recordShared(Submission *submission, VkExtent2D extent, float flowScale, VkImageView current,
                                  VkImageView previous) {
    if (!ready_ || submission == nullptr || current == VK_NULL_HANDLE || !configure(extent, flowScale)) return false;
    if (!imagesCleared_) {
        // Everything starts at zero, then stays GENERAL; the outputs leave it only to be sampled.
        VkCommandBuffer command = submission->command;
        const VkClearColorValue zero{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        std::vector<Image *> all;
        for (Image &image : images_) all.push_back(&image);
        all.push_back(&dummy_);
        for (Image *image : all) {
            context_->barrier(command, image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                              VK_ACCESS_TRANSFER_WRITE_BIT);
            context_->vk().vkCmdClearColorImage(command, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            context_->barrier(command, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            image->layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        outputLayouts_.fill(VK_IMAGE_LAYOUT_GENERAL);
        imagesCleared_ = true;
    }
    if (started_) {
        ++frameIndex_;
    } else {
        frameIndex_ = 0;
        started_ = true;
    }
    try {
        const Plan &shared = plan(1, 0);
        return record(submission, shared, shared.shared, current, previous);
    } catch (const std::exception &error) {
        LOGE("frame generation: shared passes failed: %s", error.what());
        return false;
    }
}

bool FrameGenerator::historyReady() const {
    return ready_ && started_ && fg::IsFrameGenerationHistoryReady(frameIndex_);
}

VkImageView FrameGenerator::recordGeneration(Submission *submission, uint32_t index, uint32_t count, VkImageView current,
                                             VkImageView previous) {
    if (!historyReady() || submission == nullptr || count == 0 || count > fg::MaximumGenerations || index >= count ||
        previous == VK_NULL_HANDLE || outputs_[index] < 0) {
        return VK_NULL_HANDLE;
    }
    Image &output = images_[outputs_[index]];
    VkCommandBuffer command = submission->command;
    if (outputLayouts_[index] != VK_IMAGE_LAYOUT_GENERAL) {
        context_->barrier(command, output.image, outputLayouts_[index], VK_IMAGE_LAYOUT_GENERAL,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, VK_ACCESS_SHADER_WRITE_BIT);
        outputLayouts_[index] = VK_IMAGE_LAYOUT_GENERAL;
    }
    try {
        const Plan &generation = plan(count, index);
        if (!record(submission, generation, generation.generation, current, previous)) return VK_NULL_HANDLE;
    } catch (const std::exception &error) {
        LOGE("frame generation: generation %u/%u failed: %s", index, count, error.what());
        return VK_NULL_HANDLE;
    }
    context_->barrier(command, output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    outputLayouts_[index] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return output.view;
}

}  // namespace xrimmersive::vulkan
