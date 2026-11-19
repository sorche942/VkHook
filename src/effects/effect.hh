#ifndef EFFECT_HPP_INCLUDED
#define EFFECT_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>
#include <memory>
#include <stdexcept>

#include "vulkan_include.hh"
#include "params/effect_param.hh"

namespace VkHook
{
    // Thrown by an effect's constructor when it cannot be built (compile failure,
    // Vulkan error, unusable resource). The effect has already released whatever
    // it created; the creator drops the effect and carries on without it.
    class EffectBuildError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    class Effect
    {
    public:
        void virtual applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) = 0;
        // Called once per present, for the swapchain image about to be submitted,
        // and only after that image's previous submission has completed. It may
        // therefore rewrite the CPU-visible state that submission read, but ONLY
        // the state belonging to `imageIndex`: submissions of the other images
        // may still be executing. Effects with per-frame data (ReShade uniforms)
        // keep one copy of it per swapchain image and write copy `imageIndex`
        // here.
        //
        // Returns true to request fence-safe re-recording of this image's primary
        // commands (descriptor bindings changed).
        virtual bool updateFrame(uint32_t /* imageIndex */) { return false; }
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
        virtual ~Effect() = default;

    private:
    };
} // namespace VkHook

#endif // EFFECT_HPP_INCLUDED
