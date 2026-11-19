#pragma once

// Control plane of the layer: config selection, hotkeys, hot-reload requests
// coming from the overlay, and the overlay state handed back to it. Everything
// here runs on the present thread at a present boundary.

#include "vulkan_include.hh"
#include "config.hh"
#include "diag/gpu_crash.hh"
#include "effects/effect_registry.hh"
#include "logical_device.hh"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace VkHook
{
    // Depth capture is a process-wide choice: the hooks run inside the game's
    // command recording, so the persisted setting is latched once and never
    // re-read.
    void latchDepthCaptureSetting();
    bool depthCaptureActive();

    extern std::shared_ptr<Config> baseConfig;  // always VkHook.conf
    extern std::shared_ptr<Config> config;      // current config (base + overlay)
    extern EffectRegistry          effectRegistry;  // single source of truth for effect configs

    // detected game and the active profile files (set by initConfigs / the overlay)
    extern std::string detectedGameName;
    extern std::string activeProfileName;
    extern std::string activeProfilePath;
    extern std::string activeShaderProfilePath;

    // A resize was seen; the chain is rebuilt once the size has been stable
    struct ResizeDebounceState
    {
        std::chrono::steady_clock::time_point lastResizeTime;
        bool                                  pending = false;
    };
    extern ResizeDebounceState resizeDebounce;

    void initConfigs();
    void switchConfig(const std::string& configPath, const std::string& shaderPath = "");

    // available effects by source (cached until invalidateAvailableEffects)
    void getAvailableEffects(Config* config,
                             std::vector<std::string>& currentConfigEffects,
                             std::vector<std::string>& defaultConfigEffects,
                             std::map<std::string, std::string>& effectPaths);
    void invalidateAvailableEffects();

    // Edge-triggered key query: true on the key-down edge only.
    bool handleKeyPress(uint32_t keySymbol, bool& wasPressed);

    // Mark the start of an input frame (dispatch de-duplication resets).
    void beginInputFrame();

    // Everything the present path used to keep in function-local statics.
    struct DeviceControlState
    {
        DeviceControlState();

        uint32_t keySymbol       = 0;
        uint32_t reloadKeySymbol = 0;
        uint32_t overlayKeySymbol = 0;
        bool     initLogged      = false;

        bool pressed        = false;
        bool presentEffect  = true;
        bool reloadPressed  = false;
        bool overlayPressed = false;

        std::string cachedConfigPath;
        std::string cachedConfigName;

        DiagState diag;
    };

    // Per-device control state. Caller holds globalLock.
    DeviceControlState& deviceControlLocked(LogicalDevice* logicalDevice);
    void releaseDeviceControlLocked(LogicalDevice* logicalDevice);

    // Present-boundary control step: settings reload, hotkeys, overlay requests,
    // hot-reload, resize debounce and the overlay state update. Returns whether
    // effects are on for this present. Caller holds globalLock.
    bool runFrontendFrameLocked(LogicalDevice* logicalDevice, DeviceControlState& state);
} // namespace VkHook
