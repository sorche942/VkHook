#pragma once

#include <functional>
#include <utility>
#include <vector>

namespace VkHook
{
    // Releases resources in the reverse of the order they were acquired. For
    // libwayland objects that is the only safe order: a proxy is created from
    // its parent (and inherits the parent's event queue), so it must be
    // destroyed before that parent, and every proxy before the wl_event_queue
    // they are attached to. Destroying the queue first leaves proxies pointing
    // at freed memory; a later wl_proxy_destroy on them then reads it.
    class ReleaseStack
    {
    public:
        using Release = std::function<void()>;

        ReleaseStack() = default;
        ReleaseStack(const ReleaseStack&) = delete;
        ReleaseStack& operator=(const ReleaseStack&) = delete;

        // Call right after the resource is created.
        void push(Release release)
        {
            m_entries.push_back(std::move(release));
        }

        // Runs every release, newest first, and empties the stack.
        void releaseAll()
        {
            while (!m_entries.empty())
            {
                Release release = std::move(m_entries.back());
                m_entries.pop_back();
                release();
            }
        }

        bool empty() const
        {
            return m_entries.empty();
        }

    private:
        std::vector<Release> m_entries;
    };
} // namespace VkHook
