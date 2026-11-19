#pragma once

// Control plane of the layer: config selection, hotkeys, hot-reload requests
// coming from the overlay, and the overlay state handed back to it. The control
// step runs on a present thread at a present boundary with NO layer lock held
// (it does file I/O and shader scans); it takes LogicalDevice::stateLock itself
// only for short accesses to the pending-reload state.

#include "vulkan_include.hh"
#include "config.hh"
#include "diag/gpu_crash.hh"
#include "effects/effect_registry.hh"
#include "logical_device.hh"

#include <atomic>

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

    // Per-device control state. Only the thread that wins
    // LogicalDevice::frontendBusy touches the plain members; `lastEffectsOn`
    // is what the other present threads of the device reuse meanwhile.
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

        // last effects-on decision, published at the end of every control step
        std::atomic<bool> lastEffectsOn{true};
    };

    // Per-device control state; the reference stays valid until
    // releaseDeviceControl. Thread-safe (own small mutex).
    DeviceControlState& deviceControl(LogicalDevice* logicalDevice);
    void releaseDeviceControl(LogicalDevice* logicalDevice);

    // Present-boundary control step: settings reload, hotkeys, overlay requests,
    // hot-reload, resize debounce and the overlay state update. Returns whether
    // effects are on for this present. No lock held by the caller. If another
    // present thread of the device is already inside the step, returns that
    // thread's previous decision without running it.
    bool runFrontendFrame(LogicalDevice* logicalDevice, DeviceControlState& state);
} // namespace VkHook
