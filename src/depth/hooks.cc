#include "layer/entry.hh"
#include "layer/guard.hh"
#include "layer/registry.hh"

#include "depth/tracker.hh"

#include "format.hh"
#include "logger.hh"
#include "util.hh"

#include <string>
#include <type_traits>
#include <vector>

// Depth-capture hooks. Everything in this file is only handed out by
// vkGetDeviceProcAddr while depth capture is on (InterceptGate::DepthCapture),
// so a game that does not use it never calls into the layer from command
// recording. The recording hooks look the device up lock-free and then touch only
// the recorded command buffer's CmdState (depth/tracker.hh); no hook takes a
// shared lock per draw. Each hook runs its bookkeeping inside guardLayerWork (a
// throw soft-disables the device) and ALWAYS forwards the application's call.

namespace VkHook
{
namespace
{
    // How long an app-side destroy waits for the layer's in-flight work that may
    // still read the object. Never fails the destroy.
    constexpr uint32_t kDestroySyncTimeoutMs = 2000;

    // Waits with no lock held until the layer's submissions up to `serial` are
    // done, then lets the app's destroy proceed regardless of the outcome.
    void waitLayerSubmissionsForDestroy(LogicalDevice* logicalDevice, uint64_t serial, const char* entryPoint)
    {
        guardLayerWork(logicalDevice, entryPoint, [&] {
            const VkResult vr = logicalDevice->queues.waitSerial(serial, kDestroySyncTimeoutMs);
            if (vr != VK_SUCCESS)
            {
                VKHOOK_LOG_DEBUG([&](std::string& s) {
                    s = std::string(entryPoint) + ": layer submissions not provably complete after "
                        + std::to_string(kDestroySyncTimeoutMs) + "ms (VkResult " + std::to_string(static_cast<int>(vr))
                        + "); destroying anyway";
                });
            }
        });
    }

    // A tracked depth image is one the layer can sample later: a 2D, single-level,
    // single-layer depth attachment.
    bool isTrackableDepthImage(const VkImageCreateInfo& info)
    {
        return isDepthFormat(info.format) && (info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
               && info.imageType == VK_IMAGE_TYPE_2D && info.mipLevels == 1 && info.arrayLayers == 1;
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Render passes

    template <typename CreateInfo, typename Pfn>
    VkResult createRenderPassTracked(const char* name, VkDevice device, const CreateInfo* pCreateInfo,
                                     const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass,
                                     Pfn DeviceDispatch::*createFn)
    {
        using Attachment = std::remove_cv_t<std::remove_pointer_t<decltype(CreateInfo::pAttachments)>>;

        auto deviceRef = findDevice(GetKey(device));
        if (!deviceRef)
            return failUnknownObject(name, VK_ERROR_DEVICE_LOST);
        LogicalDevice* logicalDevice = deviceRef.get();

        // the layer reads depth after the pass: a DONT_CARE store becomes STORE
        std::vector<Attachment> attachments;
        CreateInfo              modifiedCreateInfo;
        const CreateInfo*       createInfo = pCreateInfo;
        guardLayerWork(logicalDevice, name, [&] {
            if (!pCreateInfo || pCreateInfo->attachmentCount == 0)
                return;
            attachments.assign(pCreateInfo->pAttachments, pCreateInfo->pAttachments + pCreateInfo->attachmentCount);
            bool changed = false;
            for (auto& attachment : attachments)
                changed = forceDepthAttachmentStoreOp(attachment) || changed;
            if (!changed)
                return;
            modifiedCreateInfo              = *pCreateInfo;
            modifiedCreateInfo.pAttachments = attachments.data();
            createInfo                      = &modifiedCreateInfo;
        });

        VkResult vr = (logicalDevice->vkd.*createFn)(device, createInfo, pAllocator, pRenderPass);
        if (vr == VK_SUCCESS && pRenderPass && createInfo && logicalDevice->depth)
            guardLayerWork(logicalDevice, name, [&] { logicalDevice->depth->onCreateRenderPass(*pRenderPass, *createInfo); });
        return vr;
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo,
                                                           const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass)
    {
        return createRenderPassTracked("vkCreateRenderPass", device, pCreateInfo, pAllocator, pRenderPass,
                                       &DeviceDispatch::CreateRenderPass);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
                                                            const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass)
    {
        return createRenderPassTracked("vkCreateRenderPass2", device, pCreateInfo, pAllocator, pRenderPass,
                                       &DeviceDispatch::CreateRenderPass2);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_CreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
                                                               const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass)
    {
        return createRenderPassTracked("vkCreateRenderPass2KHR", device, pCreateInfo, pAllocator, pRenderPass,
                                       &DeviceDispatch::CreateRenderPass2KHR);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_DestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks* pAllocator)
    {
        if (!device)
            return;
        auto deviceRef = findDevice(GetKey(device));
        if (!deviceRef)
        {
            failUnknownObject("vkDestroyRenderPass", VK_ERROR_DEVICE_LOST);
            return;
        }
        if (deviceRef->depth)
            guardLayerWork(deviceRef.get(), "vkDestroyRenderPass", [&] { deviceRef->depth->onDestroyRenderPass(renderPass); });
        deviceRef->vkd.DestroyRenderPass(device, renderPass, pAllocator);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Images, views, framebuffers

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_CreateImage(VkDevice                     device,
                                                      const VkImageCreateInfo*     pCreateInfo,
                                                      const VkAllocationCallbacks* pAllocator,
                                                      VkImage*                     pImage)
    {
        auto deviceRef = findDevice(GetKey(device));
        if (!deviceRef)
            return failUnknownObject("vkCreateImage", VK_ERROR_DEVICE_LOST);
        LogicalDevice* logicalDevice = deviceRef.get();

        if (!logicalDevice->depth || !pCreateInfo || !isTrackableDepthImage(*pCreateInfo))
            return logicalDevice->vkd.CreateImage(device, pCreateInfo, pAllocator, pImage);

        // The capture samples the image, so it needs SAMPLED and the preserving
        // copy needs TRANSFER_SRC. TRANSIENT cannot coexist with either
        // (VUID-VkImageCreateInfo-usage-00963) and on tiled renderers leaves the
        // image without backing outside the app's pass.
        VkImageCreateInfo modifiedCreateInfo = *pCreateInfo;
        modifiedCreateInfo.usage &= ~VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        modifiedCreateInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        const VkResult result = logicalDevice->vkd.CreateImage(device, &modifiedCreateInfo, pAllocator, pImage);
        if (result != VK_SUCCESS)
            return result;
        guardLayerWork(logicalDevice, "vkCreateImage", [&] {
            logicalDevice->depth->onCreateImage(*pImage, *pCreateInfo, modifiedCreateInfo.usage);
            VKHOOK_LOG_DEBUG([&](std::string& s) {
                s = "tracked depth image " + convertToString(*pImage) + " " + convertToString(pCreateInfo->format) + " "
                    + std::to_string(pCreateInfo->extent.width) + "x" + std::to_string(pCreateInfo->extent.height)
                    + " samples: " + convertToString(pCreateInfo->samples);
            });
        });
        return result;
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator)
    {
        if (!image)
            return;
        auto deviceRef = findDevice(GetKey(device));
        if (!deviceRef)
        {
            failUnknownObject("vkDestroyImage", VK_ERROR_DEVICE_LOST);
            return;
        }
        LogicalDevice* logicalDevice = deviceRef.get();

        // A layer submission may still read an image the capture sampled: wait
        // for it with no lock held. An image never selected was never read.
        DepthTracker::Destroyed destroyed;
        uint64_t                submittedSerial = 0;
        if (logicalDevice->depth)
            guardLayerWork(logicalDevice, "vkDestroyImage", [&] {
                submittedSerial = logicalDevice->timeline.submittedSerial();
                destroyed       = logicalDevice->depth->onDestroyImage(image);
            });
        if (destroyed.layerReads)
            waitLayerSubmissionsForDestroy(logicalDevice, submittedSerial, "vkDestroyImage");
        logicalDevice->vkd.DestroyImage(logicalDevice->device, image, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_CreateImageView(VkDevice device,
                                                          const VkImageViewCreateInfo* pCreateInfo,
                                                          const VkAllocationCallbacks* pAllocator,
                                                          VkImageView* view)
    {
        auto deviceRef = findDevice(GetKey(device));
        if (!deviceRef)
            return failUnknownObject("vkCreateImageView", VK_ERROR_DEVICE_LOST);
        LogicalDevice* logicalDevice = deviceRef.get();

        const VkResult result = logicalDevice->vkd.CreateImageView(device, pCreateInfo, pAllocator, view);
        if (result == VK_SUCCESS && logicalDevice->depth)
            guardLayerWork(logicalDevice, "vkCreateImageView", [&] { logicalDevice->depth->onCreateImageView(*view, *pCreateInfo); });
        return result;
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_DestroyImageView(VkDevice device, VkImageView imageView, const VkAllocationCallbacks* pAllocator)
    {
        if (!imageView)
            return;
        auto deviceRef = findDevice(GetKey(device));
        if (!deviceRef)
        {
            failUnknownObject("vkDestroyImageView", VK_ERROR_DEVICE_LOST);
            return;
        }
        // the layer samples through views of its own, never the application's
        if (deviceRef->depth)
            guardLayerWork(deviceRef.get(), "vkDestroyImageView", [&] { deviceRef->depth->onDestroyImageView(imageView); });
        deviceRef->vkd.DestroyImageView(device, imageView, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_CreateFramebuffer(VkDevice device,
                                                            const VkFramebufferCreateInfo* pCreateInfo,
                                                            const VkAllocationCallbacks* pAllocator,
                                                            VkFramebuffer* pFramebuffer)
    {
        auto deviceRef = findDevice(GetKey(device));
        if (!deviceRef)
            return failUnknownObject("vkCreateFramebuffer", VK_ERROR_DEVICE_LOST);
        LogicalDevice* logicalDevice = deviceRef.get();

        const VkResult result = logicalDevice->vkd.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
        if (result == VK_SUCCESS && logicalDevice->depth)
            guardLayerWork(logicalDevice, "vkCreateFramebuffer",
                           [&] { logicalDevice->depth->onCreateFramebuffer(*pFramebuffer, *pCreateInfo); });
        return result;
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_DestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks* pAllocator)
    {
        if (!framebuffer)
            return;
        auto deviceRef = findDevice(GetKey(device));
        if (!deviceRef)
        {
            failUnknownObject("vkDestroyFramebuffer", VK_ERROR_DEVICE_LOST);
            return;
        }
        if (deviceRef->depth)
            guardLayerWork(deviceRef.get(), "vkDestroyFramebuffer", [&] { deviceRef->depth->onDestroyFramebuffer(framebuffer); });
        deviceRef->vkd.DestroyFramebuffer(device, framebuffer, pAllocator);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Render pass / rendering scopes

    // Before the application overwrites a depth image that already drew this
    // frame, the only injection the layer makes: a transfer-only copy into
    // layer-owned storage, so the capture still finds the frame's depth.
    void preserveBeforeOverwrite(DepthTracker& tracker, CmdState& state, VkCommandBuffer commandBuffer, const DepthScope& scope)
    {
        if (scope.image != VK_NULL_HANDLE && scope.clearsOnLoad && tracker.needsPreserve(state, scope.image))
            tracker.recordPreserveCopy(state, commandBuffer, scope.image, scope.initialLayout);
    }

    template <typename Pfn, typename... Extra>
    void trackedBeginRenderPass(const char* name, VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
                                Pfn DeviceDispatch::*beginFn, Extra... extra)
    {
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
        {
            failUnknownObject(name, VK_ERROR_DEVICE_LOST);
            return;
        }
        if (logicalDevice->depth && pRenderPassBegin)
            guardLayerWork(logicalDevice.get(), name, [&] {
                DepthTracker& tracker = *logicalDevice->depth;
                CmdState&     state   = tracker.cmd(commandBuffer);
                const DepthScope scope = tracker.scopeFromRenderPassBegin(*pRenderPassBegin);
                preserveBeforeOverwrite(tracker, state, commandBuffer, scope);
                state.beginScope(scope);
            });
        (logicalDevice->vkd.*beginFn)(commandBuffer, pRenderPassBegin, extra...);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                                                         const VkRenderPassBeginInfo* pRenderPassBegin,
                                                         VkSubpassContents contents)
    {
        trackedBeginRenderPass("vkCmdBeginRenderPass", commandBuffer, pRenderPassBegin, &DeviceDispatch::CmdBeginRenderPass, contents);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdBeginRenderPass2(VkCommandBuffer commandBuffer,
                                                          const VkRenderPassBeginInfo* pRenderPassBegin,
                                                          const VkSubpassBeginInfo* pSubpassBeginInfo)
    {
        trackedBeginRenderPass("vkCmdBeginRenderPass2", commandBuffer, pRenderPassBegin, &DeviceDispatch::CmdBeginRenderPass2, pSubpassBeginInfo);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer,
                                                             const VkRenderPassBeginInfo* pRenderPassBegin,
                                                             const VkSubpassBeginInfo* pSubpassBeginInfo)
    {
        trackedBeginRenderPass("vkCmdBeginRenderPass2KHR", commandBuffer, pRenderPassBegin, &DeviceDispatch::CmdBeginRenderPass2KHR, pSubpassBeginInfo);
    }

    template <typename Pfn>
    void trackedBeginRendering(const char* name, VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo,
                               Pfn DeviceDispatch::*beginFn)
    {
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
        {
            failUnknownObject(name, VK_ERROR_DEVICE_LOST);
            return;
        }
        if (logicalDevice->depth && pRenderingInfo)
            guardLayerWork(logicalDevice.get(), name, [&] {
                DepthTracker& tracker = *logicalDevice->depth;
                CmdState&     state   = tracker.cmd(commandBuffer);
                const DepthScope scope = tracker.scopeFromRenderingInfo(*pRenderingInfo);
                preserveBeforeOverwrite(tracker, state, commandBuffer, scope);
                state.beginScope(scope);
            });

        if (pRenderingInfo && pRenderingInfo->pDepthAttachment)
        {
            VkRenderingAttachmentInfo depthAttachment = *pRenderingInfo->pDepthAttachment;
            if (forceDepthAttachmentStoreOp(depthAttachment))
            {
                VkRenderingInfo renderingInfo  = *pRenderingInfo;
                renderingInfo.pDepthAttachment = &depthAttachment;
                (logicalDevice->vkd.*beginFn)(commandBuffer, &renderingInfo);
                return;
            }
        }
        (logicalDevice->vkd.*beginFn)(commandBuffer, pRenderingInfo);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo)
    {
        trackedBeginRendering("vkCmdBeginRendering", commandBuffer, pRenderingInfo, &DeviceDispatch::CmdBeginRendering);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdBeginRenderingKHR(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo)
    {
        trackedBeginRendering("vkCmdBeginRenderingKHR", commandBuffer, pRenderingInfo, &DeviceDispatch::CmdBeginRenderingKHR);
    }

    template <typename Pfn, typename... Args>
    void trackedEnd(const char* name, VkCommandBuffer commandBuffer, Pfn DeviceDispatch::*endFn, Args... args)
    {
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
        {
            failUnknownObject(name, VK_ERROR_DEVICE_LOST);
            return;
        }
        (logicalDevice->vkd.*endFn)(commandBuffer, args...);
        if (logicalDevice->depth)
            guardLayerWork(logicalDevice.get(), name, [&] { logicalDevice->depth->cmd(commandBuffer).endScope(); });
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdEndRenderPass(VkCommandBuffer commandBuffer)
    {
        trackedEnd("vkCmdEndRenderPass", commandBuffer, &DeviceDispatch::CmdEndRenderPass);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdEndRenderPass2(VkCommandBuffer commandBuffer, const VkSubpassEndInfo* pSubpassEndInfo)
    {
        trackedEnd("vkCmdEndRenderPass2", commandBuffer, &DeviceDispatch::CmdEndRenderPass2, pSubpassEndInfo);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdEndRenderPass2KHR(VkCommandBuffer commandBuffer, const VkSubpassEndInfo* pSubpassEndInfo)
    {
        trackedEnd("vkCmdEndRenderPass2KHR", commandBuffer, &DeviceDispatch::CmdEndRenderPass2KHR, pSubpassEndInfo);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdEndRendering(VkCommandBuffer commandBuffer)
    {
        trackedEnd("vkCmdEndRendering", commandBuffer, &DeviceDispatch::CmdEndRendering);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdEndRenderingKHR(VkCommandBuffer commandBuffer)
    {
        trackedEnd("vkCmdEndRenderingKHR", commandBuffer, &DeviceDispatch::CmdEndRenderingKHR);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Draws: counted into the command buffer's own state; nothing is looked up
    // per draw beyond the thread's cached CmdState.

    template <typename Pfn, typename... Args>
    void trackedDraw(const char* name, VkCommandBuffer commandBuffer, uint32_t drawCount, Pfn DeviceDispatch::*drawFn, Args... args)
    {
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
        {
            failUnknownObject(name, VK_ERROR_DEVICE_LOST);
            return;
        }
        if (logicalDevice->depth)
            guardLayerWork(logicalDevice.get(), name, [&] { logicalDevice->depth->cmd(commandBuffer).draw(drawCount); });
        (logicalDevice->vkd.*drawFn)(commandBuffer, args...);
    }

#define VKHOOK_UNPAREN(...) __VA_ARGS__
#define VKHOOK_DRAW_HOOK(Name, DrawCount, Params, Args) \
    VKAPI_ATTR void VKAPI_CALL VkHook_##Name(VkCommandBuffer commandBuffer, VKHOOK_UNPAREN Params) \
    { \
        trackedDraw("vk" #Name, commandBuffer, DrawCount, &DeviceDispatch::Name, VKHOOK_UNPAREN Args); \
    }

    VKHOOK_DRAW_HOOK(CmdDraw, 1,
        (uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance),
        (vertexCount, instanceCount, firstVertex, firstInstance))
    VKHOOK_DRAW_HOOK(CmdDrawIndexed, 1,
        (uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance),
        (indexCount, instanceCount, firstIndex, vertexOffset, firstInstance))
    VKHOOK_DRAW_HOOK(CmdDrawIndirect, drawCount,
        (VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride),
        (buffer, offset, drawCount, stride))
    VKHOOK_DRAW_HOOK(CmdDrawIndexedIndirect, drawCount,
        (VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride),
        (buffer, offset, drawCount, stride))
    VKHOOK_DRAW_HOOK(CmdDrawIndirectCount, maxDrawCount,
        (VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride),
        (buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride))
    VKHOOK_DRAW_HOOK(CmdDrawIndirectCountKHR, maxDrawCount,
        (VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride),
        (buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride))
    VKHOOK_DRAW_HOOK(CmdDrawIndexedIndirectCount, maxDrawCount,
        (VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride),
        (buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride))
    VKHOOK_DRAW_HOOK(CmdDrawIndexedIndirectCountKHR, maxDrawCount,
        (VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride),
        (buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride))
#undef VKHOOK_DRAW_HOOK
#undef VKHOOK_UNPAREN

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Secondary command buffers, out-of-pass clears

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdExecuteCommands(VkCommandBuffer commandBuffer,
                                                         uint32_t commandBufferCount,
                                                         const VkCommandBuffer* pCommandBuffers)
    {
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
        {
            failUnknownObject("vkCmdExecuteCommands", VK_ERROR_DEVICE_LOST);
            return;
        }
        if (logicalDevice->depth && pCommandBuffers)
            guardLayerWork(logicalDevice.get(), "vkCmdExecuteCommands", [&] {
                DepthTracker& tracker = *logicalDevice->depth;
                CmdState&     primary = tracker.cmd(commandBuffer);
                for (uint32_t i = 0; i < commandBufferCount; ++i)
                    primary.absorb(tracker.cmd(pCommandBuffers[i]));
            });
        logicalDevice->vkd.CmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);
    }

    // The clear's layout argument is the image's exact layout here, so the copy
    // before it needs no guess.
    VKAPI_ATTR void VKAPI_CALL VkHook_CmdClearDepthStencilImage(VkCommandBuffer commandBuffer,
                                                                VkImage image,
                                                                VkImageLayout imageLayout,
                                                                const VkClearDepthStencilValue* pDepthStencil,
                                                                uint32_t rangeCount,
                                                                const VkImageSubresourceRange* pRanges)
    {
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
        {
            failUnknownObject("vkCmdClearDepthStencilImage", VK_ERROR_DEVICE_LOST);
            return;
        }
        if (logicalDevice->depth)
            guardLayerWork(logicalDevice.get(), "vkCmdClearDepthStencilImage", [&] {
                DepthTracker& tracker = *logicalDevice->depth;
                CmdState&     state   = tracker.cmd(commandBuffer);
                if (tracker.needsPreserve(state, image))
                    tracker.recordPreserveCopy(state, commandBuffer, image, imageLayout);
            });
        logicalDevice->vkd.CmdClearDepthStencilImage(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Barriers: the layout log (H11). No lookup: every depth-aspect image barrier
    // is logged and the submit fold keeps the ones that are tracked.

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdPipelineBarrier(VkCommandBuffer commandBuffer,
                                                         VkPipelineStageFlags srcStageMask,
                                                         VkPipelineStageFlags dstStageMask,
                                                         VkDependencyFlags dependencyFlags,
                                                         uint32_t memoryBarrierCount,
                                                         const VkMemoryBarrier* pMemoryBarriers,
                                                         uint32_t bufferMemoryBarrierCount,
                                                         const VkBufferMemoryBarrier* pBufferMemoryBarriers,
                                                         uint32_t imageMemoryBarrierCount,
                                                         const VkImageMemoryBarrier* pImageMemoryBarriers)
    {
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
        {
            failUnknownObject("vkCmdPipelineBarrier", VK_ERROR_DEVICE_LOST);
            return;
        }
        if (logicalDevice->depth && imageMemoryBarrierCount > 0)
            guardLayerWork(logicalDevice.get(), "vkCmdPipelineBarrier", [&] {
                CmdState& state = logicalDevice->depth->cmd(commandBuffer);
                for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i)
                    if (pImageMemoryBarriers[i].subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
                        state.note(pImageMemoryBarriers[i].image, pImageMemoryBarriers[i].newLayout, 0);
            });
        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount,
                                              pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers,
                                              imageMemoryBarrierCount, pImageMemoryBarriers);
    }

    template <typename Pfn>
    void trackedBarrier2(const char* name, VkCommandBuffer commandBuffer, const VkDependencyInfo* pDependencyInfo,
                         Pfn DeviceDispatch::*barrierFn)
    {
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
        {
            failUnknownObject(name, VK_ERROR_DEVICE_LOST);
            return;
        }
        if (logicalDevice->depth && pDependencyInfo && pDependencyInfo->imageMemoryBarrierCount > 0)
            guardLayerWork(logicalDevice.get(), name, [&] {
                CmdState& state = logicalDevice->depth->cmd(commandBuffer);
                for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; ++i)
                {
                    const VkImageMemoryBarrier2& barrier = pDependencyInfo->pImageMemoryBarriers[i];
                    if (barrier.subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)
                        state.note(barrier.image, barrier.newLayout, 0);
                }
            });
        (logicalDevice->vkd.*barrierFn)(commandBuffer, pDependencyInfo);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo* pDependencyInfo)
    {
        trackedBarrier2("vkCmdPipelineBarrier2", commandBuffer, pDependencyInfo, &DeviceDispatch::CmdPipelineBarrier2);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdPipelineBarrier2KHR(VkCommandBuffer commandBuffer, const VkDependencyInfo* pDependencyInfo)
    {
        trackedBarrier2("vkCmdPipelineBarrier2KHR", commandBuffer, pDependencyInfo, &DeviceDispatch::CmdPipelineBarrier2KHR);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Command buffer lifetime (H8): the CmdState follows the buffer, and the
    // storage bound to a recorded buffer is released with it.

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                                                             const VkCommandBufferBeginInfo* pBeginInfo)
    {
        // The loader reaches this hook only for command buffers of a device this
        // layer created (see failUnknownObject): an unknown device means the
        // application uses a command buffer of a destroyed device.
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
            return failUnknownObject("vkBeginCommandBuffer", VK_ERROR_DEVICE_LOST);
        if (logicalDevice->depth)
            guardLayerWork(logicalDevice.get(), "vkBeginCommandBuffer", [&] { logicalDevice->depth->onBeginCommandBuffer(commandBuffer); });
        return logicalDevice->vkd.BeginCommandBuffer(commandBuffer, pBeginInfo);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_AllocateCommandBuffers(VkDevice device,
                                                                 const VkCommandBufferAllocateInfo* pAllocateInfo,
                                                                 VkCommandBuffer* pCommandBuffers)
    {
        auto logicalDevice = findDevice(GetKey(device));
        if (!logicalDevice)
            return failUnknownObject("vkAllocateCommandBuffers", VK_ERROR_DEVICE_LOST);

        const VkResult result = logicalDevice->vkd.AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
        if (result == VK_SUCCESS && pAllocateInfo && pCommandBuffers && logicalDevice->depth)
            guardLayerWork(logicalDevice.get(), "vkAllocateCommandBuffers", [&] {
                logicalDevice->depth->onAllocateCommandBuffers(pAllocateInfo->commandPool, pAllocateInfo->level,
                                                               pAllocateInfo->commandBufferCount, pCommandBuffers);
            });
        return result;
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_FreeCommandBuffers(VkDevice device,
                                                         VkCommandPool commandPool,
                                                         uint32_t commandBufferCount,
                                                         const VkCommandBuffer* pCommandBuffers)
    {
        auto logicalDevice = findDevice(GetKey(device));
        if (!logicalDevice)
        {
            failUnknownObject("vkFreeCommandBuffers", VK_ERROR_DEVICE_LOST);
            return;
        }
        if (pCommandBuffers && logicalDevice->depth)
            guardLayerWork(logicalDevice.get(), "vkFreeCommandBuffers", [&] {
                logicalDevice->depth->onFreeCommandBuffers(commandPool, commandBufferCount, pCommandBuffers);
            });
        logicalDevice->vkd.FreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_ResetCommandPool(VkDevice device, VkCommandPool commandPool, VkCommandPoolResetFlags flags)
    {
        auto logicalDevice = findDevice(GetKey(device));
        if (!logicalDevice)
            return failUnknownObject("vkResetCommandPool", VK_ERROR_DEVICE_LOST);
        if (logicalDevice->depth)
            guardLayerWork(logicalDevice.get(), "vkResetCommandPool", [&] { logicalDevice->depth->onResetCommandPool(commandPool); });
        return logicalDevice->vkd.ResetCommandPool(device, commandPool, flags);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_DestroyCommandPool(VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks* pAllocator)
    {
        auto logicalDevice = findDevice(GetKey(device));
        if (!logicalDevice)
        {
            failUnknownObject("vkDestroyCommandPool", VK_ERROR_DEVICE_LOST);
            return;
        }
        if (logicalDevice->depth)
            guardLayerWork(logicalDevice.get(), "vkDestroyCommandPool", [&] { logicalDevice->depth->onDestroyCommandPool(commandPool); });
        logicalDevice->vkd.DestroyCommandPool(device, commandPool, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_ResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags)
    {
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
            return failUnknownObject("vkResetCommandBuffer", VK_ERROR_DEVICE_LOST);
        if (logicalDevice->depth)
            guardLayerWork(logicalDevice.get(), "vkResetCommandBuffer", [&] { logicalDevice->depth->onResetCommandBuffer(commandBuffer); });
        return logicalDevice->vkd.ResetCommandBuffer(commandBuffer, flags);
    }

    const InterceptEntry kDepthIntercepts[] = {
        VKHOOK_INTERCEPT(DepthCapture, CreateImage),
        VKHOOK_INTERCEPT(DepthCapture, DestroyImage),
        VKHOOK_INTERCEPT(DepthCapture, CreateImageView),
        VKHOOK_INTERCEPT(DepthCapture, DestroyImageView),
        VKHOOK_INTERCEPT(DepthCapture, CreateRenderPass),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CreateRenderPass2, nullptr, VK_API_VERSION_1_2),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CreateRenderPass2KHR, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, 0),
        VKHOOK_INTERCEPT(DepthCapture, DestroyRenderPass),
        VKHOOK_INTERCEPT(DepthCapture, CreateFramebuffer),
        VKHOOK_INTERCEPT(DepthCapture, DestroyFramebuffer),
        VKHOOK_INTERCEPT(DepthCapture, BeginCommandBuffer),
        VKHOOK_INTERCEPT(DepthCapture, AllocateCommandBuffers),
        VKHOOK_INTERCEPT(DepthCapture, FreeCommandBuffers),
        VKHOOK_INTERCEPT(DepthCapture, ResetCommandPool),
        VKHOOK_INTERCEPT(DepthCapture, DestroyCommandPool),
        VKHOOK_INTERCEPT(DepthCapture, ResetCommandBuffer),
        VKHOOK_INTERCEPT(DepthCapture, CmdBeginRenderPass),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CmdBeginRenderPass2, nullptr, VK_API_VERSION_1_2),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CmdBeginRenderPass2KHR, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, 0),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CmdBeginRendering, nullptr, VK_API_VERSION_1_3),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CmdBeginRenderingKHR, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, 0),
        VKHOOK_INTERCEPT(DepthCapture, CmdEndRenderPass),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CmdEndRenderPass2, nullptr, VK_API_VERSION_1_2),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CmdEndRenderPass2KHR, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, 0),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CmdEndRendering, nullptr, VK_API_VERSION_1_3),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CmdEndRenderingKHR, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, 0),
        VKHOOK_INTERCEPT(DepthCapture, CmdClearDepthStencilImage),
        VKHOOK_INTERCEPT(DepthCapture, CmdExecuteCommands),
        VKHOOK_INTERCEPT(DepthCapture, CmdPipelineBarrier),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CmdPipelineBarrier2, nullptr, VK_API_VERSION_1_3),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CmdPipelineBarrier2KHR, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, 0),
        VKHOOK_INTERCEPT(DepthCapture, CmdDraw),
        VKHOOK_INTERCEPT(DepthCapture, CmdDrawIndexed),
        VKHOOK_INTERCEPT(DepthCapture, CmdDrawIndirect),
        VKHOOK_INTERCEPT(DepthCapture, CmdDrawIndexedIndirect),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CmdDrawIndirectCount, nullptr, VK_API_VERSION_1_2),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CmdDrawIndirectCountKHR, VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME, 0),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CmdDrawIndexedIndirectCount, nullptr, VK_API_VERSION_1_2),
        VKHOOK_INTERCEPT_EXT(DepthCapture, CmdDrawIndexedIndirectCountKHR, VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME, 0),
    };
} // namespace

    InterceptTable depthInterceptTable()
    {
        return makeInterceptTable(kDepthIntercepts);
    }
} // namespace VkHook
