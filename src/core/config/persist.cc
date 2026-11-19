#include "persist.hh"

#include "logger.hh"

#include <algorithm>
#include <exception>

namespace VkHook
{
    PersistenceQueue& PersistenceQueue::instance()
    {
        // Never destroyed: a static destructor joining a worker during process
        // teardown races whatever the host is tearing down. The layer calls
        // shutdown() explicitly when it knows the process is going away.
        static PersistenceQueue* queue = new PersistenceQueue();
        return *queue;
    }

    PersistenceQueue::~PersistenceQueue()
    {
        shutdown();
    }

    std::future<bool> PersistenceQueue::ready(bool value)
    {
        std::promise<bool> promise;
        promise.set_value(value);
        return promise.get_future();
    }

    void PersistenceQueue::resolve(Entry& entry, bool value)
    {
        for (auto& waiter : entry.waiters)
            waiter.set_value(value);
        entry.waiters.clear();
    }

    std::future<bool> PersistenceQueue::submit(Request request)
    {
        if (!request.job)
            return ready(false);

        std::future<bool> future;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping)
            {
                Logger::warn("persist: dropped a job submitted after shutdown: " + request.key);
                return ready(false);
            }

            const auto cancelPendingWrites = [this](const std::string& key) {
                for (auto it = m_entries.begin(); it != m_entries.end();)
                {
                    if (it->kind == Kind::Write && it->key == key)
                    {
                        resolve(*it, false);
                        it = m_entries.erase(it);
                    }
                    else
                        ++it;
                }
            };

            switch (request.kind)
            {
                case Kind::Write:
                    if (m_tombstones.count(request.key))
                    {
                        Logger::debug("persist: write to a removed file ignored: " + request.key);
                        return ready(false);
                    }
                    break;
                case Kind::Create:
                    m_tombstones.erase(request.key);
                    break;
                case Kind::Remove:
                    cancelPendingWrites(request.key);
                    m_tombstones.insert(request.key);
                    break;
                case Kind::Rename:
                    cancelPendingWrites(request.key);
                    m_tombstones.insert(request.key);
                    m_tombstones.erase(request.key2);
                    break;
                case Kind::Other:
                    break;
            }

            if (!m_started)
            {
                try
                {
                    m_thread = std::thread(&PersistenceQueue::run, this);
                }
                catch (const std::exception& e)
                {
                    Logger::err(std::string("persist: could not start the writer thread: ") + e.what());
                    return ready(false);
                }
                m_started = true;
            }

            std::promise<bool> promise;
            future = promise.get_future();

            // coalesce onto the newest pending Write for this key, but never
            // past another job that touches the same file
            if (request.kind == Kind::Write)
            {
                for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it)
                {
                    if (it->key != request.key && it->key2 != request.key)
                        continue;
                    if (it->kind == Kind::Write)
                    {
                        it->job = std::move(request.job);
                        it->waiters.push_back(std::move(promise));
                        return future;
                    }
                    break;
                }
            }

            Entry entry;
            entry.kind = request.kind;
            entry.key = std::move(request.key);
            entry.key2 = std::move(request.key2);
            entry.job = std::move(request.job);
            entry.waiters.push_back(std::move(promise));
            m_entries.push_back(std::move(entry));
        }
        m_wake.notify_one();
        return future;
    }

    std::future<bool> PersistenceQueue::write(std::string key, std::function<bool()> job)
    {
        return submit({Kind::Write, std::move(key), {}, std::move(job)});
    }

    std::future<bool> PersistenceQueue::create(std::string key, std::function<bool()> job)
    {
        return submit({Kind::Create, std::move(key), {}, std::move(job)});
    }

    std::future<bool> PersistenceQueue::remove(std::string key, std::function<bool()> job)
    {
        return submit({Kind::Remove, std::move(key), {}, std::move(job)});
    }

    std::future<bool> PersistenceQueue::rename(std::string from, std::string to, std::function<bool()> job)
    {
        return submit({Kind::Rename, std::move(from), std::move(to), std::move(job)});
    }

    std::future<bool> PersistenceQueue::other(std::string key, std::function<bool()> job)
    {
        return submit({Kind::Other, std::move(key), {}, std::move(job)});
    }

    void PersistenceQueue::run()
    {
        for (;;)
        {
            Entry entry;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this] { return m_stopping || !m_entries.empty(); });

                if (m_entries.empty())
                    return;  // stopping and drained

                entry = std::move(m_entries.front());
                m_entries.pop_front();
                m_running = true;
            }

            bool ok = false;
            try
            {
                ok = entry.job();
            }
            catch (const std::exception& e)
            {
                // a background write must never take the layer down
                Logger::err(std::string("persist: job failed: ") + e.what());
            }
            catch (...)
            {
                Logger::err("persist: job failed with an unknown exception");
            }
            resolve(entry, ok);

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_running = false;
                if (m_entries.empty())
                    m_done.notify_all();
            }
        }
    }

    void PersistenceQueue::noteCreated(const std::string& key)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tombstones.erase(key);
    }

    void PersistenceQueue::waitForIdle()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_done.wait(lock, [this] { return m_entries.empty() && !m_running; });
    }

    void PersistenceQueue::shutdown()
    {
        std::thread worker;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
            worker = std::move(m_thread);
        }
        m_wake.notify_all();
        if (worker.joinable())
            worker.join();
    }
} // namespace VkHook
