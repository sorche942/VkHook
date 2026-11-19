#include "logical_swapchain.hh"

#include "gpu/retire.hh"
#include "logger.hh"

#include <type_traits>

namespace VkHook
{
namespace
{
    void retireFramebuffers(LogicalDevice* device, std::vector<VkFramebuffer>& framebuffers)
    {
        for (VkFramebuffer framebuffer : framebuffers)
            device->retire(VK_OBJECT_TYPE_FRAMEBUFFER, framebuffer);
        framebuffers.clear();
    }

    void retirePerImage(LogicalDevice* device, std::vector<DepthResolvePerImage>& perImage)
    {
        for (DepthResolvePerImage& image : perImage)
        {
            device->retire(VK_OBJECT_TYPE_IMAGE_VIEW, image.imageView);
            device->retire(VK_OBJECT_TYPE_IMAGE, image.image);
            device->retire(VK_OBJECT_TYPE_DEVICE_MEMORY, image.memory);
        }
        perImage.clear();
    }
} // namespace

    void retireDepthResolveFields(LogicalDevice* device, DepthResolveFields& f)
    {
        f.depthResolveSourceView = VK_NULL_HANDLE;

        retireFramebuffers(device, f.depthResolveFramebuffers);
        retireFramebuffers(device, f.depthResolveMsaaFramebuffers);
        retireFramebuffers(device, f.depthResolveTransformFramebuffers);

        device->retire(VK_OBJECT_TYPE_RENDER_PASS, f.depthResolveMsaaRenderPass);
        f.depthResolveMsaaRenderPass = VK_NULL_HANDLE;
        device->retire(VK_OBJECT_TYPE_RENDER_PASS, f.depthResolveTransformRenderPass);
        f.depthResolveTransformRenderPass = VK_NULL_HANDLE;
        device->retire(VK_OBJECT_TYPE_PIPELINE, f.depthResolvePipeline);
        f.depthResolvePipeline = VK_NULL_HANDLE;
        device->retire(VK_OBJECT_TYPE_PIPELINE_LAYOUT, f.depthResolvePipelineLayout);
        f.depthResolvePipelineLayout = VK_NULL_HANDLE;
        device->retire(VK_OBJECT_TYPE_RENDER_PASS, f.depthResolveRenderPass);
        f.depthResolveRenderPass = VK_NULL_HANDLE;
        // the pool frees its sets
        device->retire(VK_OBJECT_TYPE_DESCRIPTOR_POOL, f.depthResolveDescriptorPool);
        f.depthResolveDescriptorPool = VK_NULL_HANDLE;
        device->retire(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, f.depthResolveDescriptorSetLayout);
        f.depthResolveDescriptorSetLayout = VK_NULL_HANDLE;
        device->retire(VK_OBJECT_TYPE_SAMPLER, f.depthResolveSampler);
        f.depthResolveSampler = VK_NULL_HANDLE;

        retirePerImage(device, f.depthResolvePerImage);
        retirePerImage(device, f.depthResolveTransformedPerImage);

        f.depthResolveDescriptorSets.clear();
        f.depthResolveFormat          = VK_FORMAT_UNDEFINED;
        f.depthResolveExtent          = {0, 0, 1};
        f.depthResolveUsesShader      = false;
        f.depthResolveIsMsaa          = false;
        f.depthResolveMsaaTransform   = false;
        f.depthResolveSourceSamples   = VK_SAMPLE_COUNT_1_BIT;
        f.depthResolveMode            = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    }

    void SwapchainShared::release(bool gpuIdle)
    {
        if (!logicalDevice)
            return;
        LogicalDevice& d = *logicalDevice;
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
        next->passthrough             = passthrough;
        next->maxEffectSlots          = maxEffectSlots;
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

        if (logicalDevice)
            retireDepthResolveFields(logicalDevice, *this);
    }

    LogicalSwapchain::~LogicalSwapchain()
    {
        releaseGeneration();
    }
} // namespace VkHook
