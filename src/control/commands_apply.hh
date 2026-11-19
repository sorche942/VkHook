#pragma once

// Core's half of the overlay protocol. The overlay (overlay/overlay_ui.hh) only
// reads an immutable CoreSnapshot and pushes Commands; this is the code that
// builds that snapshot and applies those commands (AGENTS.md invariant 8).
//
// One OverlayControl exists per device (DeviceControlState::overlay). Once per
// present, on the thread that wins LogicalDevice::frontendBusy and with no layer
// lock held:
//   applyCommands()    drains the CommandQueue, applies every command and
//                      reports what the control step has to act on;
//   publishSnapshot()  builds the CoreSnapshot the next UI frame reads.
// Depth commands go to the depth tracker's own entry points; nothing here
// writes a LogicalDevice field.

#include "overlay/commands.hh"
#include "overlay/core_snapshot.hh"
#include "overlay/overlay_ui.hh"
#include "vulkan_include.hh"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace VkHook
{
    class EffectRegistry;
    struct LogicalDevice;

    // What the add-effects page can offer, as the control step computes it.
    struct OverlayState
    {
        std::vector<std::string> currentConfigEffects;  // ReShade effects from the current config
        std::vector<std::string> defaultConfigEffects;  // ReShade effects from the default VkHook.conf
        std::map<std::string, std::string> effectPaths; // Effect name -> file path
        std::string configPath;
        std::string configName;  // Just the filename (e.g., "tunic.conf")
        bool effectsEnabled = true;
    };

    // What one batch of commands asks the control step to do.
    struct OverlayRequests
    {
        bool settingsSaved = false;       // settings changed: re-read key bindings and input blocking
        bool shaderPathsChanged = false;  // rescan the available effects
        bool toggleEffects = false;       // the global effects on/off
        bool rebuild = false;             // rebuild the chain from the registry
        bool reloadFromDisk = false;      // re-read the config and preset (the reload key's path)
        std::string pendingConfigPath;    // load this config
        bool pendingShaderProfile = false;
        std::string pendingShaderProfilePath;  // activate this preset ("" = none)
    };

    class OverlayControl
    {
    public:
        explicit OverlayControl(LogicalDevice* device);
        // Destroys the UI: every OverlayRenderer made from it must be gone.
        ~OverlayControl();
        OverlayControl(const OverlayControl&) = delete;
        OverlayControl& operator=(const OverlayControl&) = delete;

        // The device's UI, created with the first swapchain that can show it.
        // Safe from any thread; null until then.
        OverlayUi* ui() const { return uiPtr.load(std::memory_order_acquire); }

        // Creates the UI once. Two threads can race here; the loser's UI is
        // discarded. Returns null when ImGui cannot be set up.
        OverlayUi* ensureUi();

        // Toasts are the one overlay call that any thread may make.
        void pushToast(LogLevel level, const std::string& message);

        // --- control step (the frontend thread only) ---

        // Detected game and the files its profile lives in; read once when the
        // first frame runs. Also reloads the preset list.
        void initProfiles();
        void refreshShaderProfiles();
        const std::string& activeShaderProfilePath() const { return activeShaderProfilePathValue; }

        // Drains and applies every queued command.
        OverlayRequests applyCommands();

        // The add-effects page's data for the next snapshot; a change
        // invalidates the page's cached list.
        void setAvailable(OverlayState newState);

        // Builds the CoreSnapshot the views read and publishes it.
        void publishSnapshot();

    private:
        void gatherDepth(DepthInfo& depth, DeviceInfo& device);
        CoreSnapshotPtr buildSnapshot();
        struct CommandVisitor;

        bool autoSaveProfile(bool block = false);
        void saveConfig(const std::string& name);
        bool switchProfile(const std::string& name);
        void setActiveProfile(const std::string& name);
        void queueLastProfile();
        void activateActiveProfile();  // makes the control step load the active preset
        void importPreset(const std::string& sourcePath);

        LogicalDevice* logicalDevice;
        EffectRegistry* effectRegistry;

        // Declaration order is destruction order in reverse: the UI goes before
        // the queue and slot it uses.
        CommandQueue commands;
        CoreSnapshotSlot slot;
        OverlayPersistentState persistent;
        std::unique_ptr<OverlayUi> uiOwner;
        std::atomic<OverlayUi*> uiPtr{nullptr};

        OverlayState state;
        OverlayRequests requests;
        uint64_t availableVersion = 0;
        uint64_t snapshotGeneration = 0;

        bool profilesInitialized = false;
        std::string activeGameName;
        std::string activeProfileName;
        std::string activeProfilePath;  // the game's .conf
        std::string activeShaderProfileName;
        std::string activeShaderProfilePathValue;
        std::vector<std::string> shaderProfiles;
    };
} // namespace VkHook
