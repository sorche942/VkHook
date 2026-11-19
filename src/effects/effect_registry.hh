#ifndef EFFECT_REGISTRY_HPP_INCLUDED
#define EFFECT_REGISTRY_HPP_INCLUDED

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "compiler.hh"
#include "config.hh"
#include "effect_config.hh"
#include "effect_id.hh"
#include "reshade_compile.hh"
#include "shader_cache.hh"
#include "shader_catalog.hh"

namespace VkHook
{
    // EffectRegistry is the single source of truth for the chain's entries, their
    // parameters and macros. UI commands write here, rendering reads from here.
    //
    // Keys. An entry is keyed by its EffectId text (EffectId::format(); see
    // effect_id.hh). Every method taking an effect name also accepts a *legacy
    // name* ("Clarity", "Clarity.2", a `name = file.fx` definition) that
    // ensureEffect() has already migrated: the registry keeps an alias from the
    // legacy name to the entry's id, so callers holding old names (a config
    // loaded from disk) keep working while the chain converges on ids.
    //
    // Lifetime. One registry belongs to one device (invariant 13): it owns the
    // shader catalog, the compile cache's memory level and the compile worker.
    // Nothing here is process-global. The compiler worker is destroyed first
    // (it runs callbacks into this object), so a registry may be destroyed with
    // reparses in flight.
    //
    // Threading. `mutex` guards the entries; it is never held across file I/O,
    // a parse or a compile (invariant 1). Methods that return collections return
    // copies: nothing handed out refers into the registry's storage except the
    // EffectParam pointers, which stay valid until parameterGeneration() moves
    // and for a bounded number of generations after (see retireParameters).
    class EffectRegistry
    {
    public:
        EffectRegistry();
        ~EffectRegistry();
        EffectRegistry(const EffectRegistry&) = delete;
        EffectRegistry& operator=(const EffectRegistry&) = delete;

        // Initialize registry from config file
        void initialize(Config* config);

        // A snapshot of every entry (enabled + disabled), without parameters.
        std::vector<EffectConfig> getAllEffects() const;

        // Clones of every parameter of every entry (for UI)
        std::vector<std::unique_ptr<EffectParam>> getAllParameters() const;

        // Toggle effect enabled state. For a name the registry has no entry for
        // yet, the state is remembered and applied when ensureEffect creates it.
        void setEffectEnabled(const std::string& effectName, bool enabled);

        // Get enabled state for a specific effect
        bool isEffectEnabled(const std::string& effectName) const;

        // The one preprocess setup shared by the parameter parse and the runtime
        // compile: same prelude, depth macros, include paths and user macros.
        ReshadePreprocessSetup preprocessSetup(uint32_t bufferWidth,
                                               uint32_t bufferHeight,
                                               uint32_t colorBitDepth,
                                               std::vector<PreprocessorDefinition> userDefs) const;

        // Extent used for BUFFER_WIDTH/HEIGHT when effect parameters are parsed
        // (before any swapchain's real extent is known). Defaults to 1920x1080.
        void setCompileExtent(uint32_t width, uint32_t height);

        // Get parameter by name
        std::optional<EffectParam*>       getParameter(const std::string& effectName, const std::string& paramName);
        std::optional<const EffectParam*> getParameter(const std::string& effectName, const std::string& paramName) const;

        // Get all parameters for a specific effect (returns pointers, not clones)
        std::vector<EffectParam*> getParametersForEffect(const std::string& effectName);

        // Get config reference for effects to read values
        Config* getConfig() const { return rootConfig.load(std::memory_order_acquire); }

        // True for the names of the removed built-in effects (cas, dls, fxaa,
        // smaa, deband, lut) and their numbered instances (cas.2). Legacy
        // config and preset references to these are dropped.
        static bool isLegacyBuiltInName(const std::string& name);

        // Resolve an effect to its installed .fx path, or "DLSS5" for the native
        // neural renderer. `name` may be an id, a file name, a stem, a path or a
        // legacy name; `config` supplies `name = path` definitions and
        // reshadeIncludePath. Backed by the shader catalog, so a lookup is an
        // index probe, not a directory walk. Preset import calls it before
        // initialize() has set the root config.
        std::string resolveEffectPath(const std::string& name, Config* config) const;

        // Add an effect if not already present (for dynamically added effects).
        // `instanceName` is an id or a legacy name; `effectType` optionally
        // names the shader (file name, stem, path or definition). A legacy name
        // or an id without a technique is migrated to the id of the file's
        // first technique. If the shader file changed on disk since last load,
        // re-parses (keeping the entry's enabled state and current values).
        void ensureEffect(const std::string& instanceName, const std::string& effectType = "");

        // Remove an effect completely from the registry (parameters, config, everything)
        void removeEffect(const std::string& name);

        // Check if effect exists in registry
        bool hasEffect(const std::string& name) const;

        // The registry key (id text) the name resolves to: the name itself for an
        // entry, its migrated id for a legacy name, else the name unchanged.
        std::string canonicalName(const std::string& name) const;

        // Get the file path for an effect (for ReShade effects)
        std::string getEffectFilePath(const std::string& name) const;

        // The shader's base type name (file stem, e.g. "CAS"; "DLSS5")
        std::string getEffectType(const std::string& name) const;

        // Check if an effect failed to compile
        bool hasEffectFailed(const std::string& name) const;

        // Get compilation error for an effect (empty if no error)
        std::string getEffectError(const std::string& name) const;

        // Set compilation error for an effect (marks it as failed)
        void setEffectError(const std::string& name, const std::string& error);

        // The preprocessor definitions of an effect (ReShade only), as a copy.
        std::vector<PreprocessorDefinition> getPreprocessorDefs(const std::string& effectName) const;

        // Sets a macro's value and re-parses the effect's parameter list on the
        // compile worker: macro-gated uniforms appear (and vanish) without a
        // restart. Values of parameters that survive are kept, a failing macro
        // value marks the entry failed until a later value compiles. Returns
        // immediately; see waitForParses.
        void setPreprocessorDefValue(const std::string& effectName, const std::string& macroName, const std::string& value);

        // Blocks until no macro re-parse is queued or running, or `timeout`
        // passes. True when idle. For tests and orderly shutdown.
        bool waitForParses(std::chrono::milliseconds timeout);

        // Selected effects management (ordered list for UI). The getter returns a
        // snapshot with legacy names mapped to their ids.
        std::vector<std::string> getSelectedEffects() const;
        void setSelectedEffects(const std::vector<std::string>& effects);
        void clearSelectedEffects();

        // monotonic counter, bumped whenever a structural mutation can free or
        // replace parameters. cached param pointers must not outlive a bump.
        uint64_t parameterGeneration() const { return paramGeneration.load(std::memory_order_acquire); }

        // Check if effects have been initialized from config (first load complete)
        bool isInitializedFromConfig() const { return initializedFromConfig.load(std::memory_order_acquire); }

        // Initialize selected effects from config (call once at startup)
        void initializeSelectedEffectsFromConfig();

        // The codegen options parses and re-parses compile with. They are part of
        // the compile cache's key, so a chain build that compiled with a device's
        // runtime options (runtimeCodegenOptions) records them here and the
        // registry's own parses then hit the same cache entries.
        void                  setCodegenOptions(const ReshadeCodegenOptions& options);
        ReshadeCodegenOptions codegenOptions() const;

        // The compiled-module cache (memory level + disk), owned by this registry.
        ShaderCache&    shaderCache() { return *cache; }
        // The off-thread compile worker. Chain builds submit to it (see compiler.hh).
        EffectCompiler& compiler() { return *worker; }
        ShaderCatalog&  catalog() { return shaderCatalog; }

    private:
        std::list<EffectEntry> entries;
        std::vector<std::string> selectedEffects;  // Ordered list of selected effects for UI
        // legacy name -> id text. A legacy name stays an alias for as long as its
        // entry exists.
        std::unordered_map<std::string, std::string> aliases;
        // enabled states set before the entry existed
        std::unordered_map<std::string, bool> pendingEnabled;
        // Parameter sets replaced or removed; kept for a few generations so a
        // reader that resolved a pointer just before the bump never sees it freed.
        std::deque<std::vector<std::unique_ptr<EffectParam>>> retiredParameters;
        std::atomic<bool> initializedFromConfig{false};
        // Named rootConfig rather than config: initReshadeEffect builds a local
        // EffectConfig called config, and this is a different type entirely --
        // the parsed VkHook.conf.
        std::atomic<Config*> rootConfig{nullptr};
        ReshadeCodegenOptions codegen;
        uint32_t compileWidth = 1920;
        uint32_t compileHeight = 1080;
        mutable std::mutex mutex;
        std::atomic<uint64_t> paramGeneration{0};

        // names already reported as having no shader: the control step asks for
        // every selected effect each frame, and a ghost must not log each time
        mutable std::mutex           warnMutex;
        mutable std::set<std::string> warnedMissing;

        // macro re-parses in flight
        std::mutex              parseMutex;
        std::condition_variable parseIdle;
        int                     parsesPending = 0;

        mutable ShaderCatalog          shaderCatalog;
        std::unique_ptr<ShaderCache>   cache;
        // Last member: destroyed first, which joins the worker before anything
        // its callbacks touch goes away.
        std::unique_ptr<EffectCompiler> worker;

        // Resolves a shader reference through the catalog, refreshing the roots
        // from the config first. No registry lock is held.
        std::string findEffectPath(const std::string& name, Config* config) const;
        void        refreshCatalogRoots(Config* config) const;

        // Resolves the shader of `instanceName` and parses and compile-checks it:
        // reads files and runs the compiler, so no lock is held and nothing here
        // touches the registry's state. The entry's id is provisional: its technique
        // is resolved (the file's first when the name carried none) but its ordinal
        // is only a hint, settled by finalizeOrdinal under the lock. nullopt when
        // the shader cannot be found.
        std::optional<EffectEntry> buildEntry(const std::string& instanceName, const std::string& effectType,
                                              Config* root, uint32_t width, uint32_t height) const;
        EffectEntry buildReshade(const EffectId& id, const std::string& legacyName, const std::string& path,
                                 Config* root, uint32_t width, uint32_t height) const;

        // Internal helpers (assume mutex is held)
        EffectEntry*       findEntry(const std::string& effectName);
        const EffectEntry* findEntry(const std::string& effectName) const;
        // smallest ordinal >= `wanted` not taken by an entry or alias of the same technique
        uint32_t           freeOrdinal(const EffectId& id, uint32_t wanted) const;
        void               retireParameters(std::vector<std::unique_ptr<EffectParam>>&& params);
        // Puts a built entry in place of the one named `requestedName` (a re-parse)
        // or alongside the others (new), migrating names as needed.
        void               install(EffectEntry&& entry, const std::string& requestedName);
        std::string        canonicalLocked(const std::string& name) const;

        static EffectParam* findParam(EffectEntry& effect, const std::string& paramName);
        static const EffectParam* findParam(const EffectEntry& effect, const std::string& paramName);

        void applyReparse(const std::string& name, uint64_t version, const EffectCompileResult& result);
    };

} // namespace VkHook

#endif // EFFECT_REGISTRY_HPP_INCLUDED
