#include "layer/entry.hh"
#include "layer/guard.hh"
#include "layer/registry.hh"

#include "control/frontend.hh"
#include "depth/tracker.hh"

#include "command_buffer.hh"
#include "format.hh"
#include "settings_manager.hh"
#include "diag/gpu_crash.hh"
#include "logger.hh"
#include "util.hh"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

// Depth-capture hooks. Everything in this file is only handed out by
// vkGetDeviceProcAddr while depth capture is on (InterceptGate::DepthCapture),
// so a game that does not use it never calls into the layer from command
// recording. Each hook looks the device up lock-free, does its bookkeeping
// under the device's stateLock inside guardLayerWork (a throw soft-disables the
// device), releases the lock and then ALWAYS forwards the application's call.
// The tracker functions marked `Locked` (and the other tracker entry points
// called here) run with dev->stateLock held by the hook.

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

    // Drops everything tracked per command buffer. Needs logicalDevice->stateLock.
    void dropCommandBufferStateLocked(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer)
    {
        logicalDevice->commandBufferRecordedDrawCounts.erase(commandBuffer);
        logicalDevice->commandBufferDepthStates.erase(commandBuffer);
        logicalDevice->pendingTransferLinkedDepthScopes.erase(commandBuffer);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Render passes

    template <typename CreateInfo, typename Pfn>
    VkResult createRenderPassTracked(const char* name, const char* infoName, VkDevice device,
                                     const CreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                                     VkRenderPass* pRenderPass, Pfn DeviceDispatch::*createFn)
    {
        using Attachment = std::remove_cv_t<std::remove_pointer_t<decltype(CreateInfo::pAttachments)>>;

        auto deviceRef = findDevice(GetKey(device));
        if (!deviceRef)
            return failUnknownObject(name, VK_ERROR_DEVICE_LOST);
        LogicalDevice* logicalDevice = deviceRef.get();

        // keep depth attachments STORE so the layer can read them after the pass
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
            modifiedCreateInfo = *pCreateInfo;
            modifiedCreateInfo.pAttachments = attachments.data();
            VKHOOK_LOG_DEBUG([&](std::string& s) {
                s = std::string("forcing depth attachment storeOp=STORE for ") + infoName
                    + " with attachmentCount=" + std::to_string(modifiedCreateInfo.attachmentCount);
            });
            createInfo = &modifiedCreateInfo;
        });

        VkResult vr = (logicalDevice->vkd.*createFn)(device, createInfo, pAllocator, pRenderPass);
        if (vr == VK_SUCCESS && pRenderPass && createInfo)
        {
            guardLayerWork(logicalDevice, name, [&] {
                std::lock_guard<std::mutex> l(logicalDevice->stateLock);
                for (uint32_t ai = 0; ai < createInfo->attachmentCount; ai++)
                {
                    if (isDepthFormat(createInfo->pAttachments[ai].format))
                    {
                        // used by the deferred depth copy to pick the source layout
                        logicalDevice->renderPassDepthFinalLayouts[*pRenderPass] = createInfo->pAttachments[ai].finalLayout;
                        break;
                    }
                }
            });
        }
        return vr;
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* pCreateInfo,
                                                           const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass)
    {
        return createRenderPassTracked("vkCreateRenderPass", "VkRenderPassCreateInfo", device, pCreateInfo, pAllocator,
                                       pRenderPass, &DeviceDispatch::CreateRenderPass);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
                                                            const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass)
    {
        return createRenderPassTracked("vkCreateRenderPass2", "VkRenderPassCreateInfo2", device, pCreateInfo, pAllocator,
                                       pRenderPass, &DeviceDispatch::CreateRenderPass2);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_CreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
                                                               const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass)
    {
        return createRenderPassTracked("vkCreateRenderPass2KHR", "VkRenderPassCreateInfo2KHR", device, pCreateInfo, pAllocator,
                                       pRenderPass, &DeviceDispatch::CreateRenderPass2KHR);
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
        guardLayerWork(deviceRef.get(), "vkDestroyRenderPass", [&] {
            std::lock_guard<std::mutex> l(deviceRef->stateLock);
            deviceRef->renderPassDepthFinalLayouts.erase(renderPass);
        });
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

        if (!isDepthFormat(pCreateInfo->format)
            || (pCreateInfo->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
            return logicalDevice->vkd.CreateImage(device, pCreateInfo, pAllocator, pImage);

        VKHOOK_LOG_DEBUG([&](std::string& s) {
            s = "detected depth image with format: " + convertToString(pCreateInfo->format) + " "
                + std::to_string(pCreateInfo->extent.width) + "x" + std::to_string(pCreateInfo->extent.height)
                + " samples: " + convertToString(pCreateInfo->samples);
        });

        VkImageCreateInfo modifiedCreateInfo = *pCreateInfo;
        // resolve needs TRANSFER_SRC, the shader fallback needs SAMPLED.
        //
        // TRANSIENT can't coexist with either (VUID-VkImageCreateInfo-usage-00963).
        // mobile apps set it for lazy allocation, and on tiled renderers the
        // image then has no backing outside the app's pass — sampling it gives
        // garbage. that's why depth was blank in roblox while colour postproc
        // worked. strip it so the image is sampleable.
        if (modifiedCreateInfo.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)
        {
            modifiedCreateInfo.usage &= ~VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
            Logger::info("stripped VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT from depth image "
                         "(format=" + convertToString(pCreateInfo->format)
                         + ", samples=" + convertToString(pCreateInfo->samples)
                         + ") — required for layer-side sampling/resolve");
        }
        modifiedCreateInfo.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        // Metadata reflects the *actual* usage flags passed to the driver
        // (post-modification), so downstream code can correctly reason about
        // what the image can do.
        DepthImageMetadata metadata = {
            modifiedCreateInfo.usage,
            pCreateInfo->samples,
            pCreateInfo->tiling,
        };
        VkResult result = logicalDevice->vkd.CreateImage(device, &modifiedCreateInfo, pAllocator, pImage);
        if (result != VK_SUCCESS)
            return result;

        guardLayerWork(logicalDevice, "vkCreateImage", [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            logicalDevice->depthImages.push_back(*pImage);
            logicalDevice->depthFormats.push_back(pCreateInfo->format);
            logicalDevice->depthImageExtents[*pImage] = pCreateInfo->extent;
            logicalDevice->depthImageMetadata[*pImage] = metadata;
            VKHOOK_LOG_DEBUG([&](std::string& s) {
                s = "tracked depth image metadata: image=" + convertToString(*pImage)
                    + " usage=0x" + formatHexU64(static_cast<uint64_t>(metadata.usage))
                    + " samples=" + convertToString(metadata.samples)
                    + " tiling=" + convertToString(metadata.tiling)
                    + " transient=" + std::string((metadata.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0 ? "true" : "false");
            });
        });

        return result;
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

        VkResult result = logicalDevice->vkd.CreateImageView(device, pCreateInfo, pAllocator, view);
        if (result != VK_SUCCESS)
            return result;

        guardLayerWork(logicalDevice, "vkCreateImageView", [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);

            DepthSnapshotTarget snapshotTarget = selectDepthSnapshotTargetFromImage(logicalDevice, pCreateInfo->image);
            if (snapshotTarget.swapchain != VK_NULL_HANDLE)
            {
                VKHOOK_LOG_DEBUG([&](std::string& s) {
                    s = "tracked snapshot target image view: appView=" + convertToString(*view)
                        + " image=" + convertToString(pCreateInfo->image)
                        + " swapchain=" + convertToString(snapshotTarget.swapchain)
                        + " imageIndex=" + std::to_string(snapshotTarget.imageIndex);
                });
                logicalDevice->snapshotTargetViewStates[*view] = snapshotTarget;
            }

            if ((pCreateInfo->subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) == 0)
                return;

            auto imageIt = std::find(logicalDevice->depthImages.begin(), logicalDevice->depthImages.end(), pCreateInfo->image);
            if (imageIt == logicalDevice->depthImages.end())
                return;

            size_t i = std::distance(logicalDevice->depthImages.begin(), imageIt);
            // Bounds-check the parallel vectors before indexing.
            if (i >= logicalDevice->depthFormats.size())
            {
                Logger::warn("CreateImageView: depth image index " + std::to_string(i)
                             + " out of depthFormats range (" + std::to_string(logicalDevice->depthFormats.size())
                             + "); skipping depth view tracking for image="
                             + convertToString(pCreateInfo->image));
                return;
            }
            VkImageView sampledView = getOrCreateTrackedDepthSampleViewLocked(logicalDevice, pCreateInfo->image, logicalDevice->depthFormats[i]);
            VKHOOK_LOG_DEBUG([&](std::string& s) {
                s = "tracked depth image view created: appView=" + convertToString(*view)
                    + " sampledView=" + convertToString(sampledView)
                    + " image=" + convertToString(pCreateInfo->image)
                    + " aspect=" + convertToString(pCreateInfo->subresourceRange.aspectMask);
            });
            DepthState depth;
            depth.image = pCreateInfo->image;
            depth.imageView = sampledView;
            depth.format = logicalDevice->depthFormats[i];
            auto extentIt = logicalDevice->depthImageExtents.find(depth.image);
            if (extentIt != logicalDevice->depthImageExtents.end())
                depth.extent = extentIt->second;
            auto metadataIt = logicalDevice->depthImageMetadata.find(depth.image);
            if (metadataIt != logicalDevice->depthImageMetadata.end())
            {
                depth.samples = metadataIt->second.samples;
                depth.transient = (metadataIt->second.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0;
            }
            logicalDevice->depthViewStates[*view] = depth;
        });

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
        LogicalDevice* logicalDevice = deviceRef.get();

        // In-flight layer submissions can still be reading a depth handle the app
        // is destroying; destroy-while-read is the freeze at target recreation.
        bool     viewTrackedByLayer = false;
        uint64_t submittedSerial    = 0;
        guardLayerWork(logicalDevice, "vkDestroyImageView", [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            viewTrackedByLayer = logicalDevice->snapshotTargetViewStates.count(imageView) != 0
                || logicalDevice->depthViewStates.count(imageView) != 0;
            logicalDevice->snapshotTargetViewStates.erase(imageView);
            logicalDevice->depthViewStates.erase(imageView);
            clearTrackedDepthScopesLocked(logicalDevice, [imageView](const DepthState& state) { return state.imageView == imageView; });

            for (auto it = logicalDevice->framebufferDepthStates.begin(); it != logicalDevice->framebufferDepthStates.end();)
            {
                if (it->second.imageView == imageView)
                    it = logicalDevice->framebufferDepthStates.erase(it);
                else
                    ++it;
            }

            if (logicalDevice->activeDepthState.imageView == imageView)
            {
                logicalDevice->activeDepthState = {};
                // Clear stale pin: the pinned view was just destroyed, so
                // getDepthState() would fall back to the (now-empty) active state.
                logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
                // roblox re-creates its scene targets mid-generation; nothing ours
                // may keep referencing the old image
                armDepthTransitionHoldLocked(logicalDevice, "active depth view destroyed");
                DepthState depth = getDepthState(logicalDevice);
                updateDeviceDepthStateLocked(logicalDevice, depth, "DestroyImageView");
            }
            else if (logicalDevice->pinnedDepthImageView == imageView)
            {
                // Pinned view destroyed but it wasn't the active depth — just
                // clear the pin so we fall back to auto-promotion.
                Logger::debug("DestroyImageView: clearing stale pinned depth view");
                logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
            }

            if (viewTrackedByLayer)
                submittedSerial = logicalDevice->timeline.submittedSerial();
        });

        if (viewTrackedByLayer)
            waitLayerSubmissionsForDestroy(logicalDevice, submittedSerial, "vkDestroyImageView");
        logicalDevice->vkd.DestroyImageView(device, imageView, pAllocator);
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

        VkResult result = logicalDevice->vkd.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
        if (result != VK_SUCCESS)
            return result;

        guardLayerWork(logicalDevice, "vkCreateFramebuffer", [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++)
            {
                auto it = logicalDevice->depthViewStates.find(pCreateInfo->pAttachments[i]);
                if (it != logicalDevice->depthViewStates.end())
                {
                    VKHOOK_LOG_DEBUG([&](std::string& s) {
                        s = "tracked depth framebuffer attachment: framebuffer=" + convertToString(*pFramebuffer)
                            + " attachmentView=" + convertToString(pCreateInfo->pAttachments[i])
                            + " sampledView=" + convertToString(it->second.imageView)
                            + " image=" + convertToString(it->second.image);
                    });
                    logicalDevice->framebufferDepthStates[*pFramebuffer] = it->second;
                    break;
                }
            }

            DepthSnapshotTarget snapshotTarget = selectDepthSnapshotTargetFromImageViews(
                logicalDevice, pCreateInfo->pAttachments, pCreateInfo->attachmentCount);
            if (snapshotTarget.swapchain != VK_NULL_HANDLE)
            {
                VKHOOK_LOG_DEBUG([&](std::string& s) {
                    s = "tracked framebuffer snapshot target: framebuffer=" + convertToString(*pFramebuffer)
                        + " swapchain=" + convertToString(snapshotTarget.swapchain)
                        + " imageIndex=" + std::to_string(snapshotTarget.imageIndex);
                });
                logicalDevice->framebufferSnapshotTargets[*pFramebuffer] = snapshotTarget;
            }
            else
            {
                VKHOOK_LOG_DEBUG([&](std::string& s) {
                    s = "framebuffer has no snapshot target: framebuffer=" + convertToString(*pFramebuffer)
                        + " attachments=" + std::to_string(pCreateInfo->attachmentCount);
                });
            }
        });

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
        guardLayerWork(deviceRef.get(), "vkDestroyFramebuffer", [&] {
            std::lock_guard<std::mutex> l(deviceRef->stateLock);
            deviceRef->framebufferDepthStates.erase(framebuffer);
            deviceRef->framebufferSnapshotTargets.erase(framebuffer);
        });
        deviceRef->vkd.DestroyFramebuffer(device, framebuffer, pAllocator);
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

        bool     imageTrackedByLayer = false;
        uint64_t submittedSerial     = 0;
        guardLayerWork(logicalDevice, "vkDestroyImage", [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);

            // Check if this is a tracked depth image
            auto it = std::find(logicalDevice->depthImages.begin(), logicalDevice->depthImages.end(), image);
            if (it == logicalDevice->depthImages.end())
                return;

            // the layer's own sampled view and the app's image can both still be
            // read by in-flight work: wait for it after the lock is released
            imageTrackedByLayer = true;
            submittedSerial     = logicalDevice->timeline.submittedSerial();
            size_t i = std::distance(logicalDevice->depthImages.begin(), it);

            // Remove from tracking lists
            logicalDevice->depthImageExtents.erase(image);
            logicalDevice->depthImageMetadata.erase(image);
            logicalDevice->depthImages.erase(it);
            if (i < logicalDevice->depthImageViews.size())
            {
                logicalDevice->retire(VK_OBJECT_TYPE_IMAGE_VIEW, handleToU64(logicalDevice->depthImageViews[i]));
                logicalDevice->depthImageViews.erase(logicalDevice->depthImageViews.begin() + i);
            }
            if (i < logicalDevice->depthFormats.size())
                logicalDevice->depthFormats.erase(logicalDevice->depthFormats.begin() + i);

            for (auto viewIt = logicalDevice->depthViewStates.begin(); viewIt != logicalDevice->depthViewStates.end();)
            {
                if (viewIt->second.image == image)
                    viewIt = logicalDevice->depthViewStates.erase(viewIt);
                else
                    ++viewIt;
            }
            for (auto fbIt = logicalDevice->framebufferDepthStates.begin(); fbIt != logicalDevice->framebufferDepthStates.end();)
            {
                if (fbIt->second.image == image)
                    fbIt = logicalDevice->framebufferDepthStates.erase(fbIt);
                else
                    ++fbIt;
            }

            clearTrackedDepthScopesLocked(logicalDevice, [image](const DepthState& state) { return state.image == image; });

            if (logicalDevice->activeDepthState.image == image)
            {
                logicalDevice->activeDepthState = {};
                // Also clear the pin if it references a view of this image
                if (logicalDevice->pinnedDepthImageView != VK_NULL_HANDLE)
                {
                    auto pinIt = logicalDevice->depthViewStates.find(logicalDevice->pinnedDepthImageView);
                    if (pinIt == logicalDevice->depthViewStates.end() || pinIt->second.image == image)
                        logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
                }
                // same as the view path: resolve targets and copies still
                // reference the image being recreated
                armDepthTransitionHoldLocked(logicalDevice, "active depth image destroyed");
                DepthState depth = getDepthState(logicalDevice);
                updateDeviceDepthStateLocked(logicalDevice, depth, "DestroyImage");
            }
            else
            {
                // Image wasn't active, but clear pin if it referenced this image
                if (logicalDevice->pinnedDepthImageView != VK_NULL_HANDLE)
                {
                    auto pinIt = logicalDevice->depthViewStates.find(logicalDevice->pinnedDepthImageView);
                    if (pinIt != logicalDevice->depthViewStates.end() && pinIt->second.image == image)
                    {
                        Logger::debug("DestroyImage: clearing stale pinned depth view (image destroyed)");
                        logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
                    }
                }
            }
        });

        if (imageTrackedByLayer)
            waitLayerSubmissionsForDestroy(logicalDevice, submittedSerial, "vkDestroyImage");
        logicalDevice->vkd.DestroyImage(logicalDevice->device, image, pAllocator);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Render pass / rendering scopes

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

        guardLayerWork(logicalDevice.get(), name, [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            VkImageLayout rpDepthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            auto rpLayoutIt = logicalDevice->renderPassDepthFinalLayouts.find(pRenderPassBegin->renderPass);
            if (rpLayoutIt != logicalDevice->renderPassDepthFinalLayouts.end())
                rpDepthFinalLayout = rpLayoutIt->second;
            beginTrackedDepthScope(logicalDevice.get(),
                                   commandBuffer,
                                   selectDepthStateFromRenderPassBegin(logicalDevice.get(), pRenderPassBegin),
                                   selectDepthSnapshotTargetFromRenderPassBegin(logicalDevice.get(), pRenderPassBegin),
                                   rpDepthFinalLayout);
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

    // Needs logicalDevice->stateLock.
    void seedTrackedDepthScopeFromRenderingInfo(LogicalDevice* logicalDevice,
                                                VkCommandBuffer commandBuffer,
                                                const VkRenderingInfo* pRenderingInfo)
    {
        VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (pRenderingInfo && pRenderingInfo->pDepthAttachment)
            depthFinalLayout = pRenderingInfo->pDepthAttachment->imageLayout;

        beginTrackedDepthScope(logicalDevice,
                               commandBuffer,
                               selectDepthStateFromRenderingInfo(logicalDevice, pRenderingInfo),
                               selectDepthSnapshotTargetFromRenderingInfo(logicalDevice, pRenderingInfo),
                               depthFinalLayout);
    }

    template <typename Pfn>
    void trackedBeginRendering(const char* name, const char* infoName, VkCommandBuffer commandBuffer,
                               const VkRenderingInfo* pRenderingInfo, Pfn DeviceDispatch::*beginFn)
    {
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
        {
            failUnknownObject(name, VK_ERROR_DEVICE_LOST);
            return;
        }

        guardLayerWork(logicalDevice.get(), name, [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            seedTrackedDepthScopeFromRenderingInfo(logicalDevice.get(), commandBuffer, pRenderingInfo);
        });

        if (pRenderingInfo)
        {
            VkRenderingInfo renderingInfo = *pRenderingInfo;
            VkRenderingAttachmentInfo depthAttachment = {};
            VkRenderingAttachmentInfo stencilAttachment = {};
            bool changed = false;

            if (pRenderingInfo->pDepthAttachment)
            {
                depthAttachment = *pRenderingInfo->pDepthAttachment;
                changed = forceDepthAttachmentStoreOp(depthAttachment, false) || changed;
                renderingInfo.pDepthAttachment = &depthAttachment;
            }

            if (pRenderingInfo->pStencilAttachment)
            {
                stencilAttachment = *pRenderingInfo->pStencilAttachment;
                changed = forceDepthAttachmentStoreOp(stencilAttachment, true) || changed;
                renderingInfo.pStencilAttachment = &stencilAttachment;
            }

            if (changed)
            {
                VKHOOK_LOG_DEBUG([&](std::string& s) {
                    s = std::string("forcing depth attachment storeOp=STORE for ") + infoName
                        + " on commandBuffer=" + convertToString(commandBuffer);
                });
                (logicalDevice->vkd.*beginFn)(commandBuffer, &renderingInfo);
                return;
            }
        }

        (logicalDevice->vkd.*beginFn)(commandBuffer, pRenderingInfo);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo)
    {
        trackedBeginRendering("vkCmdBeginRendering", "VkRenderingInfo", commandBuffer, pRenderingInfo, &DeviceDispatch::CmdBeginRendering);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdBeginRenderingKHR(VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo)
    {
        trackedBeginRendering("vkCmdBeginRenderingKHR", "VkRenderingInfoKHR", commandBuffer, pRenderingInfo, &DeviceDispatch::CmdBeginRenderingKHR);
    }

    // After the application ended a scope: promote the scope's depth and, for the
    // deferred-copy capture methods, publish it for the next QueueSubmit.
    // NO command recording happens here.
    void finishTrackedDepthScope(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer, const char* reason)
    {
        const auto settings = settingsManager.snapshot();
        const int  method   = settings->depthCaptureMethod;
        std::lock_guard<std::mutex> l(logicalDevice->stateLock);
        DepthState promoted = {};
        DepthSnapshotTarget snapTarget = {};
        if (method == 1 || method == 2)
        {
            VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            endTrackedDepthScope(logicalDevice, commandBuffer, reason, &promoted, &snapTarget, &depthFinalLayout);
            DepthState currentActive = logicalDevice->activeDepthState;

            if (hasDepthState(promoted) && (snapTarget.swapchain != VK_NULL_HANDLE || sameDepthState(currentActive, promoted)))
            {
                VkImageLayout sourceLayout = (depthFinalLayout != VK_IMAGE_LAYOUT_UNDEFINED)
                    ? depthFinalLayout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                // the promoted depth may belong to the generation being torn down
                if (!logicalDevice->depthTransitionHold)
                    logicalDevice->depthCopy.publish(promoted, sourceLayout);
            }
        }
        else
        {
            // Method 0: just end the scope normally (no capture)
            endTrackedDepthScope(logicalDevice, commandBuffer, reason, &promoted, &snapTarget);
        }
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
        guardLayerWork(logicalDevice.get(), name, [&] {
            finishTrackedDepthScope(logicalDevice.get(), commandBuffer, name + 2);  // reason: the name without "vk"
        });
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
    // Draws: count them per command buffer so the scope with the most draws wins

    template <typename Pfn, typename... Args>
    void trackedDraw(const char* name, VkCommandBuffer commandBuffer, uint32_t drawCount, Pfn DeviceDispatch::*drawFn, Args... args)
    {
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
        {
            failUnknownObject(name, VK_ERROR_DEVICE_LOST);
            return;
        }

        guardLayerWork(logicalDevice.get(), name, [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            countTrackedDepthDraw(logicalDevice.get(), commandBuffer, drawCount);
        });
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
    // Transfers and secondary command buffers

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdCopyImage(VkCommandBuffer commandBuffer,
                                                   VkImage srcImage,
                                                   VkImageLayout srcImageLayout,
                                                   VkImage dstImage,
                                                   VkImageLayout dstImageLayout,
                                                   uint32_t regionCount,
                                                   const VkImageCopy* pRegions)
    {
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
        {
            failUnknownObject("vkCmdCopyImage", VK_ERROR_DEVICE_LOST);
            return;
        }

        logicalDevice->vkd.CmdCopyImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
        guardLayerWork(logicalDevice.get(), "vkCmdCopyImage", [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            tryActivatePendingTransferLinkedDepthScope(logicalDevice.get(), commandBuffer, dstImage, "CmdCopyImage");
        });
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_CmdBlitImage(VkCommandBuffer commandBuffer,
                                                   VkImage srcImage,
                                                   VkImageLayout srcImageLayout,
                                                   VkImage dstImage,
                                                   VkImageLayout dstImageLayout,
                                                   uint32_t regionCount,
                                                   const VkImageBlit* pRegions,
                                                   VkFilter filter)
    {
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
        {
            failUnknownObject("vkCmdBlitImage", VK_ERROR_DEVICE_LOST);
            return;
        }

        logicalDevice->vkd.CmdBlitImage(commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter);
        guardLayerWork(logicalDevice.get(), "vkCmdBlitImage", [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            tryActivatePendingTransferLinkedDepthScope(logicalDevice.get(), commandBuffer, dstImage, "CmdBlitImage");
        });
    }

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

        guardLayerWork(logicalDevice.get(), "vkCmdExecuteCommands", [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            accumulateExecutedCommandBufferDraws(logicalDevice.get(), commandBuffer, pCommandBuffers, commandBufferCount);
        });
        logicalDevice->vkd.CmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);
    }

    // ReShade-style depth preservation for out-of-render-pass depth clears.
    // If the app calls vkCmdClearDepthStencilImage on a tracked depth image,
    // record a snapshot BEFORE the clear.
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

        guardLayerWork(logicalDevice.get(), "vkCmdClearDepthStencilImage", [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            const auto& activeDepth = logicalDevice->activeDepthState;
            if (activeDepth.image == image && hasDepthState(activeDepth))
            {
                // Snapshot helpers read tracked resources under stateLock. The
                // clear's explicit layout is authoritative, not the last draw's.
                DepthState sourceDepth = activeDepth;
                sourceDepth.observedLayout = imageLayout;
                VKHOOK_LOG_DEBUG([&](std::string& s) {
                    s = "CmdClearDepthStencilImage: preserving depth before clear of active depth image="
                        + convertToString(image);
                });
                recordDepthResolveSnapshotForAllSwapchains(logicalDevice.get(), commandBuffer, sourceDepth);
            }
        });
        logicalDevice->vkd.CmdClearDepthStencilImage(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Command buffer lifetime: drop the per-command-buffer tracking state

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                                                             const VkCommandBufferBeginInfo* pBeginInfo)
    {
        // The loader reaches this hook only for command buffers of a device this
        // layer created (see failUnknownObject), so an unknown device means the
        // application is using a command buffer of a destroyed device. There is no
        // dispatch table to call down through, and succeeding without recording
        // anything would hide the bug: fail with a once-only log instead.
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
            return failUnknownObject("vkBeginCommandBuffer", VK_ERROR_DEVICE_LOST);

        guardLayerWork(logicalDevice.get(), "vkBeginCommandBuffer", [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            logicalDevice->commandBufferRecordedDrawCounts[commandBuffer] = 0;
            logicalDevice->commandBufferDepthStates.erase(commandBuffer);
            logicalDevice->pendingTransferLinkedDepthScopes.erase(commandBuffer);
        });
        return logicalDevice->vkd.BeginCommandBuffer(commandBuffer, pBeginInfo);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_AllocateCommandBuffers(VkDevice device,
                                                                 const VkCommandBufferAllocateInfo* pAllocateInfo,
                                                                 VkCommandBuffer* pCommandBuffers)
    {
        auto logicalDevice = findDevice(GetKey(device));
        if (!logicalDevice)
            return failUnknownObject("vkAllocateCommandBuffers", VK_ERROR_DEVICE_LOST);

        VkResult result = logicalDevice->vkd.AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);
        if (result == VK_SUCCESS && pAllocateInfo && pCommandBuffers)
        {
            guardLayerWork(logicalDevice.get(), "vkAllocateCommandBuffers", [&] {
                std::lock_guard<std::mutex> l(logicalDevice->stateLock);
                auto& poolBuffers = logicalDevice->commandPoolBuffers[pAllocateInfo->commandPool];
                poolBuffers.insert(poolBuffers.end(), pCommandBuffers, pCommandBuffers + pAllocateInfo->commandBufferCount);
            });
        }
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

        if (pCommandBuffers)
        {
            guardLayerWork(logicalDevice.get(), "vkFreeCommandBuffers", [&] {
                std::lock_guard<std::mutex> l(logicalDevice->stateLock);
                auto poolIt = logicalDevice->commandPoolBuffers.find(commandPool);
                for (uint32_t i = 0; i < commandBufferCount; ++i)
                {
                    dropCommandBufferStateLocked(logicalDevice.get(), pCommandBuffers[i]);
                    if (poolIt != logicalDevice->commandPoolBuffers.end())
                    {
                        auto& poolBuffers = poolIt->second;
                        poolBuffers.erase(std::remove(poolBuffers.begin(), poolBuffers.end(), pCommandBuffers[i]), poolBuffers.end());
                    }
                }
            });
        }
        logicalDevice->vkd.FreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_ResetCommandPool(VkDevice device, VkCommandPool commandPool, VkCommandPoolResetFlags flags)
    {
        auto logicalDevice = findDevice(GetKey(device));
        if (!logicalDevice)
            return failUnknownObject("vkResetCommandPool", VK_ERROR_DEVICE_LOST);

        guardLayerWork(logicalDevice.get(), "vkResetCommandPool", [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            auto poolIt = logicalDevice->commandPoolBuffers.find(commandPool);
            if (poolIt == logicalDevice->commandPoolBuffers.end())
                return;
            for (VkCommandBuffer commandBuffer : poolIt->second)
                dropCommandBufferStateLocked(logicalDevice.get(), commandBuffer);
        });
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

        guardLayerWork(logicalDevice.get(), "vkDestroyCommandPool", [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            auto poolIt = logicalDevice->commandPoolBuffers.find(commandPool);
            if (poolIt == logicalDevice->commandPoolBuffers.end())
                return;
            for (VkCommandBuffer commandBuffer : poolIt->second)
                dropCommandBufferStateLocked(logicalDevice.get(), commandBuffer);
            logicalDevice->commandPoolBuffers.erase(poolIt);
        });
        logicalDevice->vkd.DestroyCommandPool(device, commandPool, pAllocator);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_ResetCommandBuffer(VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags)
    {
        auto logicalDevice = findDevice(GetKey(commandBuffer));
        if (!logicalDevice)
            return failUnknownObject("vkResetCommandBuffer", VK_ERROR_DEVICE_LOST);

        guardLayerWork(logicalDevice.get(), "vkResetCommandBuffer", [&] {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            dropCommandBufferStateLocked(logicalDevice.get(), commandBuffer);
        });
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
        VKHOOK_INTERCEPT(DepthCapture, CmdBlitImage),
        VKHOOK_INTERCEPT(DepthCapture, CmdClearDepthStencilImage),
        VKHOOK_INTERCEPT(DepthCapture, CmdCopyImage),
        VKHOOK_INTERCEPT(DepthCapture, CmdExecuteCommands),
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
