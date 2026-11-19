#include "wayland_syncobj_gate.hh"

#include "logger.hh"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

// The state machine of formal/VkHookFormal/SyncobjGate.lean. There is no timeout
// escape: letting a commit through behind an open present is itself the protocol
// error the gate exists to prevent (the model reaches it from the old 100 ms budget).
// Waiters are never held across anything but the other side's short critical section:
// a commit send, or the driver's present call.
namespace VkHook::WaylandSyncobjGate
{
    // One per surface. Everything below is guarded by `m`.
    //
    // A presenter waits for the open bracket (another presenter) and for every commit
    // already mid-send to finish; a commit waits for an open bracket. A waiting
    // presenter holds new commits back (`writersWaiting`), so a steady stream of
    // commits cannot starve it.
    struct CommitScope::Surface
    {
        std::mutex m;
        std::condition_variable changed;
        int readers = 0;           // commits mid-send
        int writersWaiting = 0;    // presenters waiting to open a bracket
        bool open = false;         // a present bracket is open
        std::thread::id owner;     // the presenting thread while `open`
    };

    namespace
    {
        using Surface = CommitScope::Surface;

        std::mutex registryMutex;
        std::unordered_map<uint32_t, std::shared_ptr<Surface>> surfaces;

        std::atomic<int> openCount{0};
        std::atomic<uint64_t> held{0};

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
        std::unique_lock<std::mutex> lock(surface->m);
        if (surface->open && surface->owner == std::this_thread::get_id())
            return;  // nested bracket on the same thread

        // waits for commits already being sent, and for another presenter
        ++surface->writersWaiting;
        surface->changed.wait(lock, [&] { return !surface->open && surface->readers == 0; });
        --surface->writersWaiting;
        surface->owner = std::this_thread::get_id();
        surface->open = true;
        openCount.fetch_add(1, std::memory_order_release);
    }

    void endPresent(uint32_t surfaceId)
    {
        auto surface = surfaceFor(surfaceId);
        {
            std::lock_guard<std::mutex> lock(surface->m);
            if (!surface->open || surface->owner != std::this_thread::get_id())
                return;
            surface->open = false;
            surface->owner = {};
            openCount.fetch_sub(1, std::memory_order_release);
        }
        surface->changed.notify_all();
    }

    bool anyPresentOpen()
    {
        return openCount.load(std::memory_order_acquire) > 0;
    }

    CommitScope::CommitScope(uint32_t surfaceId)
    {
        m_surface = surfaceFor(surfaceId);
        std::unique_lock<std::mutex> lock(m_surface->m);
        if (m_surface->open && m_surface->owner == std::this_thread::get_id())
            return;  // the presenter's own commit, inside its bracket

        const bool wait = m_surface->open || m_surface->writersWaiting > 0;
        if (wait)
            m_surface->changed.wait(lock, [&] { return !m_surface->open && m_surface->writersWaiting == 0; });
        ++m_surface->readers;
        m_held = true;
        lock.unlock();
        if (wait)
        {
            held.fetch_add(1, std::memory_order_relaxed);
            Logger::info("Wayland: held a window commit behind an in-flight present");
        }
    }

    CommitScope::~CommitScope()
    {
        if (!m_held)
            return;
        bool last;
        {
            std::lock_guard<std::mutex> lock(m_surface->m);
            last = --m_surface->readers == 0;
        }
        if (last)
            m_surface->changed.notify_all();
    }

    uint64_t heldCount()
    {
        return held.load(std::memory_order_relaxed);
    }

    void resetForTests()
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        // a test that leaked an open bracket must not poison the next one
        surfaces.clear();
        openCount.store(0, std::memory_order_relaxed);
        held.store(0, std::memory_order_relaxed);
    }
} // namespace VkHook::WaylandSyncobjGate
