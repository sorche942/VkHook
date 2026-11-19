// The PersistenceQueue is the only thing that writes user files, so its
// ordering rules are the contract: FIFO, coalescing, and "a queued autosave
// never resurrects a deleted preset". ConfigStore's snapshot and queued
// save are checked here too, since they sit on top of it.

#include "config_serializer.hh"
#include "control/preset_model.hh"
#include "persist.hh"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using VkHook::ConfigParam;
using VkHook::ConfigSerializer;
using VkHook::PersistenceQueue;

namespace
{
    int failures = 0;

    void expect(bool condition, const std::string& what)
    {
        if (!condition)
        {
            ++failures;
            std::printf("  FAILED: %s\n", what.c_str());
        }
    }

    // A job that parks the writer thread until released, so later submits queue up behind it.
    struct Gate
    {
        std::promise<void> release;
        std::shared_future<void> released = release.get_future().share();
        std::promise<void> started;
        std::function<bool()> job()
        {
            return [this] {
                started.set_value();
                released.wait();
                return true;
            };
        }
    };

    void test_runs_every_job_in_order()
    {
        PersistenceQueue queue;
        std::vector<int> order;
        for (int i = 0; i < 100; ++i)
            queue.other("k" + std::to_string(i), [&order, i] { order.push_back(i); return true; });
        queue.waitForIdle();
        bool ordered = order.size() == 100;
        for (int i = 0; ordered && i < 100; ++i)
            ordered = order[i] == i;
        expect(ordered, "other jobs run FIFO, all of them");
    }

    void test_future_reports_result_and_exceptions()
    {
        PersistenceQueue queue;
        auto ok = queue.other("a", [] { return true; });
        auto no = queue.other("b", [] { return false; });
        auto boom = queue.other("c", []() -> bool { throw std::runtime_error("boom"); });
        auto after = queue.other("d", [] { return true; });
        expect(ok.get(), "a successful job resolves true");
        expect(!no.get(), "a failing job resolves false");
        expect(!boom.get(), "a throwing job resolves false");
        expect(after.get(), "a throwing job does not poison the queue");
    }

    void test_writes_to_one_key_coalesce_to_the_latest()
    {
        PersistenceQueue queue;
        Gate gate;
        auto blocker = queue.other("blocker", gate.job());
        gate.started.get_future().wait();

        std::vector<int> ran;
        std::vector<std::future<bool>> results;
        for (int i = 0; i < 5; ++i)
            results.push_back(queue.write("preset", [&ran, i] { ran.push_back(i); return true; }));
        auto other = queue.write("other-preset", [&ran] { ran.push_back(100); return true; });

        gate.release.set_value();
        blocker.get();
        for (auto& r : results)
            expect(r.get(), "every coalesced waiter resolves with the surviving job's result");
        other.get();
        expect(ran.size() == 2 && ran[0] == 4 && ran[1] == 100,
               "five queued writes to one key ran once, as the newest, and other keys are untouched");
    }

    void test_write_does_not_coalesce_across_a_remove()
    {
        PersistenceQueue queue;
        Gate gate;
        auto blocker = queue.other("blocker", gate.job());
        gate.started.get_future().wait();

        std::vector<std::string> ran;
        auto w1 = queue.write("p", [&ran] { ran.push_back("write1"); return true; });
        auto rm = queue.remove("p", [&ran] { ran.push_back("remove"); return true; });
        auto create = queue.create("p", [&ran] { ran.push_back("create"); return true; });
        auto w2 = queue.write("p", [&ran] { ran.push_back("write2"); return true; });
        gate.release.set_value();
        blocker.get();
        w1.get(); rm.get(); create.get(); w2.get();
        // the pre-remove write is cancelled; the post-create one runs after the create
        expect(ran == std::vector<std::string>({"remove", "create", "write2"}),
               "a remove cancels earlier writes and a later write runs after the create");
    }

    void test_submit_after_shutdown_is_rejected_and_shutdown_drains()
    {
        PersistenceQueue queue;
        std::atomic<int> ran{0};
        for (int i = 0; i < 20; ++i)
            queue.other("k", [&ran] { ran++; return true; });
        queue.shutdown();
        expect(ran.load() == 20, "shutdown drains everything queued");
        auto late = queue.other("late", [&ran] { ran++; return true; });
        expect(!late.get(), "a submit after shutdown resolves false");
        expect(ran.load() == 20, "a submit after shutdown never runs");
        queue.shutdown();  // idempotent
    }

    // formal/PersistenceQueue.lean `drainAtLastInstance` then `issue` (sat trace
    // restart_after_drain): the last vkDestroyInstance drains with waitForIdle() and does not stop
    // the writer, so a later instance's jobs are accepted and run.
    void test_drain_then_submit_then_drain()
    {
        PersistenceQueue queue;
        std::atomic<int> ran{0};
        std::vector<std::string> order;
        std::mutex orderMutex;
        const auto note = [&](const char* tag) {
            std::lock_guard<std::mutex> lock(orderMutex);
            order.push_back(tag);
            ran++;
            return true;
        };

        // instance 1: queued work lands at its last vkDestroyInstance
        auto a = queue.other("a", [&] { return note("a"); });
        auto b = queue.write("w", [&] { return note("w1"); });
        queue.waitForIdle();
        expect(ran.load() == 2, "the drain ran every queued job");
        expect(a.wait_for(std::chrono::seconds(0)) == std::future_status::ready && a.get(),
               "a drained job's future is already resolved true");
        expect(b.get(), "a drained write resolves true");

        // instance 2: the queue accepts again
        auto c = queue.other("c", [&] { return note("c"); });
        auto d = queue.write("w", [&] { return note("w2"); });
        queue.waitForIdle();
        expect(c.get() && d.get(), "jobs submitted after a drain run and resolve true");
        expect(ran.load() == 4, "nothing was dropped across drain, submit, drain");
        expect((order == std::vector<std::string>{"a", "w1", "c", "w2"}), "order is kept across the drain");

        // a drain with nothing queued returns and changes nothing
        queue.waitForIdle();
        auto e = queue.other("e", [&] { return note("e"); });
        expect(e.get(), "still accepting after an empty drain");

        // only shutdown() rejects, matching `shutdown` in the model
        queue.shutdown();
        auto late = queue.other("late", [&] { return note("late"); });
        expect(!late.get() && ran.load() == 5, "shutdown, not drain, rejects submits");
    }

    void test_idle_is_safe_without_any_job()
    {
        PersistenceQueue queue;
        queue.waitForIdle();
        expect(true, "waitForIdle on an unused queue returns");
    }

    void test_concurrent_submits_all_run()
    {
        PersistenceQueue queue;
        std::mutex mutex;
        std::vector<int> seen;
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
        {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 300; ++i)
                    queue.other("k", [&, v = t * 300 + i] {
                        std::lock_guard<std::mutex> lock(mutex);
                        seen.push_back(v);
                        return true;
                    });
            });
        }
        for (auto& th : threads)
            th.join();
        queue.waitForIdle();
        expect(seen.size() == 8u * 300u, "every concurrently submitted job ran exactly once");
    }

    // The defect this queue exists for: an autosave that is still queued (or is
    // queued by a frame that has not noticed yet) when the user deletes a preset
    // must not write the file back.
    void test_queued_autosave_cannot_resurrect_a_deleted_preset()
    {
        const std::string path = ConfigSerializer::getShaderProfilePath("game", "keep-me-deleted");
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        VkHook::EffectStack stack;
        stack.instances.push_back({{"fx.fx", "fx", 1}, true, {{"Strength", {"1"}}}, {}});
        const VkHook::SerializedPreset preset = VkHook::serialize(stack);
        auto autosave = [&] {
            return ConfigSerializer::writeShaderProfile(path, preset.ini, preset.sidecar);
        };

        PersistenceQueue queue;
        expect(queue.create(path, [&] { return ConfigSerializer::createShaderProfile("game", "keep-me-deleted"); }).get(),
               "the preset is created through the queue");
        expect(std::filesystem::exists(path), "preset exists");

        Gate gate;
        auto blocker = queue.other("blocker", gate.job());
        gate.started.get_future().wait();

        auto before = queue.write(path, autosave);   // queued before the delete
        auto removed = queue.remove(path, [&] { return ConfigSerializer::deleteShaderProfile("game", "keep-me-deleted"); });
        auto after = queue.write(path, autosave);    // a frame that has not seen the delete yet
        gate.release.set_value();
        blocker.get();

        expect(!before.get(), "the autosave queued before the delete is cancelled");
        expect(removed.get(), "the delete ran");
        expect(!after.get(), "the autosave queued after the delete is dropped");
        queue.waitForIdle();
        expect(!std::filesystem::exists(path), "the deleted preset did not come back");
        expect(!std::filesystem::exists(ConfigSerializer::getShaderProfileSidecarPath(path)),
               "neither did its sidecar");

        // recreating the same name lifts the tombstone, and saves work again
        expect(queue.create(path, [&] { return ConfigSerializer::createShaderProfile("game", "keep-me-deleted"); }).get(),
               "the preset can be created again");
        expect(queue.write(path, autosave).get(), "autosaves work again after a create");
        expect(std::filesystem::exists(path), "preset exists again");
    }

    void test_rename_moves_the_tombstone()
    {
        PersistenceQueue queue;
        std::atomic<int> ran{0};
        queue.rename("old", "new", [] { return true; }).get();
        expect(!queue.write("old", [&ran] { ran++; return true; }).get(), "a write to the renamed-away path is dropped");
        expect(queue.write("new", [&ran] { ran++; return true; }).get(), "a write to the new path runs");
        queue.noteCreated("old");
        expect(queue.write("old", [&ran] { ran++; return true; }).get(), "noteCreated lifts the tombstone");
        expect(ran.load() == 2, "only the permitted writes ran");
    }

} // namespace

int main()
{
    char temporary[] = "/tmp/vkhook-persist-tests-XXXXXX";
    char* tempDir = mkdtemp(temporary);
    if (!tempDir)
        return 1;
    setenv("XDG_CONFIG_HOME", tempDir, 1);
    setenv("HOME", tempDir, 1);
    setenv("VKHOOK_LOG_LEVEL", "none", 1);

    test_runs_every_job_in_order();
    test_future_reports_result_and_exceptions();
    test_writes_to_one_key_coalesce_to_the_latest();
    test_write_does_not_coalesce_across_a_remove();
    test_submit_after_shutdown_is_rejected_and_shutdown_drains();
    test_drain_then_submit_then_drain();
    test_idle_is_safe_without_any_job();
    test_concurrent_submits_all_run();
    test_queued_autosave_cannot_resurrect_a_deleted_preset();
    test_rename_moves_the_tombstone();

    std::filesystem::remove_all(tempDir);
    std::printf("%s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;
}
