#ifndef COMPILE_CACHE_HPP_INCLUDED
#define COMPILE_CACHE_HPP_INCLUDED

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace VkHook
{
    // FNV-1a. Stable across runs and platforms, which matters because the key
    // has to mean the same thing on the next reload as it did when the entry
    // was stored.
    inline constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;

    // `seed` chains hashes: feeding one result in as the next seed folds several
    // buffers into one digest.
    inline uint64_t hashBytes(const void* data, size_t size, uint64_t seed = kFnvOffsetBasis)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        uint64_t hash = seed;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    // LRU-free, FIFO-bounded cache for expensive compile results. Used to skip
    // re-parsing and re-codegenning an unchanged ReShade effect on a chain
    // reload, which is what made toggling an effect stall the present thread.
    //
    // Values are shared and immutable: a hit hands out the stored object instead
    // of copying a whole compiled module, and an entry evicted while an effect
    // still uses it stays alive through that effect's reference.
    //
    // The key is a digest of everything the compile depends on plus a small flag
    // word (see ShaderCache for how it is built). Thread-safe: an off-thread
    // compiler can share one instance with the present thread.
    template <typename Value>
    class CompileCache
    {
    public:
        struct Key
        {
            uint64_t hash = 0;
            uint32_t flags = 0;

            bool operator==(const Key& other) const
            {
                return hash == other.hash && flags == other.flags;
            }
        };

        explicit CompileCache(size_t capacity = 64)
            : m_capacity(capacity == 0 ? 1 : capacity)
        {
        }

        using Handle = std::shared_ptr<const Value>;

        // Null on a miss.
        Handle lookup(const Key& key) const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_entries.find(key);
            return it == m_entries.end() ? Handle() : it->second;
        }

        void store(const Key& key, Handle value)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_entries.find(key) != m_entries.end())
                return;

            if (m_entries.size() >= m_capacity)
            {
                const Key oldest = m_order.front();
                m_order.pop_front();
                m_entries.erase(oldest);
            }

            m_entries.emplace(key, std::move(value));
            m_order.push_back(key);
        }

        size_t size() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_entries.size();
        }

        void clear()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_entries.clear();
            m_order.clear();
        }

    private:
        struct KeyHash
        {
            size_t operator()(const Key& key) const
            {
                return static_cast<size_t>(key.hash ^ (static_cast<uint64_t>(key.flags) * 0x9E3779B97F4A7C15ull));
            }
        };

        mutable std::mutex m_mutex;
        size_t m_capacity;
        std::unordered_map<Key, Handle, KeyHash> m_entries;
        std::deque<Key> m_order;
    };
} // namespace VkHook

#endif // COMPILE_CACHE_HPP_INCLUDED
