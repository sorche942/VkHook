#include "layer/entry.hh"
#include "layer/state.hh"

#include "control/frontend.hh"
#include "depth/tracker.hh"
#include "diag/gpu_crash.hh"

#include "command_buffer.hh"
#include "format.hh"
#include "vk_handle.hh"
#include "settings_manager.hh"
#include "imgui_overlay.hh"
#include "logger.hh"
#include "util.hh"
#include "wayland_display.hh"
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
    thread_local std::vector<VkSemaphore>          tlsToastWaitSemaphores;
    thread_local std::vector<VkSemaphore>          tlsPresentSemaphores;
    thread_local std::vector<VkPipelineStageFlags> tlsWaitStages;

    // Everything one present needs, snapshotted under globalLock so the
    // submission and the present itself run without it.
    struct PresentFrame
    {
        std::shared_ptr<LogicalDevice>                 device;
        DiagState*                                     diag = nullptr;  // owned by the device's control state
        bool                                           effectsOn = false;
        bool                                           hasPassthrough = false;
        std::vector<std::shared_ptr<LogicalSwapchain>> swapchains;
        std::vector<uint32_t>                          indices;
    };

    // Forwards the application's present to the next layer and runs the
    // device-lost diagnostics on the result.
    VkResult forwardPresent(LogicalDevice* logicalDevice, VkQueue queue, const VkPresentInfoKHR* presentInfo,
                            const char* diagContext, const char* panicReason)
    {
        VkResult vr = logicalDevice->vkd.QueuePresentKHR(queue, presentInfo);
        reportDeviceLostDiagnostics(logicalDevice, queue, diagContext, vr);
        if (vr == VK_ERROR_DEVICE_LOST)
            panicLayer(logicalDevice, panicReason);
        return vr;
    }

    // Submit the overlay command buffer if visible; returns the semaphore the
    // present has to wait on.
    VkResult submitOverlayFrame(LogicalDevice* logicalDevice, LogicalSwapchain* swapchain,
                                uint32_t index, VkSemaphore& outSemaphore)
    {
        outSemaphore = swapchain->semaphores[index];  // Default: wait on effects semaphore

        if (!logicalDevice->imguiOverlay)
            return VK_SUCCESS;

        VkCommandBuffer overlayCmd = logicalDevice->imguiOverlay->recordFrame(
            index, swapchain->imageViews[index],
            swapchain->imageExtent.width, swapchain->imageExtent.height);

        if (overlayCmd == VK_NULL_HANDLE)
            return VK_SUCCESS;

        // The wait must cover the capture blit's TRANSFER stage as well as the
        // overlay's colour output, or the blit could read the swapchain image
        // before the effect pass that produced it has finished writing.
        VkPipelineStageFlags overlayWaitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo overlaySubmit = {};
        overlaySubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        overlaySubmit.waitSemaphoreCount = 1;
        overlaySubmit.pWaitSemaphores = &swapchain->semaphores[index];
        overlaySubmit.pWaitDstStageMask = &overlayWaitStage;
        overlaySubmit.commandBufferCount = 1;
        overlaySubmit.pCommandBuffers = &overlayCmd;
        overlaySubmit.signalSemaphoreCount = 1;
        overlaySubmit.pSignalSemaphores = &swapchain->overlaySemaphores[index];

        // Use fence to track command buffer completion (prevents reuse while in flight)
        VkFence overlayFence = logicalDevice->imguiOverlay->getCommandBufferFence(index);
        recordSubmittedBatch(logicalDevice, "overlay", index,
                             swapchain->semaphores[index], swapchain->overlaySemaphores[index], overlayFence);
        VkResult vr = logicalDevice->vkd.QueueSubmit(logicalDevice->queue, 1, &overlaySubmit, overlayFence);
        if (vr == VK_SUCCESS)
            outSemaphore = swapchain->overlaySemaphores[index];

        return vr;
    }

    // Step 1: presents that never reach the effect path. An optical-flow-only
    // queue is passed straight down; a soft-disabled device presents the app's
    // images (plus an overlay-only frame while a toast is showing).
    std::optional<VkResult> presentSpecialCases(VkQueue queue, const VkPresentInfoKHR* pPresentInfo, uint64_t& frameSerial)
    {
        std::shared_ptr<LogicalDevice> passThroughDevice;
        bool softDisabled = false;
        bool hasToasts = false;
        {
            scoped_lock l(globalLock);
            passThroughDevice = findDeviceLocked(GetKey(queue));
            if (!passThroughDevice)
                return failUnknownObject("vkQueuePresentKHR", VK_ERROR_DEVICE_LOST);
            frameSerial = ++passThroughDevice->presentSerial;
            softDisabled = passThroughDevice->softDisabled.load(std::memory_order_acquire);
            const bool deviceLost = isDeviceLost(passThroughDevice.get());
            if (softDisabled && deviceLost)
                hasToasts = false;
            if (softDisabled && passThroughDevice->imguiOverlay && !deviceLost)
                hasToasts = passThroughDevice->imguiOverlay->hasPendingToasts();
        }
        if (queue == passThroughDevice->dlss5OpticalFlowQueue)
        {
            // An optical-only queue is not a graphics presentation path.
            std::lock_guard queueLock(passThroughDevice->dlss5OpticalFlowQueueMutex);
            return passThroughDevice->vkd.QueuePresentKHR(queue, pPresentInfo);
        }

        if (!softDisabled)
            return std::nullopt;

        // Soft-disable fast path: panicLayer() has flipped this bit. The layer is
        // no longer trusted to touch the GPU, so we pass straight through to the
        // driver, except that we still submit an overlay-only frame so any pending
        // toast notification stays visible. The overlay's render pass only writes
        // to the swapchain image, so the present must wait on the overlay's signal
        // semaphore.
        if (!hasToasts || !passThroughDevice->imguiOverlay || pPresentInfo->swapchainCount == 0)
            return forwardPresent(passThroughDevice.get(), queue, pPresentInfo,
                                  "vkQueuePresentKHR(soft-disabled)", "Vulkan device lost during soft-disabled present");

        // Submit an overlay-only frame for each swapchain, chained onto
        // the original wait semaphores so the present stays ordered.
        auto& presentWaitSems = tlsToastWaitSemaphores;
        presentWaitSems.clear();
        presentWaitSems.reserve(pPresentInfo->swapchainCount);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        for (unsigned int i = 0; i < pPresentInfo->swapchainCount; i++)
        {
            auto swapRef = findSwapchain(pPresentInfo->pSwapchains[i]);
            if (!swapRef)
            {
                presentWaitSems.push_back(
                    i < pPresentInfo->waitSemaphoreCount ? pPresentInfo->pWaitSemaphores[i] : VK_NULL_HANDLE);
                continue;
            }
            LogicalSwapchain* swap = swapRef.get();
            uint32_t index = pPresentInfo->pImageIndices[i];
            if (index >= swap->imageCount || index >= swap->overlaySemaphores.size())
            {
                presentWaitSems.push_back(VK_NULL_HANDLE);
                continue;
            }

            VkCommandBuffer overlayCmd = passThroughDevice->imguiOverlay->recordFrame(
                index, swap->imageViews[index],
                swap->imageExtent.width, swap->imageExtent.height);

            if (overlayCmd == VK_NULL_HANDLE)
            {
                // No overlay work this frame — fall back to waiting on the
                // original semaphore (or none if there isn't one).
                presentWaitSems.push_back(
                    i < pPresentInfo->waitSemaphoreCount ? pPresentInfo->pWaitSemaphores[i] : VK_NULL_HANDLE);
                continue;
            }

            VkSubmitInfo oi = {};
            oi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            oi.pWaitDstStageMask = &waitStage;
            if (i < pPresentInfo->waitSemaphoreCount && pPresentInfo->pWaitSemaphores[i] != VK_NULL_HANDLE)
            {
                oi.waitSemaphoreCount = 1;
                oi.pWaitSemaphores = &pPresentInfo->pWaitSemaphores[i];
            }
            oi.commandBufferCount = 1;
            oi.pCommandBuffers = &overlayCmd;
            oi.signalSemaphoreCount = 1;
            oi.pSignalSemaphores = &swap->overlaySemaphores[index];

            VkFence overlayFence = passThroughDevice->imguiOverlay->getCommandBufferFence(index);
            VkResult vr = passThroughDevice->vkd.QueueSubmit(
                passThroughDevice->queue, 1, &oi, overlayFence);
            if (vr != VK_SUCCESS)
            {
                Logger::err("Toast-only overlay submit failed: " + std::to_string(vr));
                if (vr == VK_ERROR_DEVICE_LOST)
                    panicLayer(passThroughDevice.get(), "Vulkan device lost during toast-only overlay submit");
                return forwardPresent(passThroughDevice.get(), queue, pPresentInfo,
                                      "vkQueuePresentKHR(toast fallback)", "Vulkan device lost during toast fallback present");
            }
            presentWaitSems.push_back(swap->overlaySemaphores[index]);
        }

        VkPresentInfoKHR pi = *pPresentInfo;
        pi.waitSemaphoreCount = static_cast<uint32_t>(presentWaitSems.size());
        pi.pWaitSemaphores = presentWaitSems.data();
        return forwardPresent(passThroughDevice.get(), queue, &pi,
                              "vkQueuePresentKHR(toast overlay)", "Vulkan device lost during toast overlay present");    }

    // Step 2: a surface neither backend handles gets the app's present untouched.
    std::optional<VkResult> presentUnsupportedSurface(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
    {
        if (isWayland() || isX11())
            return std::nullopt;

        auto device = findDevice(GetKey(queue));
        if (!device)
            return failUnknownObject("vkQueuePresentKHR", VK_ERROR_DEVICE_LOST);
        if (!device->queue)
            return forwardPresent(device.get(), queue, pPresentInfo,
                                  "vkQueuePresentKHR(unsupported-surface direct)",
                                  "Vulkan device lost during unsupported-surface direct present");
        return forwardPresent(device.get(), queue, pPresentInfo,
                              "vkQueuePresentKHR(unsupported-surface passthrough)",
                              "Vulkan device lost during unsupported-surface passthrough present");
    }

    // Step 3: run the control step (hotkeys, hot-reload, overlay requests) and
    // snapshot the device, swapchains and image indices of this present.
    std::optional<VkResult> snapshotPresentFrame(VkQueue queue, const VkPresentInfoKHR* pPresentInfo, PresentFrame& frame)
    {
        std::unique_lock<std::mutex> l(globalLock);

        frame.device = findDeviceLocked(GetKey(queue));
        if (!frame.device)
            return failUnknownObject("vkQueuePresentKHR", VK_ERROR_DEVICE_LOST);
        if (!frame.device->queue)
        {
            auto device = frame.device;
            l.unlock();
            return device->vkd.QueuePresentKHR(queue, pPresentInfo);
        }

        DeviceControlState& control = deviceControlLocked(frame.device.get());
        frame.diag = &control.diag;
        frame.effectsOn = runFrontendFrameLocked(frame.device.get(), control);

        frame.swapchains.reserve(pPresentInfo->swapchainCount);
        frame.indices.reserve(pPresentInfo->swapchainCount);
        for (unsigned int i = 0; i < pPresentInfo->swapchainCount; i++)
        {
            auto swapRef = findSwapchainLocked(pPresentInfo->pSwapchains[i]);
            if (!swapRef)
            {
                Logger::err("present references unknown swapchain");
                return VK_ERROR_OUT_OF_DATE_KHR;
            }

            LogicalSwapchain* logicalSwapchain = swapRef.get();
            uint32_t index = pPresentInfo->pImageIndices[i];
            if (logicalSwapchain->passthrough)
            {
                // Real images were served for this swapchain; only the real
                // image list bounds the index, and the whole present goes
                // straight through.
                if (index >= logicalSwapchain->images.size())
                {
                    Logger::err("present image index out of bounds for passthrough swapchain");
                    return VK_ERROR_OUT_OF_DATE_KHR;
                }
                frame.hasPassthrough = true;
            }
            else if (index >= logicalSwapchain->imageCount
                || index >= logicalSwapchain->semaphores.size()
                || index >= logicalSwapchain->overlaySemaphores.size()
                || index >= logicalSwapchain->imageViews.size())
            {
                Logger::err("present image index out of bounds for swapchain");
                return VK_ERROR_OUT_OF_DATE_KHR;
            }

            const auto& commandBuffers = frame.effectsOn
                ? logicalSwapchain->commandBuffersEffect
                : logicalSwapchain->commandBuffersNoEffect;
            if (!logicalSwapchain->passthrough && index >= commandBuffers.size())
            {
                Logger::err("present command buffer index out of bounds");
                return VK_ERROR_OUT_OF_DATE_KHR;
            }

            frame.swapchains.push_back(std::move(swapRef));
            frame.indices.push_back(index);
        }
        return std::nullopt;
    }

    // Depth recovery is intentionally non-blocking. If the currently selected
    // depth image does not exactly match the present swapchain, do not submit our
    // existing effect command buffer: it may reference a depth image that the
    // game has just resized/destroyed. Returns true when the application's present
    // must pass straight through while depth is disabled.
    bool prepareDepthForPresent(LogicalDevice* logicalDevice, const PresentFrame& frame)
    {
        const auto& presentSwapchains = frame.swapchains;
        const auto& presentIndices    = frame.indices;
        bool bypassDepthForPresent = false;
        {
            scoped_lock depthLock(globalLock);

            // Depth promotion and pin changes happen while the application is
            // recording/submitting its render work. Rebuild our command
            // buffers here, at the next present boundary, after every affected
            // swapchain's previous effect submissions have completed. Without
            // consuming this flag, newly discovered depth is captured but the
            // effects keep using command buffers recorded without depth until
            // a full config reload (F10/Delete) occurs.
            if (logicalDevice->depthReallocPending && !logicalDevice->depthTransitionHold)
            {
                // let the input settle: a burst of transform clicks used to arm a
                // rebuild per frame, each freeing and re-recording the whole chain
                const auto now = std::chrono::steady_clock::now();
                if (!logicalDevice->depthReallocFirstArmed.time_since_epoch().count())
                    logicalDevice->depthReallocFirstArmed = now;

                constexpr auto kDepthRebuildSettle = std::chrono::milliseconds(150);
                if (now - logicalDevice->depthReallocFirstArmed < kDepthRebuildSettle)
                {
                    // stay armed; the recorded buffers are still valid and just carry
                    // the previous transform for another frame or two
                }
                else
                {
                    // Safe mode (the default) idles the queue before the rebuild
                    // below frees command buffers and tears down resolve targets.
                    // The per-image fences only cover the layer's own submissions;
                    // the app's command buffers reference the same resolve
                    // framebuffers and images through the injected resolve subpass,
                    // so freeing them mid-flight hangs the GPU (device lost). The
                    // old full-reload path idled the queue here too; fast mode is
                    // the opt-in for skipping it. This idle is bounded: a stuck
                    // queue must not park the present thread under globalLock.
                    const bool idleOk = settingsManager.getDepthRebuildMode() != 0
                        || waitQueueIdleBounded(logicalDevice, "safe depth rebuild");

                    if (idleOk)
                    {
                        for (const auto& [_, sc] : swapchainMap)
                            if (sc && sc->logicalDevice == logicalDevice)
                                sc->depthReallocPending = true;
                        logicalDevice->depthReallocPending = false;
                        logicalDevice->depthReallocFirstArmed = {};
                    }
                    else
                    {
                        // everything stays armed; back the retry off so one bad
                        // queue is not a stall every frame
                        logicalDevice->depthReallocFirstArmed = now + std::chrono::seconds(2);
                        bypassDepthForPresent = true;
                    }
                }
            }

            DepthState effectiveDepth = getDepthState(logicalDevice);
            if (hasDepthState(effectiveDepth) &&
                !validateDepthStateForResolve(logicalDevice, effectiveDepth))
                effectiveDepth = {};

            for (const auto& scPtr : presentSwapchains)
            {
                LogicalSwapchain* sc = scPtr.get();
                if (!sc || sc->logicalDevice != logicalDevice || !sc->depthReallocPending
                    || sc->commandBuffersEffect.empty() || logicalDevice->depthTransitionHold)
                    continue;

                if (!depthRebuildFencesReady(logicalDevice, sc))
                {
                    // Keep the request armed and pass through until the last
                    // submission using this swapchain's buffers has completed.
                    bypassDepthForPresent = true;
                    continue;
                }

                DepthState swapchainDepth{};
                if (depthMatchesSwapchainExtent(effectiveDepth, sc))
                    swapchainDepth = effectiveDepth;
                else if (logicalDevice->pinnedDepthImageView == VK_NULL_HANDLE)
                    selectDepthCandidateForSwapchainLocked(logicalDevice, sc, swapchainDepth);

                if (!hasDepthState(effectiveDepth) && hasDepthState(swapchainDepth))
                {
                    effectiveDepth = swapchainDepth;
                    logicalDevice->activeDepthState = swapchainDepth;
                }

                reallocateCommandBuffers(logicalDevice, sc, swapchainDepth);
                sc->depthReallocPending = false;
                VKHOOK_LOG_DEBUG([&](std::string& s) {
                    s = std::string("deferred depth change rebuilt presented swapchain")
                        + (hasDepthState(swapchainDepth) ? " with depth " : " without depth ")
                        + std::to_string(swapchainDepth.extent.width) + "x"
                        + std::to_string(swapchainDepth.extent.height);
                });
            }

            for (size_t presentIndex = 0; presentIndex < presentSwapchains.size(); ++presentIndex)
            {
                LogicalSwapchain* sc = presentSwapchains[presentIndex].get();
                if (!sc || sc->logicalDevice != logicalDevice)
                    continue;

                // An in-flight fence for this image means its command buffer may
                // still be executing. Submitting the next one unfenced would
                // leave that work untracked, and the rebuild gate can only see
                // fenced submissions, so skip our submit for this frame the way
                // the depth-copy ring skips a busy slot.
                const uint32_t presentImage = presentIndices[presentIndex];
                if (presentImage < sc->effectSubmitFences.size()
                    && presentImage < sc->effectSubmitFenceUsed.size()
                    && sc->effectSubmitFenceUsed[presentImage]
                    && sc->effectSubmitFences[presentImage] != VK_NULL_HANDLE
                    && logicalDevice->vkd.GetFenceStatus(
                           logicalDevice->device, sc->effectSubmitFences[presentImage]) != VK_SUCCESS)
                {
                    bypassDepthForPresent = true;
                    break;
                }

                DepthState currentDepth = getDepthState(logicalDevice);
                const bool currentValid = !hasDepthState(currentDepth)
                    || validateDepthStateForResolve(logicalDevice, currentDepth);
                const bool exactMatch = currentValid
                    && depthMatchesSwapchainExtent(currentDepth, sc);

                auto retryIt = depthRetryStates.find(logicalDevice);
                const bool retryDisabled = retryIt != depthRetryStates.end() && retryIt->second.disabled;

                if (hasDepthState(currentDepth) && (!currentValid || !exactMatch))
                {
                    armDepthRetryLocked(logicalDevice, sc,
                                        currentValid ? "depth extent does not match swapchain"
                                                     : "depth image is no longer valid");
                    bypassDepthForPresent = true;
                    break;
                }

                if (retryDisabled)
                {
                    bypassDepthForPresent = true;

                    if (depthRetryDueLocked(logicalDevice))
                    {
                        DepthState candidate{};
                        if (!selectDepthCandidateForSwapchainLocked(logicalDevice, sc, candidate))
                            candidate = getDepthState(logicalDevice);
                        if (depthMatchesSwapchainExtent(candidate, sc)
                            && validateDepthStateForResolve(logicalDevice, candidate))
                        {
                            if (depthRebuildFencesReady(logicalDevice, sc))
                            {
                                logicalDevice->activeDepthState = candidate;
                                reallocateCommandBuffers(logicalDevice, sc, candidate);
                                logicalDevice->depthReallocPending = false;
                                for (auto& [_, swapchain] : swapchainMap)
                                    if (swapchain && swapchain->logicalDevice == logicalDevice
                                        && swapchain.get() != sc)
                                        swapchain->depthReallocPending = true;
                                auto& retry = depthRetryStates[logicalDevice];
                                retry.disabled = false;
                                retry.retryPending = false;
                                retry.retryAt = {};
                                bypassDepthForPresent = false;
                            }
                            else
                                scheduleDepthRetryLocked(logicalDevice, true);
                        }
                        else
                        {
                            VKHOOK_LOG_DEBUG([](std::string& s) {
                                s = "depth retry: no swapchain-sized depth buffer yet (exact or chrome-inset)";
                            });
                            scheduleDepthRetryLocked(logicalDevice, false);
                        }
                    }
                    break;
                }
            }

        }
        return bypassDepthForPresent;
    }

    // Submits the effect chain and the overlay for every presented swapchain and
    // collects the semaphores the real present waits on. A value is returned when
    // the present must end here with that result.
    std::optional<VkResult> submitFrame(VkQueue queue, const VkPresentInfoKHR* pPresentInfo, PresentFrame& frame,
                                        uint64_t frameSerial)
    {
        LogicalDevice* logicalDevice = frame.device.get();
        auto& presentSemaphores = tlsPresentSemaphores;
        auto& waitStages        = tlsWaitStages;
        presentSemaphores.clear();
        presentSemaphores.reserve(pPresentInfo->swapchainCount);
        waitStages.assign(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

        for (unsigned int i = 0; i < pPresentInfo->swapchainCount; i++)
        {
            LogicalSwapchain* logicalSwapchain = frame.swapchains[i].get();
            uint32_t index = frame.indices[i];
            // Update effect uniforms only when effects are active (saves CPU+GPU when off).
            // Wrapped in try/catch so a misbehaving effect's per-frame update can
            // never escape into vkQueuePresentKHR (which is noexcept from the
            // app's perspective).
            if (frame.effectsOn)
            {
                try
                {
                    bool frameCommandsChanged = false;
                    for (auto& effect : logicalSwapchain->effects)
                        frameCommandsChanged |= effect->updateFrame(index);
                    if (frameCommandsChanged)
                    {
                        // Updating ordinary descriptor bindings invalidates the
                        // executable CB. Re-record only this fence-safe image.
                        scoped_lock recordLock(globalLock);
                        DepthState frameDepth = getDepthState(logicalDevice);
                        if (!depthMatchesSwapchainExtent(frameDepth, logicalSwapchain)
                            || !validateDepthStateForResolve(logicalDevice, frameDepth))
                            frameDepth = {};
                        writeCommandBuffers(logicalDevice, logicalSwapchain, logicalSwapchain->effects,
                                            logicalSwapchain->commandBuffersEffect, frameDepth, index);
                    }
                }
                catch (const std::exception& e)
                {
                    panicLayer(logicalDevice,
                        std::string("Effect update threw: ") + e.what());
                    return logicalDevice->vkd.QueuePresentKHR(queue, pPresentInfo);
                }
                catch (...)
                {
                    panicLayer(logicalDevice, "Effect update threw unknown exception");
                    return logicalDevice->vkd.QueuePresentKHR(queue, pPresentInfo);
                }
            }

            const auto& commandBuffers = frame.effectsOn
                ? logicalSwapchain->commandBuffersEffect
                : logicalSwapchain->commandBuffersNoEffect;

            // Submit effect command buffer
            VkSubmitInfo submitInfo = {};
            submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.waitSemaphoreCount = i == 0 ? pPresentInfo->waitSemaphoreCount : 0;
            submitInfo.pWaitSemaphores    = i == 0 ? pPresentInfo->pWaitSemaphores : nullptr;
            submitInfo.pWaitDstStageMask  = i == 0 ? waitStages.data() : nullptr;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers    = &commandBuffers[index];
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores    = &logicalSwapchain->semaphores[index];

            VkResult vr = VK_SUCCESS;
            // Set once an effect's auxiliary submissions (DLSS5 optical flow) feed this
            // submission through their own semaphores: a failed submit strands them.
            bool auxiliaryQueued = false;
            {
                std::unique_lock opticalLock(logicalDevice->dlss5OpticalFlowQueueMutex, std::defer_lock);
                if (logicalDevice->dlss5OpticalFlowQueue) opticalLock.lock();
                // serialized with the destroy hooks and the drain: between this
                // check and the submit the render thread must not be able to
                // retire the generation these buffers reference
                std::unique_lock submitLock(globalLock);

                // a depth-generation change leaves the recorded buffers pointing
                // at dying resources; submit nothing until the rebuild re-records
                if (logicalDevice->depthTransitionHold || logicalDevice->depthReallocPending
                    || logicalSwapchain->depthReallocPending)
                    break;

                if (frame.effectsOn)
                {
                    const VkSemaphore* applicationWaits = submitInfo.pWaitSemaphores;
                    for (auto& effect : logicalSwapchain->effects)
                    {
                        vr = effect->prepareSubmission(index, frameSerial, submitInfo);
                        if (vr != VK_SUCCESS) break;
                    }
                    auxiliaryQueued = submitInfo.pWaitSemaphores != applicationWaits;
                    // Auxiliary submissions may already have consumed the app's
                    // waits. Never attempt an unsynchronized passthrough on failure.
                    if (vr != VK_SUCCESS)
                    {
                        // No final fence was submitted for partially queued work.
                        if (logicalDevice->dlss5OpticalFlowQueue && vr != VK_ERROR_DEVICE_LOST)
                        {
                            logicalDevice->vkd.QueueWaitIdle(logicalDevice->queue);
                            logicalDevice->vkd.QueueWaitIdle(logicalDevice->dlss5OpticalFlowQueue);
                        }
                        reportDeviceLostDiagnostics(logicalDevice, logicalDevice->queue, "effect auxiliary submit", vr);
                        // A failed preparation is terminal for the effect (DLSS5's
                        // optical-flow semaphores cannot resume): stop the effects
                        // instead of failing every later present.
                        submitLock.unlock();
                        if (opticalLock.owns_lock()) opticalLock.unlock();
                        panicLayer(logicalDevice, vr == VK_ERROR_DEVICE_LOST
                            ? std::string("Vulkan device lost during effect auxiliary submit")
                            : "effect auxiliary submit failed (VkResult " + std::to_string(vr) + ")");
                        return vr;
                    }
                }

                // Signal this image's fence so we can safely update its
                // descriptor sets and rebuild its command buffer in future frames.
                //
                // The fence must be unsignaled to submit with it, and the rebuild
                // gate reads "signaled" as "this image's last submission finished".
                // So the reset belongs here, once that is already true. An
                // in-flight fence is left alone and this submit goes unfenced: the
                // next present re-arms it. (Resetting it early would let the old
                // submission's signal track the new work and disarm the gate.)
                VkFence effectFence = (index < logicalSwapchain->effectSubmitFences.size())
                    ? logicalSwapchain->effectSubmitFences[index] : VK_NULL_HANDLE;
                if (effectFence != VK_NULL_HANDLE
                    && index < logicalSwapchain->effectSubmitFenceUsed.size()
                    && logicalSwapchain->effectSubmitFenceUsed[index])
                {
                    VkResult fenceStatus = logicalDevice->vkd.GetFenceStatus(logicalDevice->device, effectFence);
                    if (fenceStatus == VK_SUCCESS)
                        logicalDevice->vkd.ResetFences(logicalDevice->device, 1, &effectFence);
                    else
                    {
                        if (fenceStatus != VK_NOT_READY)
                            Logger::warn("effect submit fence status returned " + std::to_string(fenceStatus));
                        effectFence = VK_NULL_HANDLE;
                    }
                }
                recordSubmittedBatch(logicalDevice, "effect", index,
                                     submitInfo.waitSemaphoreCount > 0 ? submitInfo.pWaitSemaphores[0] : VK_NULL_HANDLE,
                                     logicalSwapchain->semaphores[index], effectFence);
                vr = logicalDevice->vkd.QueueSubmit(logicalDevice->queue, 1, &submitInfo, effectFence);
                if (vr == VK_SUCCESS && index < logicalSwapchain->effectSubmitFenceUsed.size())
                    logicalSwapchain->effectSubmitFenceUsed[index] = true;
                if (vr != VK_SUCCESS && vr != VK_ERROR_DEVICE_LOST && logicalDevice->dlss5OpticalFlowQueue)
                {
                    // The OF prepass may have succeeded while this fenced final
                    // submission failed; retire its work before any teardown.
                    logicalDevice->vkd.QueueWaitIdle(logicalDevice->queue);
                    logicalDevice->vkd.QueueWaitIdle(logicalDevice->dlss5OpticalFlowQueue);
                }
            }
            if (vr != VK_SUCCESS)
            {
                reportDeviceLostDiagnostics(logicalDevice, logicalDevice->queue, "vkQueueSubmit(effect)", vr);
                if (vr == VK_ERROR_DEVICE_LOST)
                    panicLayer(logicalDevice, "Vulkan device lost during effect submit");
                else if (auxiliaryQueued)
                    panicLayer(logicalDevice, "effect submit failed after its auxiliary work was queued (VkResult "
                        + std::to_string(vr) + ")");
                return vr;
            }

            maybeDumpDepthResolveImage(logicalDevice, logicalSwapchain, index, logicalDevice->queue, *frame.diag);

            VkSemaphore finalSemaphore;
            try
            {
                vr = submitOverlayFrame(logicalDevice, logicalSwapchain, index, finalSemaphore);
            }
            catch (const std::exception& e)
            {
                panicLayer(logicalDevice, std::string("Overlay submit threw: ") + e.what());
                return logicalDevice->vkd.QueuePresentKHR(queue, pPresentInfo);
            }
            catch (...)
            {
                panicLayer(logicalDevice, "Overlay submit threw unknown exception");
                return logicalDevice->vkd.QueuePresentKHR(queue, pPresentInfo);
            }
            if (vr != VK_SUCCESS)
            {
                reportDeviceLostDiagnostics(logicalDevice, logicalDevice->queue, "submitOverlayFrame", vr);
                if (vr == VK_ERROR_DEVICE_LOST)
                    panicLayer(logicalDevice, "Vulkan device lost during overlay submit");
                return vr;
            }

            presentSemaphores.push_back(finalSemaphore);
        }
        return std::nullopt;
    }

    // Step 6: the real present, and flagging swapchains for reset when the
    // surface asks for it.
    VkResult finishPresent(VkQueue queue, const VkPresentInfoKHR* pPresentInfo, LogicalDevice* logicalDevice)
    {
        VkPresentInfoKHR presentInfo   = *pPresentInfo;
        presentInfo.waitSemaphoreCount = tlsPresentSemaphores.size();
        presentInfo.pWaitSemaphores    = tlsPresentSemaphores.data();

        VkResult presentResult = forwardPresent(logicalDevice, queue, &presentInfo,
                                                "vkQueuePresentKHR(final)", "Vulkan device lost during final present");

        // OUT_OF_DATE / SUBOPTIMAL: flag the affected swapchains for reset. The
        // result is returned unchanged: swallowing OUT_OF_DATE hid the standard
        // recreate-now signal from the application, whose present path then
        // waited forever on frames the compositor would never take.
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
        {
            Logger::warn("QueuePresentKHR returned " + std::string(presentResult == VK_ERROR_OUT_OF_DATE_KHR ? "OUT_OF_DATE" : "SUBOPTIMAL") + ", flagging swapchains for reset");
            scoped_lock l(globalLock);
            for (unsigned int i = 0; i < pPresentInfo->swapchainCount; i++)
            {
                auto swapIt = swapchainMap.find(pPresentInfo->pSwapchains[i]);
                if (swapIt != swapchainMap.end() && swapIt->second)
                {
                    // Reset the source view so the next rebuild tears the old
                    // resolve images down and frees them. Nulling the image
                    // handles here instead made destroyDepthResolveResources skip
                    // them, leaking a set of full-res images per present.
                    swapIt->second->depthResolveSourceView = VK_NULL_HANDLE;
                }
            }
        }

        return presentResult;
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
    {
        uint64_t frameSerial = 0;
        if (auto done = presentSpecialCases(queue, pPresentInfo, frameSerial))
            return *done;
        if (auto done = presentUnsupportedSurface(queue, pPresentInfo))
            return *done;

        // Mark new input frame so dispatch deduplication resets
        beginInputFrame();

        PresentFrame frame;
        if (auto done = snapshotPresentFrame(queue, pPresentInfo, frame))
            return *done;
        LogicalDevice* logicalDevice = frame.device.get();

        if (frame.hasPassthrough)
        {
            // this swapchain was never faked, so submit no layer buffers for
            // it; the app's own present is the only safe one
            return forwardPresent(logicalDevice, queue, pPresentInfo,
                                  "vkQueuePresentKHR(passthrough swapchain)", "Vulkan device lost during passthrough present");
        }

        // Test hook: rebuilds the depth path while the app keeps presenting.
        maybeForceSmokeDepthRefresh(logicalDevice, *frame.diag);

        if (prepareDepthForPresent(logicalDevice, frame))
        {
            // Crucially, don't submit the old command buffer while depth is
            // invalid. This is what prevents a resize race from escalating
            // into VK_ERROR_DEVICE_LOST. The app's own present remains intact.
            return forwardPresent(logicalDevice, queue, pPresentInfo,
                                  "vkQueuePresentKHR(depth-disabled passthrough)",
                                  "Vulkan device lost during depth-disabled passthrough present");
        }

        if (auto done = submitFrame(queue, pPresentInfo, frame, frameSerial))
            return *done;

        return finishPresent(queue, pPresentInfo, logicalDevice);
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
