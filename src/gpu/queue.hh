#pragma once

// QueueState per VkQueue (formal/VkHookFormal/LockDiscipline.lean, AGENTS.md
// invariants 1 and 2).
//
// Every vkQueue* call, from the application's hooks and from the layer alike,
// goes through the device's QueueTable and holds that queue's mutex for the
// duration of the single driver call and nothing longer. The real queue entry
// points live only here (QueueDispatch); no other file can call them, so a
// call that skips the mutex does not compile.
//
// GPU waits never hold any lock. The layer's own waits are fence waits on a
// fence of a submission it already made (submitAndWait, drain); the app's
// vkQueueWaitIdle / vkDeviceWaitIdle are emulated the same way: an empty fenced
// submit under the queue mutex, then a wait with nothing held.

#include "gpu/timeline.hh"
#include "cow_slot.hh"
#include "vulkan_include.hh"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace VkHook
{
    // Everything gpu/queue.cc may call on the driver (or the next layer).
    struct QueueDispatch
    {
        PFN_vkQueueSubmit                    submit                = nullptr;
        PFN_vkQueueSubmit2                   submit2               = nullptr;
        PFN_vkQueueSubmit2KHR                submit2KHR            = nullptr;
        PFN_vkQueueBindSparse                bindSparse            = nullptr;
        PFN_vkQueuePresentKHR                present               = nullptr;
        PFN_vkQueueWaitIdle                  waitIdle              = nullptr;
        PFN_vkQueueBeginDebugUtilsLabelEXT   beginLabel            = nullptr;
        PFN_vkQueueEndDebugUtilsLabelEXT     endLabel              = nullptr;
        PFN_vkQueueInsertDebugUtilsLabelEXT  insertLabel           = nullptr;
        PFN_vkCreateFence                    createFence           = nullptr;
        PFN_vkDestroyFence                   destroyFence          = nullptr;
        PFN_vkResetFences                    resetFences           = nullptr;
        PFN_vkGetFenceStatus                 getFenceStatus        = nullptr;
        PFN_vkWaitForFences                  waitForFences         = nullptr;
    };

    void fillQueueDispatch(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, QueueDispatch* table);

    struct QueueState
    {
        VkQueue      queue  = VK_NULL_HANDLE;
        uint32_t     family = UINT32_MAX;  // UINT32_MAX until vkGetDeviceQueue* was seen
        uint32_t     index  = 0;
        TimelineLane lane   = 0;
        // the device's real vkQueuePresentKHR (not process-global: H4)
        PFN_vkQueuePresentKHR realPresent = nullptr;
        // Held only around one driver call on this queue.
        std::mutex mutex;
    };

    class QueueTable
    {
    public:
        // `timeline` must outlive the table (both are members of the device).
        void init(VkDevice device, const QueueDispatch& dispatch, Timeline& timeline);

        // Lock-free. Null when the queue was never registered.
        QueueState* find(VkQueue queue) const;
        // Registers (or refines the family of) a queue; idempotent.
        QueueState& add(VkQueue queue, uint32_t family, uint32_t index);
        // A hook saw a queue it was never told about (the app got it some other way).
        QueueState& findOrAdd(VkQueue queue);

        VkResult submit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence,
                        uint64_t* serial = nullptr);
        VkResult submit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence, bool khr,
                         uint64_t* serial = nullptr);
        VkResult bindSparse(VkQueue queue, uint32_t count, const VkBindSparseInfo* infos, VkFence fence);
        VkResult present(VkQueue queue, const VkPresentInfoKHR* info);
        void     beginLabel(VkQueue queue, const VkDebugUtilsLabelEXT* label);
        void     endLabel(VkQueue queue);
        void     insertLabel(VkQueue queue, const VkDebugUtilsLabelEXT* label);

        // Layer submit whose `fence` is registered as a timeline probe on success.
        VkResult submitProbed(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence,
                              uint64_t* serial = nullptr);

        // Layer submit that waits for completion on a pooled fence with no lock
        // held. VK_TIMEOUT leaves the fence pending in the timeline (recycled by
        // reclaim() once it signals) and the caller must retire what the
        // submission reads instead of destroying it.
        VkResult submitAndWait(VkQueue queue, const VkSubmitInfo& submit, uint32_t timeoutMs);

        // vkQueueWaitIdle / vkDeviceWaitIdle semantics without holding a lock
        // while waiting. A timeout of UINT32_MAX waits forever.
        VkResult waitIdle(VkQueue queue, uint32_t timeoutMs = UINT32_MAX);
        VkResult deviceWaitIdle(uint32_t timeoutMs = UINT32_MAX);

        // Waits (no lock held) until the device's completed serial reaches `serial`
        // by waiting on the probe fences that cover it. VK_NOT_READY: some work
        // up to `serial` has no probe, so completion cannot be proven; VK_TIMEOUT.
        VkResult waitSerial(uint64_t serial, uint32_t timeoutMs);

        // Polls the timeline's probes and recycles pooled fences that signalled.
        void reclaim();

        // After vkDeviceWaitIdle in vkDestroyDevice.
        void destroy();

        template <typename Fn>
        void forEach(Fn&& fn) const
        {
            if (auto snapshot = queues.load())
                for (const auto& state : *snapshot)
                    fn(*state);
        }

    private:
        using Vector = std::vector<std::shared_ptr<QueueState>>;

        QueueState& requireState(VkQueue queue);
        VkFence     acquireFence();
        void        releaseFence(VkFence fence);
        VkResult    emptyFencedSubmit(QueueState& state, VkFence fence);

        VkDevice        device = VK_NULL_HANDLE;
        QueueDispatch   fn;
        Timeline*       timeline = nullptr;
        CowSlot<Vector> queues;
        std::mutex      registerLock;  // registration only: not a hot-path lock

        std::mutex                  poolLock;
        std::vector<VkFence>        freeFences;
        std::unordered_set<uint64_t> pooled;  // handles owned by the pool
    };
} // namespace VkHook
