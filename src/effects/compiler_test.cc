// Tests for the effect compiler: compileEffect (technique selection, macros,
// errors) and the EffectCompiler worker (equivalence with the synchronous
// path, per-channel cancellation, shutdown, onDone delivery).
//
// usage: compiler_test <fixtures dir>

#include "compiler.hh"
#include "effect_id.hh"
#include "shader_cache.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace VkHook;

namespace
{
    int failures = 0;

    void expect(bool condition, const char* message)
    {
        if (!condition)
        {
            ++failures;
            std::printf("FAILED: %s\n", message);
        }
    }

    std::unique_ptr<ShaderCache> memoryOnlyCache()
    {
        return std::make_unique<ShaderCache>(16, std::string(), "compiler-test", ShaderCache::WriteScheduler{});
    }

    EffectCompileSpec makeSpec(const std::string& dir, const std::string& file, const std::string& technique,
                               const std::vector<PreprocessorDefinition>& defs = {})
    {
        EffectCompileSpec spec;
        spec.id.canonicalPath = file;
        spec.id.technique     = technique;
        spec.path             = dir + file;
        spec.setup.userDefs   = defs;
        return spec;
    }

    std::vector<std::string> uniformNames(const EffectCompileResult& result)
    {
        std::vector<std::string> names;
        if (result.module)
            for (const auto& uniform : result.module->uniforms)
                names.push_back(uniform.name);
        return names;
    }

    bool hasUniform(const EffectCompileResult& result, const std::string& name)
    {
        for (const auto& candidate : uniformNames(result))
            if (candidate == name)
                return true;
        return false;
    }

    // Two compiles of one source are not word-identical SPIR-V: the vendored
    // codegen orders some of its output by pointer, so a different allocation
    // pattern (the worker thread's malloc arena) reorders ids and debug names. What
    // must match is everything the runtime reads: the reflection and the module's
    // shape. The SPIR-V words themselves are compared for being a SPIR-V module.
    bool sameReflection(const reshadefx::module& a, const reshadefx::module& b)
    {
        auto same = [](const auto& x, const auto& y, auto&& eq) {
            return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin(), eq);
        };
        const bool entries = same(a.entry_points, b.entry_points, [](const auto& l, const auto& r) {
            return l.name == r.name && l.stage == r.stage && l.local_size_x == r.local_size_x &&
                   l.local_size_y == r.local_size_y && l.local_size_z == r.local_size_z;
        });
        const bool textures = same(a.textures, b.textures, [](const auto& l, const auto& r) {
            return l.unique_name == r.unique_name && l.semantic == r.semantic && l.width == r.width &&
                   l.height == r.height && l.levels == r.levels && l.format == r.format;
        });
        const bool samplers = same(a.samplers, b.samplers, [](const auto& l, const auto& r) {
            return l.unique_name == r.unique_name && l.texture_name == r.texture_name && l.binding == r.binding;
        });
        const bool uniforms = same(a.uniforms, b.uniforms, [](const auto& l, const auto& r) {
            return l.name == r.name && l.size == r.size && l.offset == r.offset;
        });
        const bool techniques = same(a.techniques, b.techniques, [&](const auto& l, const auto& r) {
            return l.name == r.name && same(l.passes, r.passes, [](const auto& pl, const auto& pr) {
                       return pl.vs_entry_point == pr.vs_entry_point && pl.ps_entry_point == pr.ps_entry_point &&
                              pl.cs_entry_point == pr.cs_entry_point &&
                              std::equal(std::begin(pl.render_target_names), std::end(pl.render_target_names),
                                         std::begin(pr.render_target_names));
                   });
        });
        return entries && textures && samplers && uniforms && techniques && a.total_uniform_size == b.total_uniform_size &&
               a.num_sampler_bindings == b.num_sampler_bindings && a.num_texture_bindings == b.num_texture_bindings &&
               !a.spirv.empty() && !b.spirv.empty() && a.spirv.front() == b.spirv.front();
    }

    bool sameResult(const EffectCompileResult& a, const EffectCompileResult& b)
    {
        const bool sameModule = (a.module == nullptr) == (b.module == nullptr) &&
                                (!a.module || sameReflection(*a.module, *b.module));
        return a.id == b.id && a.error == b.error && sameModule && a.techniqueNames == b.techniqueNames &&
               a.techniqueIndex == b.techniqueIndex && a.usedMacros == b.usedMacros &&
               uniformNames(a) == uniformNames(b);
    }

    // Blocks the worker inside the test hook until released.
    struct Gate
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool blocking = false;
        int entered = 0;

        void install(EffectCompiler& compiler)
        {
            compiler.setTestHook([this] {
                std::unique_lock<std::mutex> lock(mutex);
                ++entered;
                changed.notify_all();
                changed.wait(lock, [this] { return !blocking; });
            });
        }
        void block()
        {
            std::lock_guard<std::mutex> lock(mutex);
            blocking = true;
        }
        void release()
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                blocking = false;
            }
            changed.notify_all();
        }
        void waitEntered(int count)
        {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [&] { return entered >= count; });
        }
    };

    bool ready(const std::shared_future<ChainCompileResult>& future)
    {
        return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    struct Counter
    {
        std::atomic<int> calls{0};
        std::function<void(const ChainCompileResult&)> callback()
        {
            return [this](const ChainCompileResult&) { ++calls; };
        }
    };

    void testWorkerMatchesSynchronous(const std::string& dir)
    {
        ChainCompileSpec chain;
        chain.effects = {
            makeSpec(dir, "MultiTechnique.fx", "SinglePass"),
            makeSpec(dir, "MultiTechnique.fx", "TriplePass", {{"MULTI_EXTRA", "1", "1", "MultiTechnique"}}),
            makeSpec(dir, "MacroGatedUniform.fx", "MacroGatedUniform"),
            makeSpec(dir, "BrokenSyntax.fx", ""),
            makeSpec(dir, "DoesNotExist.fx", ""),
        };

        auto workerCache = memoryOnlyCache();
        ChainCompileResult viaWorker;
        {
            EffectCompiler compiler(*workerCache);
            viaWorker = compiler.submit(chain).get();
        }
        expect(!viaWorker.cancelled && viaWorker.effects.size() == chain.effects.size(), "the worker compiles every effect");

        auto freshCache = memoryOnlyCache();
        for (size_t i = 0; i < chain.effects.size() && i < viaWorker.effects.size(); ++i)
        {
            const EffectCompileResult direct = compileEffect(chain.effects[i], *freshCache);
            expect(sameResult(viaWorker.effects[i], direct), "the worker result equals the synchronous compileEffect result");
        }
        expect(viaWorker.effects.size() == 5 && viaWorker.effects[0].error.empty() && viaWorker.effects[1].error.empty() &&
                   viaWorker.effects[2].error.empty(),
               "the valid effects compile");
        expect(viaWorker.effects.size() == 5 && !viaWorker.effects[3].error.empty() && !viaWorker.effects[4].error.empty(),
               "a broken and a missing file report errors without failing the chain");

        // The second compile of the same source on the same cache is a hit.
        const EffectCompileResult again = compileEffect(chain.effects[0], *workerCache);
        expect(again.cacheHit && sameResult(again, viaWorker.effects[0]), "an unchanged effect is a cache hit with the same result");
    }

    void testTechniques(const std::string& dir)
    {
        auto cache = memoryOnlyCache();
        const std::vector<std::string> names = {"SinglePass", "DoublePass", "TriplePass"};
        const size_t passes[] = {1, 2, 3};
        for (size_t i = 0; i < names.size(); ++i)
        {
            const EffectCompileResult result = compileEffect(makeSpec(dir, "MultiTechnique.fx", names[i]), *cache);
            expect(result.error.empty(), "a technique of the multi-technique file compiles");
            expect(result.techniqueNames == names, "every technique is reported in file order");
            expect(result.techniqueIndex == static_cast<int>(i), "techniqueIndex follows id.technique");
            expect(result.module && result.techniqueIndex >= 0 &&
                       result.module->techniques[result.techniqueIndex].passes.size() == passes[i],
                   "the index selects the technique with the expected pass count");
        }

        const EffectCompileResult unresolved = compileEffect(makeSpec(dir, "MultiTechnique.fx", ""), *cache);
        expect(unresolved.error.empty() && unresolved.techniqueIndex == 0, "an empty technique means the file's first");

        const EffectCompileResult unknown = compileEffect(makeSpec(dir, "MultiTechnique.fx", "NoSuchTechnique"), *cache);
        expect(unknown.techniqueIndex == -1 && unknown.error == "technique 'NoSuchTechnique' not found in MultiTechnique.fx",
               "an unknown technique is an error naming it");
        expect(unknown.module && unknown.techniqueNames == names, "an unknown technique still reports the file's techniques");

        const EffectCompileResult plain = compileEffect(makeSpec(dir, "MultiTechnique.fx", "SinglePass"), *cache);
        bool macroReported = false;
        for (const auto& macro : plain.usedMacros)
            macroReported |= macro.first == "MULTI_EXTRA";
        expect(macroReported, "the user macro the file reads is reported");
        for (const auto& macro : plain.usedMacros)
            expect(macro.first.rfind("__", 0) != 0 && macro.first.rfind("BUFFER_", 0) != 0, "built-in macros are filtered out");
        expect(hasUniform(plain, "BaseGain") && !hasUniform(plain, "ExtraGain"), "the gated uniform is absent without its macro");

        const EffectCompileResult gated =
            compileEffect(makeSpec(dir, "MultiTechnique.fx", "SinglePass", {{"MULTI_EXTRA", "1", "1", "MultiTechnique"}}), *cache);
        expect(hasUniform(gated, "ExtraGain"), "the gated uniform appears with its macro");
        expect(!gated.cacheHit && plain.cacheHit, "different macros never share a cache entry; the same ones do");

        const EffectCompileResult broken = compileEffect(makeSpec(dir, "BrokenSyntax.fx", ""), *cache);
        expect(!broken.error.empty() && !broken.module && broken.techniqueIndex == -1, "a syntax error is an error result");
        const EffectCompileResult missing = compileEffect(makeSpec(dir, "DoesNotExist.fx", ""), *cache);
        expect(!missing.error.empty() && !missing.module, "a missing file is an error result");
    }

    void testCancellationAndDelivery(const std::string& dir)
    {
        auto cache = memoryOnlyCache();
        Counter counts[5];
        std::shared_future<ChainCompileResult> first, second, third, otherChannel, last;
        {
            Gate gate;   // outlives the compiler whose hook refers to it
            EffectCompiler compiler(*cache);
            gate.install(compiler);
            gate.block();

            ChainCompileSpec running;
            running.channel = 1;
            running.effects = {makeSpec(dir, "MultiTechnique.fx", "SinglePass"), makeSpec(dir, "MultiTechnique.fx", "DoublePass")};
            first = compiler.submit(running, counts[0].callback());
            gate.waitEntered(1);   // the worker holds `first` inside its hook

            ChainCompileSpec queued = running;
            second = compiler.submit(queued, counts[1].callback());   // supersedes the running one
            ChainCompileSpec other;
            other.channel = 2;
            other.effects = {makeSpec(dir, "MacroGatedUniform.fx", "MacroGatedUniform")};
            otherChannel = compiler.submit(other, counts[2].callback());
            third = compiler.submit(queued, counts[3].callback());    // supersedes `second` while it is still queued

            expect(ready(second) && second.get().cancelled, "a request superseded while queued resolves immediately as cancelled");
            expect(!ready(first), "the running request is still in flight");

            gate.release();
            expect(first.get().cancelled, "the running request is cancelled by a newer one on its channel");
            expect(first.get().effects.size() < 2, "a cancelled running request stops before its next effect");
            expect(!third.get().cancelled && third.get().effects.size() == 2 && third.get().effects[1].error.empty(),
                   "the newest request on the channel completes");
            expect(!otherChannel.get().cancelled && otherChannel.get().effects.size() == 1 &&
                       otherChannel.get().effects[0].error.empty(),
                   "a request on another channel is not cancelled");

            ChainCompileSpec after = running;
            last = compiler.submit(after, counts[4].callback());
            expect(!last.get().cancelled && last.get().effects.size() == 2, "a later request on the same channel is unaffected");
        }
        for (int i = 0; i < 5; ++i)
            expect(counts[i].calls.load() == 1, "onDone runs exactly once per request, cancelled or not");
    }

    void testDestructorWithPendingWork(const std::string& dir)
    {
        auto cache = memoryOnlyCache();
        Counter counts[3];
        Gate gate;
        std::shared_future<ChainCompileResult> futures[3];
        auto compiler = std::make_unique<EffectCompiler>(*cache);
        gate.install(*compiler);
        gate.block();

        for (int i = 0; i < 3; ++i)
        {
            ChainCompileSpec spec;
            spec.channel = static_cast<uint64_t>(i + 10);
            spec.effects = {makeSpec(dir, "MultiTechnique.fx", "TriplePass"), makeSpec(dir, "MultiTechnique.fx", "SinglePass")};
            futures[i] = compiler->submit(std::move(spec), counts[i].callback());
        }
        gate.waitEntered(1);

        // The destructor stops the worker; the blocked hook is released once it is on its way.
        std::thread destroyer([&compiler] { compiler.reset(); });
        gate.release();
        destroyer.join();

        for (int i = 0; i < 3; ++i)
        {
            expect(ready(futures[i]), "every request is resolved when the compiler is destroyed");
            expect(counts[i].calls.load() == 1, "onDone runs once per request across destruction");
        }
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: %s <fixtures dir>\n", argv[0]);
        return 2;
    }
    const std::string dir = std::string(argv[1]) + "/compile/";

    testTechniques(dir);
    testWorkerMatchesSynchronous(dir);
    testCancellationAndDelivery(dir);
    testDestructorWithPendingWork(dir);

    if (failures == 0)
        std::printf("compiler checks passed\n");
    return failures == 0 ? 0 : 1;
}
