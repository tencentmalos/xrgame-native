#pragma once

// Frame generation for the 2D big screen: Foundation's Lossless FG graph on the composite device.
// It interpolates between consecutive game frames at the Wine screen's size, before the big
// screen's reconstruction, so real and generated frames take the same filter. The game frames are
// the X server's scene slots, sampled in place: the history keeps no copy of them. Every pass runs
// on the XR thread's queue; nothing here waits for the game or the X server.

#include "xr_vulkan.h"

#include "spatial/lossless_fg/FrameGenerationDemo.h"
#include "spatial/lossless_fg/LosslessFg.h"

#include <array>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace xrimmersive::vulkan {

class FrameGenerator {
public:
    // Builds the pipelines on first use; false (logged once) when the device lacks a requirement.
    bool ensure(Context *context);
    // Images and pipelines; the device must be idle.
    void release();
    bool supported() const { return ready_; }
    bool fp16() const { return variant_ == spatial::lossless_fg::ShaderVariant::NativeFp16; }

    // Forgets the history: generation resumes after enough new frames.
    void reset() { frameIndex_ = 0; started_ = false; }
    // A new game frame `current` after `previous` (sampled views of `extent`, SHADER_READ_ONLY,
    // `previous` null for the first): records the shared passes into `submission`.
    bool recordShared(Submission *submission, VkExtent2D extent, float flowScale, VkImageView current,
                      VkImageView previous);
    // Whether the last shared frame can be interpolated (the history is full).
    bool historyReady() const;
    // Records generation `index` of `count` between the previous and the current frame and leaves
    // its image SHADER_READ_ONLY for the reconstruction; returns its view, or null.
    VkImageView recordGeneration(Submission *submission, uint32_t index, uint32_t count, VkImageView current,
                                 VkImageView previous);
    VkExtent2D extent() const { return extent_; }

private:
    struct Pipeline {
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::vector<VkDescriptorType> bindings;
    };
    // A dispatch with its bindings resolved to our resources.
    struct Binding {
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        int image = -1;     // into images_, or kCurrent / kPrevious / kDummy
        int constants = -1; // into the plan's constants
        int sampler = 0;
    };
    struct Pass {
        uint32_t pipeline = 0;
        std::array<uint32_t, 3> groups{};
        std::vector<Binding> bindings;
        bool barrier = false; // a hazard with an earlier dispatch of the same list
    };
    struct Plan {
        std::vector<spatial::lossless_fg::PackedUniformConstants> constants;
        std::vector<Pass> shared;
        std::vector<Pass> generation;
        int output = -1;
    };
    static constexpr int kCurrent = -2;
    static constexpr int kPrevious = -3;
    static constexpr int kDummy = -4;

    bool createPipelines();
    bool configure(VkExtent2D extent, float flowScale);
    void releaseImages();
    const Plan &plan(uint32_t count, uint32_t generation);
    Plan buildPlan(const spatial::lossless_fg::FrameGenerationPlan &source) const;
    bool prepareSubmission(Submission *submission);
    bool record(Submission *submission, const Plan &plan, const std::vector<Pass> &dispatches,
                VkImageView current, VkImageView previous);

    Context *context_ = nullptr;
    bool ready_ = false;
    bool unsupported_ = false;
    spatial::lossless_fg::ShaderVariant variant_ = spatial::lossless_fg::ShaderVariant::NativeFp32;
    std::map<uint32_t, uint32_t> pipelineIndex_;
    std::vector<Pipeline> pipelines_;
    std::array<VkSampler, 3> samplers_{};

    VkExtent2D extent_{};
    uint32_t flowMilli_ = 0;
    std::map<std::string, int> imageIndex_;
    std::vector<Image> images_;
    std::array<int, spatial::lossless_fg::MaximumGenerations> outputs_{};
    std::array<VkImageLayout, spatial::lossless_fg::MaximumGenerations> outputLayouts_{};
    Image dummy_{};
    bool imagesCleared_ = false;
    // Keyed by frame index modulo 6 (frame and history slots), history readiness, count, generation.
    std::map<std::tuple<uint32_t, bool, uint32_t, uint32_t>, Plan> plans_;

    // Per submission slot: descriptor sets and constants, recycled when the slot is reused.
    struct PerSubmission {
        VkDescriptorPool pool = VK_NULL_HANDLE;
        HostBuffer uniforms{};
        VkDeviceSize used = 0;
        uint64_t serial = ~0ull;
    };
    std::array<PerSubmission, Context::kSubmissionCount> perSubmission_{};
    uint32_t poolSets_ = 0;
    std::map<VkDescriptorType, uint32_t> poolSizes_;
    VkDeviceSize uniformBytes_ = 0;

    uint64_t frameIndex_ = 0; // of the last shared frame
    bool started_ = false;
};

}  // namespace xrimmersive::vulkan
