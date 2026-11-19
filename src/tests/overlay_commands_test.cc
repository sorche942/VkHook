// The overlay's command queue: ordering, draining, concurrent producers, and
// the value copy SetParam relies on.

#include "overlay/commands.hh"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace VkHook;

namespace
{
    int failures = 0;
    void expect(bool condition, const char* what, int line)
    {
        if (!condition)
        {
            ++failures;
            std::printf("FAIL:%d: %s\n", line, what);
        }
    }
} // namespace
#define CHECK(c) expect((c), #c, __LINE__)

int main()
{
    {
        CommandQueue queue;
        CHECK(queue.empty());
        queue.push(RebuildChain{});
        queue.push(SetEffectEnabled{"Bloom", false});
        queue.push(SwitchProfile{"night"});
        CHECK(!queue.empty());

        std::vector<Command> out;
        out.push_back(ToggleEffects{});  // drain appends
        queue.drain(out);
        CHECK(queue.empty());
        CHECK(out.size() == 4);
        CHECK(std::holds_alternative<ToggleEffects>(out[0]));
        CHECK(std::holds_alternative<RebuildChain>(out[1]));
        const auto* enable = std::get_if<SetEffectEnabled>(&out[2]);
        CHECK(enable && enable->effect == "Bloom" && !enable->enabled);
        const auto* sw = std::get_if<SwitchProfile>(&out[3]);
        CHECK(sw && sw->name == "night");

        std::vector<Command> again;
        queue.drain(again);
        CHECK(again.empty());
    }

    {
        // producers on several threads: nothing lost, each producer's order kept
        CommandQueue queue;
        constexpr int kThreads = 4;
        constexpr int kPerThread = 500;
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t)
            threads.emplace_back([&queue, t] {
                for (int i = 0; i < kPerThread; ++i)
                    queue.push(SetMacro{std::to_string(t), "n", std::to_string(i)});
            });

        std::vector<Command> all;
        std::atomic<bool> done{false};
        std::thread consumer([&] {
            while (!done.load())
                queue.drain(all);
        });
        for (auto& thread : threads)
            thread.join();
        done = true;
        consumer.join();
        queue.drain(all);

        CHECK(all.size() == static_cast<size_t>(kThreads * kPerThread));
        int next[kThreads] = {};
        bool ordered = true;
        for (const Command& command : all)
        {
            const auto* macro = std::get_if<SetMacro>(&command);
            if (!macro)
            {
                ordered = false;
                continue;
            }
            const int producer = std::stoi(macro->effect);
            ordered = ordered && std::stoi(macro->value) == next[producer]++;
        }
        CHECK(ordered);
    }

    {
        FloatParam target;
        target.name = "Strength";
        target.defaultValue = 0.5f;
        target.value = 0.5f;
        target.minValue = 0.0f;
        target.maxValue = 1.0f;
        FloatParam source = target;
        source.value = 0.9f;
        source.maxValue = 99.0f;

        CHECK(assignParamValue(target, source));
        CHECK(target.value == 0.9f);
        CHECK(target.maxValue == 1.0f);  // descriptive fields stay with the target

        BoolParam other;
        CHECK(!assignParamValue(target, other));
        CHECK(target.value == 0.9f);
    }

    std::printf("%s\n", failures == 0 ? "ok" : "FAILED");
    return failures == 0 ? 0 : 1;
}
