#include "command_buffer.hh"

#include "depth/capture.hh"
#include "logical_swapchain.hh"
#include "logger.hh"
#include "util.hh"

namespace VkHook
{
    std::vector<VkCommandBuffer> allocateCommandBuffer(LogicalDevice* logicalDevice, uint32_t count, VkCommandPool pool)
    {
        if (pool == VK_NULL_HANDLE)
        {
            Logger::err("allocateCommandBuffer: no command pool given");
            return {};
        }

        std::vector<VkCommandBuffer> commandBuffers(count);

        VkCommandBufferAllocateInfo allocInfo;
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.pNext              = nullptr;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool        = pool;
        allocInfo.commandBufferCount = count;

        VkResult result = allocateLayerCommandBuffers(logicalDevice, &allocInfo, commandBuffers.data());
        if (result != VK_SUCCESS)
        {
            Logger::err("ASSERT_VULKAN failed in " + std::string(__FILE__) + " : " + std::to_string(__LINE__) + "; " + std::to_string(result));
            return {};
        }
        for (uint32_t i = 0; i < count; i++)
        {
            // initialize dispatch tables for commandBuffers since the are dispatchable objects
            initializeDispatchTable(commandBuffers[i], logicalDevice->device);
        }

        return commandBuffers;
    }
    void writeCommandBuffers(LogicalDevice*                                 logicalDevice,
                             LogicalSwapchain*                              logicalSwapchain,
                             const std::vector<std::shared_ptr<VkHook::Effect>>& effects,
                             const std::vector<VkCommandBuffer>&             commandBuffers,
                             uint32_t                                       imageIndex)
    {
        VkCommandBufferBeginInfo beginInfo = {};

        beginInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.pNext            = nullptr;
        beginInfo.flags            = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        beginInfo.pInheritanceInfo = nullptr;

        // The effects read the swapchain's depth capture targets; the capture is
        // recorded and submitted ahead of these buffers at every present.
        const DepthCapture* capture = logicalSwapchain && logicalSwapchain->shared ? logicalSwapchain->shared->depthCapture.get()
                                                                                    : nullptr;

        for (uint32_t i = 0; i < commandBuffers.size(); i++)
        {
            if (imageIndex != UINT32_MAX && imageIndex != i)
                continue;
            const bool bound = capture && i < capture->imageCount();
            for (auto& effect : effects)
                effect->useDepthImage(i, bound ? capture->view(i) : VK_NULL_HANDLE,
                                      bound ? DepthCapture::kReadLayout : VK_IMAGE_LAYOUT_UNDEFINED);

            VkResult result = logicalDevice->vkd.BeginCommandBuffer(commandBuffers[i], &beginInfo);
            ASSERT_VULKAN(result);

            for (uint32_t j = 0; j < effects.size(); j++)
                effects[j]->applyEffect(i, commandBuffers[i]);

            result = logicalDevice->vkd.EndCommandBuffer(commandBuffers[i]);
            ASSERT_VULKAN(result);
        }
    }

    std::vector<VkSemaphore> createSemaphores(LogicalDevice* logicalDevice, uint32_t count)
    {
        std::vector<VkSemaphore> semaphores(count);
        VkSemaphoreCreateInfo    info;
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        info.pNext = nullptr;
        info.flags = 0;

        for (uint32_t i = 0; i < count; i++)
        {
            VkResult vr = logicalDevice->vkd.CreateSemaphore(logicalDevice->device, &info, nullptr, &semaphores[i]);
            if (vr != VK_SUCCESS)
            {
                Logger::err("createSemaphores: CreateSemaphore failed at index "
                            + std::to_string(i) + " (" + std::to_string(vr) + ")");
                // Clean up the semaphores we already created so we don't leak.
                for (uint32_t j = 0; j < i; ++j)
                {
                    if (semaphores[j] != VK_NULL_HANDLE)
                        logicalDevice->vkd.DestroySemaphore(logicalDevice->device, semaphores[j], nullptr);
                }
                semaphores.clear();
                return semaphores;
            }
        }
        return semaphores;
    }

} // namespace VkHook
