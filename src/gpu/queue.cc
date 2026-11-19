#include "gpu/queue.hh"

#include "gpu/retire.hh"
#include "logger.hh"

#include <algorithm>
#include <chrono>
#include <string>

namespace VkHook
{
    void fillQueueDispatch(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, QueueDispatch* table)
    {
#define LOAD(field, name) table->field = reinterpret_cast<decltype(table->field)>(gdpa(device, "vk" name))
        LOAD(submit, "QueueSubmit");
        LOAD(submit2, "QueueSubmit2");
        LOAD(submit2KHR, "QueueSubmit2KHR");
        LOAD(bindSparse, "QueueBindSparse");
        LOAD(present, "QueuePresentKHR");
        LOAD(waitIdle, "QueueWaitIdle");
        LOAD(beginLabel, "QueueBeginDebugUtilsLabelEXT");
        LOAD(endLabel, "QueueEndDebugUtilsLabelEXT");
        LOAD(insertLabel, "QueueInsertDebugUtilsLabelEXT");
        LOAD(createFence, "CreateFence");
        LOAD(destroyFence, "DestroyFence");
        LOAD(resetFences, "ResetFences");
        LOAD(getFenceStatus, "GetFenceStatus");
        LOAD(waitForFences, "WaitForFences");
#undef LOAD
    }

    void QueueTable::init(VkDevice dev, const QueueDispatch& dispatch, Timeline& timelineRef)
    {
        device   = dev;
        fn       = dispatch;
        timeline = &timelineRef;
    }

    QueueState* QueueTable::find(VkQueue queue) const
    {
        auto snapshot = queues.load();
        if (!snapshot)
            return nullptr;
        for (const auto& state : *snapshot)
            if (state->queue == queue)
                return state.get();
        return nullptr;
    }

    QueueState& QueueTable::add(VkQueue queue, uint32_t family, uint32_t index)
    {
        std::lock_guard<std::mutex> l(registerLock);
        if (QueueState* existing = find(queue))
        {
            // vkGetDeviceQueue* can only refine what a hook first guessed
            if (existing->family == UINT32_MAX)
            {
                existing->family = family;
                existing->index  = index;
            }
            return *existing;
        }
        auto state          = std::make_shared<QueueState>();
        state->queue        = queue;
        state->family       = family;
        state->index        = index;
        state->lane         = timeline->addLane();
        state->realPresent  = fn.present;
        auto snapshot       = queues.load();
        auto next           = std::make_shared<Vector>(snapshot ? *snapshot : Vector{});
        next->push_back(state);
        queues.store(std::move(next));
        return *state;
    }

    QueueState& QueueTable::findOrAdd(VkQueue queue)
    {
        if (QueueState* state = find(queue))
            return *state;
        return add(queue, UINT32_MAX, 0);
    }

    QueueState& QueueTable::requireState(VkQueue queue)
    {
        return findOrAdd(queue);
    }

    VkResult QueueTable::submit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence, uint64_t* serial)
    {
        QueueState& state = requireState(queue);
        std::lock_guard<std::mutex> l(state.mutex);
        const uint64_t s = timeline->nextSerial(state.lane);
        if (serial)
            *serial = s;
        return fn.submit(queue, count, submits, fence);
    }

    VkResult QueueTable::submit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* submits, VkFence fence, bool khr,
                                 uint64_t* serial)
    {
        QueueState& state = requireState(queue);
        std::lock_guard<std::mutex> l(state.mutex);
        const uint64_t s = timeline->nextSerial(state.lane);
        if (serial)
            *serial = s;
        return khr ? fn.submit2KHR(queue, count, submits, fence) : fn.submit2(queue, count, submits, fence);
    }

    VkResult QueueTable::bindSparse(VkQueue queue, uint32_t count, const VkBindSparseInfo* infos, VkFence fence)
    {
        QueueState& state = requireState(queue);
        std::lock_guard<std::mutex> l(state.mutex);
        timeline->nextSerial(state.lane);
        return fn.bindSparse(queue, count, infos, fence);
    }

    VkResult QueueTable::present(VkQueue queue, const VkPresentInfoKHR* info)
    {
        QueueState& state = requireState(queue);
        std::lock_guard<std::mutex> l(state.mutex);
        return state.realPresent(queue, info);
    }

    void QueueTable::beginLabel(VkQueue queue, const VkDebugUtilsLabelEXT* label)
    {
        QueueState& state = requireState(queue);
        std::lock_guard<std::mutex> l(state.mutex);
        fn.beginLabel(queue, label);
    }

    void QueueTable::endLabel(VkQueue queue)
    {
        QueueState& state = requireState(queue);
        std::lock_guard<std::mutex> l(state.mutex);
        fn.endLabel(queue);
    }

    void QueueTable::insertLabel(VkQueue queue, const VkDebugUtilsLabelEXT* label)
    {
        QueueState& state = requireState(queue);
        std::lock_guard<std::mutex> l(state.mutex);
        fn.insertLabel(queue, label);
    }

    VkResult QueueTable::submitProbed(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence,
                                      uint64_t* serial)
    {
        uint64_t s = 0;
        const VkResult result = submit(queue, count, submits, fence, &s);
        if (serial)
            *serial = s;
        if (result == VK_SUCCESS && fence != VK_NULL_HANDLE)
            timeline->addProbe(requireState(queue).lane, s, fence);
        return result;
    }

    VkFence QueueTable::acquireFence()
    {
        {
            std::lock_guard<std::mutex> l(poolLock);
            if (!freeFences.empty())
            {
                VkFence fence = freeFences.back();
                freeFences.pop_back();
                return fence;
            }
        }
        VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence           fence = VK_NULL_HANDLE;
        if (fn.createFence(device, &info, nullptr, &fence) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        std::lock_guard<std::mutex> l(poolLock);
        pooled.insert(handleToU64(fence));
        return fence;
    }

    void QueueTable::releaseFence(VkFence fence)
    {
        // the caller knows the fence is signalled or was never submitted
        fn.resetFences(device, 1, &fence);
        std::lock_guard<std::mutex> l(poolLock);
        freeFences.push_back(fence);
    }

    void QueueTable::reclaim()
    {
        struct Ctx
        {
            QueueTable* self;
        } ctx{this};
        FenceStatusFn status;
        status.ctx = &ctx;
        status.fn  = [](void* c, VkFence fence) -> VkResult {
            auto* self = static_cast<Ctx*>(c)->self;
            return self->fn.getFenceStatus(self->device, fence);
        };
        std::vector<VkFence> signalled;
        timeline->pollProbes(status, &signalled);
        for (VkFence fence : signalled)
        {
            bool owned;
            {
                std::lock_guard<std::mutex> l(poolLock);
                owned = pooled.count(handleToU64(fence)) != 0;
            }
            if (owned)
                releaseFence(fence);
        }
    }

    VkResult QueueTable::waitSerial(uint64_t serial, uint32_t timeoutMs)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;)
        {
            reclaim();
            if (timeline->completedSerial() >= serial)
                return VK_SUCCESS;
            const VkFence fence = timeline->probeCovering(serial);
            if (fence == VK_NULL_HANDLE)
                return VK_NOT_READY;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return VK_TIMEOUT;
            const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
            const VkResult result = fn.waitForFences(device, 1, &fence, VK_TRUE, static_cast<uint64_t>(remaining));
            if (result != VK_SUCCESS)
                return result;
        }
    }

    VkResult QueueTable::submitAndWait(VkQueue queue, const VkSubmitInfo& submitInfo, uint32_t timeoutMs)
    {
        VkFence fence = acquireFence();
        if (fence == VK_NULL_HANDLE)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        VkResult result = submitProbed(queue, 1, &submitInfo, fence);
        if (result != VK_SUCCESS)
        {
            releaseFence(fence);
            return result;
        }
        const uint64_t timeoutNs = timeoutMs == UINT32_MAX ? UINT64_MAX : static_cast<uint64_t>(timeoutMs) * 1000000ull;
        // no lock is held here
        result = fn.waitForFences(device, 1, &fence, VK_TRUE, timeoutNs);
        if (result == VK_SUCCESS)
            reclaim();
        return result;
    }

    VkResult QueueTable::emptyFencedSubmit(QueueState& state, VkFence fence)
    {
        VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        uint64_t     serial = 0;
        VkResult     result;
        {
            std::lock_guard<std::mutex> l(state.mutex);
            serial = timeline->nextSerial(state.lane);
            result = fn.submit(state.queue, 1, &empty, fence);
        }
        if (result == VK_SUCCESS)
            timeline->addProbe(state.lane, serial, fence);
        return result;
    }

    VkResult QueueTable::waitIdle(VkQueue queue, uint32_t timeoutMs)
    {
        QueueState& state = requireState(queue);
        VkFence     fence = acquireFence();
        if (fence == VK_NULL_HANDLE)
        {
            // no fence to wait on: the real call, which must hold the mutex
            std::lock_guard<std::mutex> l(state.mutex);
            return fn.waitIdle(queue);
        }
        // An empty fenced submit completes after everything submitted before it
        // on this queue; waiting for it needs no lock, unlike vkQueueWaitIdle.
        VkResult result = emptyFencedSubmit(state, fence);
        if (result != VK_SUCCESS)
        {
            releaseFence(fence);
            return result;
        }
        const uint64_t timeoutNs = timeoutMs == UINT32_MAX ? UINT64_MAX : static_cast<uint64_t>(timeoutMs) * 1000000ull;
        result = fn.waitForFences(device, 1, &fence, VK_TRUE, timeoutNs);
        if (result == VK_SUCCESS)
            reclaim();
        return result;
    }

    VkResult QueueTable::deviceWaitIdle(uint32_t timeoutMs)
    {
        VkResult result = VK_SUCCESS;
        forEach([&](QueueState& state) {
            if (result != VK_SUCCESS)
                return;
            result = waitIdle(state.queue, timeoutMs);
        });
        return result;
    }

    void QueueTable::destroy()
    {
        // the device is idle: every pooled fence is signalled or unused
        timeline->markIdle();
        std::vector<uint64_t> handles;
        {
            std::lock_guard<std::mutex> l(poolLock);
            handles.assign(pooled.begin(), pooled.end());
            pooled.clear();
            freeFences.clear();
        }
        for (uint64_t handle : handles)
            fn.destroyFence(device, u64ToHandle<VkFence>(handle), nullptr);
    }
} // namespace VkHook
