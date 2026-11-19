#include "wayland_syncobj_gate.hh"

#include "logger.hh"

#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

namespace VkHook::WaylandSyncobjGate
{
    // One per surface. `gate` is the shared/exclusive lock; `owner` and
    // `holdsGate` are guarded by the registry mutex.
    struct CommitScope::Surface
    {
        std::shared_timed_mutex gate;
        std::thread::id owner;     // the presenting thread while a bracket is open
        bool open = false;
        bool holdsGate = false;    // the bracket actually took `gate` (false after a budget timeout)
    };

    namespace
    {
        using Surface = CommitScope::Surface;

        std::mutex registryMutex;
        std::unordered_map<uint32_t, std::shared_ptr<Surface>> surfaces;

        std::atomic<int> openCount{0};
        std::atomic<uint64_t> held{0};
        std::atomic<uint64_t> budgetMicros{100'000};

        std::chrono::microseconds budget()
        {
            return std::chrono::microseconds(budgetMicros.load(std::memory_order_relaxed));
        }

        std::shared_ptr<Surface> surfaceFor(uint32_t surfaceId)
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            auto& slot = surfaces[surfaceId];
            if (!slot)
                slot = std::make_shared<Surface>();
            return slot;
        }
    } // namespace

    void beginPresent(uint32_t surfaceId)
    {
        auto surface = surfaceFor(surfaceId);
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            if (surface->open && surface->owner == std::this_thread::get_id())
                return;  // nested bracket on the same thread
        }

        // waits for commits already being sent, and for another presenter
        const bool locked = surface->gate.try_lock_for(budget());
        if (!locked)
            Logger::warn("Wayland: a window commit or present on this surface outlasted the wait budget, presenting anyway");

        std::lock_guard<std::mutex> lock(registryMutex);
        surface->owner = std::this_thread::get_id();
        surface->open = true;
        surface->holdsGate = locked;
        openCount.fetch_add(1, std::memory_order_release);
    }

    void endPresent(uint32_t surfaceId)
    {
        auto surface = surfaceFor(surfaceId);
        bool unlock = false;
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            if (!surface->open || surface->owner != std::this_thread::get_id())
                return;
            unlock = surface->holdsGate;
            surface->open = false;
            surface->holdsGate = false;
            surface->owner = {};
            openCount.fetch_sub(1, std::memory_order_release);
        }
        if (unlock)
            surface->gate.unlock();
    }

    bool anyPresentOpen()
    {
        return openCount.load(std::memory_order_acquire) > 0;
    }

    CommitScope::CommitScope(uint32_t surfaceId)
    {
        m_surface = surfaceFor(surfaceId);
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            if (m_surface->open && m_surface->owner == std::this_thread::get_id())
                return;  // the presenter's own commit, inside its bracket
        }

        if (m_surface->gate.try_lock_shared())
        {
            m_held = true;
            return;
        }

        // behind an open present: wait it out, bounded
        if (m_surface->gate.try_lock_shared_for(budget()))
        {
            m_held = true;
            held.fetch_add(1, std::memory_order_relaxed);
            Logger::info("Wayland: held a window commit behind an in-flight present");
        }
        else
        {
            held.fetch_add(1, std::memory_order_relaxed);
            Logger::warn("Wayland: present still in flight after the wait budget, letting a commit through");
        }
    }

    CommitScope::~CommitScope()
    {
        if (m_held)
            m_surface->gate.unlock_shared();
    }

    uint64_t heldCount()
    {
        return held.load(std::memory_order_relaxed);
    }

    void setWaitBudgetForTests(uint64_t microseconds)
    {
        budgetMicros.store(microseconds, std::memory_order_relaxed);
    }

    void resetForTests()
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        // a test that leaked an open bracket must not poison the next one
        surfaces.clear();
        openCount.store(0, std::memory_order_relaxed);
        held.store(0, std::memory_order_relaxed);
        budgetMicros.store(100'000, std::memory_order_relaxed);
    }
} // namespace VkHook::WaylandSyncobjGate
