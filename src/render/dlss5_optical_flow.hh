#pragma once

#include "vk_context.h"
#include <memory>

namespace VkHook
{
    // Borrows the context and optical queue. The caller serializes submissions and
    // retires all work (including partial failed prepares) before destruction.
    class Dlss5OpticalFlow
    {
    public:
        Dlss5OpticalFlow(vk::Context& context, uint32_t opticalQueueFamily, VkQueue opticalQueue,
                        VkExtent2D extent, VkFormat sourceFormat, const std::vector<VkImage>& sources,
                        const std::string& shaderPath, PFN_vkGetInstanceProcAddr gipa,
                        PFN_vkGetDeviceProcAddr gdpa);
        ~Dlss5OpticalFlow();
        Dlss5OpticalFlow(const Dlss5OpticalFlow&) = delete;
        Dlss5OpticalFlow& operator=(const Dlss5OpticalFlow&) = delete;

        // Requires this image's previous final submission to have completed.
        // Consumes binary waits, leaving all non-wait fields untouched. A failure
        // is terminal: earlier submissions may already have consumed those waits.
        VkResult prepare(uint32_t imageIndex, VkSubmitInfo& nextSubmit) noexcept;
        void recordResults(VkCommandBuffer commands) const noexcept;
        uint32_t flowWidth() const noexcept;
        uint32_t flowHeight() const noexcept;
        const vk::Buffer& forward() const noexcept;
        const vk::Buffer& backward() const noexcept;
        const vk::Buffer& cost() const noexcept;

    private:
        struct State;
        std::unique_ptr<State> state;
    };
}
