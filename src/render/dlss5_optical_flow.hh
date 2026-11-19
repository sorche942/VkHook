#pragma once

#include "gpu/queue.hh"
#include "vk_context.h"
#include <memory>

namespace VkHook
{
    // Borrows the context, the queue table and the optical queue; both queues must be
    // known to `queues` (every submit takes that queue's mutex). The caller serializes submissions and
    // retires all work (including partial failed prepares) before destruction.
    //
    // `downscale` (1, 2 or 4) is the box reduction of the luma the hardware engine sees. The engine
    // returns one vector per 4x4 of its input pixels and its time follows that vector count, so a
    // reduced input costs proportionally less and yields one vector per (4 * downscale)^2 source pixels.
    class Dlss5OpticalFlow
    {
    public:
        Dlss5OpticalFlow(vk::Context& context, QueueTable& queues, uint32_t opticalQueueFamily, VkQueue opticalQueue,
                        VkExtent2D extent, VkFormat sourceFormat, const std::vector<VkImage>& sources,
                        uint32_t downscale, const std::string& shaderPath, PFN_vkGetInstanceProcAddr gipa,
                        PFN_vkGetDeviceProcAddr gdpa);
        ~Dlss5OpticalFlow();
        Dlss5OpticalFlow(const Dlss5OpticalFlow&) = delete;
        Dlss5OpticalFlow& operator=(const Dlss5OpticalFlow&) = delete;

        // The downscale used when the user does not choose one.
        static uint32_t automaticDownscale(VkExtent2D extent) noexcept;

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
        // The packed pixels of the source image of the last prepare(), valid from its final submission on.
        const vk::Buffer& source() const noexcept;
        // Source pixels per unit of a flow vector.
        uint32_t downscale() const noexcept;

    private:
        struct State;
        std::unique_ptr<State> state;
    };
}
