#include "compiler.hh"

#include <cstdlib>
#include <exception>

#include "logger.hh"
#include "user_macros.hh"

namespace VkHook
{
    namespace
    {
        uint32_t codegenFlags(const ReshadeCodegenOptions& options)
        {
            return (options.useLocalSizeId ? 1u : 0u) | (options.emitDebugInfo ? 4u : 0u);
        }

        EffectCompileResult compileUnchecked(const EffectCompileSpec& spec, ShaderCache& cache)
        {
            EffectCompileResult result;
            result.id = spec.id;

            std::string source;
            std::string error;
            std::vector<std::pair<std::string, std::string>> usedMacros;
            if (!preprocessReshadeFile(spec.path, spec.setup, source, error, &usedMacros))
            {
                result.error = error;
                return result;
            }
            for (auto& macro : usedMacros)
            {
                if (isUserVisibleMacro(macro.first))
                    result.usedMacros.push_back(std::move(macro));
            }

            // The key is the preprocessed source (every include and macro already
            // folded in) plus the macro digest, the codegen flags and the compiler
            // tag; see ShaderCacheKey.
            const ShaderCacheKey key =
                makeShaderCacheKey(source, hashPreprocessSetup(spec.setup), codegenFlags(spec.codegen));
            result.module = cache.lookup(key);
            result.cacheHit = result.module != nullptr;
            if (!result.module)
            {
                auto fresh = std::make_shared<reshadefx::module>();
                if (!compileReshadeSource(std::move(source), spec.codegen, *fresh, error))
                {
                    result.error = error;
                    return result;
                }
                result.module = fresh;
                cache.store(key, result.module);
            }

            const auto& techniques = result.module->techniques;
            for (const auto& technique : techniques)
                result.techniqueNames.push_back(technique.name);

            if (spec.id.technique.empty())
            {
                if (techniques.empty())
                    result.error = "shader has no techniques: " + spec.id.canonicalPath;
                else
                    result.techniqueIndex = 0;
            }
            else
            {
                for (size_t i = 0; i < techniques.size(); ++i)
                {
                    if (techniques[i].name == spec.id.technique)
                    {
                        result.techniqueIndex = static_cast<int>(i);
                        break;
                    }
                }
                if (result.techniqueIndex < 0)
                    result.error = "technique '" + spec.id.technique + "' not found in " + spec.id.canonicalPath;
            }
            return result;
        }
    } // namespace

    ReshadeCodegenOptions runtimeCodegenOptions(bool isNvidiaGpu)
    {
        ReshadeCodegenOptions options;
        options.emitDebugInfo = !isNvidiaGpu;
        options.useLocalSizeId = !isNvidiaGpu;

        const char* value = std::getenv("VKHOOK_ENABLE_LOCAL_SIZE_ID");
        if (value != nullptr && *value != '\0')
        {
            if (value[0] == '0')
                options.useLocalSizeId = false;
            else if (value[0] == '1')
                options.useLocalSizeId = true;
            else
                Logger::warn(std::string("Invalid VKHOOK_ENABLE_LOCAL_SIZE_ID value '") + value + "'. Expected 0 or 1.");
        }
        return options;
    }

    EffectCompileResult compileEffect(const EffectCompileSpec& spec, ShaderCache& cache)
    {
        try
        {
            return compileUnchecked(spec, cache);
        }
        catch (const std::exception& e)
        {
            EffectCompileResult result;
            result.id = spec.id;
            result.error = std::string("compile failed: ") + e.what();
            return result;
        }
        catch (...)
        {
            EffectCompileResult result;
            result.id = spec.id;
            result.error = "compile failed: unknown exception";
            return result;
        }
    }

    EffectCompiler::EffectCompiler(ShaderCache& cache)
        : m_cache(cache)
    {
        m_thread = std::thread([this] { run(); });
    }

    EffectCompiler::~EffectCompiler()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        m_thread.join();
    }

    void EffectCompiler::setTestHook(std::function<void()> hook)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_hook = std::move(hook);
    }

    std::shared_future<ChainCompileResult> EffectCompiler::submit(ChainCompileSpec spec,
                                                                  std::function<void(const ChainCompileResult&)> onDone)
    {
        Job job;
        job.spec = std::move(spec);
        job.onDone = std::move(onDone);
        job.promise = std::make_shared<std::promise<ChainCompileResult>>();
        std::shared_future<ChainCompileResult> future = job.promise->get_future().share();

        std::vector<Job> superseded;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            job.seq = ++m_nextSeq;
            m_latest[job.spec.channel] = job.seq;
            for (auto it = m_queue.begin(); it != m_queue.end();)
            {
                if (it->spec.channel == job.spec.channel)
                {
                    superseded.push_back(std::move(*it));
                    it = m_queue.erase(it);
                }
                else
                    ++it;
            }
            for (Job& old : superseded)
            {
                ChainCompileResult result;
                result.cancelled = true;
                m_finished.push_back({std::move(old.onDone), std::move(result)});
            }
            m_queue.push_back(std::move(job));
        }
        m_wake.notify_all();

        // Resolve the futures outside the lock; the worker runs their onDone.
        for (Job& old : superseded)
        {
            ChainCompileResult result;
            result.cancelled = true;
            old.promise->set_value(std::move(result));
        }
        return future;
    }

    bool EffectCompiler::cancelled(const Job& job)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop)
            return true;
        const auto latest = m_latest.find(job.spec.channel);
        return latest != m_latest.end() && latest->second != job.seq;
    }

    ChainCompileResult EffectCompiler::compileChain(const Job& job)
    {
        ChainCompileResult result;
        for (const EffectCompileSpec& spec : job.spec.effects)
        {
            std::function<void()> hook;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                hook = m_hook;
            }
            if (hook)
                hook();
            if (cancelled(job))
            {
                result.cancelled = true;
                break;
            }
            result.effects.push_back(compileEffect(spec, m_cache));
        }
        return result;
    }

    void EffectCompiler::run()
    {
        const auto notify = [](const std::function<void(const ChainCompileResult&)>& onDone, const ChainCompileResult& result) {
            if (!onDone)
                return;
            try
            {
                onDone(result);
            }
            catch (const std::exception& e)
            {
                Logger::err(std::string("effect compiler: onDone threw: ") + e.what());
            }
            catch (...)
            {
                Logger::err("effect compiler: onDone threw");
            }
        };

        for (;;)
        {
            std::deque<Finished> finished;
            Job job;
            bool haveJob = false;
            bool stopping = false;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this] { return m_stop || !m_queue.empty() || !m_finished.empty(); });
                stopping = m_stop;
                if (!m_finished.empty())
                    finished.swap(m_finished);
                else if (!m_queue.empty())
                {
                    job = std::move(m_queue.front());
                    m_queue.pop_front();
                    haveJob = true;
                }
                else
                    return;   // stopping with nothing left
            }

            for (const Finished& done : finished)
                notify(done.onDone, done.result);
            if (!haveJob)
                continue;

            ChainCompileResult result;
            if (stopping)
                result.cancelled = true;
            else
                result = compileChain(job);
            job.promise->set_value(result);
            notify(job.onDone, result);
        }
    }
} // namespace VkHook
