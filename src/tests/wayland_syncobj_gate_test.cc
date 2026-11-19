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

    // endPresent from another thread must not close the bracket
    {
        resetForTests();
        setWaitBudgetForTests(20000);
        std::atomic<uint64_t> waited{0};
        std::atomic<bool> presentOpen{false};
        std::atomic<bool> finish{false};

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
            auto start = Clock::now();
            commitOnce(SURFACE);
            waited.store(microsSince(start));
        });
        foreign.join();
        CHECK(waited.load() >= 18000);

        finish.store(true);
        presenter.join();
        resetForTests();
    }

    // bound: a present that never closes cannot park commits forever
    {
        resetForTests();
        setWaitBudgetForTests(20000);
        beginPresent(SURFACE); // opened by this thread, never ended

        std::atomic<uint64_t> first{0};
        std::atomic<uint64_t> second{0};

        std::thread a([&] {
            auto start = Clock::now();
            commitOnce(SURFACE);
            first.store(microsSince(start));
        });
        a.join();

        std::thread b([&] {
            auto start = Clock::now();
            commitOnce(SURFACE);
            second.store(microsSince(start));
        });
        b.join();

        CHECK(first.load() >= 18000);
        CHECK(first.load() < 500000);
        CHECK(second.load() >= 18000);
        CHECK(second.load() < 500000);
        CHECK(heldCount() == 2);

        resetForTests();
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
