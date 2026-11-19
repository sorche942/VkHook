#include "control/frontend.hh"

#include "control/preset_import.hh"
#include "present/swapchain.hh"

#include "keyboard_input.hh"
#include "keyboard_input_wayland.hh"
#include "wayland_input_common.hh"
#include "wayland_display.hh"
#include "input_blocker.hh"
#include "config.hh"
#include "config_serializer.hh"
#include "settings_manager.hh"
#include "effects/effect_registry.hh"
#include "imgui_overlay.hh"
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
        g_depthCaptureLatched = settingsManager.getDepthCapture();
        g_depthCaptureLatchedValid = true;
    }

    // what the hooks ask instead of the live setting; see the latch above
    bool depthCaptureActive()
    {
        if (!g_depthCaptureLatchedValid)
            latchDepthCaptureSetting();
        return g_depthCaptureLatched;
    }

    std::shared_ptr<Config> baseConfig = nullptr;
    std::shared_ptr<Config> config = nullptr;
    EffectRegistry effectRegistry;

    static std::once_flag initConfigsOnceFlag;

    // Detected game info (set once at init, used by overlay for profiles)
    std::string detectedGameName;
    std::string activeProfileName;
    std::string activeProfilePath;
    std::string activeShaderProfilePath;

    ResizeDebounceState resizeDebounce;
    static constexpr int64_t RESIZE_DEBOUNCE_MS = 200;

    // Cached available effects data (to avoid re-parsing config every frame)
    struct CachedEffectsData
    {
        std::vector<std::string> currentConfigEffects;
        std::vector<std::string> defaultConfigEffects;
        std::map<std::string, std::string> effectPaths;
        std::string configPath;
        bool initialized = false;
    };
    CachedEffectsData cachedEffects;

    bool handleKeyPress(uint32_t keySymbol, bool& wasPressed)
    {
        if (isKeyPressed(keySymbol))
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

    void beginInputFrame()
    {
        if (isWayland())
            beginWaylandInputFrame();
        beginKeyboardInputFrame();
    }

    DeviceControlState::DeviceControlState()
    {
        const auto settings = settingsManager.snapshot();
        keySymbol        = convertToKeySym(settings->toggleKey);
        reloadKeySymbol  = convertToKeySym(settings->reloadKey);
        overlayKeySymbol = convertToKeySym(settings->overlayKey);
        presentEffect    = settings->enableOnLaunch;
        lastEffectsOn.store(presentEffect, std::memory_order_relaxed);
    }

    namespace
    {
        // Own small mutex: the map is touched by present threads of every device
        // and by vkDestroyDevice. States are heap-allocated, so a reference
        // stays valid until releaseDeviceControl for that device.
        std::mutex deviceControlMutex;
        std::unordered_map<LogicalDevice*, std::unique_ptr<DeviceControlState>> deviceControlStates;

        // Clears LogicalDevice::frontendBusy when the frame ends, on every path.
        struct FrontendBusyGuard
        {
            LogicalDevice* device;
            ~FrontendBusyGuard() { device->frontendBusy.store(false, std::memory_order_release); }
        };
    }

    DeviceControlState& deviceControl(LogicalDevice* logicalDevice)
    {
        std::lock_guard<std::mutex> lock(deviceControlMutex);
        auto& slot = deviceControlStates[logicalDevice];
        if (!slot)
            slot = std::make_unique<DeviceControlState>();
        return *slot;
    }

    void releaseDeviceControl(LogicalDevice* logicalDevice)
    {
        std::lock_guard<std::mutex> lock(deviceControlMutex);
        deviceControlStates.erase(logicalDevice);
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

            // move shaders out of the old reshade/packages/ tree before anything
            // scans for them
            ConfigSerializer::migrateLegacyReshadePackages();

            // Initialize settings manager (single source of truth for settings)
            settingsManager.initialize();

            // latch depth capture for the process: the hooks run inside the game's
            // command recording, so a mid-frame re-arm is off the table
            latchDepthCaptureSetting();

            // Load base config (VkHook.conf) - used for paths, effect definitions
            baseConfig = std::make_shared<Config>();

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

            // Load current config if specified, otherwise use base
            if (!currentConfigPath.empty())
            {
                std::ifstream file(currentConfigPath);
                if (file.good())
                {
                    config = std::make_shared<Config>(currentConfigPath);
                    config->setFallback(baseConfig.get());
                    if (!detectedGameName.empty())
                    {
                        applyShaderProfile(config.get(), activeShaderProfilePath);
                    }
                    Logger::info("current config: " + currentConfigPath);
                }
                else
                {
                    config = baseConfig;
                }
            }
            else
            {
                config = baseConfig;
            }

            // Initialize effect registry with current config
            effectRegistry.initialize(config.get());
        });
    }

    // Switch to a new config (called from overlay)
    void switchConfig(const std::string& configPath, const std::string& shaderPath)
    {
        Logger::info("switching to config: " + configPath);

        // Create new config from file (starts with no overrides)
        config = std::make_shared<Config>(configPath);
        config->setFallback(baseConfig.get());
        applyShaderProfile(config.get(), shaderPath);

        // Also clear any overrides on the base config to avoid stale values
        if (baseConfig)
            baseConfig->clearOverrides();

        // Re-initialize registry with new config
        effectRegistry.initialize(config.get());

        Logger::info("switched to config: " + configPath);
    }

    // Helper function to get available effects separated by source (uses cache)
    void getAvailableEffects(Config* config,
                             std::vector<std::string>& currentConfigEffects,
                             std::vector<std::string>& defaultConfigEffects,
                             std::map<std::string, std::string>& effectPaths)
    {
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
        cachedEffects.currentConfigEffects = currentConfigEffects;
        cachedEffects.defaultConfigEffects = defaultConfigEffects;
        cachedEffects.effectPaths = effectPaths;
        cachedEffects.configPath = config->getConfigFilePath();
        cachedEffects.initialized = true;
    }

    void invalidateAvailableEffects()
    {
        cachedEffects.initialized = false;
    }

    // Build and update overlay state for rendering
    static void updateOverlayState(LogicalDevice* logicalDevice, bool effectsEnabled, DeviceControlState& state)
    {
        if (!logicalDevice->imguiOverlay || !logicalDevice->imguiOverlay->isVisible())
            return;

        OverlayState overlayState;

        // No fallback to config - registry is the single source of truth
        // (initialized from config at first swapchain creation)

        getAvailableEffects(config.get(), overlayState.currentConfigEffects,
                            overlayState.defaultConfigEffects, overlayState.effectPaths);
        overlayState.configPath = config->getConfigFilePath();

        // Cache the filename extraction — config path rarely changes
                if (overlayState.configPath != state.cachedConfigPath)
        {
            state.cachedConfigPath = overlayState.configPath;
            state.cachedConfigName = std::filesystem::path(state.cachedConfigPath).filename().string();
        }
        overlayState.configName = state.cachedConfigName;
        overlayState.effectsEnabled = effectsEnabled;

        // Ensure all selected effects are in the registry
        for (const auto& effectName : logicalDevice->imguiOverlay->getSelectedEffects())
        {
            if (effectRegistry.hasEffect(effectName))
                continue;
            auto pathIt = overlayState.effectPaths.find(effectName);
            std::string effectPath = (pathIt != overlayState.effectPaths.end()) ? pathIt->second : "";
            effectRegistry.ensureEffect(effectName, effectPath);
        }

        // Parameters now read directly from EffectRegistry, no need to pass via state
        logicalDevice->imguiOverlay->updateState(std::move(overlayState));
    }

    bool runFrontendFrame(LogicalDevice* logicalDevice, DeviceControlState& state)
    {
        // One present thread runs the control step per device; the others reuse
        // its last decision instead of racing on the hotkey/reload state.
        if (logicalDevice->frontendBusy.exchange(true, std::memory_order_acq_rel))
            return state.lastEffectsOn.load(std::memory_order_acquire);
        FrontendBusyGuard busyGuard{logicalDevice};

        // No layer lock is held here: the step does file I/O (config change
        // checks, reloads), shader scans and overlay work. stateLock is taken
        // only for the short pending-reload / depthReallocPending accesses.
        const auto settings = settingsManager.snapshot();

        // Check if settings were saved (the UI already published the new settings)
        if (logicalDevice->imguiOverlay && logicalDevice->imguiOverlay->hasSettingsSaved())
        {
            state.keySymbol = convertToKeySym(settings->toggleKey);
            state.reloadKeySymbol = convertToKeySym(settings->reloadKey);
            state.overlayKeySymbol = convertToKeySym(settings->overlayKey);
            initInputBlocker(settings->overlayBlockInput);
            setInputBlocked(logicalDevice->imguiOverlay->isVisible());
            logicalDevice->imguiOverlay->clearSettingsSaved();
            Logger::info("Settings reloaded from SettingsManager");
        }

        // Check if shader paths were changed (refresh available effects list)
        if (logicalDevice->imguiOverlay && logicalDevice->imguiOverlay->hasShaderPathsChanged())
        {
            invalidateAvailableEffects();  // force re-scan of available effects
            logicalDevice->imguiOverlay->clearShaderPathsChanged();
            Logger::info("Shader paths changed, effect list refreshed");
        }

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
                    logicalDevice->depthReallocPending = false;
                    reloadNow = true;
                }
            }
            if (reloadNow)
                reloadAllSwapchains(logicalDevice, reloadEffects);
        }

        if (!state.initLogged)
        {
            Logger::info("hot-reload initialized, config: " + config->getConfigFilePath());
            state.initLogged = true;
        }

        // Toggle effect on/off (keyboard)
        if (handleKeyPress(state.keySymbol, state.pressed))
            state.presentEffect = !state.presentEffect;

        // Hot-reload: check for key press or config file change
        bool shouldReload = false;
        bool reloadFromDisk = false;
        if (handleKeyPress(state.reloadKeySymbol, state.reloadPressed))
        {
            Logger::debug("reload key pressed");
            shouldReload = true;
            reloadFromDisk = true;
        }
        if (config->hasConfigChanged())
        {
            Logger::debug("config file changed detected");
            shouldReload = true;
            reloadFromDisk = true;
        }

        // Toggle overlay on/off
        if (handleKeyPress(state.overlayKeySymbol, state.overlayPressed))
        {
            if (logicalDevice->imguiOverlay)
                logicalDevice->imguiOverlay->toggle();
        }

        // Toggle effects on/off via overlay checkbox
        if (logicalDevice->imguiOverlay && logicalDevice->imguiOverlay->hasToggleEffectsRequest())
        {
            state.presentEffect = !state.presentEffect;
            logicalDevice->imguiOverlay->clearToggleEffectsRequest();
        }

        // only the resolve path needs rebuilding, which the deferred realloc
        // does per swapchain generation. the full chain reload this used to
        // trigger recompiles every shader and re-records every command buffer
        // for a change that does not touch the chain.
        if (logicalDevice->imguiOverlay && logicalDevice->imguiOverlay->hasDepthPinChanged())
        {
            logicalDevice->imguiOverlay->clearDepthPinChanged();
            std::lock_guard<std::mutex> lock(logicalDevice->stateLock);
            logicalDevice->depthReallocPending = true;
        }

        if (logicalDevice->imguiOverlay && logicalDevice->imguiOverlay->hasModifiedParams())
        {
            // Modified parameters live in EffectRegistry — effects pick
            // them up at reload time, so all we need to do here is clear
            // the request and trigger a reload.
            logicalDevice->imguiOverlay->clearApplyRequest();
            shouldReload = true;
        }

        if (shouldReload)
        {
            if (logicalDevice->imguiOverlay)
                logicalDevice->imguiOverlay->refreshShaderProfiles();
            Logger::info("hot-reloading config and effects...");
            auto reloadSelectedEffects = [&]() {
                invalidateAvailableEffects();
                std::vector<std::string> effects = logicalDevice->imguiOverlay
                    ? logicalDevice->imguiOverlay->getActiveEffects()
                    : config->getOption<std::vector<std::string>>("effects", {});
                std::lock_guard<std::mutex> lock(logicalDevice->stateLock);
                logicalDevice->pendingChainEffects = std::move(effects);
                logicalDevice->pendingChainReload = true;
            };

            // the overlay owns the active profile; the static is only for
            // early calls before it exists.
            std::string overlayShaderPath = logicalDevice->imguiOverlay
                ? logicalDevice->imguiOverlay->getActiveShaderProfilePath()
                : std::string();
            if (overlayShaderPath.empty())
                overlayShaderPath = activeShaderProfilePath;

            // Check if overlay wants to load a different config
            if (logicalDevice->imguiOverlay && logicalDevice->imguiOverlay->hasPendingConfig())
            {
                std::string newConfigPath = logicalDevice->imguiOverlay->getPendingConfigPath();
                switchConfig(newConfigPath, overlayShaderPath);
                // Update overlay with effects from the new config
                std::vector<std::string> newEffects = config->getOption<std::vector<std::string>>("effects", {});
                std::vector<std::string> disabledEffects = config->getOption<std::vector<std::string>>("disabledEffects", {});
                logicalDevice->imguiOverlay->setSelectedEffects(newEffects, disabledEffects);
                logicalDevice->imguiOverlay->clearPendingConfig();
                reloadSelectedEffects();
            }
            else if (logicalDevice->imguiOverlay && logicalDevice->imguiOverlay->hasPendingShaderProfile())
            {
                const std::string shaderPath = logicalDevice->imguiOverlay->getPendingShaderProfilePath();
                if (!shaderPath.empty())
                {
                    switchConfig(config->getConfigFilePath(), shaderPath);
                    std::vector<std::string> newEffects = config->getOption<std::vector<std::string>>("effects", {});
                    std::vector<std::string> disabledEffects = config->getOption<std::vector<std::string>>("disabledEffects", {});
                    logicalDevice->imguiOverlay->setSelectedEffects(newEffects, disabledEffects);
                }
                else
                {
                    switchConfig(config->getConfigFilePath(), "");
                    const std::vector<std::string> newEffects = config->getOption<std::vector<std::string>>("effects", {});
                    const std::vector<std::string> disabledEffects = config->getOption<std::vector<std::string>>("disabledEffects", {});
                    logicalDevice->imguiOverlay->setSelectedEffects(newEffects, disabledEffects);
                }
                logicalDevice->imguiOverlay->clearPendingShaderProfile();
                reloadSelectedEffects();
            }
            else
            {
                // only the reload key or an external file change re-reads the
                // config and re-applies the profile; a UI apply rebuilds the
                // chain from the registry, which is already the truth
                if (reloadFromDisk)
                {
                    config->reload();
                    applyShaderProfile(config.get(), overlayShaderPath);
                }
                reloadSelectedEffects();
            }
        }

        // Check for debounced resize reload (separate from config reload)
        // Only call steady_clock::now() when a resize is actually pending
        if (resizeDebounce.pending)
        {
            auto resizeElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - resizeDebounce.lastResizeTime).count();

            if (resizeElapsed >= RESIZE_DEBOUNCE_MS)
            {
                Logger::info("debounced resize reload after " + std::to_string(resizeElapsed) + "ms");
                resizeDebounce.pending = false;

                // Get selected effects from registry (single source of truth);
                // the reload runs at the start of the next control step.
                std::vector<std::string> effects = effectRegistry.getSelectedEffects();
                std::lock_guard<std::mutex> lock(logicalDevice->stateLock);
                logicalDevice->pendingChainEffects = std::move(effects);
                logicalDevice->pendingChainReload = true;
            }
        }

        updateOverlayState(logicalDevice, state.presentEffect, state);
        state.lastEffectsOn.store(state.presentEffect, std::memory_order_release);
        return state.presentEffect;
    }
} // namespace VkHook
