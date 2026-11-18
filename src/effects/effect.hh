#ifndef EFFECT_HPP_INCLUDED
#define EFFECT_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <memory>

#include "vulkan_include.hh"
#include "params/effect_param.hh"

namespace VKIntox
{
    class Effect
    {
    public:
        void virtual applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) = 0;
        void virtual updateEffect(){};
        // Called only after this swapchain image's previous submission completed.
        // True requests fence-safe re-recording of this image's primary commands.
        virtual bool updateFrame(uint32_t /* imageIndex */) { updateEffect(); return false; }
        // Called under the submission lock, after the depth-generation gate.
        // May submit auxiliary GPU work and replace this submission's waits.
        virtual VkResult prepareSubmission(uint32_t /* imageIndex */, uint64_t /* frameSerial */,
                                           VkSubmitInfo& /* submission */) noexcept { return VK_SUCCESS; }
        // Parameter names are omitted on the unused default hooks: subclasses that
        // do consume depth override these, and leaving the names off keeps the
        // base declaration free of unused-parameter noise.
        void virtual useDepthImage(uint32_t /* imageIndex */,
                                   VkImageView /* depthImageView */,
                                   VkImageLayout /* depthImageLayout */ = VK_IMAGE_LAYOUT_UNDEFINED){};
        virtual std::vector<std::unique_ptr<EffectParam>> getParameters() const { return {}; }
        virtual ~Effect() = default;

    private:
    };
} // namespace VKIntox

#endif // EFFECT_HPP_INCLUDED
