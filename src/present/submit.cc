#include "layer/entry.hh"
#include "layer/registry.hh"
#include "layer/guard.hh"

#include "control/frontend.hh"
#include "depth/tracker.hh"
#include "diag/gpu_crash.hh"

#include "command_buffer.hh"
#include "format.hh"
#include "settings_manager.hh"
#include "logger.hh"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace VkHook
{
namespace
{
    // What the application's own vkQueueSubmit already did, so an exception in
    // the layer's bookkeeping after it never submits the batch twice.
    struct AppSubmit
    {
        bool     forwarded = false;
        VkResult result    = VK_SUCCESS;
    };

    VkResult forwardSubmit(LogicalDevice* logicalDevice, AppSubmit& app, VkQueue queue, uint32_t submitCount,
                           const VkSubmitInfo* pSubmits, VkFence fence, const char* context)
    {
        app.result    = logicalDevice->queues.submit(queue, submitCount, pSubmits, fence);
        app.forwarded = true;
        reportDeviceLostDiagnostics(logicalDevice, queue, context, app.result);
        if (app.result == VK_ERROR_DEVICE_LOST)
            panicLayer(logicalDevice, "Vulkan device lost during QueueSubmit passthrough");
        return app.result;
    }

    // --- v3 Deferred Depth Copy: QueueSubmit interception ---
    // When depthCaptureMethod == 1 or 2, we intercept QueueSubmit to inject a
    // depth copy command buffer into the last submit. The copy is recorded
    // into a ring-buffered CB from a dedicated pool, using the correct source
    // layout obtained from the render pass's depth attachment finalLayout at
    // CmdBeginRenderPass time.
    //
    // The pending copy and the ring live in DepthCopyState and are guarded by
    // its own mutex. Lock order: stateLock -> depthCopy, so nothing in
    // withRing() touches stateLock-backed state; storage is prepared before and
    // published after. The queue mutex is only ever taken inside
    // device->queues, around one driver call.
    VkResult queueSubmitImpl(LogicalDevice* logicalDevice, AppSubmit& app, VkQueue queue, uint32_t submitCount,
                             const VkSubmitInfo* pSubmits, VkFence fence)
    {
        if (queue == logicalDevice->dlss5OpticalFlowQueue)
        {
            // Optical submissions are not graphics/depth producers.
            return forwardSubmit(logicalDevice, app, queue, submitCount, pSubmits, fence, "vkQueueSubmit(optical flow)");
        }

        if (submitCount > 0 && pSubmits)
        {
            // journal the app's batch so a stuck queue can be traced past it
            VkSemaphore appWait = VK_NULL_HANDLE;
            if (pSubmits[0].waitSemaphoreCount > 0 && pSubmits[0].pWaitSemaphores)
                appWait = pSubmits[0].pWaitSemaphores[0];
            VkSemaphore appSignal = VK_NULL_HANDLE;
            const VkSubmitInfo& lastSubmit = pSubmits[submitCount - 1];
            if (lastSubmit.signalSemaphoreCount > 0 && lastSubmit.pSignalSemaphores)
                appSignal = lastSubmit.pSignalSemaphores[lastSubmit.signalSemaphoreCount - 1];
            recordSubmittedBatch(logicalDevice, "app", UINT32_MAX, appWait, appSignal, fence);
        }

        const auto settings = settingsManager.snapshot();
        const int  method   = std::clamp(settings->depthCaptureMethod, 0, 2);
        if (method < 1 || method > 2
            || !depthCaptureActive()
            || logicalDevice->depthTransitionHold.load(std::memory_order_acquire)
            || logicalDevice->softDisabled.load(std::memory_order_acquire)
            || submitCount == 0 || !pSubmits)
            return forwardSubmit(logicalDevice, app, queue, submitCount, pSubmits, fence, "vkQueueSubmit(passthrough)");

        // Claim the pending copy. Clearing the flag is unconditional and happens
        // under the depth-copy lock, so a copy can be published mid-read without
        // tearing the DepthState and without two submits racing for it.
        DepthState captureDepth;
        VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!logicalDevice->depthCopy.consume(captureDepth, sourceLayout))
            return forwardSubmit(logicalDevice, app, queue, submitCount, pSubmits, fence, "vkQueueSubmit(passthrough)");

        bool depthValid = false;
        {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            depthValid = hasDepthState(captureDepth) && validateDepthStateForResolve(logicalDevice, captureDepth);
        }
        if (!depthValid)
        {
            Logger::debug("QueueSubmit v3: skipping invalid/stale depth state");
            return forwardSubmit(logicalDevice, app, queue, submitCount, pSubmits, fence, "vkQueueSubmit(passthrough-invalid-depth)");
        }

        // Prepare the storage under stateLock and snapshot what the recording
        // needs into locals, so the ring section below holds only depthCopy.
        const VkFormat storageFormat = (sourceLayout == VK_IMAGE_LAYOUT_GENERAL)
            ? VK_FORMAT_R32_SFLOAT
            : captureDepth.format;

        VkImage storageImage = VK_NULL_HANDLE;
        VkImageView storageImageView = VK_NULL_HANDLE;
        bool storageWasValid = false;
        {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            ensurePersistentDepthStorage(logicalDevice, storageFormat, captureDepth.extent);
            const auto& storage = logicalDevice->depthCaptureStorage;
            storageImage = storage.image;
            storageImageView = storage.view;
            storageWasValid = storage.valid;
        }

        if (storageImage == VK_NULL_HANDLE)
            return forwardSubmit(logicalDevice, app, queue, submitCount, pSubmits, fence, "vkQueueSubmit(passthrough-no-depth-storage)");

        // --- ring section: init, reserve, fence, record, submit ---
        bool ringBusy = false;            // slot in flight or ring unavailable: skip the copy
        VkResult slotFenceError = VK_SUCCESS;
        VkResult submitResult = VK_SUCCESS;
        VkResult appSubmitResult = VK_SUCCESS;

        logicalDevice->depthCopy.withRing([&](DepthCopyState::RingAccess& ring) {
            if (!ring.ready())
            {
                VkCommandPoolCreateInfo poolCI = {};
                poolCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
                poolCI.queueFamilyIndex = logicalDevice->queueFamilyIndex;

                VkCommandPool pool = VK_NULL_HANDLE;
                VkResult poolResult = logicalDevice->vkd.CreateCommandPool(logicalDevice->device, &poolCI, nullptr, &pool);
                if (poolResult != VK_SUCCESS || pool == VK_NULL_HANDLE)
                {
                    Logger::err("QueueSubmit v3: could not create depth copy pool (" + std::to_string(poolResult) + ")");
                    ringBusy = true;
                    return;
                }

                std::vector<VkCommandBuffer> buffers(DepthCopyState::RING_SIZE, VK_NULL_HANDLE);
                VkCommandBufferAllocateInfo cbai = {};
                cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                cbai.commandPool = pool;
                cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                cbai.commandBufferCount = DepthCopyState::RING_SIZE;
                VkResult cbResult = logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &cbai, buffers.data());
                if (cbResult != VK_SUCCESS)
                {
                    Logger::err("QueueSubmit v3: could not allocate depth copy command buffers (" + std::to_string(cbResult) + ")");
                    logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, pool, nullptr);
                    ringBusy = true;
                    return;
                }

                for (auto cb : buffers)
                    initializeDispatchTable(cb, logicalDevice->device);

                std::vector<VkFence> fences(DepthCopyState::RING_SIZE, VK_NULL_HANDLE);
                VkFenceCreateInfo fci = {};
                fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                bool fencesOk = true;
                for (uint32_t i = 0; i < DepthCopyState::RING_SIZE; ++i)
                {
                    VkResult fr = logicalDevice->vkd.CreateFence(logicalDevice->device, &fci, nullptr, &fences[i]);
                    if (fr != VK_SUCCESS)
                    {
                        Logger::err("QueueSubmit v3: failed to create depth copy fence for slot " + std::to_string(i) + " (" + std::to_string(fr) + ")");
                        fencesOk = false;
                        break;
                    }
                }
                if (!fencesOk)
                {
                    for (VkFence f : fences)
                        if (f != VK_NULL_HANDLE)
                            logicalDevice->vkd.DestroyFence(logicalDevice->device, f, nullptr);
                    logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, pool, nullptr);
                    ringBusy = true;
                    return;
                }

                ring.install(pool, std::move(buffers), std::move(fences));
                Logger::debug("depth copy ring buffer initialized: " + std::to_string(DepthCopyState::RING_SIZE) + " CBs + fences");
            }

            const uint32_t slotIndex = ring.reserveSlot();
            VkCommandBuffer copyCB = ring.buffer(slotIndex);
            VkFence slotFence = ring.fence(slotIndex);

            // never block on a ring slot. an unsignaled fence means the old buffer is still
            // in flight: skip the optional copy and let the app's submit proceed, or a
            // stalled GPU becomes a multi-second present-thread freeze.
            if (slotFence != VK_NULL_HANDLE)
            {
                VkResult fenceStatus = logicalDevice->vkd.GetFenceStatus(logicalDevice->device, slotFence);
                if (fenceStatus == VK_NOT_READY)
                {
                    VKHOOK_LOG_DEBUG([&](std::string& s) {
                        s = "QueueSubmit v3: depth copy ring slot " + std::to_string(slotIndex)
                            + " still in flight; skipping optional depth copy";
                    });
                    ringBusy = true;
                    return;
                }
                if (fenceStatus != VK_SUCCESS)
                {
                    slotFenceError = fenceStatus;
                    return;
                }
                logicalDevice->vkd.ResetFences(logicalDevice->device, 1, &slotFence);
            }

            logicalDevice->vkd.ResetCommandBuffer(copyCB, 0);

            const bool isMsaa = captureDepth.samples != VK_SAMPLE_COUNT_1_BIT;
            const VkImageAspectFlags depthAspect = isStencilFormat(captureDepth.format)
                ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                : VK_IMAGE_ASPECT_DEPTH_BIT;

            // Record the copy
            VkCommandBufferBeginInfo cbbi = {};
            cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            logicalDevice->vkd.BeginCommandBuffer(copyCB, &cbbi);

            // Barrier: source from KNOWN sourceLayout → TRANSFER_SRC
            VkImageMemoryBarrier barriers[2] = {};
            barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[0].image = captureDepth.image;
            barriers[0].oldLayout = (sourceLayout != VK_IMAGE_LAYOUT_UNDEFINED) ? sourceLayout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barriers[0].srcAccessMask = (sourceLayout == VK_IMAGE_LAYOUT_GENERAL)
                ? (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)
                : (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[0].subresourceRange = {depthAspect, 0, 1, 0, 1};

            // Barrier: storage → TRANSFER_DST
            barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[1].image = storageImage;
            barriers[1].oldLayout = storageWasValid ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
            barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[1].srcAccessMask = storageWasValid ? VK_ACCESS_SHADER_READ_BIT : 0;
            barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

            VkPipelineStageFlags srcStages = (sourceLayout == VK_IMAGE_LAYOUT_GENERAL)
                ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                : (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);

            logicalDevice->vkd.CmdPipelineBarrier(copyCB, srcStages, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

            if (isMsaa)
            {
                VkImageResolve resolveRegion = {};
                resolveRegion.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
                resolveRegion.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
                resolveRegion.extent = {captureDepth.extent.width, captureDepth.extent.height, 1};
                logicalDevice->vkd.CmdResolveImage(copyCB, captureDepth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, storageImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &resolveRegion);
            }
            else
            {
                VkImageCopy copyRegion = {};
                copyRegion.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
                copyRegion.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
                copyRegion.extent = {captureDepth.extent.width, captureDepth.extent.height, 1};
                logicalDevice->vkd.CmdCopyImage(copyCB, captureDepth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, storageImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
            }

            // Barrier: storage → SHADER_READ_ONLY
            VkImageMemoryBarrier readOnlyBarrier = {};
            readOnlyBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            readOnlyBarrier.image = storageImage;
            readOnlyBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            readOnlyBarrier.newLayout = isStencilFormat(storageFormat) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
            readOnlyBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            readOnlyBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            readOnlyBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            readOnlyBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            readOnlyBarrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            logicalDevice->vkd.CmdPipelineBarrier(copyCB, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &readOnlyBarrier);

            // Restore the source to the layout the layer observed: the shader
            // resolve barriers from that layout, so leaving it in TRANSFER_SRC
            // here makes the two disagree and faults the GPU.
            if (captureDepth.image != storageImage)
            {
                const VkImageLayout restoreLayout = (sourceLayout != VK_IMAGE_LAYOUT_UNDEFINED)
                    ? sourceLayout : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                VkImageMemoryBarrier restoreBarrier = {};
                restoreBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                restoreBarrier.image = captureDepth.image;
                restoreBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                restoreBarrier.newLayout = restoreLayout;
                restoreBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                restoreBarrier.dstAccessMask = (restoreLayout == VK_IMAGE_LAYOUT_GENERAL)
                    ? (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT)
                    : (VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
                restoreBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                restoreBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                restoreBarrier.subresourceRange = {depthAspect, 0, 1, 0, 1};
                const VkPipelineStageFlags restoreDstStages = (restoreLayout == VK_IMAGE_LAYOUT_GENERAL)
                    ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                    : (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
                logicalDevice->vkd.CmdPipelineBarrier(copyCB, VK_PIPELINE_STAGE_TRANSFER_BIT, restoreDstStages,
                                                      0, 0, nullptr, 0, nullptr, 1, &restoreBarrier);
            }

            logicalDevice->vkd.EndCommandBuffer(copyCB);

            VKHOOK_LOG_DEBUG([&](std::string& s) {
                s = "QueueSubmit v3: depth copy via ring CB index=" + std::to_string(slotIndex)
                    + " " + std::to_string(captureDepth.extent.width) + "x" + std::to_string(captureDepth.extent.height)
                    + " fmt=" + std::to_string(storageFormat)
                    + " srcLayout=" + std::to_string(static_cast<uint32_t>(sourceLayout))
                    + (isMsaa ? " [MSAA resolve]" : " [copy]");
            });

            // app submits go through untouched, fence included. the copy rides
            // its own submit so an app-side fence wait can never be starved,
            // and its barriers still order it after the frame's depth writes.
            submitResult = logicalDevice->queues.submit(queue, submitCount, pSubmits, fence);
            appSubmitResult = submitResult;
            if (submitResult == VK_SUCCESS)
            {
                VkSubmitInfo copySubmitInfo = {};
                copySubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                copySubmitInfo.commandBufferCount = 1;
                copySubmitInfo.pCommandBuffers = &copyCB;
                submitResult = logicalDevice->queues.submitProbed(queue, 1, &copySubmitInfo, slotFence);
            }
        });

        if (ringBusy)
            return forwardSubmit(logicalDevice, app, queue, submitCount, pSubmits, fence, "vkQueueSubmit(passthrough-ring-busy)");

        if (slotFenceError != VK_SUCCESS)
        {
            if (slotFenceError == VK_ERROR_DEVICE_LOST)
            {
                reportDeviceLostDiagnostics(logicalDevice, queue, "QueueSubmit v3 depth-copy fence status", slotFenceError);
                panicLayer(logicalDevice, "Device lost during depth-copy ring fence status");
                return slotFenceError;
            }
            Logger::warn("QueueSubmit v3: depth copy ring fence status failed (" + std::to_string(slotFenceError) + ")");
            return forwardSubmit(logicalDevice, app, queue, submitCount, pSubmits, fence, "vkQueueSubmit(passthrough-ring-status)");
        }

        // the app's batch went out inside the ring section
        app.forwarded = true;
        app.result    = appSubmitResult;

        // The copy went out; publish the storage as the deepest state and let the
        // barrier in the next copy treat its contents as valid. If the submit
        // failed the storage is left marked invalid so the next copy starts from
        // UNDEFINED rather than assuming contents that never landed.
        if (submitResult == VK_SUCCESS)
        {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            logicalDevice->depthCaptureStorage.valid = true;
            DepthState storageState = captureDepth;
            storageState.image = storageImage;
            storageState.imageView = storageImageView;
            storageState.format = storageFormat;
            storageState.samples = VK_SAMPLE_COUNT_1_BIT;
            storageState.transient = false;
            storageState.observedLayout = isStencilFormat(storageFormat)
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
            updateDeviceDepthStateLocked(logicalDevice, storageState, "QueueSubmit v3");
        }

        reportDeviceLostDiagnostics(logicalDevice, queue, "vkQueueSubmit(depth-copy)", submitResult);
        if (submitResult == VK_ERROR_DEVICE_LOST)
            panicLayer(logicalDevice, "Vulkan device lost during depth-copy submit");
        return submitResult;
    }

    // The ring section's results. When the app's own batch succeeded but the
    // copy submit failed, the app still gets its batch's result.
    VKAPI_ATTR VkResult VKAPI_CALL VkHook_QueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits,
                                                       VkFence fence)
    {
        // the device stays alive for the whole call; no lock is held
        auto deviceRef = findDevice(GetKey(queue));
        if (!deviceRef)
            return failUnknownObject("vkQueueSubmit", VK_ERROR_DEVICE_LOST);
        LogicalDevice* logicalDevice = deviceRef.get();

        AppSubmit app;
        try
        {
            return queueSubmitImpl(logicalDevice, app, queue, submitCount, pSubmits, fence);
        }
        catch (const std::exception& e)
        {
            reportHookException(logicalDevice, "vkQueueSubmit", e.what());
        }
        catch (...)
        {
            reportHookException(logicalDevice, "vkQueueSubmit", nullptr);
        }
        // the layer failed; the application's batch still has to run, exactly once
        if (app.forwarded)
            return app.result;
        return logicalDevice->queues.submit(queue, submitCount, pSubmits, fence);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // The remaining queue entry points. Each holds the queue's mutex around the
    // one driver call (QueueTable); none holds a layer lock.

    template <typename Fn>
    VkResult withQueueDevice(VkQueue queue, const char* name, Fn&& fn)
    {
        auto device = findDevice(GetKey(queue));
        if (!device)
            return failUnknownObject(name, VK_ERROR_DEVICE_LOST);
        return fn(*device);
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_QueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* infos, VkFence fence)
    {
        return withQueueDevice(queue, "vkQueueSubmit2", [&](LogicalDevice& d) { return d.queues.submit2(queue, count, infos, fence, false); });
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_QueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* infos, VkFence fence)
    {
        return withQueueDevice(queue, "vkQueueSubmit2KHR", [&](LogicalDevice& d) { return d.queues.submit2(queue, count, infos, fence, true); });
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_QueueBindSparse(VkQueue queue, uint32_t count, const VkBindSparseInfo* infos, VkFence fence)
    {
        return withQueueDevice(queue, "vkQueueBindSparse", [&](LogicalDevice& d) { return d.queues.bindSparse(queue, count, infos, fence); });
    }

    // vkQueueWaitIdle: an empty fenced submit under the queue mutex, then the
    // wait with nothing held (the real call would wait under the mutex).
    VKAPI_ATTR VkResult VKAPI_CALL VkHook_QueueWaitIdle(VkQueue queue)
    {
        return withQueueDevice(queue, "vkQueueWaitIdle", [&](LogicalDevice& d) { return d.queues.waitIdle(queue); });
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_DeviceWaitIdle(VkDevice handle)
    {
        auto device = findDevice(GetKey(handle));
        if (!device)
            return failUnknownObject("vkDeviceWaitIdle", VK_ERROR_DEVICE_LOST);
        return device->queues.deviceWaitIdle();
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_QueueBeginDebugUtilsLabelEXT(VkQueue queue, const VkDebugUtilsLabelEXT* label)
    {
        if (auto device = findDevice(GetKey(queue)))
            device->queues.beginLabel(queue, label);
        else
            failUnknownObject("vkQueueBeginDebugUtilsLabelEXT", VK_ERROR_DEVICE_LOST);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_QueueEndDebugUtilsLabelEXT(VkQueue queue)
    {
        if (auto device = findDevice(GetKey(queue)))
            device->queues.endLabel(queue);
        else
            failUnknownObject("vkQueueEndDebugUtilsLabelEXT", VK_ERROR_DEVICE_LOST);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_QueueInsertDebugUtilsLabelEXT(VkQueue queue, const VkDebugUtilsLabelEXT* label)
    {
        if (auto device = findDevice(GetKey(queue)))
            device->queues.insertLabel(queue, label);
        else
            failUnknownObject("vkQueueInsertDebugUtilsLabelEXT", VK_ERROR_DEVICE_LOST);
    }

    // The layer learns a queue's family from the application's own lookups.
    VKAPI_ATTR void VKAPI_CALL VkHook_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue)
    {
        auto deviceRef = findDevice(GetKey(device));
        if (!deviceRef)
        {
            failUnknownObject("vkGetDeviceQueue", VK_ERROR_DEVICE_LOST);
            return;
        }
        deviceRef->vkd.GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
        if (pQueue && *pQueue)
            deviceRef->queues.add(*pQueue, queueFamilyIndex, queueIndex);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo, VkQueue* pQueue)
    {
        auto deviceRef = findDevice(GetKey(device));
        if (!deviceRef)
        {
            failUnknownObject("vkGetDeviceQueue2", VK_ERROR_DEVICE_LOST);
            return;
        }
        deviceRef->vkd.GetDeviceQueue2(device, pQueueInfo, pQueue);
        if (pQueueInfo && pQueue && *pQueue)
            deviceRef->queues.add(*pQueue, pQueueInfo->queueFamilyIndex, pQueueInfo->queueIndex);
    }

    const InterceptEntry kSubmitIntercepts[] = {
        VKHOOK_INTERCEPT(Unconditional, QueueSubmit),
        VKHOOK_INTERCEPT(Unconditional, GetDeviceQueue),
        VKHOOK_INTERCEPT_EXT(Downstream, GetDeviceQueue2, nullptr, VK_API_VERSION_1_1),
        VKHOOK_INTERCEPT_EXT(Downstream, QueueSubmit2, nullptr, VK_API_VERSION_1_3),
        VKHOOK_INTERCEPT_EXT(Downstream, QueueSubmit2KHR, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, 0),
        VKHOOK_INTERCEPT(Downstream, QueueBindSparse),
        VKHOOK_INTERCEPT(Downstream, QueueWaitIdle),
        VKHOOK_INTERCEPT(Downstream, DeviceWaitIdle),
        // instance-level extension, device-dispatched: the next layer decides
        VKHOOK_INTERCEPT(Downstream, QueueBeginDebugUtilsLabelEXT),
        VKHOOK_INTERCEPT(Downstream, QueueEndDebugUtilsLabelEXT),
        VKHOOK_INTERCEPT(Downstream, QueueInsertDebugUtilsLabelEXT),
    };
} // namespace

    InterceptTable submitInterceptTable()
    {
        return makeInterceptTable(kSubmitIntercepts);
    }
} // namespace VkHook
