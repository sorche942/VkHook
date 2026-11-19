#pragma once

// Control plane of the layer: config selection, hotkeys, the overlay's commands
// and the snapshot handed back to it. The control step runs on a present thread
// at a present boundary with NO layer lock held (it does file I/O and shader
// scans); it takes LogicalDevice::stateLock itself only for short accesses to
// the pending-reload state.

#include "vulkan_include.hh"
#include "config.hh"
#include "control/commands_apply.hh"
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

    void initConfigs();
    void switchConfig(const std::string& configPath, const std::string& shaderPath = "");

    // available effects by source (cached until invalidateAvailableEffects)
    void getAvailableEffects(Config* config,
                             std::vector<std::string>& currentConfigEffects,
                             std::vector<std::string>& defaultConfigEffects,
                             std::map<std::string, std::string>& effectPaths);
    void invalidateAvailableEffects();

    class InputSession;

    // Edge-triggered key query on the swapchain's input session: true on the
    // key-down edge only. A null session (an unsupported windowing system) never
    // presses anything.
    bool handleKeyPress(InputSession* session, uint32_t keySymbol, bool& wasPressed);

    // Per-device control state, owned by LogicalDevice::control. Only the thread
    // that wins LogicalDevice::frontendBusy touches the plain members;
    // `lastEffectsOn` is what the other present threads of the device reuse
    // meanwhile.
    struct DeviceControlState
    {
        explicit DeviceControlState(LogicalDevice* device);

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

        // The overlay: its UI, the command queue it fills and the snapshot it
        // reads. Declared last so it is destroyed first.
        OverlayControl overlay;

        // A resize was seen: swapchain creation notes it, and the chain is
        // rebuilt once the size has been stable for the debounce interval.
        void noteResize();
        std::atomic<bool>    resizePending{false};
        std::atomic<int64_t> lastResizeNs{0};
    };

    // The device's control state (created in vkCreateDevice, so never null for
    // a device the layer tracks). The reference stays valid until the device is destroyed.
    inline DeviceControlState& deviceControl(LogicalDevice* logicalDevice) { return *logicalDevice->control; }

    // Present-boundary control step: overlay commands, settings reload, hotkeys,
    // hot-reload, resize debounce and the overlay snapshot. `session` is the
    // input session of the swapchain being presented (null when it has none).
    // Returns whether effects are on for this present. No lock held by the
    // caller. If another present thread of the device is already inside the
    // step, returns that thread's previous decision without running it.
    bool runFrontendFrame(LogicalDevice* logicalDevice, DeviceControlState& state, InputSession* session);
} // namespace VkHook
