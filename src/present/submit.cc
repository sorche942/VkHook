#include "layer/entry.hh"
#include "layer/registry.hh"
#include "layer/guard.hh"

#include "depth/tracker.hh"
#include "diag/gpu_crash.hh"

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

    // The application's submit: its command buffers' depth logs are folded into
    // the tracker (layouts and the frame's draw histogram), then the batch goes
    // down unchanged. Nothing is injected into the submission.
    void foldDepth(LogicalDevice* logicalDevice, uint32_t submitCount, const VkSubmitInfo* pSubmits)
    {
        if (!logicalDevice->depth || logicalDevice->softDisabled.load(std::memory_order_acquire) || !pSubmits)
            return;
        guardLayerWork(logicalDevice, "vkQueueSubmit", [&] {
            for (uint32_t i = 0; i < submitCount; ++i)
                logicalDevice->depth->onSubmit(pSubmits[i].pCommandBuffers, pSubmits[i].commandBufferCount);
        });
    }

    void foldDepth2(LogicalDevice* logicalDevice, uint32_t submitCount, const VkSubmitInfo2* pSubmits)
    {
        if (!logicalDevice->depth || logicalDevice->softDisabled.load(std::memory_order_acquire) || !pSubmits)
            return;
        guardLayerWork(logicalDevice, "vkQueueSubmit2", [&] {
            for (uint32_t i = 0; i < submitCount; ++i)
                for (uint32_t c = 0; c < pSubmits[i].commandBufferInfoCount; ++c)
                {
                    const VkCommandBuffer buffer = pSubmits[i].pCommandBufferInfos[c].commandBuffer;
                    logicalDevice->depth->onSubmit(&buffer, 1);
                }
        });
    }

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

        foldDepth(logicalDevice, submitCount, pSubmits);
        return forwardSubmit(logicalDevice, app, queue, submitCount, pSubmits, fence, "vkQueueSubmit");
    }

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
        return withQueueDevice(queue, "vkQueueSubmit2", [&](LogicalDevice& d) {
            foldDepth2(&d, count, infos);
            return d.queues.submit2(queue, count, infos, fence, false);
        });
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_QueueSubmit2KHR(VkQueue queue, uint32_t count, const VkSubmitInfo2* infos, VkFence fence)
    {
        return withQueueDevice(queue, "vkQueueSubmit2KHR", [&](LogicalDevice& d) {
            foldDepth2(&d, count, infos);
            return d.queues.submit2(queue, count, infos, fence, true);
        });
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
