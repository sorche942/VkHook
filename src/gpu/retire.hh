#pragma once

// Deferred destruction of layer-owned Vulkan objects, keyed on the device's GPU
// serial (formal/VkHookFormal/RetireQueue.lean, AGENTS.md invariant 3).
//
//   retire(type, handle)   tag with the device's current *submitted* serial
//   collect(completed)     destroy entries whose tag <= completed (run at present)
//   drainAll()             destroy everything; only after vkDeviceWaitIdle in
//                          vkDestroyDevice
//
// Nothing else destroys layer objects the GPU may still read. There is no
// process-global queue and no "flush when it looks safe".
//
// Entries are plain records; the queue never copies dispatch tables. The
// device binds one destroy function at creation that switches on the object
// type. Entries of one batch are destroyed dependents-first (see rank()).

#include "gpu/timeline.hh"
#include "vulkan_include.hh"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <vector>

namespace VkHook
{
    struct RetiredObject
    {
        uint64_t     serial = 0;  // submitted serial at retire() time
        uint64_t     seq    = 0;  // retire order, newest has the highest
        VkObjectType type   = VK_OBJECT_TYPE_UNKNOWN;
        uint64_t     handle = 0;
        uint64_t     extra  = 0;  // parent: VkCommandPool for COMMAND_BUFFER, VkDescriptorPool for DESCRIPTOR_SET
    };

    // Lower rank is destroyed first within one batch (a framebuffer before its
    // render pass, a view before its image, memory last). Ties: newest first.
    int retireRank(VkObjectType type);

    // Raw handle <-> u64, for both the pointer (x64) and integer (i686) ABIs.
    template <typename Handle>
    uint64_t handleToU64(Handle handle)
    {
        if constexpr (std::is_pointer_v<Handle>)
            return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
        else
            return static_cast<uint64_t>(handle);
    }

    template <typename Handle>
    Handle u64ToHandle(uint64_t value)
    {
        if constexpr (std::is_pointer_v<Handle>)
            return reinterpret_cast<Handle>(static_cast<uintptr_t>(value));
        else
            return static_cast<Handle>(value);
    }

    class RetireQueue
    {
    public:
        // Destroys one object. Called with no queue lock held, outside the
        // queue's own lock; it may call into the driver but never waits on the GPU.
        using DestroyFn = void (*)(void* ctx, const RetiredObject& object);

        explicit RetireQueue(const Timeline& timeline) : timeline(&timeline) {}

        void bind(DestroyFn fn, void* ctx);

        // Tags with timeline.submittedSerial() and queues. A null handle is ignored.
        void retire(VkObjectType type, uint64_t handle, uint64_t extra = 0);

        // Destroys every entry with serial <= completedSerial; returns how many.
        size_t collect(uint64_t completedSerial);

        // Destroys everything regardless of serial. The caller has waited for
        // the device to go idle.
        size_t drainAll();

        size_t   pending() const;
        uint64_t oldestPendingSerial() const;  // 0 when empty

    private:
        size_t destroyBatch(std::vector<RetiredObject>& batch);

        const Timeline*            timeline;
        mutable std::mutex         lock;
        std::vector<RetiredObject> entries;
        uint64_t                   nextSeq = 0;
        DestroyFn                  destroyFn  = nullptr;
        void*                      destroyCtx = nullptr;
    };
} // namespace VkHook
