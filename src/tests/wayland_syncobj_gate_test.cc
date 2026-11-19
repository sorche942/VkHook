// A missing hold here ends as a compositor-killed wayland connection at
// runtime, so the present/commit ordering is driven directly with threads.
#include "wayland_syncobj_gate.hh"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace VkHook::WaylandSyncobjGate;
using Clock = std::chrono::steady_clock;

namespace
{
    int failures = 0;

    void check(bool ok, const char* what, int line)
    {
        if (!ok)
        {
            ++failures;
            std::fprintf(stderr, "FAIL:%d: %s\n", line, what);
        }
    }

    // one commit send, as the marshal hook brackets it
    void commitOnce(uint32_t surface)
    {
        CommitScope scope(surface);
    }

    uint64_t microsSince(Clock::time_point start)
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
    }
} // namespace

#define CHECK(cond) check((cond), #cond, __LINE__)

int main()
{
    constexpr uint32_t SURFACE = 43;

    // no present open: commits pass straight through
    {
        resetForTests();
        auto start = Clock::now();
        commitOnce(SURFACE);
        CHECK(microsSince(start) < 5000);
        CHECK(heldCount() == 0);
    }

    // the presenter's own commit inside its present is never held
    {
        resetForTests();
        beginPresent(SURFACE);
        auto start = Clock::now();
        commitOnce(SURFACE);
        CHECK(microsSince(start) < 5000);
        CHECK(heldCount() == 0);
        endPresent(SURFACE);
    }

    // a foreign commit waits until the presenter closes the bracket
    {
        resetForTests();
        std::atomic<bool> presentOpen{false};
        std::atomic<bool> presentFinished{false};
        std::atomic<uint64_t> foreignWait{0};

        std::thread presenter([&] {
            beginPresent(SURFACE);
            presentOpen.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            presentFinished.store(true);
            endPresent(SURFACE);
        });

        while (!presentOpen.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        std::thread foreign([&] {
            auto start = Clock::now();
            commitOnce(SURFACE);
            foreignWait.store(microsSince(start));
            CHECK(presentFinished.load()); // released only after the bracket closed
        });

        presenter.join();
        foreign.join();

        CHECK(foreignWait.load() >= 25000);
        CHECK(heldCount() == 1);

        // and once closed, commits pass instantly
        auto start = Clock::now();
        commitOnce(SURFACE);
        CHECK(microsSince(start) < 5000);
    }

    // endPresent from another thread must not close the bracket: the foreign commit
    // stays held until the owner closes it, however long that takes
    {
        resetForTests();
        std::atomic<bool> presentOpen{false};
        std::atomic<bool> finish{false};
        std::atomic<bool> committed{false};

        std::thread presenter([&] {
            beginPresent(SURFACE);
            presentOpen.store(true);
            while (!finish.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            endPresent(SURFACE);
        });

        while (!presentOpen.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        endPresent(SURFACE); // different thread: ignored

        std::thread foreign([&] {
            commitOnce(SURFACE);
            committed.store(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        CHECK(!committed.load()); // no wait budget lets it through

        finish.store(true);
        presenter.join();
        foreign.join();
        CHECK(committed.load());
        CHECK(heldCount() == 1);
        resetForTests();
    }

    // a second presenter waits for the first one's bracket, and every commit
    // behind it stays held until both are done
    {
        resetForTests();
        std::atomic<bool> firstOpen{false};
        std::atomic<bool> firstDone{false};
        std::atomic<bool> secondOpened{false};
        std::thread first([&] {
            beginPresent(SURFACE);
            firstOpen.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            firstDone.store(true);
            endPresent(SURFACE);
        });
        while (!firstOpen.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::thread second([&] {
            beginPresent(SURFACE);
            CHECK(firstDone.load());
            secondOpened.store(true);
            endPresent(SURFACE);
        });
        first.join();
        second.join();
        CHECK(secondOpened.load());
        CHECK(!anyPresentOpen());
    }

    // a presenter is not starved by a steady stream of commits
    {
        resetForTests();
        std::atomic<bool> stop{false};
        std::vector<std::thread> committers;
        for (int i = 0; i < 4; ++i)
            committers.emplace_back([&] {
                while (!stop.load())
                    commitOnce(SURFACE);
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto start = Clock::now();
        beginPresent(SURFACE);
        const uint64_t waited = microsSince(start);
        endPresent(SURFACE);
        stop.store(true);
        for (auto& t : committers)
            t.join();
        CHECK(waited < 20'000'000);
    }

    // surfaces are independent: a present on one never holds a commit on another
    {
        resetForTests();
        beginPresent(SURFACE);
        auto start = Clock::now();
        std::thread other([&] { commitOnce(SURFACE + 1); });
        other.join();
        CHECK(microsSince(start) < 5000);
        CHECK(heldCount() == 0);
        endPresent(SURFACE);
    }

    // the check-then-act window: no commit may be mid-send while a present
    // bracket is open, however the threads interleave. A gate that only
    // checked "is a present open?" before sending lets a commit begin just
    // before beginPresent and still be in flight inside it.
    {
        resetForTests();
        std::atomic<bool> stop{false};
        std::atomic<int> inSend{0};
        std::atomic<int> violations{0};
        std::atomic<uint64_t> commits{0};

        std::vector<std::thread> committers;
        for (int i = 0; i < 4; ++i)
        {
            committers.emplace_back([&] {
                while (!stop.load())
                {
                    CommitScope scope(SURFACE);
                    inSend.fetch_add(1);
                    std::this_thread::yield();
                    inSend.fetch_sub(1);
                    commits.fetch_add(1);
                }
            });
        }

        for (int i = 0; i < 3000; ++i)
        {
            beginPresent(SURFACE);
            if (inSend.load() != 0)
                violations.fetch_add(1);
            std::this_thread::yield();
            if (inSend.load() != 0)
                violations.fetch_add(1);
            endPresent(SURFACE);
            std::this_thread::yield();
        }
        stop.store(true);
        for (auto& t : committers)
            t.join();

        CHECK(violations.load() == 0);
        CHECK(commits.load() > 0);
        CHECK(!anyPresentOpen());
    }

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
