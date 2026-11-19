#ifndef VKHOOK_CONFIG_PERSIST_HH_INCLUDED
#define VKHOOK_CONFIG_PERSIST_HH_INCLUDED

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace VkHook
{
    // The single ordered writer for every file VkHook mutates: preset saves,
    // deletes, renames, creates, imports, settings and shader-manager config.
    // One background thread runs the jobs in submission order, so the present
    // thread never blocks on disk and two threads can never reorder writes to
    // the same file.
    //
    // Ordering rules, applied at submit time:
    //  - Write jobs for a key coalesce: a pending Write is replaced by the
    //    newest one, and every waiter of the replaced job gets the result of
    //    the one that actually ran.
    //  - Remove (and the old side of Rename) cancels pending Writes for the key
    //    and tombstones it, so an autosave queued before or after a delete can
    //    never bring the file back. Create (and the new side of Rename) lifts
    //    the tombstone.
    //  - Everything else runs FIFO.
    class PersistenceQueue
    {
    public:
        enum class Kind
        {
            Write,   // update an existing file; coalesces, dropped after Remove
            Create,  // makes a file exist (new preset, import)
            Remove,  // deletes a file
            Rename,  // key -> key2
            Other,   // plain FIFO job (settings, shader manager, pointers)
        };

        struct Request
        {
            Kind kind = Kind::Other;
            std::string key;   // path (or logical name) the job mutates
            std::string key2;  // Rename destination
            std::function<bool()> job;
        };

        PersistenceQueue() = default;
        PersistenceQueue(const PersistenceQueue&) = delete;
        PersistenceQueue& operator=(const PersistenceQueue&) = delete;
        ~PersistenceQueue();

        static PersistenceQueue& instance();

        // The future resolves to the job's result; false if the job threw, was
        // coalesced into a newer one that failed, was cancelled, or the queue
        // is stopped.
        std::future<bool> submit(Request request);

        std::future<bool> write(std::string key, std::function<bool()> job);
        std::future<bool> create(std::string key, std::function<bool()> job);
        std::future<bool> remove(std::string key, std::function<bool()> job);
        std::future<bool> rename(std::string from, std::string to, std::function<bool()> job);
        std::future<bool> other(std::string key, std::function<bool()> job);

        // For a job that creates a file whose path is only known once it runs
        // (an import picks a free name): lifts any tombstone on that path.
        void noteCreated(const std::string& key);

        // Blocks until every queued job has finished.
        void waitForIdle();

        // Drains the queue and joins the thread. Submits afterwards are logged
        // and rejected. Idempotent.
        void shutdown();

    private:
        struct Entry
        {
            Kind kind = Kind::Other;
            std::string key;
            std::string key2;
            std::function<bool()> job;
            std::vector<std::promise<bool>> waiters;
        };

        void run();
        static std::future<bool> ready(bool value);
        static void resolve(Entry& entry, bool value);

        std::mutex m_mutex;
        std::condition_variable m_wake;
        std::condition_variable m_done;
        std::deque<Entry> m_entries;
        std::set<std::string> m_tombstones;
        std::thread m_thread;
        bool m_started = false;
        bool m_stopping = false;
        bool m_running = false;
    };
} // namespace VkHook

#endif // VKHOOK_CONFIG_PERSIST_HH_INCLUDED
