#include "layer/entry.hh"
#include "layer/registry.hh"
#include "layer/guard.hh"

#include "control/frontend.hh"
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

    // The real present, bracketed so Mesa's explicit-sync points inside it
    // are not interleaved with window commits from other threads.
    VkResult realPresent(LogicalDevice* logicalDevice, VkQueue queue, const VkPresentInfoKHR* presentInfo,
                         const char* diagContext, const char* panicReason)
    {
        installWaylandInterposeHooks();

        const std::shared_ptr<InputSession> session = presentSession(*logicalDevice, presentInfo);
        const uint32_t surfaceId = session ? session->waylandSurfaceId() : 0;
        if (surfaceId)
            WaylandSyncobjGate::beginPresent(surfaceId);

        const VkResult vr = logicalDevice->queues.present(queue, presentInfo);

        if (surfaceId)
            WaylandSyncobjGate::endPresent(surfaceId);

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
        bool                              depthBypass = false;  // depth unusable this frame
        bool                              rebuild     = false;  // chain rebuild due
        bool                              recovered   = false;  // the rebuild ends a depth retry
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

    // Depth recovery is intentionally non-blocking (invariant 6). Decides, under
    // stateLock, which presented swapchains need a chain rebuild and which must
    // run CopyOnly this frame because their depth is unusable; never rebuilds
    // here. A chain whose depth does not match must not submit its effect
    // buffers: they may reference a depth image the game has just destroyed.
    void decideDepth(LogicalDevice* logicalDevice, std::vector<PresentSlot>& slots)
    {
        const bool hold = logicalDevice->depthTransitionHold.load(std::memory_order_acquire);

        // Depth promotion and pin changes happen while the application is
        // recording/submitting its render work. The chains are rebuilt here,
        // at the next present boundary, once the input settled: a burst of
        // transform clicks used to arm a rebuild per frame.
        if (logicalDevice->depthReallocPending && !hold)
        {
            const auto now = std::chrono::steady_clock::now();
            if (!logicalDevice->depthReallocFirstArmed.time_since_epoch().count())
                logicalDevice->depthReallocFirstArmed = now;

            constexpr auto kDepthRebuildSettle = std::chrono::milliseconds(150);
            if (now - logicalDevice->depthReallocFirstArmed >= kDepthRebuildSettle)
            {
                for (auto& [_, sc] : logicalDevice->swapchains)
                    if (sc)
                        sc->depthReallocPending = true;
                logicalDevice->depthReallocPending    = false;
                logicalDevice->depthReallocFirstArmed = {};
            }
        }

        for (PresentSlot& slot : slots)
        {
            LogicalSwapchain* sc = slot.swapchain.get();
            if (!sc || !sc->faked())
                continue;

            if (sc->depthReallocPending && !hold && !sc->commandBuffersEffect.empty())
                slot.rebuild = true;

            DepthState currentDepth = getDepthState(logicalDevice);
            const bool currentValid = !hasDepthState(currentDepth) || validateDepthStateForResolve(logicalDevice, currentDepth);
            const bool exactMatch   = currentValid && depthMatchesSwapchainExtent(currentDepth, sc);

            if (hasDepthState(currentDepth) && (!currentValid || !exactMatch))
            {
                armDepthRetryLocked(logicalDevice, sc,
                                    currentValid ? "depth extent does not match swapchain"
                                                 : "depth image is no longer valid");
                slot.depthBypass = true;
                continue;
            }

            if (logicalDevice->depthRetry.disabled)
            {
                slot.depthBypass = true;
                if (!depthRetryDueLocked(logicalDevice))
                    continue;

                DepthState candidate{};
                if (!selectDepthCandidateForSwapchainLocked(logicalDevice, sc, candidate))
                    candidate = getDepthState(logicalDevice);
                if (depthMatchesSwapchainExtent(candidate, sc) && validateDepthStateForResolve(logicalDevice, candidate))
                {
                    logicalDevice->activeDepthState = candidate;
                    slot.rebuild                    = true;
                    slot.recovered                  = true;
                }
                else
                {
                    VKHOOK_LOG_DEBUG([](std::string& s) {
                        s = "depth retry: no swapchain-sized depth buffer yet (exact or chrome-inset)";
                    });
                    scheduleDepthRetryLocked(logicalDevice, false);
                }
            }
        }
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

        bool anyFaked = false;
        for (const PresentSlot& slot : slots)
            anyFaked |= slot.swapchain && slot.swapchain->faked();

        if (anyFaked)
        {
            {
                std::lock_guard<std::mutex> l(logicalDevice->stateLock);
                decideDepth(logicalDevice, slots);
            }

            // chain rebuilds: off-lock builds, swapped in under the short lock
            for (PresentSlot& slot : slots)
            {
                if (!slot.rebuild)
                    continue;
                const VkSwapchainKHR handle = slot.swapchain->handle;
                if (rebuildSwapchainChain(logicalDevice, slot.swapchain, nullptr))
                {
                    slot.swapchain = findSwapchain(*logicalDevice, handle);
                    if (!slot.swapchain)
                        continue;
                    slot.depthBypass = false;
                    Logger::debug("deferred depth change rebuilt presented swapchain");
                    if (slot.recovered)
                    {
                        std::lock_guard<std::mutex> l(logicalDevice->stateLock);
                        logicalDevice->depthRetry = DepthRetryState{};
                        logicalDevice->depthReallocPending = false;
                        for (auto& [_, other] : logicalDevice->swapchains)
                            if (other && other.get() != slot.swapchain.get())
                                other->depthReallocPending = true;
                    }
                }
                else if (slot.recovered)
                {
                    std::lock_guard<std::mutex> l(logicalDevice->stateLock);
                    scheduleDepthRetryLocked(logicalDevice, true);
                }
            }
        }

        // modes, and the bounds every faked swapchain needs
        const bool depthHold = logicalDevice->depthTransitionHold.load(std::memory_order_acquire);
        std::vector<PresentMode> modes(slots.size());
        for (size_t i = 0; i < slots.size(); ++i)
        {
            PresentSlot& slot = slots[i];
            LogicalSwapchain* sc = slot.swapchain.get();
            ModeInputs in;
            in.faked             = sc && sc->faked();
            in.effectsOn         = effectsOn;
            in.softDisabled      = softDisabled;
            in.depthHold         = depthHold;
            in.generationPending = slot.depthBypass || (sc && sc->depthReallocPending);
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
                        DepthState frameDepth;
                        {
                            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
                            frameDepth = getDepthState(logicalDevice);
                            if (!depthMatchesSwapchainExtent(frameDepth, sc)
                                || !validateDepthStateForResolve(logicalDevice, frameDepth))
                                frameDepth = {};
                        }
                        writeCommandBuffers(logicalDevice, sc, sc->effects, sc->commandBuffersEffect, frameDepth, index);
                    }
                });
                if (!updated)
                    slot.mode = target.mode = PresentMode::CopyOnly;  // the layer just soft-disabled itself
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
            maybeDumpDepthResolveImage(logicalDevice, slots[i].swapchain.get(), index, submitQueue, control.diag);
        }

        VkPresentInfoKHR presentInfo   = *pPresentInfo;
        presentInfo.waitSemaphoreCount = static_cast<uint32_t>(presentWaits.size());
        presentInfo.pWaitSemaphores    = presentWaits.data();

        const VkResult presentResult = realPresent(logicalDevice, queue, &presentInfo,
                                                   "vkQueuePresentKHR(final)", "Vulkan device lost during final present");

        // OUT_OF_DATE / SUBOPTIMAL: flag the affected swapchains for a rebuild. The
        // result is returned unchanged: swallowing OUT_OF_DATE hid the standard
        // recreate-now signal from the application, whose present path then
        // waited forever on frames the compositor would never take.
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
        {
            Logger::warn("QueuePresentKHR returned " + std::string(presentResult == VK_ERROR_OUT_OF_DATE_KHR ? "OUT_OF_DATE" : "SUBOPTIMAL") + ", flagging swapchains for reset");
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            for (const PresentSlot& slot : slots)
                if (slot.swapchain)
                    slot.swapchain->depthReallocPending = true;
        }

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
