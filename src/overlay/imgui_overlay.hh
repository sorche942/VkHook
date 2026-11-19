#ifndef IMGUI_OVERLAY_HPP_INCLUDED
#define IMGUI_OVERLAY_HPP_INCLUDED

#include <atomic>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <mutex>
#include <map>
#include <set>
#include <cstdint>
#include <tuple>

#include "vulkan_include.hh"
#include "logical_device.hh"
#include "keyboard_input.hh"
#include "effects/params/effect_param.hh"
#include "config_serializer.hh"
#include "input_blocker.hh"
#include "settings_manager.hh"
#include "logger.hh"

struct ImGuiContext;
namespace ImGui { class FileBrowser; }

namespace VkHook
{
    class Effect;
    class EffectRegistry;
    struct DiagnosticsState;

    struct OverlayState
    {
        std::vector<std::string> disabledEffects;       // Effects that are unchecked (in list but not rendered)
        std::vector<std::string> currentConfigEffects;  // ReShade effects from current config (e.g., tunic.conf)
        std::vector<std::string> defaultConfigEffects;  // ReShade effects from default VkHook.conf (no duplicates)
        std::map<std::string, std::string> effectPaths; // Effect name -> file path (for reshade effects)
        std::string configPath;
        std::string configName;  // Just the filename (e.g., "tunic.conf")
        bool effectsEnabled = true;
        // Parameters now read directly from EffectRegistry
    };

    // UI preferences that persist across swapchain recreation
    // Effect-related state is managed by EffectRegistry
    // Settings are managed by SettingsManager
    // UI visibility state that survives swapchain recreation.
    struct OverlayPersistentState
    {
        bool visible = false;
    };

    // Notification window shown at the top-right for layer errors and notices. It is
    // non-interactive and auto-dismisses after a few seconds.
    struct ToastNotification
    {
        std::string                         message;
        LogLevel                            level = LogLevel::Error;
        std::chrono::steady_clock::time_point createdAt = std::chrono::steady_clock::now();
    };

    // Snapshot of the depth-capture state, gathered each frame for the Advanced
    // tab. The overlay reads these from LogicalDevice under globalLock.
    struct DepthCandidateInfo
    {
        VkImageView         imageView = VK_NULL_HANDLE;
        VkFormat            format = VK_FORMAT_UNDEFINED;
        VkExtent3D          extent = {0, 0, 1};
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        VkImageLayout       observedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool                transient = false;
        uint32_t            drawCount = 0;
        bool                hasPresentableSnapshotTarget = false;
    };

    struct DepthInfo
    {
        DepthCandidateInfo active;              // Currently active depth buffer (effective: pinned or auto)
        std::vector<DepthCandidateInfo> candidates; // All tracked candidates
        VkExtent2D          swapchainExtent = {0, 0}; // Current window size, for pin defaults
        VkResolveModeFlags supportedResolveModes = 0; // HW support (for UI greying)
        bool                depthCaptureEnabled = false;
        bool                depthResolveIsMsaa = false;
        VkResolveModeFlagBits depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        bool                depthIsPinned = false;       // Whether a manual pin is active
        VkImageView         pinnedView = VK_NULL_HANDLE;  // The pinned view handle (for UI highlighting)
    };

    // identity of the VkPhysicalDevice the game is actually using. reported
    // verbatim because the sysfs DRM scan can't tell hybrid GPUs apart.
    struct DeviceInfo
    {
        std::string gpuName;
        std::string gpuDriverInfo;
        std::string gpuPciSlot;
        uint32_t    gpuApiVersion = 0;
        uint32_t    gpuVendorId = 0;
    };

    // Detailed shader-test results in collapsible groups (view_shader_test.cc)
    void renderShaderTestResults(
        const std::vector<std::tuple<std::string, std::string, bool, std::string>>& results,
        const std::set<std::string>& depthShaderNames);

    class ImGuiOverlay
    {
    public:
        ImGuiOverlay(LogicalDevice* device, VkFormat swapchainFormat, uint32_t imageCount, OverlayPersistentState* persistentState);
        ~ImGuiOverlay();

        void toggle();
        bool isVisible() const { return visible; }

        // Push a toast notification. Thread-safe, auto-dismissed after ~5s.
        void pushToast(LogLevel level, const std::string& message);
        bool hasPendingToasts() const;

        void updateState(OverlayState newState);

        // Returns modified parameters when Apply is clicked, empty otherwise
        std::vector<std::unique_ptr<EffectParam>> getModifiedParams() const;
        bool hasModifiedParams() const { return applyRequested; }
        void clearApplyRequest() { applyRequested = false; reloadNeeded = false; }

        // Config switching
        bool hasPendingConfig() const { return !pendingConfigPath.empty(); }
        std::string getPendingConfigPath() const { return pendingConfigPath; }
        void clearPendingConfig() { pendingConfigPath.clear(); }
        bool hasPendingShaderProfile() const { return pendingShaderProfile; }
        std::string getPendingShaderProfilePath() const { return pendingShaderProfilePath; }
        void clearPendingShaderProfile() { pendingShaderProfilePath.clear(); pendingShaderProfile = false; }

        // Depth pin changed (needs command buffer reallocation)
        bool hasDepthPinChanged() const { return depthPinChanged; }
        void clearDepthPinChanged() { depthPinChanged = false; }

        // Effects toggle (global on/off)
        bool hasToggleEffectsRequest() const { return toggleEffectsRequested; }
        void clearToggleEffectsRequest() { toggleEffectsRequested = false; }

        // Set the effect registry (single source of truth for enabled states)
        void setEffectRegistry(EffectRegistry* registry) { effectRegistry = registry; }

        // Set game/profile info for auto-save (called from vkhook.cpp after detection)
        void setGameProfile(const std::string& gameName, const std::string& profileName, const std::string& profilePath)
        {
            activeGameName = gameName;
            activeProfileName = profileName;
            activeProfilePath = profilePath;
            refreshShaderProfiles();
        }

        void refreshShaderProfiles();
        void setActiveShaderProfile(const std::string& profileName);
        // save the outgoing profile, activate profileName and queue a reload.
        // atomic so the debounced auto-save can't hit the new path first.
        bool switchShaderProfile(const std::string& profileName);
        std::string getActiveShaderProfilePath() const { return activeShaderProfilePath; }

        // Trigger debounced reload (for config switch)
        void markDirty() { paramsDirty = true; lastChangeTime = std::chrono::steady_clock::now(); }

        // Settings were saved (keybindings need reload)
        bool hasSettingsSaved() const { return settingsSaved; }
        void clearSettingsSaved() { settingsSaved = false; }

        // Shader paths were changed (effect list needs refresh)
        bool hasShaderPathsChanged() const { return shaderPathsChanged; }
        void clearShaderPathsChanged() { shaderPathsChanged = false; }

        // Returns list of effects that should be active (enabled, for reloading)
        std::vector<std::string> getActiveEffects() const;

        // Returns all selected effects (enabled + disabled, for parameter collection)
        const std::vector<std::string>& getSelectedEffects() const;

        // Set effects list (when loading a different config)
        // disabledEffects: effects that should be unchecked (in list but not rendered)
        void setSelectedEffects(const std::vector<std::string>& effects,
                                const std::vector<std::string>& disabledEffects = {});

        // Records this frame's UI into a command buffer, or returns
        // VK_NULL_HANDLE when there is nothing to draw (or imageIndex is outside
        // the per-image resources). On a non-null result the caller submits the
        // command buffer with the fence from getCommandBufferFence().
        VkCommandBuffer recordFrame(uint32_t imageIndex, VkImageView imageView, uint32_t width, uint32_t height);

        // The fence to pass to the QueueSubmit that executes recordFrame's
        // command buffer. It is RESET here, immediately before the submit, so
        // every early return in recordFrame leaves it signalled. Call exactly
        // once per submit, right before vkQueueSubmit, and only for a non-null
        // recordFrame result.
        VkFence getCommandBufferFence(uint32_t imageIndex);

        // The swapchain the overlay draws on changed format or image count.
        // Rebuilds the render pass and the per-image resources to match; a
        // no-op when neither changed. Returns false if the old resources could
        // not be retired safely (they are kept and drawing is refused for the
        // out-of-range images).
        bool onSwapchainChanged(VkFormat swapchainFormat, uint32_t imageCount);

        // Collect all in-flight command buffer fences (for drain before depth image destroy)
        void collectCommandFences(std::vector<VkFence>& out) const;

    private:
        // Makes this overlay's ImGuiContext current for the scope, restoring
        // the previous one on exit. Every entry point that touches ImGui holds one.
        class ContextScope
        {
        public:
            explicit ContextScope(ImGuiContext* context);
            ~ContextScope();
            ContextScope(const ContextScope&) = delete;
            ContextScope& operator=(const ContextScope&) = delete;
        private:
            ImGuiContext* previous;
        };

        bool initVulkanBackend(VkFormat swapchainFormat, uint32_t imageCount);
        bool createRenderPass(VkFormat swapchainFormat);
        bool createImageResources(uint32_t imageCount);
        void destroyImageResources();
        void queueLastShaderProfile();
        // Queues one settings write for every change made since the last flush.
        void flushSettingsSave();
        void saveToPersistentState();
        void saveCurrentConfig();

        // View rendering methods (implemented in separate files)
        void renderAddEffectsView();
        void renderConfigManagerView();
        void renderSettingsView(const KeyboardState& keyboard);
        void renderShaderManagerView();
        void renderShaderTestSection();  // Shader test UI (part of shader manager)
        void startShaderTest();          // Initialize shader test queue and start
        void processShaderTest();        // Process one shader per frame (runs every frame)
        void renderMainView(const KeyboardState& keyboard);
        void importShaderPreset(const std::string& sourcePath);
        void renderDiagnosticsView();
        // Samples frame time, FPS and GPU stats. Called every overlay frame, not
        // just while the Diagnostics tab is open, so the tab has real history.
        void sampleDiagnostics();
        void renderAboutView();
        void renderAdvancedView();  // Depth buffer controls in Settings
        // Secondary settings pages with a native Back button.
        void renderDepthBuffersView();
        enum SubView
        {
            SubView_None = 0,
            SubView_DepthBuffers,
            SubView_Diagnostics,
        };
        int openSubView = SubView_None;
        void gatherDepthInfo();     // Snapshot LogicalDevice depth state under globalLock
        void applyDepthPinRequests();  // Flush UI depth-pin/clear requests to LogicalDevice
        void renderToasts();       // Persistent top-right notifications

        // Overlay hitboxes for this frame, in surface pixels. published to the
        // input blocker so only pointer events over the overlay are withheld.
        std::vector<InputRect> frameInputRects;

        ImGuiContext* imguiContext = nullptr;  // owned: created in the constructor, destroyed in the destructor
        VkFormat framebufferFormat = VK_FORMAT_UNDEFINED;
        uint32_t imageResourceCount = 0;
        LogicalDevice* logicalDevice;
        OverlayPersistentState* pPersistentState;
        EffectRegistry* effectRegistry = nullptr;  // Single source of truth for enabled states
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> commandBuffers;
        std::vector<VkFence> commandBufferFences;  // Fences to track command buffer completion
        std::vector<VkFramebuffer> framebuffers;    // Pre-created per swapchain image
        std::vector<VkImageView> framebufferImageViews;  // Track which image views framebuffers were created for
        uint32_t framebufferWidth = 0;   // Dimensions used to create framebuffers
        uint32_t framebufferHeight = 0;
        OverlayState state;
        std::vector<std::pair<std::string, std::string>> pendingAddEffects;  // {instanceName, effectType} to add
        std::map<std::string, int> addEffectsDuplicateCount;  // effectType -> extra copies to add per queue action
        std::set<std::string> addEffectsRemoved;  // effectType whose original is queued for removal
        bool inSelectionMode = false;
        int insertPosition = -1;  // Position to insert effects (-1 = append to end)
        char addEffectsSearch[64] = "";  // Search filter for add effects view
        bool addEffectsFocusSearch = false;  // Focus the search box on the next frame

        // Bumped when updateState sees different effect sources, so the
        // add-effects view can cache its entry list instead of rebuilding it
        uint64_t overlayStateVersion = 0;

        // add-effects view: rebuilt only when state, search or filter changes
        struct AddEffectsEntry
        {
            std::string type;
            std::string path;
            std::string package;  // top-level directory under Shaders/, for grouping
            int         group = 0;
            int         score = 0;
        };
        std::vector<AddEffectsEntry> addEffectsEntries;
        std::vector<std::string>     addEffectsPackages;  // unique package names for the tabs
        struct AddEffectsRow
        {
            std::string header;  // non-empty on a package header row
            int         entry = -1;
        };
        std::vector<AddEffectsRow>   addEffectsRows;      // display order, headers included
        std::string                  addEffectsCacheSearch;
        std::string                  addEffectsCacheFilter;  // selected package, empty = All
        uint64_t                     addEffectsCacheVersion = UINT64_MAX;
        bool inConfigManageMode = false;
        std::vector<std::string> configList;
        std::string configListDefault;          // cached default_config, refreshed with configList
        bool configListRefreshPending = true;   // re-scan the configs dir on next render

        // Shader Manager state
        std::vector<std::string> shaderMgrParentDirs;
        std::vector<std::string> shaderMgrShaderPaths;
        std::vector<std::string> shaderMgrTexturePaths;
        bool shaderMgrInitialized = false;

        // Shader test state
        bool shaderTestRunning = false;
        bool shaderTestComplete = false;
        uint64_t shaderTestStateVersion = 0;  // overlayStateVersion the finished test ran against
        size_t shaderTestCurrentIndex = 0;
        int shaderTestDuplicateCount = 0;  // Number of duplicate shaders skipped
        std::vector<std::pair<std::string, std::string>> shaderTestQueue;  // {effectName, filePath}
        std::vector<std::string> shaderTestIncludePaths;  // Cached include paths (avoid re-reading config per shader)
        std::vector<std::tuple<std::string, std::string, bool, std::string>> shaderTestResults;  // {name, path, success, error}
        std::set<std::string> depthShaders;  // Effect names that use depth buffer (populated by shader test)
        std::set<std::string> checkedShaders; // Effects already checked for depth (avoids recompiling)

        // UI state for settings view
        int listeningForKey = 0;  // 0=none, 1=toggle, 2=reload, 3=overlay
        bool settingsSaved = false;  // True when settings saved, cleared by vkhook.cpp
        bool shaderPathsChanged = false;  // True when shader manager saved, cleared by vkhook.cpp

        // Non-interactive top-right notifications. They render even when
        // the main overlay is hidden, so fatal errors stay visible.
        mutable std::mutex toastsMutex;
        std::vector<ToastNotification> toasts;

        // UI state for Advanced (depth buffer) view
        DepthInfo depthInfo;  // Refreshed each frame before rendering
        DeviceInfo deviceInfo;  // Same, for Diagnostics GPU information
        bool depthPinPendingClear = false;   // Request to clear pinned depth (from UI)
        VkImageView depthPinPendingView = VK_NULL_HANDLE;  // Request to pin a specific view (from UI)
        bool depthPinChanged = false;         // Set when pin/clear was applied (triggers cmd buf reallocation)
        bool depthTransformPending = false;   // Set when a depth transform toggle changed (rebuild resolve path)

        bool applyRequested = false;
        // a pending edit that genuinely needs the effect chain rebuilt; a plain
        // ReShade uniform edit is applied live by updateEffect()
        bool reloadNeeded = false;
        bool toggleEffectsRequested = false;
        bool paramsDirty = false;  // True when params changed, waiting for debounce
        std::chrono::steady_clock::time_point lastChangeTime;
        bool visible = false;
        bool initialized = false;
        bool settingsDirty = false;           // a setting changed; flushed once per frame
        bool depthCaptureAtStartup = false;   // value the depth hooks were armed with
        // frame pacing for ImGui's DeltaTime and the adaptive fence timeout
        float avgFrameTimeMs = 8.0f;
        float lastGoodDt = 0.008f;
        std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();
        bool warnedImageIndex = false;
        std::shared_ptr<DiagnosticsState> diagnostics;  // sampled stats and history (view_diagnostics.cc)
        // file pickers used when the XDG portal is unavailable; created on first use
        std::shared_ptr<ImGui::FileBrowser> presetBrowser;
        std::shared_ptr<ImGui::FileBrowser> dirBrowser;

        // view state that used to live in function-local statics
        std::vector<std::string> recentEffects;   // add-effects: session recents
        std::string addEffectsFilterPackage;      // add-effects: selected package tab, empty = All
        std::string deleteProfileTarget;
        std::string renameProfileTarget;
        char renameShaderProfileName[64] = "";
        char newShaderProfileName[64] = "";
        std::string paramsActiveEffect;
        float appliedUiScale = -1.0f;
        int activeView = 0;
        bool backendInitialized = false;
        bool resetLayoutRequested = false;  // When true, reset window position/size next frame
        uint32_t currentWidth = 1920;   // Current swapchain resolution for VRAM estimates
        uint32_t currentHeight = 1080;
        char saveConfigName[64] = "";
        std::string pendingConfigPath;
        std::string pendingShaderProfilePath;
        bool pendingShaderProfile = false;
        std::string activeShaderProfileName;
        std::string activeShaderProfilePath;
        std::vector<std::string> shaderProfiles;

        // Per-app profile system
        std::string activeGameName;       // Detected executable name
        std::string activeProfileName;    // Active profile ("default", "performance", etc.)
        std::string activeProfilePath;    // Full path to active profile file
        bool profileDirty = false;        // True when changes need saving
        bool autoSaveProfile(bool block = false);  // Save current state to active profile
        void collectSaveData(            // Shared helper for save operations
            std::vector<std::string>& effects,
            std::vector<std::string>& disabledEffects,
            std::vector<ConfigParam>& params,
            std::map<std::string, std::string>& effectPaths,
            std::vector<PreprocessorDefinition>& allDefs,
            std::vector<ConfigParam>& disabledEffectParams);
    };

} // namespace VkHook

#endif // IMGUI_OVERLAY_HPP_INCLUDED
