#include "command_buffer.hh"

#include <algorithm>

#include "format.hh"
#include "graphics_pipeline.hh"
#include "image_view.hh"
#include "logical_swapchain.hh"
#include "store.hh"
#include "util.hh"

namespace VkHook
{
    // `descriptorsMayBeInUse`: the descriptor sets of this generation may be
    // referenced by a submission still in flight (recording into an
    // application command buffer against a live generation), so the shader
    // path polls the image's fence (never waits) before rewriting them. Layer
    // buffers built into an unpublished generation pass false.
    static void recordDepthResolveSnapshotImpl(LogicalDevice*    logicalDevice,
                                               LogicalSwapchain* logicalSwapchain,
                                               VkCommandBuffer   commandBuffer,
                                               uint32_t          imageIndex,
                                               const DepthState& depthState,
                                               bool              descriptorsMayBeInUse,
                                               bool              callerHoldsStateLock);

    static bool hasDepthResolveResources(const LogicalSwapchain* logicalSwapchain, size_t commandBufferCount)
    {
        return logicalSwapchain != nullptr
               && logicalSwapchain->depthResolvePerImage.size() == commandBufferCount
               && (!logicalSwapchain->depthResolveMsaaTransform
                   || logicalSwapchain->depthResolveTransformedPerImage.size() == commandBufferCount);
    }

    static bool hasDepthState(const DepthState& state)
    {
        return state.image != VK_NULL_HANDLE && state.imageView != VK_NULL_HANDLE && state.format != VK_FORMAT_UNDEFINED;
    }

    static VkImageLayout getObservedDepthAttachmentLayout(const DepthState& depthState)
    {
        return depthState.observedLayout != VK_IMAGE_LAYOUT_UNDEFINED
            ? depthState.observedLayout
            : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    static VkPipelineStageFlags getObservedDepthSourceStages(const DepthState& depthState)
    {
        if (getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_GENERAL)
            return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        if (getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
            || getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            return VK_PIPELINE_STAGE_TRANSFER_BIT;

        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }

    static VkAccessFlags getObservedDepthSourceAccess(const DepthState& depthState)
    {
        if (getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_GENERAL)
            return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        if (getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
            return VK_ACCESS_TRANSFER_WRITE_BIT;
        if (getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            return VK_ACCESS_TRANSFER_READ_BIT;

        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }

    // Restore barriers use the same mask as source barriers — both describe
    // the depth attachment's observed layout. Kept as a separate name so
    // call sites read naturally.
    static VkPipelineStageFlags getObservedDepthRestoreStages(const DepthState& depthState)
    {
        return getObservedDepthSourceStages(depthState);
    }

    static VkAccessFlags getObservedDepthRestoreAccess(const DepthState& depthState)
    {
        return getObservedDepthSourceAccess(depthState);
    }

    static bool shouldKeepObservedDepthInGeneralLayout(const DepthState& depthState)
    {
        return getObservedDepthAttachmentLayout(depthState) == VK_IMAGE_LAYOUT_GENERAL;
    }

    static VkImageLayout getInternalDepthReadOnlyLayout(VkFormat format)
    {
        return isStencilFormat(format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                       : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    }

    static VkImageLayout getDepthResolveReadOnlyLayout(const LogicalSwapchain* logicalSwapchain)
    {
        if (!logicalSwapchain)
            return VK_IMAGE_LAYOUT_UNDEFINED;

        return (logicalSwapchain->depthResolveUsesShader || logicalSwapchain->depthResolveMsaaTransform)
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : getInternalDepthReadOnlyLayout(logicalSwapchain->depthResolveFormat);
    }

    // Looks the sample view up in the device's tracking maps (stateLock held).
    // Never creates it: `needsCreate` is set when the tracked slot is empty and
    // the caller must create the view with no lock held and publish it with
    // createAndPublishTrackedDepthSampleView. `view` is the app view as the fallback.
    struct SampleViewLookup
    {
        VkImageView view        = VK_NULL_HANDLE;
        bool        needsCreate = false;
    };

    static SampleViewLookup lookupTrackedDepthSampleViewLocked(LogicalDevice* logicalDevice, const DepthState& depthState)
    {
        SampleViewLookup out;
        out.view = depthState.imageView;

        auto it = logicalDevice->depthViewStates.find(depthState.imageView);
        if (it != logicalDevice->depthViewStates.end() && it->second.imageView != VK_NULL_HANDLE)
        {
            out.view = it->second.imageView;
            return out;
        }

        // The persistent storage image is not in the parallel-indexed
        // depthImages/depthFormats/depthImageViews vectors (app images only).
        if (depthState.image == logicalDevice->depthCaptureStorage.image
            && logicalDevice->persistentStorageTracked)
            return out;

        auto imageIt = std::find(logicalDevice->depthImages.begin(), logicalDevice->depthImages.end(), depthState.image);
        if (imageIt == logicalDevice->depthImages.end())
            return out;

        const size_t index = std::distance(logicalDevice->depthImages.begin(), imageIt);
        if (index >= logicalDevice->depthFormats.size() || index >= logicalDevice->depthImageViews.size())
        {
            Logger::warn("lookupTrackedDepthSampleView: index " + std::to_string(index)
                         + " out of range (depthFormats=" + std::to_string(logicalDevice->depthFormats.size())
                         + " depthImageViews=" + std::to_string(logicalDevice->depthImageViews.size())
                         + "); returning app view");
            return out;
        }

        const VkImageView tracked = logicalDevice->depthImageViews[index];
        if (tracked != VK_NULL_HANDLE)
            out.view = tracked;
        else
            out.needsCreate = true;
        return out;
    }

    // Creates the missing tracked sample view with no lock held (a driver call)
    // and publishes it under stateLock; a concurrent creator's view wins and
    // ours, never used by the GPU, is destroyed.
    static VkImageView createAndPublishTrackedDepthSampleView(LogicalDevice* logicalDevice,
                                                              const DepthState& depthState,
                                                              bool callerHoldsStateLock)
    {
        const VkImageView created = createSingleImageView(
            logicalDevice, depthState.format, depthState.image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (created == VK_NULL_HANDLE)
            return depthState.imageView;

        std::unique_lock<std::mutex> lock(logicalDevice->stateLock, std::defer_lock);
        if (!callerHoldsStateLock)
            lock.lock();
        auto imageIt = std::find(logicalDevice->depthImages.begin(), logicalDevice->depthImages.end(), depthState.image);
        const size_t index = std::distance(logicalDevice->depthImages.begin(), imageIt);
        if (imageIt == logicalDevice->depthImages.end() || index >= logicalDevice->depthImageViews.size())
        {
            logicalDevice->vkd.DestroyImageView(logicalDevice->device, created, nullptr);
            return depthState.imageView;
        }
        VkImageView& slot = logicalDevice->depthImageViews[index];
        if (slot == VK_NULL_HANDLE)
        {
            slot = created;
            return created;
        }
        logicalDevice->vkd.DestroyImageView(logicalDevice->device, created, nullptr);
        return slot;
    }

    // must match DepthParams in the resolve shader: five int32, glsl bool is
    // 4 bytes not c++ bool's 1 (static_assert below catches drift).
    static void pushDepthTransformConstants(LogicalDevice* logicalDevice, LogicalSwapchain* logicalSwapchain, VkCommandBuffer commandBuffer)
    {
        if (logicalSwapchain->depthResolvePipelineLayout == VK_NULL_HANDLE)
            return;

        struct DepthPushConstants
        {
            int32_t depthMode;
            int32_t invertDepth;
            int32_t normalize;
            int32_t flipDepth;
            int32_t flopDepth;
        };
        static_assert(sizeof(DepthPushConstants) == sizeof(int32_t) * 5,
                      "DepthPushConstants size must be 20 bytes to match GLSL");

        DepthPushConstants pushData = {};
        const auto settings = ConfigStore::instance().snapshot();
        pushData.depthMode = static_cast<int32_t>(std::clamp(settings->depthSourceChannel, 0, 6));
        pushData.invertDepth = settings->depthInvert ? 1 : 0;
        pushData.normalize = 1;
        pushData.flipDepth = settings->depthFlip ? 1 : 0;
        pushData.flopDepth = settings->depthFlop ? 1 : 0;

        logicalDevice->vkd.CmdPushConstants(commandBuffer,
                                            logicalSwapchain->depthResolvePipelineLayout,
                                            VK_SHADER_STAGE_FRAGMENT_BIT,
                                            0,
                                            sizeof(DepthPushConstants),
                                            &pushData);
    }

    static void recordDepthResolveSnapshotViaShader(LogicalDevice*    logicalDevice,
                                                    LogicalSwapchain* logicalSwapchain,
                                                    VkCommandBuffer   commandBuffer,
                                                    uint32_t          imageIndex,
                                                    const DepthState& depthState,
                                                    bool              descriptorsMayBeInUse,
                                                    bool              extentTracked,
                                                    const VkExtent3D& tracked,
                                                    VkImageView       sampleView)
    {
        if (!logicalDevice || !logicalSwapchain || imageIndex >= logicalSwapchain->depthResolveDescriptorSets.size()
            || imageIndex >= logicalSwapchain->depthResolveFramebuffers.size())
            return;

        // CRITICAL: Zero-trust resource management - verify depth state is still valid
        // (the caller read the tracked extent under the lock) before any descriptor
        // update or command recording.
        if (!extentTracked)
        {
            Logger::debug("recordDepthResolveSnapshotViaShader: depth image no longer tracked, skipping");
            return;
        }
        if (tracked.width != depthState.extent.width || tracked.height != depthState.extent.height || tracked.depth != depthState.extent.depth)
        {
            Logger::debug("recordDepthResolveSnapshotViaShader: depth handle recycled, skipping");
            return;
        }

        // Non-blocking fence poll (GetFenceStatus, never a wait): skip the
        // descriptor update if the submission that last used this image may
        // still be reading the set. The fences live in the swapchain's shared
        // frame ring and are reset only by the submit path; a signalled fence
        // means that submission finished.
        if (descriptorsMayBeInUse && logicalSwapchain->shared
            && logicalSwapchain->fenceUsed(imageIndex)
            && imageIndex < logicalSwapchain->shared->effectFences.size()
            && logicalSwapchain->shared->effectFences[imageIndex] != VK_NULL_HANDLE)
        {
            VkResult status = logicalDevice->vkd.GetFenceStatus(
                logicalDevice->device, logicalSwapchain->shared->effectFences[imageIndex]);
            if (status != VK_SUCCESS)
            {
                Logger::debug("recordDepthResolveSnapshotViaShader: fence not ready, skipping snapshot");
                return;
            }
        }

        VkDescriptorImageInfo imageInfo = {};
        imageInfo.sampler = logicalSwapchain->depthResolveSampler;
        imageInfo.imageView = sampleView;
        imageInfo.imageLayout = shouldKeepObservedDepthInGeneralLayout(depthState)
            ? VK_IMAGE_LAYOUT_GENERAL
            : getInternalDepthReadOnlyLayout(depthState.format);

        VkWriteDescriptorSet writeDescriptorSet = {};
        writeDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeDescriptorSet.dstSet = logicalSwapchain->depthResolveDescriptorSets[imageIndex];
        writeDescriptorSet.dstBinding = 0;
        writeDescriptorSet.descriptorCount = 1;
        writeDescriptorSet.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writeDescriptorSet.pImageInfo = &imageInfo;
        logicalDevice->vkd.UpdateDescriptorSets(logicalDevice->device, 1, &writeDescriptorSet, 0, nullptr);

        VkImageMemoryBarrier sourceBarrier = {};
        sourceBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        sourceBarrier.image = depthState.image;
        sourceBarrier.oldLayout = getObservedDepthAttachmentLayout(depthState);
        sourceBarrier.newLayout = shouldKeepObservedDepthInGeneralLayout(depthState)
            ? VK_IMAGE_LAYOUT_GENERAL
            : getInternalDepthReadOnlyLayout(depthState.format);
        sourceBarrier.srcAccessMask = getObservedDepthSourceAccess(depthState);
        sourceBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceBarrier.subresourceRange.aspectMask =
            isStencilFormat(depthState.format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
        sourceBarrier.subresourceRange.baseMipLevel = 0;
        sourceBarrier.subresourceRange.levelCount = 1;
        sourceBarrier.subresourceRange.baseArrayLayer = 0;
        sourceBarrier.subresourceRange.layerCount = 1;

        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               getObservedDepthSourceStages(depthState),
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &sourceBarrier);

        VkImageMemoryBarrier resolveBarrier = {};
        resolveBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        resolveBarrier.image = logicalSwapchain->depthResolvePerImage[imageIndex].image;
        resolveBarrier.oldLayout = logicalSwapchain->depthResolvePerImage[imageIndex].initialized
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_UNDEFINED;
        resolveBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        resolveBarrier.srcAccessMask = logicalSwapchain->depthResolvePerImage[imageIndex].initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
        resolveBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        resolveBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        resolveBarrier.subresourceRange.baseMipLevel = 0;
        resolveBarrier.subresourceRange.levelCount = 1;
        resolveBarrier.subresourceRange.baseArrayLayer = 0;
        resolveBarrier.subresourceRange.layerCount = 1;

        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               logicalSwapchain->depthResolvePerImage[imageIndex].initialized
                                                   ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &resolveBarrier);

        VkRenderPassBeginInfo renderPassBeginInfo = {};
        renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassBeginInfo.renderPass = logicalSwapchain->depthResolveRenderPass;
        renderPassBeginInfo.framebuffer = logicalSwapchain->depthResolveFramebuffers[imageIndex];
        renderPassBeginInfo.renderArea.offset = {0, 0};
        renderPassBeginInfo.renderArea.extent = {logicalSwapchain->depthResolveExtent.width, logicalSwapchain->depthResolveExtent.height};
        VkClearValue clearValue = {};
        clearValue.color.float32[0] = 1.0f;
        renderPassBeginInfo.clearValueCount = 1;
        renderPassBeginInfo.pClearValues = &clearValue;

        logicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        logicalDevice->vkd.CmdBindDescriptorSets(commandBuffer,
                                                  VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                  logicalSwapchain->depthResolvePipelineLayout,
                                                  0,
                                                  1,
                                                  &logicalSwapchain->depthResolveDescriptorSets[imageIndex],
                                                  0,
                                                  nullptr);
        logicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, logicalSwapchain->depthResolvePipeline);
        cmdSetViewportScissor(logicalDevice, commandBuffer, renderPassBeginInfo.renderArea.extent);
        
        pushDepthTransformConstants(logicalDevice, logicalSwapchain, commandBuffer);
        
        logicalDevice->vkd.CmdDraw(commandBuffer, 3, 1, 0, 0);
        logicalDevice->vkd.CmdEndRenderPass(commandBuffer);

        logicalSwapchain->depthResolvePerImage[imageIndex].initialized = true;

        if (shouldKeepObservedDepthInGeneralLayout(depthState))
            return;

        sourceBarrier.oldLayout = getInternalDepthReadOnlyLayout(depthState.format);
        sourceBarrier.newLayout = getObservedDepthAttachmentLayout(depthState);
        sourceBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceBarrier.dstAccessMask = getObservedDepthRestoreAccess(depthState);
        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                               getObservedDepthRestoreStages(depthState),
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &sourceBarrier);
    }

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

        VkResult result = logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &allocInfo, commandBuffers.data());
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
                             const DepthState&                              depthState,
                             uint32_t                                       imageIndex)
    {
        VkCommandBufferBeginInfo beginInfo = {};

        beginInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.pNext            = nullptr;
        beginInfo.flags            = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        beginInfo.pInheritanceInfo = nullptr;

        const bool hasDepthResolveTarget = hasDepthResolveResources(logicalSwapchain, commandBuffers.size());

        // Defensive: if the resolve resources exist but their baked source view
        // doesn't match the current depth state (e.g. depth changed but realloc
        // hasn't run yet), treat the resolve target as unusable this frame.
        // Effects will get VK_NULL_HANDLE for the depth image and skip their
        // depth reads rather than sampling a stale/destroyed view.
        const bool depthResolveTargetUsable = hasDepthResolveTarget
            && (!hasDepthState(depthState)
                || logicalSwapchain->depthResolveSourceView == VK_NULL_HANDLE
                || logicalSwapchain->depthResolveSourceView == depthState.imageView);

        for (uint32_t i = 0; i < commandBuffers.size(); i++)
        {
            if (imageIndex != UINT32_MAX && imageIndex != i)
                continue;
            const VkImageView  boundDepthImageView = depthResolveTargetUsable
                ? (logicalSwapchain->depthResolveMsaaTransform ? logicalSwapchain->depthResolveTransformedPerImage[i].imageView
                                                              : logicalSwapchain->depthResolvePerImage[i].imageView)
                : VK_NULL_HANDLE;
            const VkImageLayout boundDepthLayout   = depthResolveTargetUsable ? getDepthResolveReadOnlyLayout(logicalSwapchain)
                                                                               : VK_IMAGE_LAYOUT_UNDEFINED;
            for (auto& effect : effects)
            {
                effect->useDepthImage(i, boundDepthImageView, boundDepthLayout);
            }

            VkResult result = logicalDevice->vkd.BeginCommandBuffer(commandBuffers[i], &beginInfo);
            ASSERT_VULKAN(result);

            if (depthResolveTargetUsable && hasDepthState(depthState))
            {
                // A native-binding re-record may replace a buffer which never
                // ran. Its CPU recording flag is not evidence of GPU contents.
                // This image's fence is complete and the snapshot is overwritten.
                if (imageIndex != UINT32_MAX)
                    logicalSwapchain->depthResolvePerImage[i].initialized = false;
                recordDepthResolveSnapshotImpl(
                    logicalDevice, logicalSwapchain, commandBuffers[i], i, depthState, false, false);
            }

            for (uint32_t j = 0; j < effects.size(); j++)
            {
                effects[j]->applyEffect(i, commandBuffers[i]);
            }

            result = logicalDevice->vkd.EndCommandBuffer(commandBuffers[i]);
            ASSERT_VULKAN(result);
        }
    }

    // Resolve a multisampled depth attachment into the 1-sample resolve target
    // via the depth-stencil resolve subpass render pass built in
    // initializeDepthResolveLayout. No draws are recorded; the resolve happens
    // implicitly at subpass end. The resolve target ends in the read-only depth
    // layout so effects can sample it.
    static void recordDepthResolveSnapshotViaMsaaSubpass(LogicalDevice*     logicalDevice,
                                                         LogicalSwapchain*  logicalSwapchain,
                                                         VkCommandBuffer    commandBuffer,
                                                         uint32_t           imageIndex,
                                                         const DepthState&  depthState)
    {
        if (imageIndex >= logicalSwapchain->depthResolveMsaaFramebuffers.size())
            return;

        const bool sourceInGeneral = (depthState.observedLayout == VK_IMAGE_LAYOUT_GENERAL);

        // When the MSAA source is in GENERAL layout, barrier it to
        // DEPTH_STENCIL_ATTACHMENT_OPTIMAL for the resolve subpass (which
        // requires attachment-optimal as its initial/final layout). We restore it
        // to GENERAL after.
        if (sourceInGeneral)
        {
            VkImageMemoryBarrier toAttachment = {};
            toAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toAttachment.image = depthState.image;
            toAttachment.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            toAttachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            toAttachment.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            toAttachment.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            toAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toAttachment.subresourceRange.aspectMask =
                isStencilFormat(depthState.format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
            toAttachment.subresourceRange.baseMipLevel = 0;
            toAttachment.subresourceRange.levelCount = 1;
            toAttachment.subresourceRange.baseArrayLayer = 0;
            toAttachment.subresourceRange.layerCount = 1;
            logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                                   0, 0, nullptr, 0, nullptr, 1, &toAttachment);
        }

        VkRenderPassBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = logicalSwapchain->depthResolveMsaaRenderPass;
        beginInfo.framebuffer = logicalSwapchain->depthResolveMsaaFramebuffers[imageIndex];
        beginInfo.renderArea.offset = {0, 0};
        beginInfo.renderArea.extent = {logicalSwapchain->depthResolveExtent.width, logicalSwapchain->depthResolveExtent.height};
        beginInfo.clearValueCount = 0;
        beginInfo.pClearValues = nullptr;

        logicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
        logicalDevice->vkd.CmdEndRenderPass(commandBuffer);

        // Restore source layout if it was GENERAL.
        if (sourceInGeneral)
        {
            VkImageMemoryBarrier toGeneral = {};
            toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toGeneral.image = depthState.image;
            toGeneral.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toGeneral.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            toGeneral.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.subresourceRange.aspectMask =
                isStencilFormat(depthState.format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
            toGeneral.subresourceRange.baseMipLevel = 0;
            toGeneral.subresourceRange.levelCount = 1;
            toGeneral.subresourceRange.baseArrayLayer = 0;
            toGeneral.subresourceRange.layerCount = 1;
            logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                   0, 0, nullptr, 0, nullptr, 1, &toGeneral);
        }

        logicalSwapchain->depthResolvePerImage[imageIndex].initialized = true;

        VKHOOK_LOG_DEBUG([&](std::string& s) {
            s = "depth MSAA resolve via subpass: image=" + convertToString(depthState.image)
                + " samples=" + convertToString(depthState.samples)
                + " mode=" + std::to_string(logicalSwapchain->depthResolveMode)
                + " sourceWasGeneral=" + std::string(sourceInGeneral ? "true" : "false");
        });
    }

    // msaa + transform: the hardware resolve (subpass 0) and the transform
    // sample (subpass 1) share one render pass, so the intermediate's layout
    // and visibility are owned by the pass instead of hand-written barriers.
    static void recordDepthResolveSnapshotViaMsaaTransform(LogicalDevice*    logicalDevice,
                                                           LogicalSwapchain* logicalSwapchain,
                                                           VkCommandBuffer   commandBuffer,
                                                           uint32_t          imageIndex,
                                                           const DepthState& depthState)
    {
        if (imageIndex >= logicalSwapchain->depthResolveTransformFramebuffers.size()
            || imageIndex >= logicalSwapchain->depthResolveDescriptorSets.size())
            return;

        const bool sourceInGeneral = (depthState.observedLayout == VK_IMAGE_LAYOUT_GENERAL);

        // The fused pass requires attachment-optimal for its depth source, the
        // same as the plain resolve subpass. Restore GENERAL after.
        if (sourceInGeneral)
        {
            VkImageMemoryBarrier toAttachment = {};
            toAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toAttachment.image = depthState.image;
            toAttachment.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            toAttachment.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            toAttachment.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            toAttachment.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            toAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toAttachment.subresourceRange.aspectMask =
                isStencilFormat(depthState.format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
            toAttachment.subresourceRange.baseMipLevel = 0;
            toAttachment.subresourceRange.levelCount = 1;
            toAttachment.subresourceRange.baseArrayLayer = 0;
            toAttachment.subresourceRange.layerCount = 1;
            logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                                   0, 0, nullptr, 0, nullptr, 1, &toAttachment);
        }

        VkRenderPassBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = logicalSwapchain->depthResolveTransformRenderPass;
        beginInfo.framebuffer = logicalSwapchain->depthResolveTransformFramebuffers[imageIndex];
        beginInfo.renderArea.offset = {0, 0};
        beginInfo.renderArea.extent = {logicalSwapchain->depthResolveExtent.width, logicalSwapchain->depthResolveExtent.height};
        beginInfo.clearValueCount = 0;
        beginInfo.pClearValues = nullptr;

        logicalDevice->vkd.CmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
        logicalDevice->vkd.CmdNextSubpass(commandBuffer, VK_SUBPASS_CONTENTS_INLINE);
        logicalDevice->vkd.CmdBindDescriptorSets(commandBuffer,
                                                  VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                  logicalSwapchain->depthResolvePipelineLayout,
                                                  0,
                                                  1,
                                                  &logicalSwapchain->depthResolveDescriptorSets[imageIndex],
                                                  0,
                                                  nullptr);
        logicalDevice->vkd.CmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, logicalSwapchain->depthResolvePipeline);
        cmdSetViewportScissor(logicalDevice, commandBuffer, beginInfo.renderArea.extent);
        pushDepthTransformConstants(logicalDevice, logicalSwapchain, commandBuffer);
        logicalDevice->vkd.CmdDraw(commandBuffer, 3, 1, 0, 0);
        logicalDevice->vkd.CmdEndRenderPass(commandBuffer);

        if (sourceInGeneral)
        {
            VkImageMemoryBarrier toGeneral = {};
            toGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toGeneral.image = depthState.image;
            toGeneral.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            toGeneral.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            toGeneral.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toGeneral.subresourceRange.aspectMask =
                isStencilFormat(depthState.format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;
            toGeneral.subresourceRange.baseMipLevel = 0;
            toGeneral.subresourceRange.levelCount = 1;
            toGeneral.subresourceRange.baseArrayLayer = 0;
            toGeneral.subresourceRange.layerCount = 1;
            logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                   0, 0, nullptr, 0, nullptr, 1, &toGeneral);
        }

        logicalSwapchain->depthResolvePerImage[imageIndex].initialized = true;

        VKHOOK_LOG_DEBUG([&](std::string& s) {
            s = "depth MSAA transform via fused pass: image=" + convertToString(depthState.image)
                + " samples=" + convertToString(depthState.samples)
                + " mode=" + std::to_string(logicalSwapchain->depthResolveMode)
                + " sourceWasGeneral=" + std::string(sourceInGeneral ? "true" : "false");
        });
    }

    // MSAA depth resolve FALLBACK via vkCmdResolveImage.
    // This mirrors ReShade's approach: use the plain vkCmdResolveImage command
    // (available since Vulkan 1.0) instead of the depth-stencil resolve subpass
    // (which requires Vulkan 1.2 or VK_KHR_depth_stencil_resolve).
    //
    // vkCmdResolveImage resolves all samples by averaging (for depth) — there's
    // no way to pick a specific sample or mode. For depth effects, averaging is
    // usually fine; the alternative (sample-zero) requires the extension.
    //
    // Layout transitions:
    //   source (MSAA depth):  ATTACHMENT_OPTIMAL → TRANSFER_SRC_OPTIMAL → ATTACHMENT_OPTIMAL
    //   target (1-sample):    read-only → TRANSFER_DST_OPTIMAL → read-only
    static void recordDepthResolveSnapshotViaCmdResolveImage(LogicalDevice*     logicalDevice,
                                                             LogicalSwapchain*  logicalSwapchain,
                                                             VkCommandBuffer    commandBuffer,
                                                             uint32_t           imageIndex,
                                                             const DepthState&  depthState)
    {
        if (imageIndex >= logicalSwapchain->depthResolvePerImage.size())
            return;

        const VkImageLayout observedLayout = getObservedDepthAttachmentLayout(depthState);
        const bool sourceInGeneral = shouldKeepObservedDepthInGeneralLayout(depthState);
        const VkImageAspectFlags aspectMask =
            isStencilFormat(depthState.format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_DEPTH_BIT;

        // --- Source barrier: MSAA depth → TRANSFER_SRC_OPTIMAL ---
        VkImageMemoryBarrier srcBarrier = {};
        srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        srcBarrier.image = depthState.image;
        srcBarrier.oldLayout = observedLayout;
        srcBarrier.newLayout = sourceInGeneral ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        srcBarrier.srcAccessMask = getObservedDepthSourceAccess(depthState);
        srcBarrier.dstAccessMask = sourceInGeneral
            ? (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)
            : VK_ACCESS_TRANSFER_READ_BIT;
        srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBarrier.subresourceRange.aspectMask = aspectMask;
        srcBarrier.subresourceRange.baseMipLevel = 0;
        srcBarrier.subresourceRange.levelCount = 1;
        srcBarrier.subresourceRange.baseArrayLayer = 0;
        srcBarrier.subresourceRange.layerCount = 1;

        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               getObservedDepthSourceStages(depthState),
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0, 0, nullptr, 0, nullptr, 1, &srcBarrier);

        // --- Target barrier: read-only → TRANSFER_DST_OPTIMAL ---
        VkImageMemoryBarrier dstBarrier = {};
        dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        dstBarrier.image = logicalSwapchain->depthResolvePerImage[imageIndex].image;
        dstBarrier.oldLayout = logicalSwapchain->depthResolvePerImage[imageIndex].initialized
            ? getInternalDepthReadOnlyLayout(logicalSwapchain->depthResolveFormat)
            : VK_IMAGE_LAYOUT_UNDEFINED;
        dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dstBarrier.srcAccessMask = logicalSwapchain->depthResolvePerImage[imageIndex].initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
        dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        dstBarrier.subresourceRange.baseMipLevel = 0;
        dstBarrier.subresourceRange.levelCount = 1;
        dstBarrier.subresourceRange.baseArrayLayer = 0;
        dstBarrier.subresourceRange.layerCount = 1;

        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               logicalSwapchain->depthResolvePerImage[imageIndex].initialized
                                                   ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0, 0, nullptr, 0, nullptr, 1, &dstBarrier);

        // --- vkCmdResolveImage: MSAA depth → 1-sample resolve target ---
        VkImageResolve resolveRegion = {};
        resolveRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        resolveRegion.srcSubresource.mipLevel = 0;
        resolveRegion.srcSubresource.baseArrayLayer = 0;
        resolveRegion.srcSubresource.layerCount = 1;
        resolveRegion.srcOffset = {0, 0, 0};
        resolveRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        resolveRegion.dstSubresource.mipLevel = 0;
        resolveRegion.dstSubresource.baseArrayLayer = 0;
        resolveRegion.dstSubresource.layerCount = 1;
        resolveRegion.dstOffset = {0, 0, 0};
        resolveRegion.extent = {depthState.extent.width, depthState.extent.height, 1};

        logicalDevice->vkd.CmdResolveImage(commandBuffer,
                                            depthState.image,
                                            sourceInGeneral ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                            logicalSwapchain->depthResolvePerImage[imageIndex].image,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            1,
                                            &resolveRegion);

        // --- Target barrier: TRANSFER_DST_OPTIMAL → read-only ---
        dstBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dstBarrier.newLayout = getInternalDepthReadOnlyLayout(logicalSwapchain->depthResolveFormat);
        dstBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dstBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                               0, 0, nullptr, 0, nullptr, 1, &dstBarrier);

        logicalSwapchain->depthResolvePerImage[imageIndex].initialized = true;

        // --- Restore source layout if we transitioned away from GENERAL ---
        if (!sourceInGeneral)
        {
            srcBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            srcBarrier.newLayout = observedLayout;
            srcBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            srcBarrier.dstAccessMask = getObservedDepthRestoreAccess(depthState);
            logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                   getObservedDepthRestoreStages(depthState),
                                                   0, 0, nullptr, 0, nullptr, 1, &srcBarrier);
        }

        VKHOOK_LOG_DEBUG([&](std::string& s) {
            s = "depth MSAA resolve via vkCmdResolveImage: image=" + convertToString(depthState.image)
                + " samples=" + convertToString(depthState.samples)
                + " sourceWasGeneral=" + std::string(sourceInGeneral ? "true" : "false");
        });
    }

    static void recordDepthResolveSnapshotImpl(LogicalDevice*    logicalDevice,
                                               LogicalSwapchain* logicalSwapchain,
                                               VkCommandBuffer   commandBuffer,
                                               uint32_t          imageIndex,
                                               const DepthState& depthState,
                                               bool              descriptorsMayBeInUse,
                                               bool              callerHoldsStateLock)
    {
        if (!logicalDevice || !logicalSwapchain)
            return;
        if (!hasDepthState(depthState))
            return;
        if (depthState.extent.width == 0 || depthState.extent.height == 0)
            return;
        if (imageIndex >= logicalSwapchain->depthResolvePerImage.size())
            return;

        // Everything read from the device's depth maps is copied out under
        // stateLock here; the lock is gone before any Vulkan call below.
        bool                  inRenderScope = false;
        bool                  imageTracked  = true;
        bool                  metadataFound = false;
        VkImageUsageFlags     metaUsage     = 0;
        VkSampleCountFlagBits metaSamples   = VK_SAMPLE_COUNT_1_BIT;
        VkImageTiling         metaTiling    = VK_IMAGE_TILING_OPTIMAL;
        bool                  extentTracked = false;
        VkExtent3D            trackedExtent = {0, 0, 1};
        SampleViewLookup      sampleLookup;
        {
            std::unique_lock<std::mutex> lock(logicalDevice->stateLock, std::defer_lock);
            if (!callerHoldsStateLock)
                lock.lock();

            // All snapshot backends need to run outside application rendering:
            // transfer commands cannot run in a render pass, and shader resolves
            // must not nest a render pass or overwrite live graphics bindings.
            const auto scope = logicalDevice->commandBufferDepthStates.find(commandBuffer);
            inRenderScope = scope != logicalDevice->commandBufferDepthStates.end() && scope->second.inRenderScope;

            // Note: persistent storage is tracked separately, not in depthImages.
            if (depthState.image != logicalDevice->depthCaptureStorage.image
                || !logicalDevice->persistentStorageTracked)
            {
                imageTracked = std::find(logicalDevice->depthImages.begin(),
                                         logicalDevice->depthImages.end(),
                                         depthState.image) != logicalDevice->depthImages.end();
            }

            auto metadataIt = logicalDevice->depthImageMetadata.find(depthState.image);
            if (metadataIt != logicalDevice->depthImageMetadata.end())
            {
                metadataFound = true;
                metaUsage     = metadataIt->second.usage;
                metaSamples   = metadataIt->second.samples;
                metaTiling    = metadataIt->second.tiling;
            }

            if (logicalSwapchain->depthResolveUsesShader)
            {
                auto extentIt = logicalDevice->depthImageExtents.find(depthState.image);
                if (extentIt != logicalDevice->depthImageExtents.end())
                {
                    extentTracked = true;
                    trackedExtent = extentIt->second;
                }
                sampleLookup = lookupTrackedDepthSampleViewLocked(logicalDevice, depthState);
            }
        }

        if (inRenderScope)
        {
            Logger::warn("Skipping depth snapshot inside an active render scope");
            return;
        }

        // Defensive: ensure the underlying depth image is still tracked and has
        // the usage flags we need. This catches the race where Roblox (or any
        // app) destroys a depth image between the time the resolve resources
        // were built and the time we record this command buffer. Without this
        // check, vkCmdCopyImage / vkCmdResolveImage would operate on a dangling
        // image handle, producing either a device-loss or silent garbage.
        if (!imageTracked)
        {
            Logger::warn("recordDepthResolveSnapshot: depth image no longer tracked; skipping (image="
                         + convertToString(depthState.image) + ")");
            return;
        }
        if (metadataFound)
        {
            const VkImageUsageFlags required = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if ((metaUsage & required) != required)
            {
                Logger::warn("recordDepthResolveSnapshot: depth image missing required usage flags; skipping (image="
                             + convertToString(depthState.image) + ")");
                return;
            }
        }

        // CRITICAL: if the framebuffer baked into the MSAA resolve path references
        // a different source view than the current depth state, skip the snapshot
        // rather than reading from the wrong image. ensureDepthResolveResources
        // should have already rebuilt the framebuffers, but if for any reason it
        // didn't (race, missed realloc, etc.), this prevents silent corruption.
        if (logicalSwapchain->depthResolveIsMsaa
            && logicalSwapchain->depthResolveSourceView != VK_NULL_HANDLE
            && logicalSwapchain->depthResolveSourceView != depthState.imageView)
        {
            Logger::warn("recordDepthResolveSnapshot: MSAA framebuffer source view mismatch (fb="
                         + convertToString(logicalSwapchain->depthResolveSourceView)
                         + " cur=" + convertToString(depthState.imageView)
                         + "); skipping snapshot until realloc catches up");
            return;
        }

        if (metadataFound)
        {
            VKHOOK_LOG_DEBUG([&](std::string& s) {
                s = "depth snapshot source state: commandBuffer=" + convertToString(commandBuffer)
                    + " image=" + convertToString(depthState.image)
                    + " observedLayout=" + convertToString(depthState.observedLayout)
                    + " usage=" + convertToString(metaUsage)
                    + " samples=" + convertToString(metaSamples)
                    + " tiling=" + convertToString(metaTiling)
                    + " transient=" + std::string((metaUsage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0 ? "true" : "false");
            });
        }
        else
        {
            VKHOOK_LOG_DEBUG([&](std::string& s) {
                s = "depth snapshot source state: commandBuffer=" + convertToString(commandBuffer)
                    + " image=" + convertToString(depthState.image)
                    + " observedLayout=" + convertToString(depthState.observedLayout)
                    + " metadata=missing";
            });
        }

        if (logicalSwapchain->depthResolveUsesShader)
        {
            VkImageView sampleView = sampleLookup.view;
            if (sampleLookup.needsCreate)
                sampleView = createAndPublishTrackedDepthSampleView(logicalDevice, depthState, callerHoldsStateLock);
            recordDepthResolveSnapshotViaShader(logicalDevice, logicalSwapchain, commandBuffer, imageIndex, depthState,
                                                descriptorsMayBeInUse, extentTracked, trackedExtent, sampleView);
            return;
        }

        // msaa + transform: one render pass resolves and transforms; the
        // effects read the transformed target, not the raw intermediate.
        if (logicalSwapchain->depthResolveMsaaTransform
            && logicalSwapchain->depthResolveTransformRenderPass != VK_NULL_HANDLE
            && imageIndex < logicalSwapchain->depthResolveTransformFramebuffers.size())
        {
            recordDepthResolveSnapshotViaMsaaTransform(logicalDevice, logicalSwapchain, commandBuffer, imageIndex, depthState);
            return;
        }

        // MSAA depth resolve via a depth-stencil resolve subpass. The render
        // pass has no draws; the implementation resolves attachment 0 (MSAA
        // depth source) into attachment 1 (1-sample resolve target) at subpass
        // end. Effects then sample attachment 1 in the read-only depth layout.
        if (logicalSwapchain->depthResolveIsMsaa
            && logicalSwapchain->depthResolveMsaaRenderPass != VK_NULL_HANDLE
            && imageIndex < logicalSwapchain->depthResolveMsaaFramebuffers.size())
        {
            recordDepthResolveSnapshotViaMsaaSubpass(logicalDevice, logicalSwapchain, commandBuffer, imageIndex, depthState);
            return;
        }

        // MSAA depth resolve FALLBACK via vkCmdResolveImage.
        // Used when:
        //   - The depth-stencil resolve subpass isn't supported (no
        //     VK_KHR_depth_stencil_resolve, or device doesn't support the
        //     requested resolve mode), OR
        //   - createDepthMsaaResolveRenderPass returned VK_NULL_HANDLE.
        // This is the path ReShade uses for MSAA depth: a plain
        // vkCmdResolveImage with aspectMask=DEPTH. It's supported on every
        // Vulkan device and doesn't require the depth-stencil resolve extension.
        if (logicalSwapchain->depthResolveIsMsaa
            && !logicalSwapchain->depthResolveMsaaTransform
            && (logicalSwapchain->depthResolveMsaaRenderPass == VK_NULL_HANDLE
                || imageIndex >= logicalSwapchain->depthResolveMsaaFramebuffers.size()))
        {
            recordDepthResolveSnapshotViaCmdResolveImage(logicalDevice, logicalSwapchain, commandBuffer, imageIndex, depthState);
            return;
        }

        VkImageMemoryBarrier memoryBarrier;
        memoryBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        memoryBarrier.pNext               = nullptr;
        memoryBarrier.image               = depthState.image;
        const bool keepGeneralLayout = shouldKeepObservedDepthInGeneralLayout(depthState);
        memoryBarrier.oldLayout           = getObservedDepthAttachmentLayout(depthState);
        memoryBarrier.newLayout           = keepGeneralLayout ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        memoryBarrier.srcAccessMask       = getObservedDepthSourceAccess(depthState);
        memoryBarrier.dstAccessMask       = keepGeneralLayout
                                                ? (VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)
                                                : VK_ACCESS_TRANSFER_READ_BIT;
        memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.subresourceRange.aspectMask =
            isStencilFormat(depthState.format) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        memoryBarrier.subresourceRange.baseMipLevel   = 0;
        memoryBarrier.subresourceRange.levelCount     = 1;
        memoryBarrier.subresourceRange.baseArrayLayer = 0;
        memoryBarrier.subresourceRange.layerCount     = 1;

        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               getObservedDepthSourceStages(depthState),
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &memoryBarrier);

        VkImageMemoryBarrier resolveBarrier = {};
        resolveBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        resolveBarrier.pNext               = nullptr;
        resolveBarrier.image               = logicalSwapchain->depthResolvePerImage[imageIndex].image;
        resolveBarrier.oldLayout = logicalSwapchain->depthResolvePerImage[imageIndex].initialized
                                       ? getInternalDepthReadOnlyLayout(logicalSwapchain->depthResolveFormat)
                                       : VK_IMAGE_LAYOUT_UNDEFINED;
        resolveBarrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        resolveBarrier.srcAccessMask       = logicalSwapchain->depthResolvePerImage[imageIndex].initialized ? VK_ACCESS_SHADER_READ_BIT : 0;
        resolveBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        resolveBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        resolveBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        resolveBarrier.subresourceRange.baseMipLevel   = 0;
        resolveBarrier.subresourceRange.levelCount     = 1;
        resolveBarrier.subresourceRange.baseArrayLayer = 0;
        resolveBarrier.subresourceRange.layerCount     = 1;

        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               logicalSwapchain->depthResolvePerImage[imageIndex].initialized
                                                   ? (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &resolveBarrier);

        VkImageCopy copyRegion = {};
        copyRegion.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        copyRegion.srcSubresource.mipLevel       = 0;
        copyRegion.srcSubresource.baseArrayLayer = 0;
        copyRegion.srcSubresource.layerCount     = 1;
        copyRegion.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        copyRegion.dstSubresource.mipLevel       = 0;
        copyRegion.dstSubresource.baseArrayLayer = 0;
        copyRegion.dstSubresource.layerCount     = 1;
        copyRegion.extent.width                  = depthState.extent.width;
        copyRegion.extent.height                 = depthState.extent.height;
        copyRegion.extent.depth                  = 1;

        logicalDevice->vkd.CmdCopyImage(commandBuffer,
                                         depthState.image,
                                         keepGeneralLayout ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         logicalSwapchain->depthResolvePerImage[imageIndex].image,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         1,
                                         &copyRegion);

        resolveBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        resolveBarrier.newLayout     = getInternalDepthReadOnlyLayout(logicalSwapchain->depthResolveFormat);
        resolveBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        resolveBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &resolveBarrier);

        logicalSwapchain->depthResolvePerImage[imageIndex].initialized = true;

        if (!keepGeneralLayout)
        {
            memoryBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            memoryBarrier.newLayout     = getObservedDepthAttachmentLayout(depthState);
            memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            memoryBarrier.dstAccessMask = getObservedDepthRestoreAccess(depthState);
            logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                                   VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                   getObservedDepthRestoreStages(depthState),
                                                   0,
                                                   0,
                                                   nullptr,
                                                   0,
                                                   nullptr,
                                                   1,
                                                   &memoryBarrier);
        }
    }

    void recordDepthResolveSnapshot(LogicalDevice*  logicalDevice,
                                    LogicalSwapchain* logicalSwapchain,
                                    VkCommandBuffer commandBuffer,
                                    uint32_t imageIndex,
                                    const DepthState& depthState)
    {
        recordDepthResolveSnapshotImpl(logicalDevice, logicalSwapchain, commandBuffer, imageIndex, depthState, true, true);
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
