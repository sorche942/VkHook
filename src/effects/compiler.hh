#ifndef EFFECT_COMPILER_HPP_INCLUDED
#define EFFECT_COMPILER_HPP_INCLUDED

// The one compile path for ReShade effects.
//
// compileEffect preprocesses a file once, looks the result up in a ShaderCache
// and compiles on a miss. The registry parse (parameter discovery), the chain
// build and the shader check all use it, so they see the same prelude, macros
// and uniforms. EffectCompiler runs whole chains of it on one worker thread so
// that no compile happens on the present thread or under a layer lock.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "effect_id.hh"
#include "reshade/effect_module.hh"
#include "reshade_compile.hh"
#include "shader_cache.hh"

namespace VkHook
{
    struct EffectCompileSpec
    {
        EffectId id;
        std::string path;                 // the .fx file
        ReshadePreprocessSetup setup;
        // Part of the cache key. The default is what the registry's parameter
        // parse has always used; the runtime picks it per GPU (runtimeCodegenOptions).
        ReshadeCodegenOptions codegen;
    };

    struct EffectCompileResult
    {
        EffectId id;
        std::string error;                                   // empty on success
        std::shared_ptr<const reshadefx::module> module;     // the whole file: every technique
        std::vector<std::string> techniqueNames;              // every technique, file order
        // Index in module->techniques of id.technique (0 when id.technique is
        // empty). -1, with `error` set, when the technique is not in the file.
        int techniqueIndex = -1;
        // User-visible macros the preprocessor read (name, value); the
        // built-in and underscore-prefixed ones are filtered out.
        std::vector<std::pair<std::string, std::string>> usedMacros;
        bool cacheHit = false;
    };

    struct ChainCompileSpec
    {
        std::vector<EffectCompileSpec> effects;
        uint64_t channel = 0;
    };

    struct ChainCompileResult
    {
        // One entry per effect of the spec, in order; fewer when cancelled
        // part-way (those finished before the cancellation), none when it was
        // cancelled while queued.
        std::vector<EffectCompileResult> effects;
        bool cancelled = false;
    };

    // Codegen options for the effects the layer runs on a GPU: NVIDIA drivers
    // get no debug info and no LocalSizeId; VKHOOK_ENABLE_LOCAL_SIZE_ID=0/1
    // overrides the latter.
    ReshadeCodegenOptions runtimeCodegenOptions(bool isNvidiaGpu);

    // Synchronous; the single implementation. Never throws: every failure
    // (unreadable file, preprocessor or compiler error, unknown technique) is in
    // `error`. Safe to call concurrently (ShaderCache is thread-safe).
    EffectCompileResult compileEffect(const EffectCompileSpec& spec, ShaderCache& cache);

    // One worker thread, no layer locks, no file I/O under its own mutex.
    class EffectCompiler
    {
    public:
        // `cache` must outlive the compiler.
        explicit EffectCompiler(ShaderCache& cache);
        // Cancels everything pending (running work stops before its next
        // effect), runs the outstanding onDone callbacks and joins. Returns
        // only once an installed test hook has returned.
        ~EffectCompiler();

        EffectCompiler(const EffectCompiler&) = delete;
        EffectCompiler& operator=(const EffectCompiler&) = delete;

        // A new submit on a channel supersedes the older requests of that
        // channel: queued ones resolve immediately with cancelled = true, a running
        // one stops between effects and resolves with cancelled = true. `onDone`
        // runs on the worker thread exactly once per request, also for cancelled
        // ones (for a request cancelled while queued, as soon as the worker is
        // free).
        std::shared_future<ChainCompileResult> submit(ChainCompileSpec spec,
                                                      std::function<void(const ChainCompileResult&)> onDone = {});

        // Test seam: `hook` runs on the worker thread, with no compiler lock
        // held, before each effect is compiled (and before the cancellation
        // check for it). A test blocks in it to hold a request in flight
        // deterministically. Pass an empty function to remove it.
        void setTestHook(std::function<void()> hook);

    private:
        struct Job
        {
            uint64_t seq = 0;
            ChainCompileSpec spec;
            std::function<void(const ChainCompileResult&)> onDone;
            std::shared_ptr<std::promise<ChainCompileResult>> promise;
        };
        struct Finished
        {
            std::function<void(const ChainCompileResult&)> onDone;
            ChainCompileResult result;
        };

        void run();
        // True when the job was superseded on its channel or the compiler is stopping.
        bool cancelled(const Job& job);
        ChainCompileResult compileChain(const Job& job);

        ShaderCache& m_cache;
        std::mutex m_mutex;
        std::condition_variable m_wake;
        std::deque<Job> m_queue;
        std::deque<Finished> m_finished;   // cancelled while queued; onDone still owed
        std::unordered_map<uint64_t, uint64_t> m_latest;  // channel -> newest seq
        std::function<void()> m_hook;
        uint64_t m_nextSeq = 0;
        bool m_stop = false;
        std::thread m_thread;   // last: starts in the constructor body
    };
} // namespace VkHook

#endif // EFFECT_COMPILER_HPP_INCLUDED
