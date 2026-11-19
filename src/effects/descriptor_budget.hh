#ifndef DESCRIPTOR_BUDGET_HPP_INCLUDED
#define DESCRIPTOR_BUDGET_HPP_INCLUDED

#include <cstdint>
#include <vector>

#include "vulkan_include.hh"

namespace VkHook
{
    // What one ReShade effect allocates from its descriptor pool, derived from
    // the per-set needs rather than padded: a pool smaller than this fails to
    // allocate, a larger one wastes driver memory on every effect of every chain.
    //
    // Per swapchain image the effect owns
    //  - one uniform-buffer set (when it has uniforms), and
    //  - one image/sampler set per sampler group: the input set, plus a back
    //    buffer set when it writes the back buffer more than once, plus an
    //    output set when it writes it more than twice.
    struct DescriptorBudget
    {
        uint32_t maxSets = 0;
        uint32_t uniformBuffers = 0;
        uint32_t sampledImages = 0;   // COMBINED_IMAGE_SAMPLER descriptors
        uint32_t storageImages = 0;   // STORAGE_IMAGE descriptors

        // Only non-empty types: Vulkan rejects a pool size of zero descriptors.
        std::vector<VkDescriptorPoolSize> poolSizes() const
        {
            std::vector<VkDescriptorPoolSize> sizes;
            if (uniformBuffers)
                sizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uniformBuffers});
            if (sampledImages)
                sizes.push_back({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampledImages});
            if (storageImages)
                sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storageImages});
            return sizes;
        }
    };

    // Number of image/sampler descriptor sets per swapchain image for an effect
    // that writes the back buffer `outputWrites` times.
    inline uint32_t samplerSetGroups(int outputWrites)
    {
        return 1u + (outputWrites > 1 ? 1u : 0u) + (outputWrites > 2 ? 1u : 0u);
    }

    inline DescriptorBudget computeDescriptorBudget(uint32_t imageCount,
                                                    uint32_t sampledBindings,
                                                    uint32_t storageBindings,
                                                    uint32_t samplerGroups,
                                                    bool     hasUniformBuffer)
    {
        DescriptorBudget budget;
        const uint32_t samplerSets = imageCount * samplerGroups;
        const uint32_t uniformSets = hasUniformBuffer ? imageCount : 0u;
        budget.maxSets = samplerSets + uniformSets;
        budget.uniformBuffers = uniformSets;
        budget.sampledImages = samplerSets * sampledBindings;
        budget.storageImages = samplerSets * storageBindings;
        return budget;
    }
} // namespace VkHook

#endif // DESCRIPTOR_BUDGET_HPP_INCLUDED
