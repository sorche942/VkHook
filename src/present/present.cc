#include "layer/entry.hh"
#include "layer/registry.hh"
#include "layer/guard.hh"

#include "control/frontend.hh"
#include "depth/capture.hh"
#include "depth/tracker.hh"
#include "diag/gpu_crash.hh"
#include "present/present_mode.hh"
#include "present/swapchain.hh"

#include "command_buffer.hh"
#include "format.hh"
#include "store.hh"
#include "input_session.hh"
#include "overlay/overlay_renderer.hh"
#include "logger.hh"
#include "util.hh"
#include "wayland_interpose_patch.hh"
#include "wayland_syncobj_gate.hh"
#include "effects/effect.hh"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace VkHook
{
namespace
{
    // Scratch buffers reused across presents on the same thread to avoid per-frame
    // heap allocations. They hold no state between presents.
    thread_local std::vector<VkSemaphore> tlsPresentWaits;

    // What the real present has consumed so far, for the exception fallback: once
    // the layer submitted, the app's waits are gone and the app's present can no
    // longer be forwarded unchanged.
    struct PresentProgress
    {
        bool layerSubmitted = false;
    };

    // The input session of the first presented swapchain that has one: its
    // surface is what the hotkeys poll and what the syncobj gate guards.
    std::shared_ptr<InputSession> presentSession(LogicalDevice& logicalDevice, const VkPresentInfoKHR* presentInfo)
    {
        for (uint32_t i = 0; i < presentInfo->swapchainCount; ++i)
        {
            const std::shared_ptr<LogicalSwapchain> swapchain = findSwapchain(logicalDevice, presentInfo->pSwapchains[i]);
            if (swapchain && swapchain->shared && swapchain->shared->input && swapchain->shared->input->alive())
                return swapchain->shared->input;
        }
        return nullptr;
    }

    // The Wayland surface ids of every presented swapchain's own input session,
    // each once: a present that mixes swapchains of several windows must keep
    // every one of their commits out of the open syncobj sequence.
    void gateSurfaces(LogicalDevice& logicalDevice, const VkPresentInfoKHR* presentInfo, std::vector<uint32_t>& out)
    {
        out.clear();
        for (uint32_t i = 0; i < presentInfo->swapchainCount; ++i)
        {
            const std::shared_ptr<LogicalSwapchain> swapchain = findSwapchain(logicalDevice, presentInfo->pSwapchains[i]);
            if (!swapchain || !swapchain->shared || !swapchain->shared->input || !swapchain->shared->input->alive())
                continue;
            const uint32_t surfaceId = swapchain->shared->input->waylandSurfaceId();
            if (surfaceId && std::find(out.begin(), out.end(), surfaceId) == out.end())
                out.push_back(surfaceId);
        }
    }

    // The real present, bracketed so Mesa's explicit-sync points inside it
    // are not interleaved with window commits from other threads.
    VkResult realPresent(LogicalDevice* logicalDevice, VkQueue queue, const VkPresentInfoKHR* presentInfo,
                         const char* diagContext, const char* panicReason)
    {
        installWaylandInterposeHooks();

        thread_local std::vector<uint32_t> surfaces;
        gateSurfaces(*logicalDevice, presentInfo, surfaces);
        for (uint32_t surfaceId : surfaces)
            WaylandSyncobjGate::beginPresent(surfaceId);

        const VkResult vr = logicalDevice->queues.present(queue, presentInfo);

        for (auto it = surfaces.rbegin(); it != surfaces.rend(); ++it)
            WaylandSyncobjGate::endPresent(*it);

        reportDeviceLostDiagnostics(logicalDevice, queue, diagContext, vr);
        if (vr == VK_ERROR_DEVICE_LOST)
            panicLayer(logicalDevice, panicReason);
        return vr;
    }

    // What one presented swapchain needs this frame.
    struct PresentSlot
    {
        std::shared_ptr<LogicalSwapchain> swapchain;  // null: the layer does not track it
        uint32_t                          index = 0;
        PresentMode                       mode  = PresentMode::Passthrough;
        // the frame's depth, kept alive until the capture's submission is queued
        DepthSource                       depth;
    };

    struct SubmitContext
    {
        LogicalDevice*    device = nullptr;
        LogicalSwapchain* swapchain = nullptr;
        uint32_t          index = 0;
        uint64_t          frameSerial = 0;
    };

    VkResult prepareEffectSubmission(void* context, uint32_t, VkSubmitInfo& submit)
    {
        auto* ctx = static_cast<SubmitContext*>(context);
        for (auto& effect : ctx->swapchain->effects)
        {
            const VkResult vr = effect->prepareSubmission(ctx->index, ctx->frameSerial, submit);
            if (vr != VK_SUCCESS)
                return vr;
        }
        return VK_SUCCESS;
    }

    VkFence overlayFenceFor(void* context, uint32_t)
    {
        auto* ctx = static_cast<SubmitContext*>(context);
        return ctx->swapchain->shared->overlay->fenceForSubmit(ctx->index);
    }

    // The layer's submit queue: the present queue when it belongs to the
    // family the layer's command buffers were recorded for, else the layer's own.
    VkQueue chooseSubmitQueue(LogicalDevice* logicalDevice, VkQueue presentQueue)
    {
        const QueueState* state = logicalDevice->queues.find(presentQueue);
        if (state && state->family == logicalDevice->queueFamilyIndex)
            return presentQueue;
        return logicalDevice->queue;
    }

    // Waits for the image's previous layer submission before its command buffer
    // is re-recorded or resubmitted. Bounded; no lock is held. False on timeout.
    bool waitImageFence(LogicalDevice* logicalDevice, LogicalSwapchain& sc, uint32_t index)
    {
        SwapchainShared& shared = *sc.shared;
        if (index >= shared.effectFences.size() || index >= shared.effectFenceUsed.size())
            return true;
        const VkFence fence = shared.effectFences[index];
        if (fence == VK_NULL_HANDLE || !shared.effectFenceUsed[index])
            return true;

        constexpr uint64_t kFenceTimeoutNs = 2'000'000'000ull;
        const VkResult waited = logicalDevice->vkd.WaitForFences(logicalDevice->device, 1, &fence, VK_TRUE, kFenceTimeoutNs);
        if (waited != VK_SUCCESS)
        {
            reportDeviceLostDiagnostics(logicalDevice, logicalDevice->queue, "effect fence wait", waited);
            panicLayer(logicalDevice, waited == VK_ERROR_DEVICE_LOST
                ? std::string("Vulkan device lost waiting for an effect fence")
                : "effect fence did not signal in time (VkResult " + std::to_string(waited) + ")");
            return false;
        }
        logicalDevice->vkd.ResetFences(logicalDevice->device, 1, &fence);
        shared.effectFenceUsed[index] = 0;
        return true;
    }

    VkResult presentImpl(LogicalDevice* logicalDevice, VkQueue queue, const VkPresentInfoKHR* pPresentInfo,
                         PresentProgress& progress)
    {
        const uint64_t frameSerial = logicalDevice->presentSerial.fetch_add(1, std::memory_order_relaxed) + 1;

        // retired objects whose serial completed are destroyed here, on the
        // present thread, with no lock held
        logicalDevice->queues.reclaim();
        logicalDevice->retireQueue.collect(logicalDevice->timeline.completedSerial());

        if (queue == logicalDevice->dlss5OpticalFlowQueue || !logicalDevice->queue || pPresentInfo->swapchainCount == 0)
        {
            // An optical-only queue is not a graphics presentation path.
            return realPresent(logicalDevice, queue, pPresentInfo, "vkQueuePresentKHR(passthrough)",
                               "Vulkan device lost during passthrough present");
        }

        const bool softDisabled = logicalDevice->softDisabled.load(std::memory_order_acquire);
        if (softDisabled && isDeviceLost(logicalDevice))
            return realPresent(logicalDevice, queue, pPresentInfo, "vkQueuePresentKHR(device lost)",
                               "Vulkan device lost during present on a lost device");

        // The window this present draws to: its input session serves the hotkeys
        const std::shared_ptr<InputSession> session = presentSession(*logicalDevice, pPresentInfo);
        // Mark new input frame so dispatch deduplication resets
        if (session)
            session->beginFrame();

        // The control step (hotkeys, hot-reload, overlay requests) runs with no
        // lock held. A soft-disabled device no longer runs it: the layer is not
        // trusted to do more than copy.
        DeviceControlState& control = deviceControl(logicalDevice);
        bool effectsOn = false;
        if (!softDisabled)
            effectsOn = runFrontendFrame(logicalDevice, control, session.get());

        // Snapshot the swapchains of this present; every present thread keeps its
        // generation alive by reference.
        std::vector<PresentSlot> slots(pPresentInfo->swapchainCount);
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
        {
            slots[i].index     = pPresentInfo->pImageIndices[i];
            slots[i].swapchain = findSwapchain(*logicalDevice, pPresentInfo->pSwapchains[i]);
        }

        // Test hook: rebuilds the depth path while the app keeps presenting.
        maybeForceSmokeDepthRefresh(logicalDevice, control.diag);

        // modes, and the bounds every faked swapchain needs
        std::vector<PresentMode> modes(slots.size());
        for (size_t i = 0; i < slots.size(); ++i)
        {
            PresentSlot& slot = slots[i];
            LogicalSwapchain* sc = slot.swapchain.get();
            ModeInputs in;
            in.faked             = sc && sc->faked();
            in.effectsOn         = effectsOn;
            in.softDisabled      = softDisabled;
            slot.mode            = choosePresentMode(in);
            modes[i]             = slot.mode;

            if (!sc)
                continue;
            if (sc->faked())
            {
                const uint32_t index = slot.index;
                if (index >= sc->imageCount || !sc->shared || index >= sc->shared->semaphores.size()
                    || index >= sc->shared->overlaySemaphores.size() || index >= sc->shared->imageViews.size()
                    || index >= sc->commandBuffersEffect.size() || index >= sc->commandBuffersNoEffect.size())
                {
                    Logger::err("present image index out of bounds for swapchain");
                    return VK_ERROR_OUT_OF_DATE_KHR;
                }
            }
            else if (slot.index >= sc->images.size())
            {
                Logger::err("present image index out of bounds for passthrough swapchain");
                return VK_ERROR_OUT_OF_DATE_KHR;
            }
        }

        const WaitAccounting accounting = accountPresentWaits(modes.data(), static_cast<uint32_t>(modes.size()),
                                                              pPresentInfo->waitSemaphoreCount);
        if (accounting.forwardUnchanged)
            return realPresent(logicalDevice, queue, pPresentInfo, "vkQueuePresentKHR(passthrough swapchains)",
                               "Vulkan device lost during passthrough present");

        // ---- layer submissions: every faked swapchain of this present ----
        const VkQueue submitQueue = chooseSubmitQueue(logicalDevice, queue);
        std::vector<PresentTarget> targets(slots.size());
        std::vector<SubmitContext> contexts(slots.size());
        for (size_t i = 0; i < slots.size(); ++i)
        {
            PresentSlot&      slot = slots[i];
            LogicalSwapchain* sc   = slot.swapchain.get();
            PresentTarget&    target = targets[i];
            target.mode = slot.mode;

            if (slot.mode == PresentMode::Passthrough)
            {
                // a mixed present needs a layer-signalled semaphore for this swapchain
                if (!sc || !sc->shared || slot.index >= sc->shared->semaphores.size())
                {
                    Logger::err("mixed present: no relay semaphore for a pass-through swapchain");
                    return VK_ERROR_OUT_OF_DATE_KHR;
                }
                target.relaySemaphore = sc->shared->semaphores[slot.index];
                continue;
            }

            SwapchainShared& shared = *sc->shared;
            const uint32_t   index  = slot.index;

            // the previous layer submission of this image is done before its buffer is touched
            if (!waitImageFence(logicalDevice, *sc, index))
                return realPresent(logicalDevice, queue, pPresentInfo, "vkQueuePresentKHR(fence timeout)",
                                   "Vulkan device lost after an effect fence timeout");

            // Update effect uniforms only when effects run (saves CPU+GPU when off).
            if (slot.mode == PresentMode::Effects)
            {
                const bool updated = guardLayerWork(logicalDevice, "effect update", [&] {
                    bool frameCommandsChanged = false;
                    for (auto& effect : sc->effects)
                        frameCommandsChanged |= effect->updateFrame(index);
                    if (frameCommandsChanged)
                    {
                        // Updating ordinary descriptor bindings invalidates the
                        // executable buffer: re-record only this fence-safe image.
                        writeCommandBuffers(logicalDevice, sc, sc->effects, sc->commandBuffersEffect, index);
                    }
                });
                if (!updated)
                    slot.mode = target.mode = PresentMode::CopyOnly;  // the layer just soft-disabled itself
            }

            // The frame's depth, copied in the layer's own command buffer ahead of
            // the effects. Without a source the target is cleared, never stale.
            if (slot.mode == PresentMode::Effects && shared.depthCapture && logicalDevice->depth)
            {
                guardLayerWork(logicalDevice, "depth capture", [&] {
                    const auto settings = ConfigStore::instance().snapshot();
                    DepthCaptureParams params;
                    params.invert      = settings->depthInvert;
                    params.flip        = settings->depthFlip;
                    params.flop        = settings->depthFlop;
                    params.resolveMode = settings->depthResolveMode == 1 ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT
                                                                         : VK_RESOLVE_MODE_AVERAGE_BIT;
                    slot.depth = logicalDevice->depth->selectForPresent(handleToU64(sc->handle), sc->imageExtent);
                    target.preCommandBuffer = shared.depthCapture->record(index, slot.depth, params);
                    const uint64_t sourceId = slot.depth.valid ? handleToU64(slot.depth.image) : 0;
                    if (sourceId != shared.lastDepthSource)
                    {
                        shared.lastDepthSource = sourceId;
                        VKHOOK_LOG_DEBUG([&](std::string& s) {
                            s = slot.depth.valid ? "depth capture source changed: image " + convertToString(slot.depth.image)
                                                       + (slot.depth.fromStorage ? " (preserved copy)" : " (live)")
                                                 : "depth capture source changed: none (far plane)";
                        });
                    }
                });
            }

            target.commandBuffer = slot.mode == PresentMode::Effects ? sc->commandBuffersEffect[index]
                                                                     : sc->commandBuffersNoEffect[index];
            target.signalSemaphore = shared.semaphores[index];
            target.fence           = index < shared.effectFences.size() ? shared.effectFences[index] : VK_NULL_HANDLE;

            contexts[i] = SubmitContext{logicalDevice, sc, index, frameSerial};
            target.context = &contexts[i];
            if (slot.mode == PresentMode::Effects)
                target.prepare = &prepareEffectSubmission;

            // the overlay frame follows in every mode
            if (shared.overlay)
            {
                guardLayerWork(logicalDevice, "overlay record", [&] {
                    const VkCommandBuffer overlayCmd = shared.overlay->record(
                        index, shared.imageViews[index], sc->imageExtent.width, sc->imageExtent.height);
                    if (overlayCmd != VK_NULL_HANDLE)
                    {
                        target.overlayCommandBuffer = overlayCmd;
                        target.overlaySemaphore     = shared.overlaySemaphores[index];
                        target.overlayFence         = &overlayFenceFor;
                    }
                });
            }

            recordSubmittedBatch(logicalDevice, slot.mode == PresentMode::Effects ? "effect" : "copy", index,
                                 pPresentInfo->waitSemaphoreCount > 0 ? pPresentInfo->pWaitSemaphores[0] : VK_NULL_HANDLE,
                                 target.signalSemaphore, target.fence);
        }

        // modes may have been downgraded by a failed effect update
        for (size_t i = 0; i < slots.size(); ++i)
            modes[i] = targets[i].mode;

        std::vector<VkSemaphore>& presentWaits = tlsPresentWaits;
        const PresentSubmitResult submitted = submitPresentBatches(
            logicalDevice->queues, submitQueue, pPresentInfo->pWaitSemaphores, pPresentInfo->waitSemaphoreCount,
            targets.data(), static_cast<uint32_t>(targets.size()), presentWaits);
        progress.layerSubmitted = true;

        // the newest layer work touching each swapchain, for its destroy
        for (size_t i = 0; i < slots.size(); ++i)
        {
            if (slots[i].swapchain && slots[i].swapchain->shared && targets[i].serial)
            {
                std::atomic<uint64_t>& last = slots[i].swapchain->shared->lastSerial;
                uint64_t seen = last.load(std::memory_order_relaxed);
                while (seen < targets[i].serial && !last.compare_exchange_weak(seen, targets[i].serial)) {}
            }
        }

        if (submitted.result != VK_SUCCESS)
        {
            reportDeviceLostDiagnostics(logicalDevice, submitQueue, "layer submit", submitted.result);
            if (submitted.result == VK_ERROR_DEVICE_LOST)
                panicLayer(logicalDevice, "Vulkan device lost during the layer submission");
            else
                panicLayer(logicalDevice, "layer submission failed (VkResult " + std::to_string(submitted.result)
                    + (submitted.auxiliaryQueued ? ", after auxiliary work was queued" : "") + ")");
            return submitted.result;
        }

        for (size_t i = 0; i < slots.size(); ++i)
        {
            if (!slots[i].swapchain || !slots[i].swapchain->faked())
                continue;
            const uint32_t index = slots[i].index;
            SwapchainShared& shared = *slots[i].swapchain->shared;
            if (index < shared.effectFenceUsed.size() && targets[i].fence != VK_NULL_HANDLE)
                shared.effectFenceUsed[index] = 1;
        }

        VkPresentInfoKHR presentInfo   = *pPresentInfo;
        presentInfo.waitSemaphoreCount = static_cast<uint32_t>(presentWaits.size());
        presentInfo.pWaitSemaphores    = presentWaits.data();

        const VkResult presentResult = realPresent(logicalDevice, queue, &presentInfo,
                                                   "vkQueuePresentKHR(final)", "Vulkan device lost during final present");

        // OUT_OF_DATE / SUBOPTIMAL are returned unchanged: swallowing them hid the
        // standard recreate-now signal from the application, whose present path
        // then waited forever on frames the compositor would never take.
        return presentResult;
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
    {
        auto deviceRef = findDevice(GetKey(queue));
        if (!deviceRef)
            return failUnknownObject("vkQueuePresentKHR", VK_ERROR_DEVICE_LOST);
        LogicalDevice* logicalDevice = deviceRef.get();

        PresentProgress progress;
        try
        {
            return presentImpl(logicalDevice, queue, pPresentInfo, progress);
        }
        catch (const std::exception& e)
        {
            reportHookException(logicalDevice, "vkQueuePresentKHR", e.what());
        }
        catch (...)
        {
            reportHookException(logicalDevice, "vkQueuePresentKHR", nullptr);
        }

        // The layer failed. Before its submission the app's present goes through
        // unchanged; after it, the app's waits are consumed and the present can
        // only wait on what the layer signalled.
        if (!progress.layerSubmitted)
            return realPresent(logicalDevice, queue, pPresentInfo, "vkQueuePresentKHR(exception fallback)",
                               "Vulkan device lost during exception fallback present");

        VkPresentInfoKHR presentInfo   = *pPresentInfo;
        presentInfo.waitSemaphoreCount = static_cast<uint32_t>(tlsPresentWaits.size());
        presentInfo.pWaitSemaphores    = tlsPresentWaits.data();
        return realPresent(logicalDevice, queue, &presentInfo, "vkQueuePresentKHR(exception fallback)",
                           "Vulkan device lost during exception fallback present");
    }

    const InterceptEntry kPresentIntercepts[] = {
        VKHOOK_INTERCEPT(Unconditional, QueuePresentKHR),
    };
} // namespace

    InterceptTable presentInterceptTable()
    {
        return makeInterceptTable(kPresentIntercepts);
    }
} // namespace VkHook
