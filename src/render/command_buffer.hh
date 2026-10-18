#ifndef COMMAND_BUFFER_HPP_INCLUDED
#define COMMAND_BUFFER_HPP_INCLUDED
#include <vector>
#include <memory>

#include "vulkan_include.hh"

#include "logical_device.hh"

#include "effects/effect.hh"
namespace VKIntox
{
    struct LogicalSwapchain;

    std::vector<VkCommandBuffer> allocateCommandBuffer(LogicalDevice* logicalDevice, uint32_t count, VkCommandPool pool = VK_NULL_HANDLE);

    void writeCommandBuffers(LogicalDevice*                                 logicalDevice,
                             LogicalSwapchain*                              logicalSwapchain,
                             const std::vector<std::shared_ptr<VKIntox::Effect>>& effects,
                             const std::vector<VkCommandBuffer>&             commandBuffers,
                             const DepthState&                              depthState,
                             uint32_t                                       imageIndex = UINT32_MAX);

    // Caller holds globalLock. Only record outside application render scopes;
    // layer-owned command buffers have no tracked application scope.
    void recordDepthResolveSnapshot(LogicalDevice*            logicalDevice,
                                    LogicalSwapchain*         logicalSwapchain,
                                    VkCommandBuffer           commandBuffer,
                                    uint32_t                  imageIndex,
                                    const DepthState&         depthState);

    std::vector<VkSemaphore> createSemaphores(LogicalDevice* logicalDevice, uint32_t count);
} // namespace VKIntox

#endif // COMMAND_BUFFER_HPP_INCLUDED
