#pragma once

// Per-device GPU serials (formal/VkHookFormal/RetireQueue.lean).
//
// Every vkQueue* call on the device draws a serial from the timeline while it
// holds that queue's mutex (so serial order equals submission order on a
// queue). The *completed* serial is fence-backed: a layer submission that
// carries a fence registers it as a probe; a signalled probe proves that every
// submission on the same queue up to its serial finished (a vkQueueSubmit fence
// signal covers all earlier submissions on that queue). No timeline semaphore is
// added to the application's device.
//
// The device-wide completed serial is the minimum over the lanes that have
// outstanding work and are probed. A lane (one per VkQueue) is probed when the
// layer submits a fenced batch on it, i.e. a queue the layer presents or
// uploads on. Queues the layer never fences cannot hold layer-owned objects
// and do not hold the serial back.
//
// This class makes no Vulkan call: fence status is read through FenceStatusFn,
// so it is unit-testable without a device. Methods take a short internal lock
// and never wait.

#include "vulkan_include.hh"

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <type_traits>
#include <vector>

namespace VkHook
{
    using TimelineLane = uint32_t;

    // returns VK_SUCCESS when the fence is signalled, VK_NOT_READY when pending
    struct FenceStatusFn
    {
        VkResult (*fn)(void* ctx, VkFence fence) = nullptr;
        void* ctx                                = nullptr;
    };

    class Timeline
    {
    public:
        // One lane per VkQueue, created when the queue is registered.
        TimelineLane addLane();

        // Next submission serial on `lane`. The caller holds the queue's mutex
        // and issues the real vkQueue* call right after.
        uint64_t nextSerial(TimelineLane lane);

        uint64_t submittedSerial() const { return submitted.load(std::memory_order_acquire); }
        uint64_t completedSerial() const;

        // The batch with `serial` on `lane` carries `fence`. A fence handle that
        // already has a probe is being reused after it signalled: the earlier
        // probe is completed first (callers reset a fence only once signalled).
        void addProbe(TimelineLane lane, uint64_t serial, VkFence fence);

        // Marks every signalled probe complete. Signalled fences are appended to
        // `signalled` so the owner can recycle them.
        void pollProbes(const FenceStatusFn& status, std::vector<VkFence>* signalled = nullptr);

        // The fence that proves `serial` complete on some lane, or VK_NULL_HANDLE
        // when no probe covers it yet. The caller waits on it with no lock held.
        VkFence probeCovering(uint64_t serial) const;

        // vkDeviceWaitIdle returned: everything submitted is complete.
        void markIdle();

        size_t probeCount() const;

    private:
        struct Lane
        {
            uint64_t last      = 0;  // newest serial issued on the lane
            uint64_t watermark = 0;  // newest serial proven complete
            bool     probed    = false;
        };
        struct Probe
        {
            TimelineLane lane;
            uint64_t     serial;
            VkFence      fence;
        };

        void     completeLocked(const Probe& probe);
        uint64_t computeLocked() const;

        std::atomic<uint64_t> submitted{0};
        mutable std::mutex    lock;
        std::vector<Lane>     lanes;
        std::vector<Probe>    probes;
        // Monotone lower bound of completedSerial(); raised by polls, completedSerial()
        // and markIdle. With no probed lane only markIdle advances it.
        mutable uint64_t      completedFloor = 0;
    };
} // namespace VkHook
