#pragma once

// Present modes and semaphore accounting (formal/VkHookFormal/PresentModes.lean,
// AGENTS.md invariant 5).
//
//   mode = faked ? (bypass ? CopyOnly : Effects) : Passthrough
//
// A bypass (effects off, soft-disabled) never selects Passthrough on a faked swapchain: the game rendered
// into the fake images, so something must copy them into the real image the
// present shows. CopyOnly is that something.
//
// Every present consumes each application wait semaphore exactly once: either
// the app's own present does (every swapchain Passthrough: forwarded
// unchanged), or the first layer submission does and the real present waits on
// layer-signalled semaphores only.

#include "gpu/queue.hh"
#include "vulkan_include.hh"

#include <cstdint>
#include <vector>

namespace VkHook
{
    enum class PresentMode
    {
        Passthrough,  // forward the app's present unchanged
        CopyOnly,     // layer submission: fake -> real copy, no effects, no depth
        Effects,      // layer submission: the effect chain
    };

    const char* presentModeName(PresentMode mode);

    struct ModeInputs
    {
        bool faked             = false;  // the layer replaced the swapchain's images
        bool effectsOn         = true;
        bool softDisabled      = false;
    };

    PresentMode choosePresentMode(const ModeInputs& inputs);

    struct WaitAccounting
    {
        // every swapchain is Passthrough: forward the app's present as is, it
        // consumes the app's waits itself and the layer submits nothing
        bool forwardUnchanged = true;
        // how many app waits the first layer submission consumes
        uint32_t layerWaitCount = 0;
        // some swapchain is Passthrough while another one is not: its present
        // wait must be a semaphore a layer submission signals, since the app's
        // waits were consumed by the layer
        bool needsRelay = false;
    };

    WaitAccounting accountPresentWaits(const PresentMode* modes, uint32_t swapchainCount, uint32_t appWaitCount);

    // One swapchain of a present, for submitPresentBatches.
    struct PresentTarget
    {
        PresentMode     mode = PresentMode::Passthrough;
        // faked: the command buffer to run and the semaphore it signals
        // runs first in the same submission when set (the depth capture)
        VkCommandBuffer preCommandBuffer = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer   = VK_NULL_HANDLE;
        VkSemaphore     signalSemaphore = VK_NULL_HANDLE;
        VkFence         fence           = VK_NULL_HANDLE;  // layer submission probe, or null
        // Passthrough target of a mixed present: signalled by an empty submission
        VkSemaphore     relaySemaphore  = VK_NULL_HANDLE;
        // optional overlay frame that follows the layer submission
        VkCommandBuffer overlayCommandBuffer = VK_NULL_HANDLE;
        VkSemaphore     overlaySemaphore     = VK_NULL_HANDLE;
        // Called once, right before the overlay's vkQueueSubmit, only when
        // overlayCommandBuffer is set. Returns the fence to submit with.
        VkFence (*overlayFence)(void* context, uint32_t target) = nullptr;
        // Called after the app's waits are in the layer submission, before it is
        // submitted (effects add their auxiliary waits here).
        VkResult (*prepare)(void* context, uint32_t target, VkSubmitInfo& submit) = nullptr;
        void* context = nullptr;
        // out: the serial of the layer submission (0 when none)
        uint64_t serial = 0;
    };

    struct PresentSubmitResult
    {
        VkResult result = VK_SUCCESS;
        // target whose preparation or submit failed
        uint32_t failedTarget = 0;
        // a failed `prepare` left auxiliary submissions queued
        bool auxiliaryQueued = false;
    };

    // Submits the layer work of one present on `queue`, in order: for each
    // faked target its layer batch (the first consuming all `appWaits`), its
    // overlay batch; then the relay batches of Passthrough targets. Fills
    // `presentWaits` with what the real present waits on, one entry per target
    // in order. Holds no lock of its own; every driver call goes through
    // `queues` (the queue's mutex around the single call).
    PresentSubmitResult submitPresentBatches(QueueTable& queues, VkQueue queue,
                                             const VkSemaphore* appWaits, uint32_t appWaitCount,
                                             PresentTarget* targets, uint32_t targetCount,
                                             std::vector<VkSemaphore>& presentWaits);
} // namespace VkHook
