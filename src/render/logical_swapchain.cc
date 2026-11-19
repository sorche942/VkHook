#include "logical_swapchain.hh"

#include "depth/capture.hh"
#include "gpu/retire.hh"
#include "logger.hh"

#include <type_traits>

namespace VkHook
{
    void SwapchainShared::release(bool gpuIdle)
    {
        if (!logicalDevice)
            return;
        LogicalDevice& d = *logicalDevice;
        // the renderer retires its own objects; it goes before the images it draws into
        overlay.reset();
        depthCapture.reset();
        auto drop = [&](VkObjectType type, auto handle) {
            if (!handle)
                return;
            if (!gpuIdle)
            {
                d.retire(type, handle);
                return;
            }
            using Handle = decltype(handle);
            if constexpr (std::is_same_v<Handle, VkFence>)
                d.vkd.DestroyFence(d.device, handle, nullptr);
            else if constexpr (std::is_same_v<Handle, VkSemaphore>)
                d.vkd.DestroySemaphore(d.device, handle, nullptr);
            else if constexpr (std::is_same_v<Handle, VkImageView>)
                d.vkd.DestroyImageView(d.device, handle, nullptr);
            else if constexpr (std::is_same_v<Handle, VkImage>)
                d.vkd.DestroyImage(d.device, handle, nullptr);
            else if constexpr (std::is_same_v<Handle, VkDeviceMemory>)
                d.vkd.FreeMemory(d.device, handle, nullptr);
            else
                d.retire(type, handle);
        };
        for (VkFence fence : effectFences)
            drop(VK_OBJECT_TYPE_FENCE, fence);
        for (VkSemaphore semaphore : semaphores)
            drop(VK_OBJECT_TYPE_SEMAPHORE, semaphore);
        for (VkSemaphore semaphore : overlaySemaphores)
            drop(VK_OBJECT_TYPE_SEMAPHORE, semaphore);
        for (VkImageView view : imageViews)
            drop(VK_OBJECT_TYPE_IMAGE_VIEW, view);
        for (VkImage image : fakeImages)
            drop(VK_OBJECT_TYPE_IMAGE, image);
        for (VkDeviceMemory memory : fakeImageMemories)
            drop(VK_OBJECT_TYPE_DEVICE_MEMORY, memory);
        effectFences.clear();
        semaphores.clear();
        overlaySemaphores.clear();
        imageViews.clear();
        fakeImages.clear();
        fakeImageMemories.clear();
    }

    SwapchainShared::~SwapchainShared()
    {
        release(false);
    }

    std::shared_ptr<LogicalSwapchain> LogicalSwapchain::cloneBase() const
    {
        auto next                     = std::make_shared<LogicalSwapchain>();
        next->logicalDevice           = logicalDevice;
        next->handle                  = handle;
        next->ownedQueueFamilyIndices = ownedQueueFamilyIndices;
        next->swapchainCreateInfo     = swapchainCreateInfo;
        next->swapchainCreateInfo.pQueueFamilyIndices =
            next->ownedQueueFamilyIndices.empty() ? nullptr : next->ownedQueueFamilyIndices.data();
        next->imageExtent             = imageExtent;
        next->format                  = format;
        next->imageCount              = imageCount;
        next->useMutableFormat        = useMutableFormat;
        next->images                  = images;
        next->imageViews              = imageViews;
        next->fakeImages              = fakeImages;
        next->pingImages              = pingImages;
        next->pongImages              = pongImages;
        next->passthrough             = passthrough;
        next->semaphores              = semaphores;
        next->overlaySemaphores       = overlaySemaphores;
        next->effectSubmitFences      = effectSubmitFences;
        next->fakeImageMemories       = fakeImageMemories;
        next->shared                  = shared;
        next->generation              = generation + 1;
        return next;
    }

    void LogicalSwapchain::releaseGeneration()
    {
        // The recorded buffers die with their pool, which the GPU may still be
        // executing: retire the pool, never destroy it here.
        if (logicalDevice && commandPool != VK_NULL_HANDLE)
            logicalDevice->retire(VK_OBJECT_TYPE_COMMAND_POOL, commandPool);
        commandPool = VK_NULL_HANDLE;
        commandBuffersEffect.clear();
        commandBuffersNoEffect.clear();

        // Effects retire their own GPU objects in their destructors.
        effects.clear();
        defaultTransfer.reset();
    }

    LogicalSwapchain::~LogicalSwapchain()
    {
        releaseGeneration();
    }
} // namespace VkHook
