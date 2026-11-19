#pragma once

// A lock-free published pointer: readers load a snapshot, writers replace the
// whole object. Used for read-mostly tables on hot paths (the device registry,
// a device's queue table), so a lookup never takes a mutex.

#include <atomic>
#include <memory>

namespace VkHook
{
    template <typename T>
    class CowSlot
    {
    public:
        std::shared_ptr<const T> load() const
        {
#if defined(__cpp_lib_atomic_shared_ptr)
            return slot.load(std::memory_order_acquire);
#else
            return std::atomic_load_explicit(&slot, std::memory_order_acquire);
#endif
        }

        void store(std::shared_ptr<const T> value)
        {
#if defined(__cpp_lib_atomic_shared_ptr)
            slot.store(std::move(value), std::memory_order_release);
#else
            std::atomic_store_explicit(&slot, std::move(value), std::memory_order_release);
#endif
        }

    private:
#if defined(__cpp_lib_atomic_shared_ptr)
        std::atomic<std::shared_ptr<const T>> slot;
#else
        std::shared_ptr<const T> slot;
#endif
    };
} // namespace VkHook
