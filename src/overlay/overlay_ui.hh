#ifndef VKHOOK_OVERLAY_UI_HH_INCLUDED
#define VKHOOK_OVERLAY_UI_HH_INCLUDED

// OverlayUi: the ImGui side of the overlay, one per device.
//
// It owns the ImGuiContext and the views, and nothing that
// belongs to a swapchain. Views read an immutable CoreSnapshot and answer with
// Commands (commands.hh); no view reaches the layer's state. What a swapchain
// needs to put a frame on screen (render pass, command buffers, fences,
// framebuffers) lives in OverlayRenderer (overlay_renderer.hh).
//
// Input: the overlay holds no input state of its own. The swapchain's
// InputSession is an argument of every frame call, so each window's pointer,
// keys and blocking stay with that window.
//
// Threading: every method except pushToast() runs on the present thread. The
// ImGuiContext is made current only inside the methods that need it and the
// previous one is restored on exit, so a host application's own ImGui is
// never disturbed.

#include <atomic>
#include <chrono>
#include <filesystem>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "commands.hh"
#include "core_snapshot.hh"
#include "input_session.hh"
#include "logger.hh"
#include "vulkan_include.hh"

struct ImGuiContext;
struct ImDrawData;
namespace ImGui { class FileBrowser; }

namespace VkHook
{
    struct LogicalDevice;
    struct DiagnosticsState;
    class OverlayRenderer;

    // UI visibility that survives the overlay being rebuilt.
    struct OverlayPersistentState
    {
        bool visible = false;
    };

    // Notification window shown at the top-right for layer errors and notices.
    // It is non-interactive and auto-dismisses after a few seconds.
    struct ToastNotification
    {
        std::string message;
        LogLevel level = LogLevel::Error;
        std::chrono::steady_clock::time_point createdAt = std::chrono::steady_clock::now();
    };

    // Detailed shader-test results in collapsible groups (view_shader_test.cc)
    void renderShaderTestResults(
        const std::vector<std::tuple<std::string, std::string, bool, std::string>>& results,
        const std::set<std::string>& depthShaderNames);

    class OverlayUi
    {
    public:
        // `device` supplies the Vulkan handles and dispatch tables for ImGui's
        // backend (never read by a view). `commands` receives what the user
        // does; `snapshots` is where core publishes what the views read. All
        // three outlive the OverlayUi. Returns null when ImGui cannot be set up.
        static std::unique_ptr<OverlayUi> create(LogicalDevice* device, OverlayPersistentState* persistentState,
                                                 CommandQueue* commands, const CoreSnapshotSlot* snapshots);
        ~OverlayUi();
        OverlayUi(const OverlayUi&) = delete;
        OverlayUi& operator=(const OverlayUi&) = delete;

        void toggle();
        bool isVisible() const { return visible; }

        // Thread-safe, auto-dismissed after about five seconds.
        void pushToast(LogLevel level, const std::string& message);
        bool hasPendingToasts() const;

        // --- the frame protocol OverlayRenderer::record drives ---

        // `input` is the InputSession of the swapchain's surface: the overlay
        // reads the pointer and keyboard from it and publishes its hitboxes and
        // blocking to it. It is used only during the call and may be null (no
        // input this frame, nothing is blocked or released).
        //
        // False when there is nothing to put on screen (hidden and no toasts) or
        // ImGui's pipeline cannot be made to match `renderPass`; the input
        // blocking on `input` is released as a side effect of the first case.
        bool beginFrameCheck(InputSession* input, VkFormat format, VkRenderPass renderPass);

        // Builds this frame's UI for a surface of width x height and returns
        // its draw data, or null on failure. Keyboard and mouse are
        // sampled from `input` (see beginFrameCheck), pacing here. The caller records the data with drawFrame().
        ImDrawData* buildFrame(uint32_t width, uint32_t height, InputSession* input);

        // Records the draw data into `commandBuffer`, inside the caller's render pass.
        void drawFrame(ImDrawData* drawData, VkCommandBuffer commandBuffer);

        // Fence timeout the renderer should wait with: 4x the measured frame
        // time, clamped to [2 ms, 50 ms].
        uint64_t fenceTimeoutNs() const;

        // --- renderer registry ---

        // Called by OverlayRenderer on construction. Binds ImGui's pipeline to
        // `renderPass` (rebuilding the backend when the format or the total
        // image ring differs from what it was built for). Returns false when
        // the backend could not be (re)built; the renderer then draws nothing.
        bool attachRenderer(OverlayRenderer* renderer, VkFormat format, VkRenderPass renderPass, uint32_t imageCount);
        void detachRenderer(OverlayRenderer* renderer);

    private:
        OverlayUi(LogicalDevice* device, OverlayPersistentState* persistentState, CommandQueue* commands,
                  const CoreSnapshotSlot* snapshots);

        // Makes this overlay's ImGuiContext current for the scope, restoring
        // the previous one on exit.
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

        bool initialize();
        bool initBackend(VkFormat format, VkRenderPass renderPass, uint32_t imageRing);
        // `extraImages` counts a renderer that is about to attach
        bool ensureBackend(VkFormat format, VkRenderPass renderPass, uint32_t extraImages);
        void shutdownBackend();
        void saveToPersistentState();
        void emit(Command command) { commands->push(std::move(command)); }
        void emitSetting(const char* key, SettingValue value);
        const AppSettings& settings() const { return *snapshot->settings; }

        // Edits made this frame; a debounced pair of commands follows them.
        void noteEdit(bool needsRebuild);
        // The chain's membership or order changed: rebuild now, save once the command lands.
        void chainEdited();
        // A preset operation settles what was pending; the layer saves the outgoing preset itself.
        void dropPendingEdits();
        void runAutoApply();

        // View rendering methods (one file each)
        void renderAddEffectsView();
        void renderConfigManagerView();
        void renderSettingsView(const KeyboardState& keyboard);
        void renderShaderManagerView();
        void renderShaderTestSection();  // Shader test UI (part of shader manager)
        void startShaderTest();          // Initialize shader test queue and start
        void processShaderTest();        // Process one shader per frame (runs every frame)
        void renderMainView();
        void renderDiagnosticsView(const InputSession* input);
        // Samples frame time, FPS and GPU stats. Called every overlay frame, not
        // just while the Diagnostics tab is open, so the tab has real history.
        void sampleDiagnostics();
        void renderAboutView();
        void renderAdvancedView();  // Depth buffer controls in Settings
        void renderDepthBuffersView();
        void renderToasts();        // Persistent top-right notifications
        enum SubView
        {
            SubView_None = 0,
            SubView_DepthBuffers,
            SubView_Diagnostics,
        };
        int openSubView = SubView_None;

        // Overlay hitboxes for this frame, in surface pixels. Published to the
        // input blocker so only pointer events over the overlay are withheld.
        std::vector<InputRect> frameInputRects;

        ImGuiContext* imguiContext = nullptr;  // owned
        LogicalDevice* logicalDevice;          // backend setup only; views never see it
        OverlayPersistentState* pPersistentState;
        CommandQueue* commands;
        const CoreSnapshotSlot* snapshots;
        CoreSnapshotPtr snapshot;  // this frame's view of the layer

        // ImGui's Vulkan backend, shared by every swapchain's renderer
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        bool backendInitialized = false;
        VkFormat backendFormat = VK_FORMAT_UNDEFINED;
        uint32_t backendRing = 0;
        std::vector<OverlayRenderer*> renderers;

        // add-effects page: the queued edit
        std::vector<std::pair<std::string, std::string>> pendingAddEffects;  // {EffectId text, effectType}; effectType resolves the shader file
        std::map<std::string, int> addEffectsDuplicateCount;  // effectType -> extra copies to add per queue action

        // The techniques a shader file declares, scanned once per file version.
        struct TechniqueScan
        {
            std::filesystem::file_time_type mtime{};
            std::vector<std::string> names;
        };
        std::map<std::string, TechniqueScan> techniqueScans;  // by path
        const std::vector<std::string>& techniquesOf(const std::string& path);
        std::set<std::string> addEffectsRemoved;  // effectTypes queued for removal (every chain entry of the file)
        bool inSelectionMode = false;
        int insertPosition = -1;  // chain position to insert at (-1 = append)
        char addEffectsSearch[64] = "";
        bool addEffectsFocusSearch = false;

        // add-effects page: rebuilt only when the available effects, search or filter change
        struct AddEffectsEntry
        {
            std::string type;
            std::string path;
            std::string package;  // top-level directory under Shaders/, for grouping
            int group = 0;
            int score = 0;
        };
        std::vector<AddEffectsEntry> addEffectsEntries;
        std::vector<std::string> addEffectsPackages;  // unique package names for the tabs
        struct AddEffectsRow
        {
            std::string header;  // non-empty on a package header row
            int entry = -1;
        };
        std::vector<AddEffectsRow> addEffectsRows;  // display order, headers included
        std::string addEffectsCacheSearch;
        std::string addEffectsCacheFilter;  // selected package, empty = All
        uint64_t addEffectsCacheVersion = UINT64_MAX;
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
        uint64_t shaderTestStateVersion = 0;  // availableVersion the finished test ran against
        size_t shaderTestCurrentIndex = 0;
        int shaderTestDuplicateCount = 0;  // Number of duplicate shaders skipped
        std::vector<std::pair<std::string, std::string>> shaderTestQueue;  // {effectName, filePath}
        std::vector<std::string> shaderTestIncludePaths;  // Cached include paths (avoid re-reading config per shader)
        std::vector<std::tuple<std::string, std::string, bool, std::string>> shaderTestResults;  // {name, path, success, error}
        std::set<std::string> depthShaders;  // Effect names that use depth buffer (populated by shader test)
        std::set<std::string> checkedShaders; // Effects already checked for depth (avoids recompiling)

        // UI state for settings view
        int listeningForKey = 0;  // 0=none, 1=toggle, 2=reload, 3=overlay

        // Non-interactive top-right notifications. They render even when
        // the main overlay is hidden, so fatal errors stay visible.
        mutable std::mutex toastsMutex;
        std::vector<ToastNotification> toasts;

        // Edit buffers for the parameter widgets: a widget needs a mutable
        // value, the snapshot's copies are const. Each buffer is refreshed from
        // the snapshot every frame and only leaves as a SetParam command.
        struct ParamEdit
        {
            std::unique_ptr<EffectParam> buffer;
            uint64_t seenFrame = 0;
        };
        std::map<std::pair<std::string, std::string>, ParamEdit> paramEdits;
        uint64_t paramEditFrame = 0;
        EffectParam& editableParam(const std::string& effect, const EffectParam& source);
        void pruneParamEdits();

        // debounce of parameter edits: a rebuild and a save follow once the
        // user pauses (see runAutoApply)
        bool paramsDirty = false;
        bool rebuildNeeded = false;  // a pending edit that needs the chain rebuilt; a plain uniform edit is live
        bool saveDue = false;
        std::chrono::steady_clock::time_point lastChangeTime;

        bool visible = false;
        bool initialized = false;
        bool depthCaptureAtStartup = false;   // value the depth hooks were armed with
        // frame pacing for ImGui's DeltaTime and the adaptive fence timeout
        float avgFrameTimeMs = 8.0f;
        float lastGoodDt = 0.008f;
        std::chrono::steady_clock::time_point lastFrameTime = std::chrono::steady_clock::now();
        std::shared_ptr<DiagnosticsState> diagnostics;  // sampled stats and history (view_diagnostics.cc)
        // file pickers used when the XDG portal is unavailable; created on first use
        std::shared_ptr<ImGui::FileBrowser> presetBrowser;
        std::shared_ptr<ImGui::FileBrowser> dirBrowser;

        // view state
        std::vector<std::string> recentEffects;   // add-effects: session recents
        std::string addEffectsFilterPackage;      // add-effects: selected package tab, empty = All
        std::string deleteProfileTarget;
        std::string renameProfileTarget;
        char renameShaderProfileName[64] = "";
        char newShaderProfileName[64] = "";
        std::string paramsActiveEffect;
        float appliedUiScale = -1.0f;
        int activeView = 0;
        bool resetLayoutRequested = false;  // When true, reset window position/size next frame
        uint32_t currentWidth = 1920;   // Current surface resolution for VRAM estimates
        uint32_t currentHeight = 1080;
        char saveConfigName[64] = "";
    };

} // namespace VkHook

#endif // VKHOOK_OVERLAY_UI_HH_INCLUDED
