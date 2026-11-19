#include "present/present_mode.hh"

#include <algorithm>

namespace VkHook
{
    const char* presentModeName(PresentMode mode)
    {
        switch (mode)
        {
        case PresentMode::Passthrough: return "Passthrough";
        case PresentMode::CopyOnly:    return "CopyOnly";
        case PresentMode::Effects:     return "Effects";
        }
        return "?";
    }

    PresentMode choosePresentMode(const ModeInputs& inputs)
    {
        if (!inputs.faked)
            return PresentMode::Passthrough;
        const bool bypass = !inputs.effectsOn || inputs.softDisabled;
        return bypass ? PresentMode::CopyOnly : PresentMode::Effects;
    }

    WaitAccounting accountPresentWaits(const PresentMode* modes, uint32_t swapchainCount, uint32_t appWaitCount)
    {
        WaitAccounting accounting;
        bool anyLayer       = false;
        bool anyPassthrough = false;
        for (uint32_t i = 0; i < swapchainCount; ++i)
        {
            if (modes[i] == PresentMode::Passthrough)
                anyPassthrough = true;
            else
                anyLayer = true;
        }

        accounting.forwardUnchanged = !anyLayer;
        accounting.layerWaitCount   = anyLayer ? appWaitCount : 0;
        accounting.needsRelay       = anyLayer && anyPassthrough;
        return accounting;
    }

    namespace
    {
        // scratch reused across presents on one thread; no state between presents
        thread_local std::vector<VkPipelineStageFlags> tlsWaitStages;

        constexpr VkPipelineStageFlags kOverlayWaitStage =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    } // namespace

    PresentSubmitResult submitPresentBatches(QueueTable& queues, VkQueue queue,
                                             const VkSemaphore* appWaits, uint32_t appWaitCount,
                                             PresentTarget* targets, uint32_t targetCount,
                                             std::vector<VkSemaphore>& presentWaits)
    {
        PresentSubmitResult out;
        presentWaits.assign(targetCount, VK_NULL_HANDLE);

        std::vector<PresentMode> modes(targetCount);
        for (uint32_t i = 0; i < targetCount; ++i)
            modes[i] = targets[i].mode;
        const WaitAccounting accounting = accountPresentWaits(modes.data(), targetCount, appWaitCount);

        tlsWaitStages.assign(accounting.layerWaitCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        bool waitsConsumed = false;

        // Faked targets first, in order. Queue submission order makes the first
        // batch's wait cover every later batch of this queue, so the app's waits
        // are consumed once, by the first layer batch.
        for (uint32_t i = 0; i < targetCount; ++i)
        {
            PresentTarget& target = targets[i];
            if (target.mode == PresentMode::Passthrough)
                continue;

            VkSubmitInfo submit = {};
            submit.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.waitSemaphoreCount   = waitsConsumed ? 0 : accounting.layerWaitCount;
            submit.pWaitSemaphores      = waitsConsumed ? nullptr : appWaits;
            submit.pWaitDstStageMask    = waitsConsumed ? nullptr : tlsWaitStages.data();
            const VkCommandBuffer buffers[2] = {target.preCommandBuffer, target.commandBuffer};
            const bool            hasPre     = target.preCommandBuffer != VK_NULL_HANDLE;
            submit.commandBufferCount   = hasPre ? 2 : 1;
            submit.pCommandBuffers      = hasPre ? buffers : &target.commandBuffer;
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores    = &target.signalSemaphore;

            const VkSemaphore* applicationWaits = submit.pWaitSemaphores;
            if (target.prepare)
            {
                const VkResult prepared = target.prepare(target.context, i, submit);
                // auxiliary submissions may already have consumed the app's waits
                out.auxiliaryQueued = submit.pWaitSemaphores != applicationWaits;
                if (prepared != VK_SUCCESS)
                {
                    out.result       = prepared;
                    out.failedTarget = i;
                    return out;
                }
            }

            const VkResult result = queues.submitProbed(queue, 1, &submit, target.fence, &target.serial);
            if (result != VK_SUCCESS)
            {
                out.result       = result;
                out.failedTarget = i;
                return out;
            }
            waitsConsumed = true;

            VkSemaphore final = target.signalSemaphore;
            if (target.overlayCommandBuffer != VK_NULL_HANDLE)
            {
                VkSubmitInfo overlay = {};
                overlay.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                overlay.waitSemaphoreCount   = 1;
                overlay.pWaitSemaphores      = &target.signalSemaphore;
                overlay.pWaitDstStageMask    = &kOverlayWaitStage;
                overlay.commandBufferCount   = 1;
                overlay.pCommandBuffers      = &target.overlayCommandBuffer;
                overlay.signalSemaphoreCount = 1;
                overlay.pSignalSemaphores    = &target.overlaySemaphore;

                // once per submit, right before the submit (it resets the fence)
                const VkFence fence = target.overlayFence ? target.overlayFence(target.context, i) : VK_NULL_HANDLE;
                uint64_t overlaySerial = 0;
                const VkResult overlayResult = queues.submit(queue, 1, &overlay, fence, &overlaySerial);
                if (overlayResult != VK_SUCCESS)
                {
                    out.result       = overlayResult;
                    out.failedTarget = i;
                    return out;
                }
                target.serial = std::max(target.serial, overlaySerial);
                final         = target.overlaySemaphore;
            }
            presentWaits[i] = final;
        }

        // Passthrough targets of a mixed present: their present wait is a
        // semaphore an (empty) layer batch signals, ordered after the batch that
        // consumed the app's waits.
        for (uint32_t i = 0; i < targetCount; ++i)
        {
            if (targets[i].mode != PresentMode::Passthrough || !accounting.needsRelay)
                continue;

            VkSubmitInfo relay = {};
            relay.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            relay.signalSemaphoreCount = 1;
            relay.pSignalSemaphores    = &targets[i].relaySemaphore;
            const VkResult result = queues.submit(queue, 1, &relay, VK_NULL_HANDLE, &targets[i].serial);
            if (result != VK_SUCCESS)
            {
                out.result       = result;
                out.failedTarget = i;
                return out;
            }
            presentWaits[i] = targets[i].relaySemaphore;
        }
        return out;
    }
} // namespace VkHook
