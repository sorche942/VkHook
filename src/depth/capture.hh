#pragma once

// DepthCapture: the layer's own copy of the frame's depth, taken in the layer's
// own command buffer at present time (AGENTS.md invariants 4 and 11).
//
// One per swapchain, created with its first generation and living as long as
// the swapchain does. It owns an R32_SFLOAT target per swapchain image, which
// every effect of every chain generation samples in SHADER_READ_ONLY_OPTIMAL,
// plus the pass, pipelines and descriptor sets that fill it. Because the effect
// command buffers only ever reference these targets, a change of the depth
// source (a new image, another sample count or layout) re-records nothing but
// this small per-present command buffer.
//
// Recording never writes into an application command buffer, and never touches
// the application's image beyond a balanced layout transition from the tracked
// end-of-frame layout and back. A multisampled source is resolved by shader
// (texelFetch); vkCmdResolveImage is never used on depth.

#include "depth/tracker.hh"
#include "logical_device.hh"

#include <memory>
#include <vector>

namespace VkHook
{
    struct DepthCaptureParams
    {
        bool                  invert = false;
        bool                  flip   = false;
        bool                  flop   = false;
        // how a multisampled source is reduced: SAMPLE_ZERO, AVERAGE, MIN or MAX
        VkResolveModeFlagBits resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    };

    class DepthCapture
    {
    public:
        // The layout the targets are in whenever an effect reads them.
        static constexpr VkImageLayout kReadLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Null (and logged) when the device refuses any of the objects.
        static std::shared_ptr<DepthCapture> create(LogicalDevice* device, VkExtent2D extent, uint32_t imageCount);
        ~DepthCapture();
        DepthCapture(const DepthCapture&)            = delete;
        DepthCapture& operator=(const DepthCapture&) = delete;

        uint32_t    imageCount() const { return static_cast<uint32_t>(targets.size()); }
        VkImageView view(uint32_t imageIndex) const { return targets[imageIndex].view; }

        // Re-records the capture of swapchain image `imageIndex` and returns its
        // command buffer, ready to submit ahead of the effect buffer. The
        // caller proved the image's previous layer submission complete. An
        // invalid `source` records a clear to the far plane: effects then read
        // a defined value, never garbage. Null only when recording failed.
        VkCommandBuffer record(uint32_t imageIndex, const DepthSource& source, const DepthCaptureParams& params);

    private:
        struct Target
        {
            VkImage       image       = VK_NULL_HANDLE;
            VkImageView   view        = VK_NULL_HANDLE;
            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            VkDescriptorSet set       = VK_NULL_HANDLE;
            VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        };
        struct Variant
        {
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
            VkResolveModeFlagBits mode    = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            VkPipeline            pipeline = VK_NULL_HANDLE;
        };

        explicit DepthCapture(LogicalDevice* device) : device(device) {}
        bool build(VkExtent2D target, uint32_t imageCount);
        VkPipeline pipelineFor(VkSampleCountFlagBits samples, VkResolveModeFlagBits mode);

        LogicalDevice* device;
        VkExtent2D     extent{};
        std::vector<Target> targets;
        VkDeviceMemory memory           = VK_NULL_HANDLE;
        VkRenderPass   renderPass       = VK_NULL_HANDLE;
        VkSampler      sampler          = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkCommandPool  commandPool      = VK_NULL_HANDLE;
        std::vector<Variant> variants;
        bool           pipelineFailureLogged = false;
    };
} // namespace VkHook
