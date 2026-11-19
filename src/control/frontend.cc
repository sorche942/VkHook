#include "control/frontend.hh"

#include "control/preset_import.hh"
#include "present/swapchain.hh"

#include "keyboard_input.hh"
#include "input_session.hh"
#include "config.hh"
#include "config_serializer.hh"
#include "store.hh"
#include "effects/effect_registry.hh"
#include "logger.hh"
#include "util.hh"

#include <algorithm>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <unordered_map>

namespace VkHook
{
    namespace
    {
        // latched at settings load
        bool g_depthCaptureLatched = false;
        bool g_depthCaptureLatchedValid = false;
    }

    // reads the persisted setting once
    void latchDepthCaptureSetting()
    {
        g_depthCaptureLatched = ConfigStore::instance().getDepthCapture();
        g_depthCaptureLatchedValid = true;
    }

    // what the hooks ask instead of the live setting; see the latch above
    bool depthCaptureActive()
    {
        if (!g_depthCaptureLatchedValid)
            latchDepthCaptureSetting();
        return g_depthCaptureLatched;
    }

    // set by initConfigs: the config file the process selects (empty: VkHook.conf alone),
    // and whether it has a shader profile to lay over it. Written once, before any
    // device exists; each device's registry applies the profile (resolving shader names
    // needs a registry).
    static std::string initialConfigPath;
    static bool        initialShaderProfilePending = false;

    static std::once_flag initConfigsOnceFlag;

    // Detected game info (set once at init, used by overlay for profiles)
    std::string detectedGameName;
    std::string activeProfileName;
    std::string activeProfilePath;
    std::string activeShaderProfilePath;

    static constexpr int64_t RESIZE_DEBOUNCE_MS = 200;

    // The process's selected config as a layer over `base`
    static std::shared_ptr<Config> makeProfileConfig(Config& base)
    {
        auto profile = std::make_shared<Config>(initialConfigPath);
        profile->setFallback(&base);
        return profile;
    }

    bool handleKeyPress(InputSession* session, uint32_t keySymbol, bool& wasPressed)
    {
        if (session && session->isKeyPressed(keySymbol))
        {
            if (!wasPressed)
            {
                wasPressed = true;
                return true;
            }
        }
        else
        {
            wasPressed = false;
        }
        return false;
    }

    DeviceControlState::DeviceControlState(LogicalDevice* device)
        : baseConfig(std::make_shared<Config>()),
          config(initialConfigPath.empty() ? baseConfig : makeProfileConfig(*baseConfig)),
          effectRegistry(config), overlay(device, &effectRegistry)
    {
        const auto settings = ConfigStore::instance().snapshot();
        keySymbol        = convertToKeySym(settings->toggleKey);
        reloadKeySymbol  = convertToKeySym(settings->reloadKey);
        overlayKeySymbol = convertToKeySym(settings->overlayKey);
        presentEffect    = settings->enableOnLaunch;
        lastEffectsOn.store(presentEffect, std::memory_order_relaxed);

        // Parses and runtime compiles of one device share cache keys only when they
        // use the same codegen options.
        effectRegistry.setCodegenOptions(runtimeCodegenOptions(device->isNvidiaGpu));
        if (initialShaderProfilePending)
            applyShaderProfile(config.get(), effectRegistry, activeShaderProfilePath);
        // the parses of the chain's effects run on the registry's worker, not here
        effectRegistry.initialize(config);
    }

    void DeviceControlState::noteResize()
    {
        lastResizeNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count(),
                           std::memory_order_relaxed);
        resizePending.store(true, std::memory_order_release);
    }

    namespace
    {
        // Clears LogicalDevice::frontendBusy when the frame ends, on every path.
        struct FrontendBusyGuard
        {
            LogicalDevice* device;
            ~FrontendBusyGuard() { device->frontendBusy.store(false, std::memory_order_release); }
        };

        // Effects of the registry that are switched on, in chain order.
        std::vector<std::string> activeEffectNames(EffectRegistry& registry)
        {
            std::vector<std::string> active;
            for (const auto& effectName : registry.getSelectedEffects())
            {
                if (registry.isEffectEnabled(effectName))
                    active.push_back(effectName);
            }
            return active;
        }

        // The registry's chain becomes `effects`; the ones in `disabled` stay listed but off.
        void selectEffects(EffectRegistry& registry, const std::vector<std::string>& effects,
                           const std::vector<std::string>& disabled)
        {
            registry.setSelectedEffects(effects);
            const std::set<std::string> disabledSet(disabled.begin(), disabled.end());
            for (const auto& effectName : effects)
                registry.setEffectEnabled(effectName, disabledSet.find(effectName) == disabledSet.end());
        }

        // The selected effects of the config just loaded
        void selectEffectsFromConfig(DeviceControlState& state)
        {
            selectEffects(state.effectRegistry, state.config->getOption<std::vector<std::string>>("effects", {}),
                          state.config->getOption<std::vector<std::string>>("disabledEffects", {}));
        }

        // Asks for a rebuild of every chain with the registry's enabled effects. While
        // an initialize() is still building the entries, the enabled set is not known
        // yet: the request is kept and made once it finishes (the control step retries).
        void requestActiveChainReload(LogicalDevice* logicalDevice, DeviceControlState& state)
        {
            invalidateAvailableEffects(state);
            if (state.effectRegistry.initializing())
            {
                state.reloadAfterInitialize = true;
                return;
            }
            state.reloadAfterInitialize = false;
            std::vector<std::string> effects = activeEffectNames(state.effectRegistry);
            std::lock_guard<std::mutex> lock(logicalDevice->stateLock);
            logicalDevice->pendingChainEffects = std::move(effects);
            logicalDevice->pendingChainReload = true;
        }
    }

    // Initialize configs: base (VkHook.conf) + current (from game profile / env / default)
    void initConfigs()
    {
        std::call_once(initConfigsOnceFlag, []()
        {
            // Ensure config directory exists for later saves
            {
                std::string baseDir = ConfigSerializer::getBaseConfigDir();
                if (!baseDir.empty())
                    mkdir(baseDir.c_str(), 0755);
            }

            // keep the config dir's version file in step with the built library
            ConfigSerializer::ensureVersionFile();

            // Initialize the settings store (single source of truth for settings);
            // it runs the pending config migrations before anything scans for shaders
            ConfigStore::instance().initialize();

            // latch depth capture for the process: the hooks run inside the game's
            // command recording, so a mid-frame re-arm is off the table
            latchDepthCaptureSetting();

            // (VkHook.conf is loaded per device, as `DeviceControlState::baseConfig`)

            // Detect the game executable
            detectedGameName = ConfigSerializer::detectGameName();

            // Determine current config path (priority order):
            // 1. VKHOOK_CONFIG_FILE env var (explicit override)
            // 2. Per-game profile (auto-created if needed)
            // 3. Legacy default_config file
            // 4. Base VkHook.conf
            std::string currentConfigPath;

            const char* envConfig = std::getenv("VKHOOK_CONFIG_FILE");
            if (envConfig && *envConfig)
            {
                currentConfigPath = envConfig;
                Logger::info("config from env: " + currentConfigPath);
            }
            else if (!detectedGameName.empty())
            {
                // Auto-create profile for this game if needed, then load it
                activeProfileName = "default";
                activeProfilePath = ConfigSerializer::getProfilePath(detectedGameName);

                // Ensure the profile file exists
                struct stat st;
                if (stat(activeProfilePath.c_str(), &st) != 0)
                {
                    // Profile doesn't exist yet — create it
                    activeProfilePath = ConfigSerializer::ensureGameProfile(detectedGameName);
                }

                if (!activeProfilePath.empty())
                {
                    if (ConfigSerializer::listShaderProfilesForGame(detectedGameName).empty())
                        ConfigSerializer::createShaderProfile(detectedGameName, "default");
                    const auto shaderProfiles = ConfigSerializer::listShaderProfilesForGame(detectedGameName);
                    std::string lastShaderProfile = ConfigSerializer::getLastShaderProfile(detectedGameName);
                    if (std::find(shaderProfiles.begin(), shaderProfiles.end(), lastShaderProfile) == shaderProfiles.end())
                        lastShaderProfile = "default";
                    activeShaderProfilePath = ConfigSerializer::getShaderProfilePath(detectedGameName, lastShaderProfile);
                    currentConfigPath = activeProfilePath;
                    Logger::info("game: " + detectedGameName + " | config: " + activeProfilePath);
                }
            }

            // Fallback: legacy default_config
            if (currentConfigPath.empty())
            {
                std::string defaultName = ConfigSerializer::getDefaultConfig();
                if (!defaultName.empty())
                    currentConfigPath = ConfigSerializer::getConfigsDir() + "/" + defaultName + ".conf";
            }

            // The config file the process selects; each device loads its own Config
            // objects from it (see DeviceControlState)
            if (!currentConfigPath.empty())
            {
                std::ifstream file(currentConfigPath);
                if (file.good())
                {
                    initialConfigPath = currentConfigPath;
                    initialShaderProfilePending = !detectedGameName.empty();
                    Logger::info("current config: " + currentConfigPath);
                }
            }
        });
    }

    // Switch to a new config (called from overlay)
    void switchConfig(DeviceControlState& state, const std::string& configPath, const std::string& shaderPath)
    {
        Logger::info("switching to config: " + configPath);

        // Create new config from file (starts with no overrides)
        state.config = std::make_shared<Config>(configPath);
        state.config->setFallback(state.baseConfig.get());
        applyShaderProfile(state.config.get(), state.effectRegistry, shaderPath);

        // Also clear any overrides on the base config to avoid stale values
        if (state.baseConfig)
            state.baseConfig->clearOverrides();

        // Rebuild the registry for the new config on its worker
        state.effectRegistry.initialize(state.config);

        Logger::info("switched to config: " + configPath);
    }

    // Helper function to get available effects separated by source (uses cache)
    void getAvailableEffects(DeviceControlState& state,
                             std::vector<std::string>& currentConfigEffects,
                             std::vector<std::string>& defaultConfigEffects,
                             std::map<std::string, std::string>& effectPaths)
    {
        Config* const config     = state.config.get();
        Config* const baseConfig = state.baseConfig.get();
        CachedEffectsData& cachedEffects = state.cachedEffects;

        // Use cache if available and config hasn't changed
        if (cachedEffects.initialized && cachedEffects.configPath == config->getConfigFilePath())
        {
            currentConfigEffects = cachedEffects.currentConfigEffects;
            defaultConfigEffects = cachedEffects.defaultConfigEffects;
            effectPaths = cachedEffects.effectPaths;
            return;
        }

        currentConfigEffects.clear();
        defaultConfigEffects.clear();
        effectPaths.clear();

        // Collect all known effect names (to avoid duplicates)
        std::set<std::string> knownEffects;

        // Get effect definitions from current config
        auto configEffects = config->getEffectDefinitions();
        for (const auto& [name, path] : configEffects)
        {
            currentConfigEffects.push_back(name);
            effectPaths[name] = path;
            knownEffects.insert(name);
        }

        // Also load effect definitions from the base config file (VkHook.conf)
        if (baseConfig && baseConfig->getConfigFilePath() != config->getConfigFilePath())
        {
            auto defaultEffects = baseConfig->getEffectDefinitions();
            for (const auto& [name, path] : defaultEffects)
            {
                if (knownEffects.find(name) == knownEffects.end())
                {
                    defaultConfigEffects.push_back(name);
                    effectPaths[name] = path;
                    knownEffects.insert(name);
                }
            }
        }

        // Helper: scan a list of directories for .fx files and add discovered effects
        auto scanDirsForFx = [&](const std::vector<std::string>& dirs)
        {
            for (const auto& dir : dirs)
            {
                try
                {
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(
                             dir, std::filesystem::directory_options::skip_permission_denied))
                    {
                        if (!entry.is_regular_file())
                            continue;

                        std::string ext = entry.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        if (ext != ".fx")
                            continue;

                        std::string effectName = entry.path().stem().string();

                        if (knownEffects.find(effectName) != knownEffects.end())
                        {
                            // config-sourced effects only carry their filename, so
                            // record the resolved path the package view needs
                            auto pathIt = effectPaths.find(effectName);
                            if (pathIt != effectPaths.end() &&
                                pathIt->second.find('/') == std::string::npos &&
                                pathIt->second.find('\\') == std::string::npos)
                                pathIt->second = entry.path().string();
                            continue;
                        }

                        defaultConfigEffects.push_back(effectName);
                        effectPaths[effectName] = entry.path().string();
                        knownEffects.insert(effectName);
                    }
                }
                catch (const std::filesystem::filesystem_error& e)
                {
                    Logger::warn("failed to scan shader path " + dir + ": " + std::string(e.what()));
                }
            }
        };

        // Collect ALL directories to scan for .fx files
        std::set<std::string> allScanDirs;  // deduplicate

        // 1. reshadeIncludePath from config (colon-separated)
        //    This is the most common way users configure shader paths,
        //    but getAvailableEffects was previously not scanning it at all.
        std::string includePath = config->getOption<std::string>("reshadeIncludePath", "");
        if (!includePath.empty())
        {
            std::stringstream ss(includePath);
            std::string dir;
            while (std::getline(ss, dir, ':'))
            {
                if (!dir.empty())
                    allScanDirs.insert(dir);
            }
        }
        // Also check the base config's reshadeIncludePath (via fallback)
        if (baseConfig && baseConfig->getConfigFilePath() != config->getConfigFilePath())
        {
            std::string baseInclude = baseConfig->getOption<std::string>("reshadeIncludePath", "");
            if (!baseInclude.empty())
            {
                std::stringstream ss(baseInclude);
                std::string dir;
                while (std::getline(ss, dir, ':'))
                {
                    if (!dir.empty())
                        allScanDirs.insert(dir);
                }
            }
        }

        // 2. Shader manager paths
        ShaderManagerConfig shaderMgrConfig = ConfigSerializer::loadShaderManagerConfig();

        // 2a. Discovered shader paths (previously the only source scanned)
        for (const auto& p : shaderMgrConfig.discoveredShaderPaths)
            allScanDirs.insert(p);

        // 2b. Parent directories — re-scan for Shaders/ subdirectories
        //     (they may have been added after the last shader_manager.conf save,
        //      or the initial scan may have missed them)
        for (const auto& parentDir : shaderMgrConfig.parentDirectories)
        {
            try
            {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(
                         parentDir, std::filesystem::directory_options::skip_permission_denied))
                {
                    if (!entry.is_directory())
                        continue;

                    std::string dirName = entry.path().filename().string();
                    // Case-insensitive comparison for "Shaders"
                    std::transform(dirName.begin(), dirName.end(), dirName.begin(), ::tolower);
                    if (dirName == "shaders")
                        allScanDirs.insert(entry.path().string());
                }
            }
            catch (const std::filesystem::filesystem_error&) {}

            // Also scan the parent dir itself — users often place .fx files
            // directly in the parent directory without a Shaders/ subdirectory.
            allScanDirs.insert(parentDir);
        }

        // Scan all collected directories for .fx files
        scanDirsForFx({allScanDirs.begin(), allScanDirs.end()});

        if (knownEffects.insert("DLSS5").second)
        {
            defaultConfigEffects.push_back("DLSS5");
            effectPaths["DLSS5"] = "Native OpenDLSS-NR / PTX (SDR, NVIDIA optical-flow history)";
        }

        // Sort discovered effects alphabetically
        std::sort(defaultConfigEffects.begin(), defaultConfigEffects.end());

        // Update cache
        state.cachedEffects.currentConfigEffects = currentConfigEffects;
        state.cachedEffects.defaultConfigEffects = defaultConfigEffects;
        state.cachedEffects.effectPaths = effectPaths;
        state.cachedEffects.configPath = config->getConfigFilePath();
        state.cachedEffects.initialized = true;
    }

    void invalidateAvailableEffects(DeviceControlState& state)
    {
        state.cachedEffects.initialized = false;
    }

    // The add-effects page's data and the snapshot the views read this frame.
    static void updateOverlayState(DeviceControlState& state, bool effectsEnabled)
    {
        OverlayControl& overlay = state.overlay;
        const OverlayUi* ui = overlay.ui();
        if (!ui || !ui->isVisible())
            return;

        OverlayState overlayState;

        // No fallback to config - registry is the single source of truth
        // (initialized from config at first swapchain creation)

        getAvailableEffects(state, overlayState.currentConfigEffects,
                            overlayState.defaultConfigEffects, overlayState.effectPaths);
        overlayState.configPath = state.config->getConfigFilePath();

        // Cache the filename extraction — config path rarely changes
        if (overlayState.configPath != state.cachedConfigPath)
        {
            state.cachedConfigPath = overlayState.configPath;
            state.cachedConfigName = std::filesystem::path(state.cachedConfigPath).filename().string();
        }
        overlayState.configName = state.cachedConfigName;
        overlayState.effectsEnabled = effectsEnabled;

        // Ensure all selected effects are in the registry. While an initialize() is
        // building them on the worker they are all about to be, and parsing them here
        // would stall the present thread on the same work.
        if (!state.effectRegistry.initializing())
        {
            for (const auto& effectName : state.effectRegistry.getSelectedEffects())
            {
                if (state.effectRegistry.hasEffect(effectName))
                    continue;
                auto pathIt = overlayState.effectPaths.find(effectName);
                std::string effectPath = (pathIt != overlayState.effectPaths.end()) ? pathIt->second : "";
                state.effectRegistry.ensureEffect(effectName, effectPath);
            }
        }

        overlay.setAvailable(std::move(overlayState));
        overlay.publishSnapshot();
    }

    bool runFrontendFrame(LogicalDevice* logicalDevice, DeviceControlState& state, InputSession* session)
    {
        // One present thread runs the control step per device; the others reuse
        // its last decision instead of racing on the hotkey/reload state.
        if (logicalDevice->frontendBusy.exchange(true, std::memory_order_acq_rel))
            return state.lastEffectsOn.load(std::memory_order_acquire);
        FrontendBusyGuard busyGuard{logicalDevice};

        // No layer lock is held here: the step does file I/O (config change
        // checks, reloads), shader scans and overlay work. stateLock is taken
        // only for the short pending-reload accesses.
        OverlayControl& overlay = state.overlay;
        overlay.initProfiles();

        // What the user did in the overlay since the last frame
        const OverlayRequests requests = overlay.applyCommands();

        // Settings were saved (the store already published the new values)
        if (requests.settingsSaved)
        {
            const auto settings = ConfigStore::instance().snapshot();
            state.keySymbol = convertToKeySym(settings->toggleKey);
            state.reloadKeySymbol = convertToKeySym(settings->reloadKey);
            state.overlayKeySymbol = convertToKeySym(settings->overlayKey);
            if (session)
            {
                session->initBlocker(settings->overlayBlockInput);
                const OverlayUi* ui = overlay.ui();
                session->setBlocked(ui && ui->isVisible());
            }
            Logger::info("Settings reloaded from ConfigStore");
        }

        // Check if shader paths were changed (refresh available effects list)
        if (requests.shaderPathsChanged)
        {
            invalidateAvailableEffects(state);  // force re-scan of available effects
            Logger::info("Shader paths changed, effect list refreshed");
        }

        // A reload asked for while the registry was still building (a config switch)
        if (state.reloadAfterInitialize && !state.effectRegistry.initializing())
            requestActiveChainReload(logicalDevice, state);

        // Deferred chain reload armed by an earlier frame. reloadAllSwapchains
        // builds new generations off-lock and swaps them in; it neither waits for
        // the GPU nor needs a lock, so the request is simply taken and run.
        {
            std::vector<std::string> reloadEffects;
            bool                     reloadNow = false;
            {
                std::lock_guard<std::mutex> lock(logicalDevice->stateLock);
                if (logicalDevice->pendingChainReload)
                {
                    reloadEffects = std::move(logicalDevice->pendingChainEffects);
                    logicalDevice->pendingChainEffects.clear();
                    logicalDevice->pendingChainReload = false;
                    reloadNow = true;
                }
            }
            if (reloadNow)
                reloadAllSwapchains(logicalDevice, reloadEffects);
        }

        if (!state.initLogged)
        {
            Logger::info("hot-reload initialized, config: " + state.config->getConfigFilePath());
            state.initLogged = true;
        }
        if (!session && !state.noSessionLogged)
        {
            Logger::debug("present has no input session: hotkeys and the overlay key are not polled");
            state.noSessionLogged = true;
        }

        // Toggle effect on/off (keyboard)
        if (handleKeyPress(session, state.keySymbol, state.pressed))
            state.presentEffect = !state.presentEffect;

        // Hot-reload: check for key press or config file change
        bool shouldReload = false;
        bool reloadFromDisk = false;
        if (handleKeyPress(session, state.reloadKeySymbol, state.reloadPressed))
        {
            Logger::debug("reload key pressed");
            shouldReload = true;
            reloadFromDisk = true;
        }
        if (state.config->hasConfigChanged())
        {
            Logger::debug("config file changed detected");
            shouldReload = true;
            reloadFromDisk = true;
        }
        // the overlay's "Reload config from disk" takes the same path as the reload key
        if (requests.reloadFromDisk)
            reloadFromDisk = true;

        // Toggle overlay on/off
        if (handleKeyPress(session, state.overlayKeySymbol, state.overlayPressed))
        {
            if (OverlayUi* ui = overlay.ui())
                ui->toggle();
        }

        // Toggle effects on/off via overlay checkbox
        if (requests.toggleEffects)
            state.presentEffect = !state.presentEffect;

        if (requests.rebuild)
        {
            // Modified parameters live in EffectRegistry — effects pick
            // them up at reload time, so all we need to do here is
            // trigger a reload.
            shouldReload = true;
        }

        if (shouldReload)
        {
            overlay.refreshShaderProfiles();
            Logger::info("hot-reloading config and effects...");
            const auto reloadSelectedEffects = [&]() { requestActiveChainReload(logicalDevice, state); };

            // the overlay's control owns the active profile; the global is only
            // for early calls before the first frame ran.
            std::string overlayShaderPath = overlay.activeShaderProfilePath();
            if (overlayShaderPath.empty())
                overlayShaderPath = activeShaderProfilePath;

            // Check if the overlay wants to load a different config
            if (!requests.pendingConfigPath.empty())
            {
                switchConfig(state, requests.pendingConfigPath, overlayShaderPath);
                // Update the registry with the effects from the new config
                selectEffectsFromConfig(state);
                reloadSelectedEffects();
            }
            else if (requests.pendingShaderProfile)
            {
                // an empty path drops the preset's overrides
                switchConfig(state, state.config->getConfigFilePath(), requests.pendingShaderProfilePath);
                selectEffectsFromConfig(state);
                reloadSelectedEffects();
            }
            else
            {
                // only the reload key, an external file change or the overlay's
                // reload re-reads the config and re-applies the profile; a UI
                // apply rebuilds the chain from the registry, which is already
                // the truth
                if (reloadFromDisk)
                {
                    // the file is the truth again: re-read it, re-apply the profile and
                    // rebuild the registry on its worker, as a config switch does
                    state.config->reload();
                    applyShaderProfile(state.config.get(), state.effectRegistry, overlayShaderPath);
                    state.effectRegistry.initialize(state.config);
                    selectEffectsFromConfig(state);
                }
                reloadSelectedEffects();
            }
        }

        // Check for debounced resize reload (separate from config reload)
        // Only call steady_clock::now() when a resize is actually pending
        if (state.resizePending.load(std::memory_order_acquire))
        {
            const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const int64_t resizeElapsed = (nowNs - state.lastResizeNs.load(std::memory_order_relaxed)) / 1'000'000;

            if (resizeElapsed >= RESIZE_DEBOUNCE_MS && !state.effectRegistry.initializing() &&
                state.resizePending.exchange(false, std::memory_order_acq_rel))
            {
                Logger::info("debounced resize reload after " + std::to_string(resizeElapsed) + "ms");

                // Get selected effects from registry (single source of truth);
                // the reload runs at the start of the next control step.
                std::vector<std::string> effects = state.effectRegistry.getSelectedEffects();
                std::lock_guard<std::mutex> lock(logicalDevice->stateLock);
                logicalDevice->pendingChainEffects = std::move(effects);
                logicalDevice->pendingChainReload = true;
            }
        }

        updateOverlayState(state, state.presentEffect);
        state.lastEffectsOn.store(state.presentEffect, std::memory_order_release);
        return state.presentEffect;
    }
} // namespace VkHook
