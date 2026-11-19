#ifndef IMGUI_OVERLAY_HPP_INCLUDED
#define IMGUI_OVERLAY_HPP_INCLUDED

// ImGuiOverlay: the transitional bridge between the overlay and the layer.
//
// The overlay proper is OverlayUi (one per device, overlay_ui.hh) and
// OverlayRenderer (one per swapchain, overlay_renderer.hh). They read a
// CoreSnapshot and answer with Commands (commands.hh) and know nothing of the
// layer's internals. This class is what core does around them, kept here until
// core owns it:
//
//   - it builds the CoreSnapshot from EffectRegistry, LogicalDevice and the
//     settings store, once per visible present;
//   - it drains the CommandQueue once per present and applies every command;
//   - it turns the commands core has to act on into the has/clear flags that
//     control/frontend.cc polls (hasModifiedParams, hasPendingConfig, ...).
//
// It owns one OverlayUi and, for the swapchain it was built with, one
// OverlayRenderer, which is the old single-swapchain wiring that present/*.cc
// still calls (recordFrame, getCommandBufferFence).
//
// Core's follow-up deletes this class:
//   present/swapchain.cc  create the device's OverlayUi (OverlayUi::create) with
//                         a CommandQueue and CoreSnapshotSlot owned by the
//                         device's control state, and one OverlayRenderer per
//                         swapchain (OverlayRenderer::create); destroy the
//                         renderer in the swapchain's destructor.
//   present/present.cc    submitOverlayFrame / presentSpecialCases call
//                         swapchain->overlay->record() and fenceForSubmit()
//                         instead of imguiOverlay->recordFrame().
//   control/frontend.cc   once per present: slot.publish(buildCoreSnapshot()),
//                         commands.drain(v), apply each command (the visitor
//                         in imgui_overlay.cc is the reference implementation,
//                         including which of them set state that frontend.cc
//                         then acts on). Replaces runFrontendFrameLocked's
//                         hasSettingsSaved / hasShaderPathsChanged /
//                         hasToggleEffectsRequest / hasDepthPinChanged /
//                         hasModifiedParams / hasPendingConfig /
//                         hasPendingShaderProfile polling and updateOverlayState.
//   depth/*               PinDepth, ClearDepthPin, RefreshDepth and
//                         DepthTransformChanged call the depth tracker's own
//                         pin / re-detect / realloc entry points instead of
//                         writing LogicalDevice depth fields.

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "commands.hh"
#include "core_snapshot.hh"
#include "overlay_renderer.hh"
#include "overlay_ui.hh"
#include "vulkan_include.hh"

namespace VkHook
{
    class EffectRegistry;
    struct LogicalDevice;

    // What the add-effects page can offer, as control/frontend.cc computes it.
    struct OverlayState
    {
        std::vector<std::string> currentConfigEffects;  // ReShade effects from the current config
        std::vector<std::string> defaultConfigEffects;  // ReShade effects from the default VkHook.conf
        std::map<std::string, std::string> effectPaths; // Effect name -> file path
        std::string configPath;
        std::string configName;  // Just the filename (e.g., "tunic.conf")
        bool effectsEnabled = true;
    };

    class ImGuiOverlay
    {
    public:
        ImGuiOverlay(LogicalDevice* device, VkFormat swapchainFormat, uint32_t imageCount,
                     OverlayPersistentState* persistentState);
        ~ImGuiOverlay();

        void toggle();
        bool isVisible() const { return ui && ui->isVisible(); }

        // Push a toast notification. Thread-safe, auto-dismissed after ~5s.
        void pushToast(LogLevel level, const std::string& message);
        bool hasPendingToasts() const { return ui && ui->hasPendingToasts(); }

        void updateState(OverlayState newState);

        // The has/clear pairs frontend.cc polls; each is set by the command
        // that asks for it (see the visitor in imgui_overlay.cc).
        bool hasModifiedParams() const { return applyRequested; }
        void clearApplyRequest() { applyRequested = false; }
        bool hasPendingConfig() const { return !pendingConfigPath.empty(); }
        std::string getPendingConfigPath() const { return pendingConfigPath; }
        void clearPendingConfig() { pendingConfigPath.clear(); }
        bool hasPendingShaderProfile() const { return pendingShaderProfile; }
        std::string getPendingShaderProfilePath() const { return pendingShaderProfilePath; }
        void clearPendingShaderProfile() { pendingShaderProfilePath.clear(); pendingShaderProfile = false; }
        bool hasDepthPinChanged() const { return depthPinChanged; }
        void clearDepthPinChanged() { depthPinChanged = false; }
        bool hasToggleEffectsRequest() const { return toggleEffectsRequested; }
        void clearToggleEffectsRequest() { toggleEffectsRequested = false; }
        bool hasSettingsSaved() const { return settingsSaved; }
        void clearSettingsSaved() { settingsSaved = false; }
        bool hasShaderPathsChanged() const { return shaderPathsChanged; }
        void clearShaderPathsChanged() { shaderPathsChanged = false; }

        void setEffectRegistry(EffectRegistry* registry) { effectRegistry = registry; }

        // Game and profile detected by control/frontend.cc
        void setGameProfile(const std::string& gameName, const std::string& profileName, const std::string& profilePath);
        void refreshShaderProfiles();
        std::string getActiveShaderProfilePath() const { return activeShaderProfilePath; }

        // Effects that should be active (enabled), for reloading
        std::vector<std::string> getActiveEffects() const;
        // All selected effects (enabled + disabled)
        const std::vector<std::string>& getSelectedEffects() const;
        // Replaces the chain (when loading a different config); effects in
        // `disabledEffects` stay listed but are unchecked.
        void setSelectedEffects(const std::vector<std::string>& effects,
                                const std::vector<std::string>& disabledEffects = {});

        // See OverlayRenderer: record this frame's UI for the swapchain image,
        // then submit the returned buffer with getCommandBufferFence().
        VkCommandBuffer recordFrame(uint32_t imageIndex, VkImageView imageView, uint32_t width, uint32_t height);
        VkFence getCommandBufferFence(uint32_t imageIndex);

        // The swapchain changed format or image count: builds a new renderer.
        // False when the old one could not be retired safely (it is kept).
        bool onSwapchainChanged(VkFormat swapchainFormat, uint32_t imageCount);

        void collectCommandFences(std::vector<VkFence>& out) const;

    private:
        // --- the layer side: snapshot and commands (imgui_overlay.cc) ---
        CoreSnapshotPtr buildSnapshot();
        void gatherDepth(DepthInfo& depth, DeviceInfo& device);
        void applyCommands(std::vector<Command>& pending);
        struct CommandVisitor;

        bool autoSaveProfile(bool block = false);
        void saveConfig(const std::string& name);
        bool switchProfile(const std::string& name);
        void setActiveProfile(const std::string& name);
        void queueLastProfile();
        void activateActiveProfile();  // makes frontend.cc load activeShaderProfilePath
        void importPreset(const std::string& sourcePath);

        LogicalDevice* logicalDevice;
        EffectRegistry* effectRegistry = nullptr;

        // Declaration order is destruction order in reverse: the renderer goes
        // before the UI it draws with, both before the queue and slot they use.
        CommandQueue commands;
        CoreSnapshotSlot slot;
        std::unique_ptr<OverlayUi> ui;
        std::unique_ptr<OverlayRenderer> renderer;

        OverlayState state;
        uint64_t availableVersion = 0;
        uint64_t snapshotGeneration = 0;

        std::string activeGameName;
        std::string activeProfileName;
        std::string activeProfilePath;  // the game's .conf
        std::string activeShaderProfileName;
        std::string activeShaderProfilePath;
        std::vector<std::string> shaderProfiles;

        bool applyRequested = false;
        std::string pendingConfigPath;
        std::string pendingShaderProfilePath;
        bool pendingShaderProfile = false;
        bool depthPinChanged = false;
        bool toggleEffectsRequested = false;
        bool settingsSaved = false;
        bool shaderPathsChanged = false;
    };

} // namespace VkHook

#endif // IMGUI_OVERLAY_HPP_INCLUDED
