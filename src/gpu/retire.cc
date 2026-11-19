#include "gpu/retire.hh"

#include <algorithm>
#include <utility>

namespace VkHook
{
    int retireRank(VkObjectType type)
    {
        switch (type)
        {
        case VK_OBJECT_TYPE_FENCE:
        case VK_OBJECT_TYPE_SEMAPHORE:
        case VK_OBJECT_TYPE_EVENT:
            return 0;
        case VK_OBJECT_TYPE_COMMAND_BUFFER:
            return 1;
        case VK_OBJECT_TYPE_COMMAND_POOL:
            return 2;
        case VK_OBJECT_TYPE_PIPELINE:
            return 3;
        case VK_OBJECT_TYPE_FRAMEBUFFER:
            return 4;
        case VK_OBJECT_TYPE_RENDER_PASS:
            return 5;
        case VK_OBJECT_TYPE_DESCRIPTOR_SET:
        case VK_OBJECT_TYPE_DESCRIPTOR_POOL:
            return 6;
        case VK_OBJECT_TYPE_PIPELINE_LAYOUT:
            return 7;
        case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT:
            return 8;
        case VK_OBJECT_TYPE_IMAGE_VIEW:
        case VK_OBJECT_TYPE_SAMPLER:
        case VK_OBJECT_TYPE_SHADER_MODULE:
        case VK_OBJECT_TYPE_BUFFER_VIEW:
            return 9;
        case VK_OBJECT_TYPE_BUFFER:
        case VK_OBJECT_TYPE_IMAGE:
            return 10;
        case VK_OBJECT_TYPE_PIPELINE_CACHE:
            return 11;
        case VK_OBJECT_TYPE_DEVICE_MEMORY:
            return 12;
        default:
            return 10;
        }
    }

    void RetireQueue::bind(DestroyFn fn, void* ctx)
    {
        std::lock_guard guard(lock);
        destroyFn  = fn;
        destroyCtx = ctx;
    }

    void RetireQueue::retire(VkObjectType type, uint64_t handle, uint64_t extra)
    {
        if (handle == 0)
            return;
        std::lock_guard guard(lock);
        entries.push_back({timeline->submittedSerial(), nextSeq++, type, handle, extra});
    }

    size_t RetireQueue::destroyBatch(std::vector<RetiredObject>& batch)
    {
        std::sort(batch.begin(), batch.end(), [](const RetiredObject& a, const RetiredObject& b) {
            const int ra = retireRank(a.type);
            const int rb = retireRank(b.type);
            return ra != rb ? ra < rb : a.seq > b.seq;
        });

        DestroyFn fn;
        void*     ctx;
        {
            std::lock_guard guard(lock);
            fn  = destroyFn;
            ctx = destroyCtx;
        }
        if (fn)
        {
            for (const RetiredObject& object : batch)
                fn(ctx, object);
        }
        return batch.size();
    }

    size_t RetireQueue::collect(uint64_t completedSerial)
    {
        std::vector<RetiredObject> batch;
        {
            std::lock_guard guard(lock);
            auto split = std::stable_partition(entries.begin(), entries.end(),
                [&](const RetiredObject& entry) { return entry.serial > completedSerial; });
            batch.assign(split, entries.end());
            entries.erase(split, entries.end());
        }
        return destroyBatch(batch);
    }

    size_t RetireQueue::drainAll()
    {
        std::vector<RetiredObject> batch;
        {
            std::lock_guard guard(lock);
            batch.swap(entries);
        }
        return destroyBatch(batch);
    }

    size_t RetireQueue::pending() const
    {
        std::lock_guard guard(lock);
        return entries.size();
    }

    uint64_t RetireQueue::oldestPendingSerial() const
    {
        std::lock_guard guard(lock);
        uint64_t oldest = 0;
        for (const RetiredObject& entry : entries)
            oldest = oldest == 0 ? entry.serial : std::min(oldest, entry.serial);
        return oldest;
    }
} // namespace VkHook
