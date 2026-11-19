#include "overlay_ui.hh"

#include "overlay_renderer.hh"
#include "logical_device.hh"
#include "vk_handle.hh"
#include "logger.hh"
#include "util.hh"
#include "mouse_input.hh"
#include "keyboard_input.hh"
#include "input_blocker.hh"
#include "config_serializer.hh"
#include "persist.hh"
#include "wayland_display.hh"
#include "x11_display.hh"
#include "wayland_input_common.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <memory>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imgui_internal.h"
#include "overlay/ui_theme.hh"
#include "vendor/imgui/backends/imgui_impl_vulkan.h"

namespace VkHook
{

    // No-op dummy for Vulkan functions ImGui requests but VkHook doesn't intercept.
    // ImGui's LoadFunctions treats nullptr returns as failures, so we need a valid pointer.
    static void VKAPI_CALL dummyVulkanFunc() {}

    // Function loader using VkHook's dispatch tables
    static PFN_vkVoidFunction imguiVulkanLoaderDummy(const char* function_name, void* user_data)
    {
        LogicalDevice* device = static_cast<LogicalDevice*>(user_data);

        // Device functions from VkHook's dispatch table
        #define CHECK_FUNC(name) if (strcmp(function_name, "vk" #name) == 0) return (PFN_vkVoidFunction)device->vkd.name

        CHECK_FUNC(AllocateCommandBuffers);
        CHECK_FUNC(AllocateDescriptorSets);
        CHECK_FUNC(AllocateMemory);
        CHECK_FUNC(BeginCommandBuffer);
        CHECK_FUNC(BindBufferMemory);
        CHECK_FUNC(BindImageMemory);
        CHECK_FUNC(CmdBeginRenderPass);
        CHECK_FUNC(CmdBindDescriptorSets);
        CHECK_FUNC(CmdBindIndexBuffer);
        CHECK_FUNC(CmdBindPipeline);
        CHECK_FUNC(CmdBindVertexBuffers);
        CHECK_FUNC(CmdCopyBufferToImage);
        CHECK_FUNC(CmdDrawIndexed);
        CHECK_FUNC(CmdEndRenderPass);
        CHECK_FUNC(CmdPipelineBarrier);
        CHECK_FUNC(CmdPushConstants);
        CHECK_FUNC(CmdSetScissor);
        CHECK_FUNC(CmdSetViewport);
        CHECK_FUNC(CreateBuffer);
        CHECK_FUNC(CreateCommandPool);
        CHECK_FUNC(CreateDescriptorPool);
        CHECK_FUNC(CreateDescriptorSetLayout);
        CHECK_FUNC(CreateFence);
        CHECK_FUNC(CreateFramebuffer);
        CHECK_FUNC(CreateGraphicsPipelines);
        CHECK_FUNC(CreateImage);
        CHECK_FUNC(CreateImageView);
        CHECK_FUNC(CreatePipelineLayout);
        CHECK_FUNC(CreateRenderPass);
        CHECK_FUNC(CreateSampler);
        CHECK_FUNC(CreateSemaphore);
        CHECK_FUNC(CreateShaderModule);
        CHECK_FUNC(CreateSwapchainKHR);
        CHECK_FUNC(DestroyBuffer);
        CHECK_FUNC(DestroyCommandPool);
        CHECK_FUNC(DestroyDescriptorPool);
        CHECK_FUNC(DestroyDescriptorSetLayout);
        CHECK_FUNC(DestroyFence);
        CHECK_FUNC(DestroyFramebuffer);
        CHECK_FUNC(DestroyImage);
        CHECK_FUNC(DestroyImageView);
        CHECK_FUNC(DestroyPipeline);
        CHECK_FUNC(DestroyPipelineLayout);
        CHECK_FUNC(DestroyRenderPass);
        CHECK_FUNC(DestroySampler);
        CHECK_FUNC(DestroySemaphore);
        CHECK_FUNC(DestroyShaderModule);
        CHECK_FUNC(DestroySwapchainKHR);
        CHECK_FUNC(EndCommandBuffer);
        CHECK_FUNC(FlushMappedMemoryRanges);
        CHECK_FUNC(FreeCommandBuffers);
        CHECK_FUNC(FreeDescriptorSets);
        CHECK_FUNC(FreeMemory);
        CHECK_FUNC(GetBufferMemoryRequirements);
        CHECK_FUNC(GetDeviceQueue);
        CHECK_FUNC(GetImageMemoryRequirements);
        CHECK_FUNC(GetSwapchainImagesKHR);
        CHECK_FUNC(MapMemory);
        CHECK_FUNC(QueueSubmit);
        CHECK_FUNC(QueueWaitIdle);
        CHECK_FUNC(ResetCommandPool);
        CHECK_FUNC(ResetFences);
        CHECK_FUNC(UnmapMemory);
        CHECK_FUNC(UpdateDescriptorSets);
        CHECK_FUNC(WaitForFences);
        // Not in the layer's dispatch table; resolve it directly.
        if (strcmp(function_name, "vkGetFenceStatus") == 0)
            return (PFN_vkVoidFunction)device->vkd.GetDeviceProcAddr(device->device, "vkGetFenceStatus");
        #undef CHECK_FUNC

        // Instance functions from VkHook's dispatch
        #define CHECK_IFUNC(name) if (strcmp(function_name, "vk" #name) == 0) return (PFN_vkVoidFunction)device->vki.name
        CHECK_IFUNC(GetPhysicalDeviceMemoryProperties);
        CHECK_IFUNC(GetPhysicalDeviceProperties);
        CHECK_IFUNC(GetPhysicalDeviceQueueFamilyProperties);
        #undef CHECK_IFUNC

        // Return a no-op dummy for unknown functions. ImGui's LoadFunctions treats
        // nullptr as a load failure, so we must return a valid function pointer.
        // These functions are never actually called by ImGui in our usage.
        return (PFN_vkVoidFunction)dummyVulkanFunc;
    }

    static ImGuiKey toImGuiKey(NavKey key)
    {
        switch (key)
        {
            case NavKey::Backspace: return ImGuiKey_Backspace;
            case NavKey::Delete: return ImGuiKey_Delete;
            case NavKey::Enter: return ImGuiKey_Enter;
            case NavKey::Left: return ImGuiKey_LeftArrow;
            case NavKey::Right: return ImGuiKey_RightArrow;
            case NavKey::Up: return ImGuiKey_UpArrow;
            case NavKey::Down: return ImGuiKey_DownArrow;
            case NavKey::Home: return ImGuiKey_Home;
            case NavKey::End: return ImGuiKey_End;
            case NavKey::PageUp: return ImGuiKey_PageUp;
            case NavKey::PageDown: return ImGuiKey_PageDown;
            case NavKey::Tab: return ImGuiKey_Tab;
            case NavKey::Escape: return ImGuiKey_Escape;
            case NavKey::Count: break;
        }
        return ImGuiKey_None;
    }


    namespace
    {
        // The one place this file releases GPU objects. The deferred queue
        // decides when the destroy runs, so tearing the UI down during device
        // teardown never calls into a destroyed VkDevice. The integrator
        // retargets this to the device's RetireQueue in one edit.
        template<typename Destroy>
        void retire(DestroyPhase phase, Destroy&& destroy)
        {
            DeferredDestroyQueue::instance().push(phase, std::forward<Destroy>(destroy));
        }
    } // namespace

    OverlayUi::ContextScope::ContextScope(ImGuiContext* context) : previous(ImGui::GetCurrentContext())
    {
        ImGui::SetCurrentContext(context);
    }

    OverlayUi::ContextScope::~ContextScope()
    {
        ImGui::SetCurrentContext(previous);
    }

    std::unique_ptr<OverlayUi> OverlayUi::create(LogicalDevice* device, OverlayPersistentState* persistentState,
                                                 CommandQueue* commands, const CoreSnapshotSlot* snapshots)
    {
        if (!device || !commands || !snapshots)
            return nullptr;
        std::unique_ptr<OverlayUi> ui(new OverlayUi(device, persistentState, commands, snapshots));
        if (!ui->initialize())
            return nullptr;
        return ui;
    }

    OverlayUi::OverlayUi(LogicalDevice* device, OverlayPersistentState* persistentState, CommandQueue* queue,
                         const CoreSnapshotSlot* slot)
        : logicalDevice(device), pPersistentState(persistentState), commands(queue), snapshots(slot),
          snapshot(slot->load())
    {
    }

    bool OverlayUi::initialize()
    {
        IMGUI_CHECKVERSION();
        // Each overlay owns its context. CreateContext makes it current; the
        // scope puts back whatever the host (or another overlay) had.
        ContextScope previous(ImGui::GetCurrentContext());
        imguiContext = ImGui::CreateContext();
        ImGui::SetCurrentContext(imguiContext);

        // imconfig disables ImGui's default shell handler, so TextLinkOpenURL
        // would silently do nothing. route it through our xdg-open helper.
        ImGui::GetPlatformIO().Platform_OpenInShellFn = [](ImGuiContext*, const char* url) -> bool
        {
            return openInShell(url);
        };
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigWindowsMoveFromTitleBarOnly = true;

        std::string iniPath = ConfigSerializer::getBaseConfigDir() + "/imgui.ini";
        std::ifstream iniFile(iniPath);
        std::string iniContent((std::istreambuf_iterator<char>(iniFile)),
                                std::istreambuf_iterator<char>());

        if (!iniContent.empty())
            ImGui::LoadIniSettingsFromMemory(iniContent.c_str(), iniContent.size());

        ImGui::StyleColorsDark();
        ImFontConfig fontConfig;
        fontConfig.SizePixels = 18.0f;
        io.Fonts->AddFontDefault(&fontConfig);
        Logger::info("ImGui: standard dark style and default font");

        depthCaptureAtStartup = settings().depthCapture;

        // Load Vulkan functions for ImGui using VkHook's dispatch tables
        if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, imguiVulkanLoaderDummy, logicalDevice))
        {
            Logger::err("Failed to load Vulkan functions for ImGui");
            ImGui::DestroyContext(imguiContext);
            imguiContext = nullptr;
            return false;
        }

        VkDescriptorPoolSize poolSizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 }
        };
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 100;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = poolSizes;
        const VkResult vr = logicalDevice->vkd.CreateDescriptorPool(logicalDevice->device, &poolInfo, nullptr, &descriptorPool);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to create ImGui descriptor pool: " + std::to_string(vr));
            descriptorPool = VK_NULL_HANDLE;
            ImGui::DestroyContext(imguiContext);
            imguiContext = nullptr;
            return false;
        }

        // Restore UI preferences from persistent state
        if (pPersistentState)
            visible = pPersistentState->visible;
        setInputBlocked(visible);

        initialized = true;
        Logger::info("ImGui overlay initialized");
        return true;
    }

    OverlayUi::~OverlayUi()
    {
        if (!renderers.empty())
            Logger::err("OverlayUi destroyed while " + std::to_string(renderers.size())
                        + " renderer(s) are still attached; destroy swapchain overlays first");

        if (imguiContext)
        {
            ImGuiContext* const previousContext = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(imguiContext);

            if (initialized)
            {
                const std::string iniPath = ConfigSerializer::getBaseConfigDir() + "/imgui.ini";
                std::string iniText = ImGui::SaveIniSettingsToMemory();
                PersistenceQueue::instance().write(iniPath, [iniPath, iniText = std::move(iniText)] {
                    return writeAtomically(iniPath, iniText);
                });
            }

            // the saves are queued on the writer thread; make sure they land before
            // the layer tears down rather than racing process exit.
            PersistenceQueue::instance().waitForIdle();

            logicalDevice->vkd.QueueWaitIdle(logicalDevice->queue);

            // Release ImGui's descriptor references before the pool is retired.
            shutdownBackend();
            ImGuiContext* const owned = imguiContext;
            ImGui::DestroyContext(owned);
            imguiContext = nullptr;
            // hand back whichever context the host had, unless that was ours
            ImGui::SetCurrentContext(previousContext == owned ? nullptr : previousContext);
        }

        if (descriptorPool != VK_NULL_HANDLE)
        {
            const VkDescriptorPool pool = descriptorPool;
            const VkDevice device = logicalDevice->device;
            const auto vkd = logicalDevice->vkd;
            retire(DestroyPhase::Descriptor, [vkd, device, pool] { vkd.DestroyDescriptorPool(device, pool, nullptr); });
        }
        Logger::info("ImGui overlay destroyed");
    }

    void OverlayUi::toggle()
    {
        visible = !visible;
        Logger::debug(std::string("overlay toggled ") + (visible ? "on" : "off"));
        setInputBlocked(visible);

        saveToPersistentState();
    }

    void OverlayUi::emitSetting(const char* key, SettingValue value)
    {
        emit(SetSetting{key, std::move(value)});
    }

    void OverlayUi::noteEdit(bool needsRebuild)
    {
        paramsDirty = true;
        saveDue = true;
        rebuildNeeded = rebuildNeeded || needsRebuild;
        lastChangeTime = std::chrono::steady_clock::now();
    }

    void OverlayUi::chainEdited()
    {
        emit(RebuildChain{});
        saveDue = true;
    }

    void OverlayUi::dropPendingEdits()
    {
        paramsDirty = false;
        rebuildNeeded = false;
        saveDue = false;
    }

    // Auto-apply: once the user pauses, rebuild the chain if an edit needs it
    // and then save the preset. A plain ReShade uniform edit is applied live by
    // the effect each frame, so it only needs the save.
    void OverlayUi::runAutoApply()
    {
        if (settings().autoApply && paramsDirty)
        {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastChangeTime).count();
            const ImGuiIO& io = ImGui::GetIO();
            const bool mouseButtonDown = io.MouseDown[0] || io.MouseDown[1];

            // Defer while either mouse button is held (dragging/resizing/sliders):
            // apply on the first frame after release once the delay has elapsed.
            if (elapsed >= settings().autoApplyDelay && !mouseButtonDown)
            {
                if (rebuildNeeded)
                    emit(RebuildChain{});
                rebuildNeeded = false;
                paramsDirty = false;
            }
        }

        if (saveDue && !paramsDirty)
        {
            emit(SaveProfile{});
            saveDue = false;
        }
    }

    EffectParam& OverlayUi::editableParam(const std::string& effect, const EffectParam& source)
    {
        ParamEdit& edit = paramEdits[{effect, source.name}];
        if (!edit.buffer || !assignParamValue(*edit.buffer, source))
            edit.buffer = source.clone();
        edit.seenFrame = paramEditFrame;
        return *edit.buffer;
    }

    void OverlayUi::pruneParamEdits()
    {
        for (auto it = paramEdits.begin(); it != paramEdits.end();)
        {
            if (it->second.seenFrame != paramEditFrame)
                it = paramEdits.erase(it);
            else
                ++it;
        }
        ++paramEditFrame;
    }
    void OverlayUi::pushToast(LogLevel level, const std::string& message)
    {
        std::lock_guard<std::mutex> lock(toastsMutex);
        // Avoid stacking identical messages — refresh timestamp instead.
        for (auto& t : toasts)
        {
            if (t.message == message && t.level == level)
            {
                t.createdAt = std::chrono::steady_clock::now();
                return;
            }
        }
        toasts.push_back({message, level, std::chrono::steady_clock::now()});
        // Hard cap so a runaway panic loop can't exhaust memory.
        if (toasts.size() > 8)
            toasts.erase(toasts.begin(), toasts.end() - 8);
    }

    bool OverlayUi::hasPendingToasts() const
    {
        std::lock_guard<std::mutex> lock(toastsMutex);
        return !toasts.empty();
    }

    void OverlayUi::renderToasts()
    {
        const auto now = std::chrono::steady_clock::now();
        std::vector<ToastNotification> snapshot;
        {
            std::lock_guard<std::mutex> lock(toastsMutex);
            toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](const auto& toast) {
                return std::chrono::duration<float>(now - toast.createdAt).count() >= 5.0f;
            }), toasts.end());
            snapshot = toasts;
        }
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float margin = ImGui::GetStyle().WindowPadding.x;
        const float toastWidth = std::max(1.0f, std::min(480.0f * appliedUiScale,
                                                       viewport->WorkSize.x - margin * 2.0f));
        float y = viewport->WorkPos.y + margin;
        for (size_t i = 0; i < snapshot.size(); ++i)
        {
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - margin, y),
                                    ImGuiCond_Always, ImVec2(1.0f, 0.0f));
            ImGui::SetNextWindowSize(ImVec2(toastWidth, 0.0f));
            char label[48];
            snprintf(label, sizeof(label), "##vkhookToast%zu", i);
            if (ImGui::Begin(label, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                    | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
                    | ImGuiWindowFlags_NoInputs))
            {
                ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
                ImGui::PushStyleColor(ImGuiCol_Text, UI::LogLevelColor(snapshot[i].level));
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(snapshot[i].message.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            y += ImGui::GetWindowHeight() + ImGui::GetStyle().ItemSpacing.y;
            ImGui::End();
        }
    }

    void OverlayUi::saveToPersistentState()
    {
        if (!pPersistentState)
            return;

        // Only save UI preferences - effect state is in the registry, settings in settingsManager
        pPersistentState->visible = visible;
    }

    bool OverlayUi::initBackend(VkFormat format, VkRenderPass renderPass, uint32_t imageRing)
    {
        ImGui_ImplVulkan_InitInfo initInfo = {};
        initInfo.ApiVersion = VK_API_VERSION_1_3;
        initInfo.Instance = logicalDevice->instance;
        initInfo.PhysicalDevice = logicalDevice->physicalDevice;
        initInfo.Device = logicalDevice->device;
        initInfo.QueueFamily = logicalDevice->queueFamilyIndex;
        initInfo.Queue = logicalDevice->queue;
        initInfo.DescriptorPool = descriptorPool;
        // ImageCount MUST cover every swapchain image the overlay draws into.
        // A smaller ring wraps too early and overwrites vertex/index buffers
        // still in flight on the GPU: ghost trails, stale vertices, black
        // corruption.
        initInfo.MinImageCount = imageRing;
        initInfo.ImageCount = imageRing;
        initInfo.PipelineInfoMain.RenderPass = renderPass;

        ImGui_ImplVulkan_Init(&initInfo);
        backendInitialized = true;
        backendFormat = format;
        backendRing = imageRing;
        Logger::debug("ImGui Vulkan backend initialized");
        return true;
    }

    void OverlayUi::shutdownBackend()
    {
        if (!backendInitialized)
            return;
        ImGui_ImplVulkan_Shutdown();
        backendInitialized = false;
        backendFormat = VK_FORMAT_UNDEFINED;
        backendRing = 0;
    }

    // ImGui's pipeline is bound to a render pass and its buffer ring to the
    // number of images drawn into, so a swapchain with another format, or more
    // images than the ring covers, rebuilds the backend. Render passes of one
    // format are compatible, so swapchains that agree share one pipeline.
    bool OverlayUi::ensureBackend(VkFormat format, VkRenderPass renderPass, uint32_t extraImages)
    {
        ContextScope scope(imguiContext);
        uint32_t ring = extraImages;
        for (const OverlayRenderer* renderer : renderers)
            ring += renderer->imageCount();
        ring = std::max(ring, 2u);

        if (backendInitialized && format == backendFormat && ring <= backendRing)
            return true;

        if (backendInitialized)
        {
            // everything any renderer submitted is tracked by its fences; wait
            // them out (bounded) before the old pipeline and buffers go
            for (OverlayRenderer* renderer : renderers)
            {
                if (!renderer->waitIdle(1'000'000'000ull))
                {
                    Logger::err("ImGui: overlay work still in flight; keeping the current backend");
                    return false;
                }
            }
            Logger::info("ImGui: rebuilding the Vulkan backend for format " + std::to_string(format) + ", "
                         + std::to_string(ring) + " images");
            shutdownBackend();
        }
        return initBackend(format, renderPass, ring);
    }

    bool OverlayUi::attachRenderer(OverlayRenderer* renderer, VkFormat format, VkRenderPass renderPass,
                                   uint32_t imageCount)
    {
        if (!ensureBackend(format, renderPass, imageCount))
            return false;
        renderers.push_back(renderer);
        return true;
    }

    void OverlayUi::detachRenderer(OverlayRenderer* renderer)
    {
        renderers.erase(std::remove(renderers.begin(), renderers.end(), renderer), renderers.end());
    }

    uint64_t OverlayUi::fenceTimeoutNs() const
    {
        return static_cast<uint64_t>(std::clamp(avgFrameTimeMs * 4.0f, 2.0f, 50.0f) * 1'000'000.0f);
    }

    bool OverlayUi::beginFrameCheck(VkFormat format, VkRenderPass renderPass)
    {
        // Informational notifications remain visible without capturing input.
        const bool hasToasts = hasPendingToasts();
        if (!visible && !hasToasts)
        {
            // drop last frame's hitboxes, or they keep swallowing input.
            frameInputRects.clear();
            setInputRects(nullptr, 0);
            setInputBlocked(false);
            setInputInteractive(false);
            setOverlayWantsKeyboard(false);
            setWaylandInputSurfaceRects(nullptr, 0);
            return false;
        }
        return ensureBackend(format, renderPass, 0);
    }

    void OverlayUi::drawFrame(ImDrawData* drawData, VkCommandBuffer commandBuffer)
    {
        ContextScope scope(imguiContext);
        ImGui_ImplVulkan_RenderDrawData(drawData, commandBuffer);
    }

    ImDrawData* OverlayUi::buildFrame(uint32_t width, uint32_t height)
    {
        ContextScope contextScope(imguiContext);
        snapshot = snapshots->load();

        // Publish input ownership before sampling this frame's pointer.
        frameInputRects.clear();
        const bool captureInput = visible && settings().overlayBlockInput;
        setInputBlocked(captureInput);
        setInputInteractive(captureInput);
        const InputRect captureRect{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
        setWaylandInputSurfaceRects(captureInput ? &captureRect : nullptr, captureInput ? 1 : 0);

        // Current resolution, for the VRAM estimate in settings
        currentWidth = width;
        currentHeight = height;

        // Set display size and frame timing BEFORE NewFrame
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
        const float requestedScale = settings().uiScale;
        const float scale = requestedScale > 0.0f ? requestedScale
            : std::clamp(static_cast<float>(height) / 900.0f, 1.0f, 2.5f);
        // Do not move a slider's geometry under the pointer while it is dragged.
        if (scale != appliedUiScale && !ImGui::IsAnyItemActive())
        {
            // Always scale fresh defaults: repeated changes must not compound.
            ImGui::GetStyle() = ImGuiStyle();
            ImGui::StyleColorsDark();
            ImGui::GetStyle().ScaleAllSizes(scale);
            ImGui::GetStyle().FontSizeBase = 18.0f;
            ImGui::GetStyle().FontScaleMain = scale;
            appliedUiScale = scale;
        }

        // DeltaTime for ImGui's internal timing (drag thresholds, animations).
        // Uses last known good value as fallback instead of hardcoded 1/60.
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;
        if (dt > 0.0f && dt < 1.0f)
        {
            io.DeltaTime = dt;
            lastGoodDt = dt;
            // exponential moving average drives the adaptive fence timeout
            avgFrameTimeMs = avgFrameTimeMs * 0.9f + (dt * 1000.0f) * 0.1f;
        }
        else
        {
            io.DeltaTime = lastGoodDt;
        }

        // Mouse input for interactivity
        const MouseState mouse = getMouseState();
        io.MousePos = ImVec2(static_cast<float>(mouse.x), static_cast<float>(mouse.y));

        io.MouseDown[0] = mouse.leftButton;
        io.MouseDown[1] = mouse.rightButton;
        io.MouseDown[2] = mouse.middleButton;
        io.MouseWheel = mouse.scrollDelta;
        // A captured game may hide or warp its native pointer. Draw the pointer
        // supplied by the input backend instead; X11 hides its native cursor
        // while the independent pointer grab is active.
        io.MouseDrawCursor = visible
            && (getSurfaceKind() == SurfaceKind::Wayland
                || (captureInput && isX11() && x11PointerGrabActive()));

        auto publishOverlayInput = [&]() {
            const bool blocking = visible && settings().overlayBlockInput;
            if (blocking)
                setInputRects(frameInputRects.data(), static_cast<int>(frameInputRects.size()));
            else
                setInputRects(nullptr, 0);

            // The input backend owns capture for the full open session, including
            // a drag that leaves the panel. Text/popups additionally request keys.
            setInputInteractive(blocking);
            setOverlayWantsKeyboard(blocking
                && (io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)));
            updatePointerPosition(static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            setInputBlocked(blocking);
            setWaylandInputSurfaceRects(blocking ? &captureRect : nullptr, blocking ? 1 : 0);
        };

        // Navigation keys arrive as one-shot presses, so each one is sent as a
        // press and a release. Text arrives as UTF-8 and goes in whole.
        const KeyboardState keyboard = getKeyboardState();
        io.AddKeyEvent(ImGuiMod_Shift, keyboard.shift);
        io.AddKeyEvent(ImGuiMod_Ctrl, keyboard.ctrl);
        io.AddKeyEvent(ImGuiMod_Alt, keyboard.alt);
        io.AddKeyEvent(ImGuiMod_Super, keyboard.super);
        if (visible)
        {
            if (!keyboard.typedChars.empty())
                io.AddInputCharactersUTF8(keyboard.typedChars.c_str());
            for (const NavKey key : keyboard.navPresses)
            {
                const ImGuiKey imguiKey = toImGuiKey(key);
                io.AddKeyEvent(imguiKey, true);
                io.AddKeyEvent(imguiKey, false);
            }
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();

        // Hidden overlays draw notifications only, without a menu or input grab.
        if (!visible)
        {
            renderToasts();
            // main window is gone; only toasts remain, so free the pointer.
            setWaylandInputSurfaceRects(nullptr, 0);
            publishOverlayInput();
            ImGui::Render();
            return ImGui::GetDrawData();
        }

        const ImVec2 displaySize(static_cast<float>(width), static_cast<float>(height));
        const ImVec2 defaultSize(std::min(720.0f * scale, displaySize.x * 0.92f),
                                 std::min(780.0f * scale, displaySize.y * 0.92f));
        const ImGuiCond layoutCondition = resetLayoutRequested ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
        ImGui::SetNextWindowSize(defaultSize, layoutCondition);
        ImGui::SetNextWindowPos(ImVec2(displaySize.x - defaultSize.x - displaySize.x * 0.04f,
                                      displaySize.y * 0.04f), layoutCondition);
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(std::min(480.0f * scale, displaySize.x), std::min(320.0f * scale, displaySize.y)),
            displaySize);
        resetLayoutRequested = false;

        processShaderTest();
        sampleDiagnostics();

        bool windowOpen = true;
        if (ImGui::Begin("VkHook", &windowOpen,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        {
            const ImVec2 position = ImGui::GetWindowPos();
            const ImVec2 size = ImGui::GetWindowSize();
            frameInputRects.push_back(InputRect{position.x, position.y, size.x, size.y});
            static const char* const labels[] = {"Effects", "Shaders", "Settings", "About"};
            if (ImGui::BeginTabBar("Views"))
            {
                for (int tab = 0; tab < 4; ++tab)
                {
                    if (!ImGui::BeginTabItem(labels[tab]))
                        continue;
                    if (activeView != tab)
                    {
                        activeView = tab;
                        openSubView = SubView_None;
                        inSelectionMode = false;
                        inConfigManageMode = false;
                    }
                    ImGui::PushID(tab);
                    ImGui::PushID(openSubView);
                    ImGui::PushID(inSelectionMode ? 1 : inConfigManageMode ? 2 : 0);
                    if (ImGui::BeginChild("Content", ImVec2(0, 0)))
                    {
                        if (openSubView == SubView_DepthBuffers)
                            renderDepthBuffersView();
                        else if (openSubView == SubView_Diagnostics)
                            renderDiagnosticsView();
                        else if (tab == 0)
                        {
                            if (inSelectionMode) renderAddEffectsView();
                            else if (inConfigManageMode) renderConfigManagerView();
                            else renderMainView();
                        }
                        else if (tab == 1) renderShaderManagerView();
                        else if (tab == 2) renderSettingsView(keyboard);
                        else renderAboutView();
                    }
                    ImGui::EndChild();
                    ImGui::PopID();
                    ImGui::PopID();
                    ImGui::PopID();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
        if (!windowOpen)
            toggle();

        // toasts last, so they draw above the overlay
        renderToasts();

        runAutoApply();
        pruneParamEdits();

        // combo dropdowns and dialogs are separate windows that stick out past
        // the main one; publish their rects too so pointer events over them are
        // withheld from the game instead of leaking through.
        if (ImGuiContext* ctx = ImGui::GetCurrentContext())
        {
            for (ImGuiWindow* w : ctx->Windows)
            {
                if (!w->Active || !(w->Flags & ImGuiWindowFlags_Popup))
                    continue;
                frameInputRects.push_back(InputRect{w->Pos.x, w->Pos.y, w->Size.x, w->Size.y});
            }
        }

        publishOverlayInput();
        ImGui::Render();
        return ImGui::GetDrawData();
    }

} // namespace VkHook
