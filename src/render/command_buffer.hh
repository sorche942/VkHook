#ifndef COMMAND_BUFFER_HPP_INCLUDED
#define COMMAND_BUFFER_HPP_INCLUDED
#include <vector>
#include <memory>

#include "vulkan_include.hh"

#include "logical_device.hh"

#include "effects/effect.hh"
namespace VkHook
{
    struct LogicalSwapchain;

    // `pool` is required (a swapchain generation's own RESET_COMMAND_BUFFER pool);
    // returns an empty vector and logs when it is null or allocation fails.
    std::vector<VkCommandBuffer> allocateCommandBuffer(LogicalDevice* logicalDevice, uint32_t count, VkCommandPool pool);

    void writeCommandBuffers(LogicalDevice*                                 logicalDevice,
                             LogicalSwapchain*                              logicalSwapchain,
                             const std::vector<std::shared_ptr<VkHook::Effect>>& effects,
                             const std::vector<VkCommandBuffer>&             commandBuffers,
                             const DepthState&                              depthState,
                             uint32_t                                       imageIndex = UINT32_MAX);

    // Reads the device's depth tracking maps: the caller holds the device
    // stateLock. Only record outside application render scopes; layer-owned
    // command buffers have no tracked application scope. Polls (never waits on)
    // the image's fence before rewriting live descriptor sets.
    void recordDepthResolveSnapshot(LogicalDevice*            logicalDevice,
                                    LogicalSwapchain*         logicalSwapchain,
                                    VkCommandBuffer           commandBuffer,
                                    uint32_t                  imageIndex,
                                    const DepthState&         depthState);

    std::vector<VkSemaphore> createSemaphores(LogicalDevice* logicalDevice, uint32_t count);
} // namespace VkHook

#endif // COMMAND_BUFFER_HPP_INCLUDED
