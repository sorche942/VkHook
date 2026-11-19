#include "effect_registry.hh"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <set>
#include <sstream>

#include "reshade_parser.hh"
#include "store.hh"
#include "config_serializer.hh"
#include "logger.hh"
#include "dlss5_config.hh"

namespace VkHook
{

    namespace
    {
        // Parameter sets kept alive after a replace, in generations. Readers
        // (ReshadeEffect::updateFrame) re-resolve their pointers on the next
        // frame after a bump; this is far more than one frame.
        constexpr size_t kRetiredParameterSets = 32;
        // Compile-worker channels: reparses use odd ones (hash << 1 | 1), chain
        // builds multiples of four, initialize this one.
        constexpr uint64_t kInitializeChannel = 2;

        bool hasFxExtension(const std::filesystem::path& path)
        {
            return lowerAscii(path.extension().string()) == ".fx";
        }

        std::string tryResolveDirectEffectPath(const std::string& value)
        {
            if (value.empty())
                return "";

            std::error_code ec;
            std::filesystem::path candidate(value);
            if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec))
            {
                if (hasFxExtension(candidate))
                    return candidate.string();
            }

            if (!candidate.has_extension())
            {
                std::filesystem::path withFx = candidate;
                withFx.replace_extension(".fx");
                if (std::filesystem::exists(withFx, ec) && std::filesystem::is_regular_file(withFx, ec))
                    return withFx.string();
            }

            return "";
        }

        // "Clarity.2" -> ("Clarity", 2): the copy suffix old instance names used.
        // Anything else -> (name, 1).
        std::pair<std::string, uint32_t> splitLegacyOrdinal(const std::string& name)
        {
            const size_t dot = name.rfind('.');
            if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size() || name.size() - dot > 4)
                return {name, 1};
            uint32_t value = 0;
            for (size_t i = dot + 1; i < name.size(); ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(name[i])))
                    return {name, 1};
                value = value * 10 + static_cast<uint32_t>(name[i] - '0');
            }
            return value >= 2 ? std::pair<std::string, uint32_t>{name.substr(0, dot), value}
                              : std::pair<std::string, uint32_t>{name, 1};
        }

        template <typename P>
        bool copyScalar(EffectParam& dst, const EffectParam& src)
        {
            auto* to = dynamic_cast<P*>(&dst);
            const auto* from = dynamic_cast<const P*>(&src);
            if (!to || !from)
                return false;
            to->value = from->value;
            return true;
        }

        template <typename P>
        bool copyVector(EffectParam& dst, const EffectParam& src)
        {
            auto* to = dynamic_cast<P*>(&dst);
            const auto* from = dynamic_cast<const P*>(&src);
            if (!to || !from || to->componentCount != from->componentCount)
                return false;
            for (uint32_t i = 0; i < to->componentCount; ++i)
                to->value[i] = from->value[i];
            return true;
        }

        // The user's current value of `src` onto the freshly parsed `dst`, when it is
        // the same kind of parameter (a re-parse must not throw away their edits).
        void carryValue(EffectParam& dst, const EffectParam& src)
        {
            if (dst.getType() != src.getType())
                return;
            copyScalar<FloatParam>(dst, src) || copyScalar<IntParam>(dst, src) || copyScalar<UintParam>(dst, src) ||
                copyScalar<BoolParam>(dst, src) || copyVector<FloatVecParam>(dst, src) ||
                copyVector<IntVecParam>(dst, src) || copyVector<UintVecParam>(dst, src);
        }

        void carryParameters(std::vector<std::unique_ptr<EffectParam>>& fresh,
                             const std::vector<std::unique_ptr<EffectParam>>& old)
        {
            for (auto& param : fresh)
                for (const auto& previous : old)
                    if (previous->name == param->name)
                    {
                        carryValue(*param, *previous);
                        break;
                    }
        }

        // A macro's saved value: `<name>@<macro>`, else the same under the legacy name.
        std::string savedMacroValue(Config* root, const std::string& name, const std::string& legacyName,
                                    const std::string& macro)
        {
            if (!root)
                return "";
            std::string value = root->getOption<std::string>(name + "@" + macro, "");
            if (value.empty() && !legacyName.empty())
                value = root->getOption<std::string>(legacyName + "@" + macro, "");
            return value;
        }
    } // anonymous namespace

    EffectRegistry::EffectRegistry(std::shared_ptr<Config> config)
        : cache(makeDefaultShaderCache()), worker(std::make_unique<EffectCompiler>(*cache))
    {
        std::lock_guard<std::mutex> lock(mutex);
        setRootLocked(std::move(config));
    }

    EffectRegistry::~EffectRegistry()
    {
        // Join the worker before the members its callbacks use are destroyed.
        worker.reset();
    }

    void EffectRegistry::refreshCatalogRoots(Config* config) const
    {
        std::vector<std::string> roots;
        if (config)
        {
            std::stringstream ss(config->getOption<std::string>("reshadeIncludePath", ""));
            std::string       dir;
            while (std::getline(ss, dir, ':'))
                if (!dir.empty())
                    roots.push_back(dir);
        }
        // loadShaderManagerConfig is cached on the file's mtime: this is a stat
        const ShaderManagerConfig shaderManager = ConfigSerializer::loadShaderManagerConfig();
        roots.insert(roots.end(), shaderManager.discoveredShaderPaths.begin(), shaderManager.discoveredShaderPaths.end());
        roots.insert(roots.end(), shaderManager.parentDirectories.begin(), shaderManager.parentDirectories.end());
        shaderCatalog.setRoots(std::move(roots));
    }

    // A reference to a shader: the built-in's id, a path, a `name = path`
    // definition, or a name looked up in the catalog.
    std::string EffectRegistry::findEffectPath(const std::string& name, Config* config) const
    {
        if (isDlss5Effect(name, config))
            return "DLSS5";
        // If caller already provides a path (absolute or relative), use it directly.
        std::string directPath = tryResolveDirectEffectPath(name);
        if (!directPath.empty())
            return directPath;

        // First check if path is directly configured (e.g. "Vibrance = /path/to/Vibrance.fx")
        const std::string configured = config->getOption<std::string>(name, "");
        std::string resolvedConfiguredPath = tryResolveDirectEffectPath(configured);
        if (!resolvedConfiguredPath.empty())
            return resolvedConfiguredPath;

        refreshCatalogRoots(config);
        std::string path = shaderCatalog.find(name);
        if (path.empty() && !configured.empty())
            path = shaderCatalog.find(configured);
        if (path.empty() && shaderCatalog.rescan())
        {
            // a shader copied in since the last scan; rescan() is rate limited
            path = shaderCatalog.find(name);
            if (path.empty() && !configured.empty())
                path = shaderCatalog.find(configured);
        }
        return path;
    }

    bool EffectRegistry::isLegacyBuiltInName(const std::string& name)
    {
        static const std::set<std::string> legacyNames = {"cas", "dls", "fxaa", "smaa", "deband", "lut"};

        std::string base = lowerAscii(name);
        // strip a numbered-instance suffix ("cas.2"), not a filename extension
        const size_t dot = base.find('.');
        if (dot != std::string::npos)
        {
            const std::string suffix = base.substr(dot + 1);
            if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(),
                                               [](unsigned char c) { return std::isdigit(c) != 0; }))
                base = base.substr(0, dot);
        }
        return legacyNames.count(base) != 0;
    }

    std::string EffectRegistry::resolveEffectPath(const std::string& name, Config* config) const
    {
        if (name.empty() || !config)
            return "";
        if (const auto id = EffectId::parse(name))
            return id->isBuiltin() ? std::string("DLSS5") : findEffectPath(id->canonicalPath, config);
        return findEffectPath(name, config);
    }

    // --- lookup helpers (mutex held) --------------------------------------------

    EffectEntry* EffectRegistry::findEntry(const std::string& effectName)
    {
        return const_cast<EffectEntry*>(std::as_const(*this).findEntry(effectName));
    }

    const EffectEntry* EffectRegistry::findEntry(const std::string& effectName) const
    {
        for (const auto& entry : entries)
            if (entry.info.name == effectName)
                return &entry;
        const auto alias = aliases.find(effectName);
        if (alias == aliases.end())
            return nullptr;
        for (const auto& entry : entries)
            if (entry.info.name == alias->second)
                return &entry;
        return nullptr;
    }

    std::string EffectRegistry::canonicalLocked(const std::string& name) const
    {
        const EffectEntry* entry = findEntry(name);
        return entry ? entry->info.name : name;
    }

    EffectParam* EffectRegistry::findParam(EffectEntry& effect, const std::string& paramName)
    {
        for (auto& param : effect.parameters)
            if (param->name == paramName)
                return param.get();
        return nullptr;
    }

    const EffectParam* EffectRegistry::findParam(const EffectEntry& effect, const std::string& paramName)
    {
        for (const auto& param : effect.parameters)
            if (param->name == paramName)
                return param.get();
        return nullptr;
    }

    uint32_t EffectRegistry::freeOrdinal(const EffectId& id, uint32_t wanted) const
    {
        uint32_t ordinal = std::max<uint32_t>(wanted, 1);
        for (;; ++ordinal)
        {
            const EffectId candidate = id.withOrdinal(ordinal);
            const std::string text = candidate.format();
            const bool taken =
                std::any_of(entries.begin(), entries.end(),
                            [&](const EffectEntry& e) { return e.info.id == candidate; }) ||
                std::any_of(aliases.begin(), aliases.end(), [&](const auto& a) { return a.second == text; });
            if (!taken)
                return ordinal;
        }
    }

    void EffectRegistry::retireParameters(std::vector<std::unique_ptr<EffectParam>>&& params)
    {
        if (params.empty())
            return;
        retiredParameters.push_back(std::move(params));
        while (retiredParameters.size() > kRetiredParameterSets)
            retiredParameters.pop_front();
    }

    // --- building (no lock) --------------------------------------------------------

    EffectEntry EffectRegistry::buildReshade(const EffectId& id, const std::string& legacyName,
                                             const std::string& path, Config* root, uint32_t width,
                                             uint32_t height) const
    {
        EffectEntry entry;
        EffectConfig& info = entry.info;
        info.id           = id;
        info.name         = id.format();
        info.legacyName   = legacyName;
        info.filePath     = path;
        info.type         = EffectType::ReShade;
        info.enabled      = true;

        // Record file modification time for change detection
        std::error_code ec;
        info.fileModTime = std::filesystem::last_write_time(path, ec);
        info.effectType  = std::filesystem::path(path).stem().string();

        // Discover the user-configurable macros first and apply the saved
        // values, so the compile below sees the same macros the runtime compile
        // will and macro-gated uniforms show up when their macro is set.
        info.preprocessorDefs = extractPreprocessorDefinitions(info.name, path, preprocessSetup(width, height, 8, {}));
        for (auto& def : info.preprocessorDefs)
        {
            const std::string savedValue = savedMacroValue(root, info.name, legacyName, def.name);
            if (!savedValue.empty())
            {
                def.value = savedValue;
                Logger::debug("EffectRegistry: loaded preprocessor def " + info.name + "@" + def.name + " = " + savedValue);
            }
        }

        // The compile is also the compile check, and its module the source of the
        // parameter list. The runtime build asks the same cache for the same key.
        EffectCompileSpec spec;
        spec.id    = id;
        spec.path  = path;
        spec.setup   = preprocessSetup(width, height, 8, info.preprocessorDefs);
        spec.codegen = codegenOptions();
        EffectCompileResult result = compileEffect(spec, *cache);

        info.techniqueNames = result.techniqueNames;
        if (info.id.technique.empty() && !info.techniqueNames.empty())
        {
            info.id.technique = info.techniqueNames.front();
            info.name         = info.id.format();
        }
        if (result.module)
        {
            entry.parameters = parametersFromModule(*result.module, info.name, root, legacyName);
        }
        if (!result.error.empty())
        {
            info.compileError = result.error;
            info.enabled      = false;  // Disable failed effects by default
            Logger::err("EffectRegistry: failed to compile " + info.name + ": " + result.error);
        }
        else
        {
            Logger::debug("EffectRegistry: loaded ReShade effect " + info.name + " with " +
                          std::to_string(entry.parameters.size()) + " parameters and " +
                          std::to_string(info.preprocessorDefs.size()) + " preprocessor defs");
        }
        for (auto& def : info.preprocessorDefs)
            def.effectName = info.name;
        return entry;
    }

    std::optional<EffectEntry> EffectRegistry::buildEntry(const std::string& instanceName,
                                                          const std::string& effectType, Config* root,
                                                          uint32_t width, uint32_t height) const
    {
        const std::string type   = effectType.empty() ? instanceName : effectType;
        const auto        parsed = EffectId::parse(instanceName);
        // Values saved under a name that is not a resolved id (an old instance name,
        // or a bare "file.fx" from a migrated preset) are read as a fallback.
        const std::string legacy = (parsed && parsed->isResolved()) ? std::string() : instanceName;

        // the native neural renderer
        if (parsed ? parsed->isBuiltin() : (isDlss5Effect(type, root) || isDlss5Effect(instanceName, root)))
        {
            EffectId id;
            id.canonicalPath = EffectId::kDlss5;
            id.ordinal = parsed ? parsed->ordinal : splitLegacyOrdinal(instanceName).second;
            return dlss5EffectEntry(id, legacy, root);
        }

        // An id names its file exactly; a legacy name resolves through its type
        // hint, then its own text (definition, stem or file name).
        const bool legacyBuiltIn = isLegacyBuiltInName(type) || isLegacyBuiltInName(instanceName);
        std::string path;
        if (parsed)
        {
            path = findEffectPath(parsed->canonicalPath, root);
            if (path.empty() && !effectType.empty())
                path = findEffectPath(effectType, root);
        }
        else
        {
            path = findEffectPath(type, root);
            if (path.empty() && type != instanceName)
                path = findEffectPath(instanceName, root);
        }
        if (path.empty() || !std::filesystem::exists(path))
        {
            // a legacy built-in has nothing to resolve, so dropping it silently
            // keeps the ghost out of the log
            bool first = false;
            {
                std::lock_guard<std::mutex> lock(warnMutex);
                first = warnedMissing.insert(instanceName).second;
            }
            if (!legacyBuiltIn && first)
                Logger::warn("EffectRegistry: could not find effect file for: " + instanceName);
            return std::nullopt;
        }
        {
            std::lock_guard<std::mutex> lock(warnMutex);
            warnedMissing.erase(instanceName);
        }

        EffectId id;
        id.canonicalPath = std::filesystem::path(path).filename().string();
        if (parsed)
        {
            id.technique = parsed->technique;
            id.ordinal   = parsed->ordinal;
        }
        else
            id.ordinal = splitLegacyOrdinal(instanceName).second;
        return buildReshade(id, legacy, path, root, width, height);
    }

    // --- initialization --------------------------------------------------------------

    void EffectRegistry::setRootLocked(std::shared_ptr<Config> config)
    {
        // A reader may have loaded the raw pointer an instant before the swap; the
        // old owner lingers for a few swaps, like retired parameter sets.
        if (rootOwner)
        {
            retiredRoots.push_back(std::move(rootOwner));
            while (retiredRoots.size() > kRetiredParameterSets)
                retiredRoots.pop_front();
        }
        rootOwner = std::move(config);
        rootConfig.store(rootOwner.get(), std::memory_order_release);
    }

    void EffectRegistry::initialize(std::shared_ptr<Config> config)
    {
        if (!config)
            return;
        uint64_t version;
        {
            std::lock_guard<std::mutex> lock(mutex);
            setRootLocked(config);
            version = ++initVersion;
            pendingEnabled.clear();
        }
        {
            std::lock_guard<std::mutex> lock(parseMutex);
            ++parsesPending;
            ++initsPending;
        }
        // The work runs in the completion callback of an empty chain: the worker
        // is the one thread allowed to parse and compile. A newer initialize
        // supersedes a queued one on this channel (its callback sees `cancelled`).
        ChainCompileSpec chain;
        chain.channel = kInitializeChannel;
        worker->submit(std::move(chain), [this, config = std::move(config), version](const ChainCompileResult& result) {
            if (!result.cancelled)
                buildAll(config, version);
            std::lock_guard<std::mutex> lock(parseMutex);
            --initsPending;
            if (--parsesPending == 0)
                parseIdle.notify_all();
        });
    }

    bool EffectRegistry::initializing() const
    {
        std::lock_guard<std::mutex> lock(parseMutex);
        return initsPending > 0;
    }

    // Runs on the compile worker.
    void EffectRegistry::buildAll(const std::shared_ptr<Config>& configOwner, uint64_t version)
    {
        Config* config = configOwner.get();
        uint32_t width;
        uint32_t height;
        {
            std::lock_guard<std::mutex> lock(mutex);
            width  = compileWidth;
            height = compileHeight;
        }

        const std::vector<std::string> effectNames = config->getOption<std::vector<std::string>>("effects");
        const std::vector<std::string> disabledEffects = config->getOption<std::vector<std::string>>("disabledEffects");
        const std::set<std::string> disabledSet(disabledEffects.begin(), disabledEffects.end());

        // Every effect is parsed and compile-checked here, which reads files: no lock
        // is held, and the finished list replaces the old one in one step.
        std::list<EffectEntry> built;
        std::unordered_map<std::string, std::string> builtAliases;
        // the chain the config names, minus the names with no installed shader (a
        // stale config must not seed a ghost), as the entries' ids
        std::vector<std::string> seeded;
        auto taken = [&](const EffectId& id) {
            return std::any_of(built.begin(), built.end(), [&](const EffectEntry& e) { return e.info.id == id; });
        };
        for (const auto& name : effectNames)
        {
            auto entry = buildEntry(name, "", config, width, height);
            if (!entry)
                continue;

            EffectId id = entry->info.id;
            uint32_t ordinal = std::max<uint32_t>(id.ordinal, 1);
            while (taken(id.withOrdinal(ordinal)))
                ++ordinal;
            if (ordinal != id.ordinal)
            {
                id.ordinal = ordinal;
                entry->info.id = id;
                entry->info.name = id.format();
                for (auto& def : entry->info.preprocessorDefs)
                    def.effectName = entry->info.name;
                for (auto& param : entry->parameters)
                    param->effectName = entry->info.name;
            }
            if (entry->info.name != name)
                builtAliases[name] = entry->info.name;
            // Set enabled state based on disabledEffects list
            if (disabledSet.count(name) || disabledSet.count(entry->info.name))
                entry->info.enabled = false;
            seeded.push_back(entry->info.name);
            built.push_back(std::move(*entry));
        }

        std::lock_guard<std::mutex> lock(mutex);
        if (version != initVersion)
            return;  // a newer initialize is already on its way
        for (auto& old : entries)
            retireParameters(std::move(old.parameters));
        entries.swap(built);
        aliases.swap(builtAliases);
        // enabled states the control step set while this was building
        for (const auto& [key, enabled] : pendingEnabled)
            if (EffectEntry* entry = findEntry(key))
                entry->info.enabled = enabled;
        pendingEnabled.clear();
        // the first build also seeds the chain; later ones keep the selection the
        // control step made for the new config
        if (!initializedFromConfig.load(std::memory_order_acquire))
        {
            selectedEffects = std::move(seeded);
            initializedFromConfig.store(true, std::memory_order_release);
        }
        for (auto& name : selectedEffects)
            name = canonicalLocked(name);
        paramGeneration.fetch_add(1, std::memory_order_release);
        Logger::debug("EffectRegistry: initialized " + std::to_string(entries.size()) + " effects");
    }

    ReshadePreprocessSetup EffectRegistry::preprocessSetup(uint32_t bufferWidth,
                                                           uint32_t bufferHeight,
                                                           uint32_t colorBitDepth,
                                                           std::vector<PreprocessorDefinition> userDefs) const
    {
        ReshadePreprocessSetup setup;
        setup.bufferWidth = bufferWidth;
        setup.bufferHeight = bufferHeight;
        setup.colorBitDepth = colorBitDepth;
        setup.userDefs = std::move(userDefs);
        setup.includePaths = ConfigSerializer::loadShaderManagerConfig().discoveredShaderPaths;
        return setup;
    }

    void EffectRegistry::setCodegenOptions(const ReshadeCodegenOptions& options)
    {
        std::lock_guard<std::mutex> lock(mutex);
        codegen = options;
    }

    ReshadeCodegenOptions EffectRegistry::codegenOptions() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return codegen;
    }

    void EffectRegistry::setCompileExtent(uint32_t width, uint32_t height)
    {
        std::lock_guard<std::mutex> lock(mutex);
        compileWidth = width;
        compileHeight = height;
    }

    // --- queries ----------------------------------------------------------------------

    std::vector<EffectConfig> EffectRegistry::getAllEffects() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<EffectConfig> snapshot;
        snapshot.reserve(entries.size());
        for (const auto& entry : entries)
            snapshot.push_back(entry.info);
        return snapshot;
    }

    std::vector<std::unique_ptr<EffectParam>> EffectRegistry::getAllParameters() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::unique_ptr<EffectParam>> params;

        for (const auto& entry : entries)
            for (const auto& p : entry.parameters)
                params.push_back(p->clone());

        return params;
    }

    void EffectRegistry::setEffectEnabled(const std::string& effectName, bool enabled)
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (EffectEntry* entry = findEntry(effectName))
            entry->info.enabled = enabled;
        else
            pendingEnabled[effectName] = enabled;
    }

    bool EffectRegistry::isEffectEnabled(const std::string& effectName) const
    {
        std::lock_guard<std::mutex> lock(mutex);

        const EffectEntry* entry = findEntry(effectName);
        return entry ? entry->info.enabled : false;
    }

    std::optional<EffectParam*> EffectRegistry::getParameter(const std::string& effectName, const std::string& paramName)
    {
        std::lock_guard<std::mutex> lock(mutex);

        EffectEntry* entry = findEntry(effectName);
        if (!entry)
            return std::nullopt;
        if (EffectParam* param = findParam(*entry, paramName))
            return param;
        return std::nullopt;
    }

    std::optional<const EffectParam*> EffectRegistry::getParameter(const std::string& effectName, const std::string& paramName) const
    {
        std::lock_guard<std::mutex> lock(mutex);

        const EffectEntry* entry = findEntry(effectName);
        if (!entry)
            return std::nullopt;
        if (const EffectParam* param = findParam(*entry, paramName))
            return param;
        return std::nullopt;
    }

    std::vector<EffectParam*> EffectRegistry::getParametersForEffect(const std::string& effectName)
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<EffectParam*> result;

        EffectEntry* entry = findEntry(effectName);
        if (!entry)
            return result;

        for (auto& param : entry->parameters)
            result.push_back(param.get());

        return result;
    }

    bool EffectRegistry::hasEffect(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return findEntry(name) != nullptr;
    }

    std::string EffectRegistry::canonicalName(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return canonicalLocked(name);
    }

    std::string EffectRegistry::getEffectFilePath(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        const EffectEntry* entry = findEntry(name);
        return entry ? entry->info.filePath : "";
    }

    std::string EffectRegistry::getEffectType(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        const EffectEntry* entry = findEntry(name);
        return entry ? entry->info.effectType : "";
    }

    bool EffectRegistry::hasEffectFailed(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        const EffectEntry* entry = findEntry(name);
        return entry ? entry->info.hasFailed() : false;
    }

    std::string EffectRegistry::getEffectError(const std::string& name) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        const EffectEntry* entry = findEntry(name);
        return entry ? entry->info.compileError : "";
    }

    void EffectRegistry::setEffectError(const std::string& name, const std::string& error)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (EffectEntry* entry = findEntry(name))
        {
            entry->info.compileError = error;
            entry->info.enabled = false;  // Disable failed effects
        }
    }

    // --- mutation --------------------------------------------------------------------

    // Settles the name of a freshly built entry and puts it in the list: in place of
    // the entry `requestedName` already names (a re-parse, which keeps that entry's
    // enabled state, ordinal and current values), or as a new one.
    void EffectRegistry::install(EffectEntry&& entry, const std::string& requestedName)
    {
        EffectConfig& info = entry.info;
        EffectEntry*  existing = findEntry(requestedName);

        if (existing)
        {
            const EffectConfig& old = existing->info;
            // the entry keeps its own identity across a re-parse; a technique it
            // could not resolve before may resolve now
            EffectId id = old.id;
            if (id.technique.empty() && !id.isBuiltin())
                id.technique = info.id.technique;
            if (!id.technique.empty() && !old.id.technique.empty() && !info.techniqueNames.empty() &&
                std::find(info.techniqueNames.begin(), info.techniqueNames.end(), id.technique) == info.techniqueNames.end())
                info.compileError = info.compileError.empty() ? "technique '" + id.technique + "' not found in " + info.filePath
                                                              : info.compileError;
            info.id      = id;
            info.name    = id.format();
            info.enabled = old.enabled;
            if (info.legacyName.empty())
                info.legacyName = old.legacyName;
            carryParameters(entry.parameters, existing->parameters);
            for (auto& def : info.preprocessorDefs)
                for (const auto& previous : old.preprocessorDefs)
                    if (previous.name == def.name)
                        def.value = previous.value;
            entry.macroVersion = existing->macroVersion + 1;
        }
        else
        {
            EffectId id = info.id;
            // a name that already was an id keeps its ordinal; a migrated one takes
            // the smallest free copy number at or after the number it carried
            id.ordinal = freeOrdinal(id, id.ordinal);
            info.id    = id;
            info.name  = id.format();
            if (auto pending = pendingEnabled.find(requestedName); pending != pendingEnabled.end())
            {
                info.enabled = pending->second;
                pendingEnabled.erase(pending);
            }
            else if (auto byName = pendingEnabled.find(info.name); byName != pendingEnabled.end())
            {
                info.enabled = byName->second;
                pendingEnabled.erase(byName);
            }
        }

        for (auto& def : info.preprocessorDefs)
            def.effectName = info.name;
        for (auto& param : entry.parameters)
            param->effectName = info.name;

        const std::string finalName = info.name;
        if (existing)
        {
            const std::string oldName = existing->info.name;
            retireParameters(std::move(existing->parameters));
            *existing = std::move(entry);
            if (oldName != finalName)
            {
                for (auto& alias : aliases)
                    if (alias.second == oldName)
                        alias.second = finalName;
                aliases[oldName] = finalName;
                std::replace(selectedEffects.begin(), selectedEffects.end(), oldName, finalName);
            }
        }
        else
            entries.push_back(std::move(entry));

        if (requestedName != finalName)
        {
            aliases[requestedName] = finalName;
            std::replace(selectedEffects.begin(), selectedEffects.end(), requestedName, finalName);
        }
        paramGeneration.fetch_add(1, std::memory_order_release);
    }

    void EffectRegistry::ensureEffect(const std::string& instanceName, const std::string& effectType)
    {
        Config* root = rootConfig.load(std::memory_order_acquire);
        if (!root)
            return;

        // The shader's timestamp is read before the lock: it is I/O. A DLSS5 entry
        // has no file; the first build is all it needs.
        const auto parsed = EffectId::parse(instanceName);
        const std::string type = effectType.empty() ? instanceName : effectType;
        const bool builtin = parsed ? parsed->isBuiltin() : (isDlss5Effect(type, root) || isDlss5Effect(instanceName, root));

        std::string path;
        std::error_code ec;
        std::filesystem::file_time_type currentModTime{};
        if (!builtin)
        {
            path = parsed ? findEffectPath(parsed->canonicalPath, root) : findEffectPath(type, root);
            if (!path.empty())
                currentModTime = std::filesystem::last_write_time(path, ec);
        }

        uint32_t width;
        uint32_t height;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const EffectEntry* existing = findEntry(instanceName);
            // File unchanged, keep existing
            if (existing && (builtin || path.empty() || ec || currentModTime == existing->info.fileModTime))
                return;
            width  = compileWidth;
            height = compileHeight;
        }

        // Parse and compile-check with no lock held, then swap the result in. Two
        // threads can both get here for one name; the later install replaces.
        std::optional<EffectEntry> built = buildEntry(instanceName, effectType, root, width, height);
        if (!built)
            return;

        std::lock_guard<std::mutex> lock(mutex);
        if (findEntry(instanceName))
            Logger::info("EffectRegistry: shader file changed on disk, re-parsed: " + instanceName);
        install(std::move(*built), instanceName);
    }

    void EffectRegistry::removeEffect(const std::string& name)
    {
        // Validate input
        if (name.empty())
        {
            Logger::warn("EffectRegistry::removeEffect called with empty name");
            return;
        }

        // Use try-catch for crash protection during removal
        try
        {
            std::lock_guard<std::mutex> lock(mutex);

            Logger::debug("EffectRegistry: removing effect '" + name + "'");

            const std::string canonical = canonicalLocked(name);
            bool removed = false;
            for (auto it = entries.begin(); it != entries.end(); ++it)
            {
                if (it->info.name != canonical)
                    continue;
                retireParameters(std::move(it->parameters));
                entries.erase(it);
                removed = true;
                break;
            }
            for (auto it = aliases.begin(); it != aliases.end();)
                it = (it->first == name || it->second == canonical) ? aliases.erase(it) : std::next(it);
            pendingEnabled.erase(name);
            pendingEnabled.erase(canonical);

            paramGeneration.fetch_add(1, std::memory_order_release);

            if (removed)
                Logger::info("EffectRegistry: removed effect '" + name + "'");
            else
                Logger::debug("EffectRegistry: effect '" + name + "' not found for removal");
        }
        catch (const std::exception& e)
        {
            Logger::err("EffectRegistry::removeEffect exception for '" + name + "': " + e.what());
        }
        catch (...)
        {
            Logger::err("EffectRegistry::removeEffect unknown exception for '" + name + "'");
        }
    }

    std::vector<PreprocessorDefinition> EffectRegistry::getPreprocessorDefs(const std::string& effectName) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        const EffectEntry* entry = findEntry(effectName);
        return entry ? entry->info.preprocessorDefs : std::vector<PreprocessorDefinition>{};
    }

    void EffectRegistry::setPreprocessorDefValue(const std::string& effectName, const std::string& macroName, const std::string& value)
    {
        EffectCompileSpec spec;
        std::string       name;
        uint64_t          version = 0;
        uint32_t          width = 0;
        uint32_t          height = 0;
        std::vector<PreprocessorDefinition> defs;
        {
            std::lock_guard<std::mutex> lock(mutex);
            EffectEntry* entry = findEntry(effectName);
            if (!entry)
                return;

            auto def = std::find_if(entry->info.preprocessorDefs.begin(), entry->info.preprocessorDefs.end(),
                                    [&](const PreprocessorDefinition& d) { return d.name == macroName; });
            if (def == entry->info.preprocessorDefs.end() || def->value == value)
                return;
            def->value = value;
            if (entry->info.type != EffectType::ReShade || entry->info.filePath.empty())
                return;

            name       = entry->info.name;
            version    = ++entry->macroVersion;
            spec.id    = entry->info.id;
            spec.path  = entry->info.filePath;
            defs       = entry->info.preprocessorDefs;
            width      = compileWidth;
            height     = compileHeight;
            spec.codegen = codegen;
        }
        // the setup reads the shader-manager config: not under the registry lock
        spec.setup = preprocessSetup(width, height, 8, std::move(defs));

        {
            std::lock_guard<std::mutex> lock(parseMutex);
            ++parsesPending;
        }
        ChainCompileSpec chain;
        chain.effects.push_back(std::move(spec));
        // one channel per effect: a newer edit supersedes this effect's older re-parse
        // and nobody else's
        chain.channel = (std::hash<std::string>{}(name) << 1) | 1;
        worker->submit(std::move(chain), [this, name, version](const ChainCompileResult& result) {
            if (!result.cancelled && !result.effects.empty())
                applyReparse(name, version, result.effects.front());
            std::lock_guard<std::mutex> lock(parseMutex);
            if (--parsesPending == 0)
                parseIdle.notify_all();
        });
    }

    // Runs on the compile worker.
    void EffectRegistry::applyReparse(const std::string& name, uint64_t version, const EffectCompileResult& result)
    {
        std::string legacy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const EffectEntry* entry = findEntry(name);
            if (!entry || entry->macroVersion != version)
                return;
            legacy = entry->info.legacyName;
        }

        std::vector<std::unique_ptr<EffectParam>> fresh;
        if (result.module)
            fresh = parametersFromModule(*result.module, name, rootConfig.load(std::memory_order_acquire), legacy);

        std::lock_guard<std::mutex> lock(mutex);
        EffectEntry* entry = findEntry(name);
        if (!entry || entry->macroVersion != version)
            return;  // edited again, replaced or removed while this compiled

        entry->info.compileError = result.error;
        if (!result.module)
            return;  // keep the parameters the entry has: the macro value is what is broken

        carryParameters(fresh, entry->parameters);
        for (auto& param : fresh)
            param->effectName = entry->info.name;
        retireParameters(std::move(entry->parameters));
        entry->parameters = std::move(fresh);
        if (!result.techniqueNames.empty())
            entry->info.techniqueNames = result.techniqueNames;
        // a macro the new setting reads that was not seen before becomes editable
        for (const auto& [macro, defaultValue] : result.usedMacros)
        {
            const bool known = std::any_of(entry->info.preprocessorDefs.begin(), entry->info.preprocessorDefs.end(),
                                           [&](const PreprocessorDefinition& d) { return d.name == macro; });
            if (known)
                continue;
            PreprocessorDefinition def;
            def.name         = macro;
            def.effectName   = entry->info.name;
            def.defaultValue = defaultValue.empty() ? "1" : defaultValue;
            def.value        = savedMacroValue(rootConfig.load(std::memory_order_acquire), entry->info.name,
                                               entry->info.legacyName, macro);
            if (def.value.empty())
                def.value = def.defaultValue;
            entry->info.preprocessorDefs.push_back(std::move(def));
        }
        paramGeneration.fetch_add(1, std::memory_order_release);
    }

    bool EffectRegistry::waitForParses(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(parseMutex);
        return parseIdle.wait_for(lock, timeout, [this] { return parsesPending == 0; });
    }

    // --- selection ---------------------------------------------------------------------

    std::vector<std::string> EffectRegistry::getSelectedEffects() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> snapshot;
        snapshot.reserve(selectedEffects.size());
        for (const auto& name : selectedEffects)
            snapshot.push_back(canonicalLocked(name));
        return snapshot;
    }

    void EffectRegistry::setSelectedEffects(const std::vector<std::string>& effects)
    {
        std::lock_guard<std::mutex> lock(mutex);
        selectedEffects = effects;
    }

    void EffectRegistry::clearSelectedEffects()
    {
        std::lock_guard<std::mutex> lock(mutex);
        selectedEffects.clear();
    }

} // namespace VkHook
