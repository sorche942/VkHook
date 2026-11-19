#include "imgui_overlay.hh"

#include "vk_handle.hh"
#include "effects/effect_registry.hh"
#include "settings_manager.hh"
#include "reshade_parser.hh"
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

    ImGuiOverlay::ContextScope::ContextScope(ImGuiContext* context) : previous(ImGui::GetCurrentContext())
    {
        ImGui::SetCurrentContext(context);
    }

    ImGuiOverlay::ContextScope::~ContextScope()
    {
        ImGui::SetCurrentContext(previous);
    }

    ImGuiOverlay::ImGuiOverlay(LogicalDevice* device, VkFormat swapchainFormat, uint32_t imageCount, OverlayPersistentState* persistentState)
        : logicalDevice(device), pPersistentState(persistentState)
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

        depthCaptureAtStartup = settingsManager.getDepthCapture();
        initVulkanBackend(swapchainFormat, imageCount);

        // Restore UI preferences from persistent state
        if (pPersistentState)
            visible = pPersistentState->visible;
        setInputBlocked(visible);

        initialized = true;
        Logger::info("ImGui overlay initialized");
    }


    ImGuiOverlay::~ImGuiOverlay()
    {
        ImGuiContext* const previousContext = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(imguiContext);

        // Auto-save profile on shutdown (before GPU cleanup)
        if (initialized && (profileDirty || paramsDirty) && (!activeProfilePath.empty() || !activeShaderProfilePath.empty()))
            autoSaveProfile();

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

        // Release ImGui's descriptor references before the deferred pool teardown.
        if (backendInitialized)
            ImGui_ImplVulkan_Shutdown();
        ImGuiContext* const owned = imguiContext;
        ImGui::DestroyContext(owned);
        imguiContext = nullptr;
        // hand back whichever context the host had, unless that was ours
        ImGui::SetCurrentContext(previousContext == owned ? nullptr : previousContext);

        // Deferred, like every other owner. The QueueWaitIdle above already makes
        // this a safe moment, but inline destruction still meant that an overlay
        // torn down during device teardown would call into a destroyed VkDevice.
        // Handing the handles over means the flush at device destroy decides when
        // they actually go, and the handle is still valid when it does.
        auto& queue   = DeferredDestroyQueue::instance();
        auto  device = logicalDevice->device;
        auto& vkd    = logicalDevice->vkd;

        const VkCommandPool   pool = commandPool;
        const VkRenderPass    pass = renderPass;
        const VkDescriptorPool descPool = descriptorPool;

        if (pool != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [vkd, device, pool] { vkd.DestroyCommandPool(device, pool, nullptr); });

        if (descPool != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Descriptor, [vkd, device, descPool] { vkd.DestroyDescriptorPool(device, descPool, nullptr); });

        // framebuffers before the pass they were created from
        for (auto fb : framebuffers)
        {
            if (fb != VK_NULL_HANDLE)
                queue.push(DestroyPhase::RenderPass, [vkd, device, fb] { vkd.DestroyFramebuffer(device, fb, nullptr); });
        }
        if (pass != VK_NULL_HANDLE)
            queue.push(DestroyPhase::RenderPass, [vkd, device, pass] { vkd.DestroyRenderPass(device, pass, nullptr); });

        // Fences are the earliest phase: a fence whose command pool is gone is
        // fine to destroy, but a command pool destroyed while a submitted fence
        // is still unsignalled is not, and the pool lands in Resource.
        for (auto fence : commandBufferFences)
        {
            if (fence != VK_NULL_HANDLE)
                queue.push(DestroyPhase::Sync, [vkd, device, fence] { vkd.DestroyFence(device, fence, nullptr); });
        }

        Logger::info("ImGui overlay destroyed");
    }

    void ImGuiOverlay::toggle()
    {
        visible = !visible;
        Logger::debug(std::string("overlay toggled ") + (visible ? "on" : "off"));
        setInputBlocked(visible);

        saveToPersistentState();
    }

    void ImGuiOverlay::refreshShaderProfiles()
    {
        shaderProfiles = ConfigSerializer::listShaderProfilesForGame(activeGameName);
        if (activeShaderProfileName.empty())
        {
            const auto lastUsedProfile = ConfigSerializer::getLastShaderProfile(activeGameName);
            if (std::find(shaderProfiles.begin(), shaderProfiles.end(), lastUsedProfile) != shaderProfiles.end())
                activeShaderProfileName = lastUsedProfile;
        }
        if (activeShaderProfileName.empty() ||
            std::find(shaderProfiles.begin(), shaderProfiles.end(), activeShaderProfileName) == shaderProfiles.end())
        {
            const auto defaultProfile = std::find(shaderProfiles.begin(), shaderProfiles.end(), "default");
            activeShaderProfileName = defaultProfile != shaderProfiles.end()
                ? *defaultProfile
                : (shaderProfiles.empty() ? std::string() : shaderProfiles.front());
        }
        activeShaderProfilePath = ConfigSerializer::getShaderProfilePath(activeGameName, activeShaderProfileName);
        queueLastShaderProfile();
    }

    void ImGuiOverlay::queueLastShaderProfile()
    {
        if (activeGameName.empty() || activeShaderProfileName.empty())
            return;
        PersistenceQueue::instance().other(
            "last-profile:" + activeGameName,
            [game = activeGameName, profile = activeShaderProfileName] {
                return ConfigSerializer::setLastShaderProfile(game, profile);
            });
    }

    void ImGuiOverlay::setActiveShaderProfile(const std::string& profileName)
    {
        activeShaderProfileName = profileName;
        activeShaderProfilePath = ConfigSerializer::getShaderProfilePath(activeGameName, activeShaderProfileName);
        queueLastShaderProfile();
    }

    bool ImGuiOverlay::switchShaderProfile(const std::string& profileName)
    {
        if (profileName.empty() || profileName == activeShaderProfileName)
            return true;

        // save the outgoing profile while it is still active, then clear the
        // dirty flags so render()'s auto-save can't clobber the new path.
        if (!autoSaveProfile(true))
            return false;

        setActiveShaderProfile(profileName);
        pendingShaderProfilePath = activeShaderProfilePath;
        pendingShaderProfile = true;
        applyRequested = true;
        paramsDirty = false;
        profileDirty = false;
        return true;
    }

    void ImGuiOverlay::pushToast(LogLevel level, const std::string& message)
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

    bool ImGuiOverlay::hasPendingToasts() const
    {
        std::lock_guard<std::mutex> lock(toastsMutex);
        return !toasts.empty();
    }

    void ImGuiOverlay::renderToasts()
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

    void ImGuiOverlay::saveToPersistentState()
    {
        if (!pPersistentState)
            return;

        // Only save UI preferences - effect state is in the registry, settings in settingsManager
        pPersistentState->visible = visible;
    }

    void ImGuiOverlay::updateState(OverlayState newState)
    {
        // only the fields the add-effects view reads; a change here invalidates
        // its cached entry list
        if (newState.currentConfigEffects != state.currentConfigEffects
            || newState.defaultConfigEffects != state.defaultConfigEffects
            || newState.effectPaths != state.effectPaths)
            ++overlayStateVersion;

        state = std::move(newState);

        if (!effectRegistry)
            return;

        // Registry is already initialized from config at swapchain creation
        // Just ensure any newly added effects are in the registry
        const auto& selectedEffects = effectRegistry->getSelectedEffects();
        for (const auto& effectName : selectedEffects)
        {
            if (!effectRegistry->hasEffect(effectName))
                effectRegistry->ensureEffect(effectName);
        }
        // No editableParams merging needed - Registry IS the source of truth
    }

    std::vector<std::unique_ptr<EffectParam>> ImGuiOverlay::getModifiedParams() const
    {
        if (!effectRegistry)
            return {};
        return effectRegistry->getAllParameters();
    }

    std::vector<std::string> ImGuiOverlay::getActiveEffects() const
    {
        std::vector<std::string> activeEffects;
        if (!effectRegistry)
            return activeEffects;

        for (const auto& effectName : effectRegistry->getSelectedEffects())
        {
            if (effectRegistry->isEffectEnabled(effectName))
                activeEffects.push_back(effectName);
        }
        return activeEffects;
    }

    const std::vector<std::string>& ImGuiOverlay::getSelectedEffects() const
    {
        static const std::vector<std::string> empty;  // immutable, so sharing it is harmless
        return effectRegistry ? effectRegistry->getSelectedEffects() : empty;
    }

    void ImGuiOverlay::collectSaveData(
        std::vector<std::string>& effects,
        std::vector<std::string>& disabledEffects,
        std::vector<ConfigParam>& params,
        std::map<std::string, std::string>& effectPaths,
        std::vector<PreprocessorDefinition>& allDefs,
        std::vector<ConfigParam>& disabledEffectParams)
    {
        if (!effectRegistry)
            return;

        effects = effectRegistry->getSelectedEffects();

        for (const auto& effectName : effects)
        {
            const bool effectEnabled = effectRegistry->isEffectEnabled(effectName);
            for (auto* p : effectRegistry->getParametersForEffect(effectName))
            {
                if (p->noSave)
                    continue;
                auto serialized = p->serialize();
                for (const auto& [suffix, value] : serialized)
                {
                    ConfigParam cp;
                    cp.effectName = p->effectName;
                    cp.paramName = suffix.empty() ? p->name : suffix;
                    cp.value = value;
                    params.push_back(cp);
                    if (!effectEnabled)
                        disabledEffectParams.push_back(cp);
                }
            }

            if (!effectEnabled)
                disabledEffects.push_back(effectName);

            std::string path = effectRegistry->getEffectFilePath(effectName);
            if (!path.empty())
                effectPaths[effectName] = path;

            const auto& defs = effectRegistry->getPreprocessorDefs(effectName);
            for (const auto& def : defs)
            {
                allDefs.push_back(def);
                if (!effectEnabled)
                    disabledEffectParams.push_back({def.effectName, "@" + def.name, def.value});
            }
        }
    }

    void ImGuiOverlay::saveCurrentConfig()
    {
        if (!effectRegistry)
            return;

        std::vector<std::string> effects, disabledEffects;
        std::vector<ConfigParam> params;
        std::vector<ConfigParam> disabledEffectParams;
        std::map<std::string, std::string> effectPaths;
        std::vector<PreprocessorDefinition> allDefs;
        collectSaveData(effects, disabledEffects, params, effectPaths, allDefs, disabledEffectParams);

        const std::string configName = saveConfigName;
        const std::string path = ConfigSerializer::getConfigsDir() + "/" + configName + ".conf";
        auto saved = PersistenceQueue::instance().create(path,
            [configName, effects, disabledEffects, params, effectPaths, allDefs] {
                return ConfigSerializer::saveConfig(configName, effects, disabledEffects, params, effectPaths, allDefs);
            });
        saved.wait();  // an explicit user save: the config list is re-read next
        configListRefreshPending = true;
        profileDirty = false;
    }

    bool ImGuiOverlay::autoSaveProfile(bool block)
    {
        if (!effectRegistry)
            return false;
        if (activeProfilePath.empty() && activeShaderProfilePath.empty())
            return true;

        std::vector<std::string> effects, disabledEffects;
        std::vector<ConfigParam> params;
        std::vector<ConfigParam> disabledEffectParams;
        std::map<std::string, std::string> effectPaths;
        std::vector<PreprocessorDefinition> allDefs;
        collectSaveData(effects, disabledEffects, params, effectPaths, allDefs, disabledEffectParams);

        // Snapshot the registry-derived data here: the write runs on the writer
        // thread and must not touch state the present thread mutates. EffectConfig
        // owns unique_ptrs and cannot be copied, so everything the job needs is
        // flattened into plain values now.
        const std::string profilePath = activeProfilePath;
        const std::string shaderPath = activeShaderProfilePath;

        std::set<std::string> disabledFiles;
        std::vector<std::string> enabledTechniques;
        std::vector<std::string> techniqueSorting;
        if (!shaderPath.empty())
        {
            const auto& allEffects = effectRegistry->getAllEffects();
            for (const auto& effectName : disabledEffects)
            {
                const auto effect = std::find_if(allEffects.begin(), allEffects.end(), [&effectName](const EffectConfig& item) {
                    return item.name == effectName;
                });
                if (effect != allEffects.end() && !effect->filePath.empty())
                    disabledFiles.insert(std::filesystem::path(effect->filePath).filename().string());
            }
            const auto& selected = effectRegistry->getSelectedEffects();
            for (const auto& name : selected)
            {
                auto effect = std::find_if(allEffects.begin(), allEffects.end(), [&name](const EffectConfig& item) {
                    return item.name == name;
                });
                if (effect == allEffects.end() || effect->filePath.empty())
                    continue;
                const std::string filename = std::filesystem::path(effect->filePath).filename().string();
                for (const auto& technique : effect->techniqueNames)
                {
                    const std::string entry = technique + "@" + filename;
                    techniqueSorting.push_back(entry);
                    if (effectRegistry->isEffectEnabled(name))
                        enabledTechniques.push_back(entry);
                }
            }
        }

        // Keyed by the preset file: repeated autosaves coalesce to the newest, and
        // a delete or rename of that preset cancels them instead of letting a
        // queued save bring the file back.
        const std::string queueKey = shaderPath.empty() ? profilePath : shaderPath;
        std::future<bool> saved = PersistenceQueue::instance().write(queueKey,
            [profilePath, shaderPath, effects, disabledEffects, params, effectPaths,
             allDefs, disabledEffectParams, disabledFiles, enabledTechniques, techniqueSorting]() mutable
            {
                bool configSaved = true;
                if (!profilePath.empty())
                {
                    // .conf carries the ReShade definitions only; values live in the
                    // .ini so a sparse preset can't inherit stale ones.
                    std::map<std::string, std::string> instancePaths = effectPaths;
                    for (const auto& [name, path] : effectPaths)
                    {
                        if (std::filesystem::path(path).extension() == ".fx")
                            instancePaths[name] = std::filesystem::path(path).filename().string();
                    }
                    configSaved = ConfigSerializer::saveToPath(profilePath, {}, {}, {},
                                                               instancePaths, {});
                }

                bool shaderSaved = true;
                if (!shaderPath.empty())
                {
                    std::vector<ConfigParam> shaderParams = params;
                    shaderParams.erase(std::remove_if(shaderParams.begin(), shaderParams.end(),
                        [&disabledFiles, &effectPaths](const ConfigParam& param) {
                            const auto path = effectPaths.find(param.effectName);
                            return path != effectPaths.end() &&
                                disabledFiles.count(std::filesystem::path(path->second).filename().string()) != 0;
                        }),
                        shaderParams.end());
                    disabledEffectParams.erase(std::remove_if(disabledEffectParams.begin(), disabledEffectParams.end(),
                        [&disabledFiles, &effectPaths](const ConfigParam& param) {
                            const auto path = effectPaths.find(param.effectName);
                            return path != effectPaths.end() &&
                                disabledFiles.count(std::filesystem::path(path->second).filename().string()) == 0;
                        }),
                        disabledEffectParams.end());
                    for (const auto& def : allDefs)
                    {
                        ConfigParam param{def.effectName, "@" + def.name, def.value};
                        shaderParams.push_back(std::move(param));
                    }
                    std::set<std::pair<std::string, std::string>> enabledParamKeys;
                    for (const auto& param : shaderParams)
                        enabledParamKeys.emplace(param.effectName, param.paramName);
                    disabledEffectParams.erase(std::remove_if(disabledEffectParams.begin(), disabledEffectParams.end(),
                        [&enabledParamKeys](const ConfigParam& param) {
                            return enabledParamKeys.count({param.effectName, param.paramName}) != 0;
                        }), disabledEffectParams.end());
                    shaderSaved = ConfigSerializer::saveShaderProfile(shaderPath, shaderParams, effects, disabledEffects,
                                                                       effectPaths, enabledTechniques, techniqueSorting,
                                                                       disabledEffectParams);
                }

                if (shaderSaved && configSaved)
                    Logger::debug("Auto-saved profile: " + profilePath);
                else
                    Logger::err("Auto-save failed for profile: " + profilePath);
                return shaderSaved && configSaved;
            });

        // the write is queued; mark clean now so the debounce does not re-arm.
        profileDirty = false;
        if (block)
            return saved.get();
        return true;
    }

    void ImGuiOverlay::setSelectedEffects(const std::vector<std::string>& effects,
                                          const std::vector<std::string>& disabledEffects)
    {
        if (!effectRegistry)
            return;

        effectRegistry->setSelectedEffects(effects);

        // Build set of disabled effects for quick lookup
        std::set<std::string> disabledSet(disabledEffects.begin(), disabledEffects.end());

        // Set enabled states in registry: disabled if in disabledEffects, enabled otherwise
        for (const auto& effectName : effects)
        {
            bool enabled = (disabledSet.find(effectName) == disabledSet.end());
            effectRegistry->setEffectEnabled(effectName, enabled);
        }
    }

    bool ImGuiOverlay::createRenderPass(VkFormat swapchainFormat)
    {
        VkAttachmentDescription attachment = {};
        attachment.format = swapchainFormat;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef = {};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        // Dependency for incoming: wait for prior color writes before overlay renders.
        // srcAccessMask MUST include COLOR_ATTACHMENT_WRITE (or TRANSFER_WRITE) so
        // that the implicit PRESENT_SRC→COLOR_ATTACHMENT_OPTIMAL layout transition
        // does not race with the effect pass that just wrote to the swapchain image.
        // The semaphore between effect-submit and overlay-submit guarantees
        // execution ordering, but the render-pass dependency governs the implicit
        // transition barrier and must correctly describe the source access.
        VkSubpassDependency dependencies[2] = {};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        // Dependency for outgoing: ensure overlay writes + layout transition complete
        // and are VISIBLE to presentation engine and DMA-BUF screen capture readers.
        // VK_ACCESS_MEMORY_READ_BIT is critical — without it, GPU caches may not be
        // flushed before PipeWire/compositor reads the image via DMA-BUF.
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

        VkRenderPassCreateInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &attachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 2;
        renderPassInfo.pDependencies = dependencies;

        const VkResult vr = logicalDevice->vkd.CreateRenderPass(logicalDevice->device, &renderPassInfo, nullptr, &renderPass);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to create ImGui render pass: " + std::to_string(vr));
            renderPass = VK_NULL_HANDLE;
            return false;
        }
        framebufferFormat = swapchainFormat;
        return true;
    }

    // Command buffers, fences and framebuffer slots, one per swapchain image.
    // The command pool must already exist.
    bool ImGuiOverlay::createImageResources(uint32_t imageCount)
    {
        commandBuffers.assign(imageCount, VK_NULL_HANDLE);
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = imageCount;
        VkResult vr = logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &allocInfo, commandBuffers.data());
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to allocate ImGui command buffers: " + std::to_string(vr));
            commandBuffers.clear();
            return false;
        }

        // signalled initially so the first frame doesn't wait; getCommandBufferFence
        // resets one only at the moment it is handed to a submit
        commandBufferFences.assign(imageCount, VK_NULL_HANDLE);
        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < imageCount; i++)
        {
            vr = logicalDevice->vkd.CreateFence(logicalDevice->device, &fenceInfo, nullptr, &commandBufferFences[i]);
            if (vr != VK_SUCCESS)
            {
                Logger::err("Failed to create ImGui fence " + std::to_string(i) + ": " + std::to_string(vr));
                destroyImageResources();
                return false;
            }
        }

        framebuffers.assign(imageCount, VK_NULL_HANDLE);
        framebufferImageViews.assign(imageCount, VK_NULL_HANDLE);
        framebufferWidth = 0;
        framebufferHeight = 0;
        imageResourceCount = imageCount;
        return true;
    }

    // Immediate destruction: callers guarantee none of it is in flight (the
    // fences are signalled) or that the device is being rolled back.
    void ImGuiOverlay::destroyImageResources()
    {
        for (auto fence : commandBufferFences)
        {
            if (fence != VK_NULL_HANDLE)
                logicalDevice->vkd.DestroyFence(logicalDevice->device, fence, nullptr);
        }
        commandBufferFences.clear();

        if (!commandBuffers.empty() && commandPool != VK_NULL_HANDLE)
        {
            logicalDevice->vkd.FreeCommandBuffers(
                logicalDevice->device, commandPool, static_cast<uint32_t>(commandBuffers.size()), commandBuffers.data());
        }
        commandBuffers.clear();

        for (auto fb : framebuffers)
        {
            if (fb != VK_NULL_HANDLE)
                logicalDevice->vkd.DestroyFramebuffer(logicalDevice->device, fb, nullptr);
        }
        framebuffers.clear();
        framebufferImageViews.clear();
        imageResourceCount = 0;
    }

    bool ImGuiOverlay::initVulkanBackend(VkFormat swapchainFormat, uint32_t imageCount)
    {
        // Load Vulkan functions for ImGui using VkHook's dispatch tables
        bool loaded = ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, imguiVulkanLoaderDummy, logicalDevice);
        if (!loaded)
        {
            Logger::err("Failed to load Vulkan functions for ImGui");
            return false;
        }
        Logger::debug("ImGui Vulkan functions loaded");

        // Create descriptor pool for ImGui
        VkDescriptorPoolSize poolSizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100 }
        };

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 100;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = poolSizes;

        VkResult vr = logicalDevice->vkd.CreateDescriptorPool(logicalDevice->device, &poolInfo, nullptr, &descriptorPool);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to create ImGui descriptor pool: " + std::to_string(vr));
            descriptorPool = VK_NULL_HANDLE;
            return false;
        }

        if (!createRenderPass(swapchainFormat))
        {
            logicalDevice->vkd.DestroyDescriptorPool(logicalDevice->device, descriptorPool, nullptr);
            descriptorPool = VK_NULL_HANDLE;
            return false;
        }

        auto rollbackBackendInit = [&]() {
            destroyImageResources();
            if (commandPool != VK_NULL_HANDLE)
            {
                logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, commandPool, nullptr);
                commandPool = VK_NULL_HANDLE;
            }
            if (renderPass != VK_NULL_HANDLE)
            {
                logicalDevice->vkd.DestroyRenderPass(logicalDevice->device, renderPass, nullptr);
                renderPass = VK_NULL_HANDLE;
            }
            if (descriptorPool != VK_NULL_HANDLE)
            {
                logicalDevice->vkd.DestroyDescriptorPool(logicalDevice->device, descriptorPool, nullptr);
                descriptorPool = VK_NULL_HANDLE;
            }
            if (backendInitialized)
                ImGui_ImplVulkan_Shutdown();
            backendInitialized = false;
        };

        // Initialize ImGui Vulkan backend
        ImGui_ImplVulkan_InitInfo initInfo = {};
        initInfo.ApiVersion = VK_API_VERSION_1_3;
        initInfo.Instance = logicalDevice->instance;
        initInfo.PhysicalDevice = logicalDevice->physicalDevice;
        initInfo.Device = logicalDevice->device;
        initInfo.QueueFamily = logicalDevice->queueFamilyIndex;
        initInfo.Queue = logicalDevice->queue;
        initInfo.DescriptorPool = descriptorPool;
        // ImageCount MUST match the actual swapchain image count.
        // Hardcoding 2 while the real swapchain has 3 (triple-buffering)
        // causes ImGui's internal vertex/index ring buffer to wrap too early,
        // overwriting buffers still in-flight on the GPU → ghost trails,
        // stale vertices, and black corruption artefacts.
        initInfo.MinImageCount = imageCount;
        initInfo.ImageCount = imageCount;
        initInfo.PipelineInfoMain.RenderPass = renderPass;

        ImGui_ImplVulkan_Init(&initInfo);
        backendInitialized = true;

        // Create command pool
        VkCommandPoolCreateInfo poolCreateInfo = {};
        poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolCreateInfo.queueFamilyIndex = logicalDevice->queueFamilyIndex;
        vr = logicalDevice->vkd.CreateCommandPool(logicalDevice->device, &poolCreateInfo, nullptr, &commandPool);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to create ImGui command pool: " + std::to_string(vr));
            commandPool = VK_NULL_HANDLE;
            rollbackBackendInit();
            return false;
        }

        if (!createImageResources(imageCount))
        {
            rollbackBackendInit();
            return false;
        }

        Logger::debug("ImGui Vulkan backend initialized");
        return true;
    }

    void ImGuiOverlay::flushSettingsSave()
    {
        if (!settingsDirty)
            return;
        settingsDirty = false;
        settingsManager.save();
    }

    VkFence ImGuiOverlay::getCommandBufferFence(uint32_t imageIndex)
    {
        if (imageIndex >= commandBufferFences.size())
            return VK_NULL_HANDLE;
        VkFence fence = commandBufferFences[imageIndex];
        const VkResult reset = logicalDevice->vkd.ResetFences(logicalDevice->device, 1, &fence);
        if (reset != VK_SUCCESS)
        {
            // submitting with a signalled fence is a validation error, so submit without one
            Logger::err("Failed to reset ImGui fence: " + std::to_string(reset));
            return VK_NULL_HANDLE;
        }
        return fence;
    }

    bool ImGuiOverlay::onSwapchainChanged(VkFormat swapchainFormat, uint32_t imageCount)
    {
        if (imageCount == 0)
            return false;

        ContextScope scope(imguiContext);

        if (!backendInitialized)
        {
            // an earlier init failed; the new swapchain is a fresh chance
            const bool ok = initVulkanBackend(swapchainFormat, imageCount);
            if (ok)
                imageResourceCount = imageCount;
            return ok;
        }
        if (swapchainFormat == framebufferFormat && imageCount == imageResourceCount)
            return true;

        // Everything the overlay submitted is tracked by its per-image fences.
        // Wait them out (bounded) before touching ImGui's pipeline or ours.
        if (!commandBufferFences.empty())
        {
            const VkResult waited = logicalDevice->vkd.WaitForFences(
                logicalDevice->device, static_cast<uint32_t>(commandBufferFences.size()),
                commandBufferFences.data(), VK_TRUE, 1'000'000'000ull);
            if (waited != VK_SUCCESS)
            {
                Logger::err("ImGui: overlay work still in flight after a swapchain change; keeping old resources ("
                            + std::to_string(waited) + ")");
                return false;
            }
        }

        Logger::info("ImGui: rebuilding overlay resources for format " + std::to_string(swapchainFormat)
                     + ", " + std::to_string(imageCount) + " images");

        // ImGui's pipeline is bound to the render pass and its ring buffers to the
        // image count, so the backend is rebuilt along with them. Pool and
        // descriptor pool survive; ImGui frees what it allocated from the latter.
        ImGui_ImplVulkan_Shutdown();
        backendInitialized = false;
        destroyImageResources();
        if (renderPass != VK_NULL_HANDLE)
        {
            logicalDevice->vkd.DestroyRenderPass(logicalDevice->device, renderPass, nullptr);
            renderPass = VK_NULL_HANDLE;
        }

        if (!createRenderPass(swapchainFormat))
            return false;

        ImGui_ImplVulkan_InitInfo initInfo = {};
        initInfo.ApiVersion = VK_API_VERSION_1_3;
        initInfo.Instance = logicalDevice->instance;
        initInfo.PhysicalDevice = logicalDevice->physicalDevice;
        initInfo.Device = logicalDevice->device;
        initInfo.QueueFamily = logicalDevice->queueFamilyIndex;
        initInfo.Queue = logicalDevice->queue;
        initInfo.DescriptorPool = descriptorPool;
        initInfo.MinImageCount = imageCount;
        initInfo.ImageCount = imageCount;
        initInfo.PipelineInfoMain.RenderPass = renderPass;
        ImGui_ImplVulkan_Init(&initInfo);
        backendInitialized = true;

        if (!createImageResources(imageCount))
        {
            Logger::err("ImGui: could not rebuild per-image resources; overlay disabled until the next swapchain");
            ImGui_ImplVulkan_Shutdown();
            backendInitialized = false;
            return false;
        }
        return true;
    }


    VkCommandBuffer ImGuiOverlay::recordFrame(uint32_t imageIndex, VkImageView imageView, uint32_t width, uint32_t height)
    {
        // Informational notifications remain visible without capturing input.
        const bool hasToasts = hasPendingToasts();
        if (!backendInitialized || (!visible && !hasToasts))
        {
            // drop last frame's hitboxes, or they keep swallowing input.
            frameInputRects.clear();
            setInputRects(nullptr, 0);
            setInputBlocked(false);
            setInputInteractive(false);
            setOverlayWantsKeyboard(false);
            setWaylandInputSurfaceRects(nullptr, 0);
            return VK_NULL_HANDLE;
        }

        // A swapchain with more images than the overlay was built for (until the
        // layer reports the change through onSwapchainChanged) must not index
        // past the per-image arrays.
        if (imageIndex >= commandBuffers.size() || imageIndex >= commandBufferFences.size())
        {
            if (!warnedImageIndex)
            {
                warnedImageIndex = true;
                Logger::warn("ImGui overlay: image index " + std::to_string(imageIndex) + " outside the "
                             + std::to_string(commandBuffers.size()) + " images it was built for; skipping its frame");
            }
            return VK_NULL_HANDLE;
        }

        ContextScope contextScope(imguiContext);

        // Publish input ownership before sampling this frame's pointer.
        frameInputRects.clear();
        const bool captureInput = visible && settingsManager.getOverlayBlockInput();
        setInputBlocked(captureInput);
        setInputInteractive(captureInput);
        const InputRect captureRect{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
        setWaylandInputSurfaceRects(captureInput ? &captureRect : nullptr, captureInput ? 1 : 0);

        // Store current resolution for VRAM estimates in settings
        currentWidth = width;
        currentHeight = height;


        // Wait for previous use of this command buffer to complete.
        // Adaptive timeout: 4x the measured frame time (generous margin for GPU
        // scheduling jitter), clamped to [2ms, 50ms].  With triple buffering the
        // fence was submitted 2-3 frames ago and is almost always already signaled.
        VkFence fence = commandBufferFences[imageIndex];
        uint64_t timeoutNs = static_cast<uint64_t>(
            std::clamp(avgFrameTimeMs * 4.0f, 2.0f, 50.0f) * 1'000'000.0f);
        VkResult fenceResult = logicalDevice->vkd.WaitForFences(logicalDevice->device, 1, &fence, VK_TRUE, timeoutNs);
        if (fenceResult == VK_TIMEOUT)
        {
            Logger::warn("ImGui fence wait timed out for image " + std::to_string(imageIndex));
            return VK_NULL_HANDLE;
        }
        if (fenceResult != VK_SUCCESS)
        {
            Logger::err("ImGui fence wait failed: " + std::to_string(fenceResult));
            return VK_NULL_HANDLE;
        }
        // The fence stays signalled here. getCommandBufferFence resets it right before
        // the submit, so none of the early returns below can strand it unsignalled.

        VkCommandBuffer cmd = commandBuffers[imageIndex];

        // Begin command buffer
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult vr = logicalDevice->vkd.BeginCommandBuffer(cmd, &beginInfo);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to begin ImGui command buffer: " + std::to_string(vr));
            return VK_NULL_HANDLE;
        }

        // Ensure framebuffer exists for this image index.
        // Recreate if the image view or dimensions changed (swapchain recreation).
        if (framebuffers.size() <= imageIndex)
        {
            framebuffers.resize(imageIndex + 1, VK_NULL_HANDLE);
            framebufferImageViews.resize(imageIndex + 1, VK_NULL_HANDLE);
        }

        bool needRecreate = (framebuffers[imageIndex] == VK_NULL_HANDLE) ||
                            (framebufferImageViews[imageIndex] != imageView) ||
                            (framebufferWidth != width) || (framebufferHeight != height);

        if (needRecreate)
        {
            if (framebuffers[imageIndex] != VK_NULL_HANDLE)
                logicalDevice->vkd.DestroyFramebuffer(logicalDevice->device, framebuffers[imageIndex], nullptr);

            VkFramebufferCreateInfo fbInfo = {};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = renderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = &imageView;
            fbInfo.width = width;
            fbInfo.height = height;
            fbInfo.layers = 1;
            vr = logicalDevice->vkd.CreateFramebuffer(logicalDevice->device, &fbInfo, nullptr, &framebuffers[imageIndex]);
            if (vr != VK_SUCCESS)
            {
                Logger::err("Failed to create ImGui framebuffer: " + std::to_string(vr));
                logicalDevice->vkd.EndCommandBuffer(cmd);
                return VK_NULL_HANDLE;
            }
            framebufferImageViews[imageIndex] = imageView;
            framebufferWidth = width;
            framebufferHeight = height;
        }

        VkFramebuffer framebuffer = framebuffers[imageIndex];

        // Set display size and frame timing BEFORE NewFrame
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)width, (float)height);
        const float requestedScale = settingsManager.getUiScale();
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
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;
        if (dt > 0.0f && dt < 1.0f)
        {
            io.DeltaTime = dt;
            lastGoodDt = dt;
            // Update adaptive fence timeout with exponential moving average
            avgFrameTimeMs = avgFrameTimeMs * 0.9f + (dt * 1000.0f) * 0.1f;
        }
        else
        {
            io.DeltaTime = lastGoodDt;
        }

        // Mouse input for interactivity
        MouseState mouse = getMouseState();
        io.MousePos = ImVec2((float)mouse.x, (float)mouse.y);

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
            const bool blocking = visible && settingsManager.getOverlayBlockInput();
            if (blocking)
                setInputRects(frameInputRects.data(), static_cast<int>(frameInputRects.size()));
            else
                setInputRects(nullptr, 0);

            // The input backend owns capture for the full open session, including
            // a drag that leaves the panel. Text/popups additionally request keys.
            const bool interactive = blocking;
            setInputInteractive(interactive);
            setOverlayWantsKeyboard(interactive
                && (io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)));
            updatePointerPosition(static_cast<float>(mouse.x), static_cast<float>(mouse.y));
            setInputBlocked(blocking);
            setWaylandInputSurfaceRects(blocking ? &captureRect : nullptr, blocking ? 1 : 0);
        };

        // Keyboard input for text fields
        // Keys are one-shot events, so we send press and release in same frame
        // Navigation keys arrive as one-shot presses, so each one is sent as a
        // press and a release. Text arrives as UTF-8 and goes in whole.
        KeyboardState keyboard = getKeyboardState();
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

        // ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();

        // Hidden overlays draw notifications only, without a menu or input grab.
        if (!visible)
        {
            // toasts render on their own so fatal errors stay visible
            renderToasts();
            // main window is gone; only toasts remain, so free the pointer.
            setWaylandInputSurfaceRects(nullptr, 0);
            publishOverlayInput();
            ImGui::Render();

            VkRenderPassBeginInfo rpBegin = {};
            rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpBegin.renderPass = renderPass;
            rpBegin.framebuffer = framebuffer;
            rpBegin.renderArea.extent.width = width;
            rpBegin.renderArea.extent.height = height;

            logicalDevice->vkd.CmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
            logicalDevice->vkd.CmdEndRenderPass(cmd);

            VkResult endRes = logicalDevice->vkd.EndCommandBuffer(cmd);
            if (endRes != VK_SUCCESS)
            {
                Logger::err("Failed to end ImGui command buffer (toast-only): " + std::to_string(endRes));
                return VK_NULL_HANDLE;
            }
            return cmd;
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
        gatherDepthInfo();
        applyDepthPinRequests();
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
                            else renderMainView(keyboard);
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

        // Global auto-apply check (runs regardless of which tab is active)
        if (settingsManager.getAutoApply() && paramsDirty)
        {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastChangeTime).count();
            const bool mouseButtonDown = ImGui::GetIO().MouseDown[0] || ImGui::GetIO().MouseDown[1];

            // Defer auto-apply while either mouse button is held (dragging/resizing/sliders).
            // Apply on the first frame after release once delay has already elapsed.
            if (elapsed >= settingsManager.getAutoApplyDelay() && !mouseButtonDown)
            {
                // a ReShade uniform edit is already live via updateEffect, so
                // only rebuild the chain when something structural changed
                if (reloadNeeded)
                    applyRequested = true;
                reloadNeeded = false;
                paramsDirty = false;
                profileDirty = true;  // Mark for auto-save to profile
            }
        }

        // Auto-save profile when changes are applied
        if (profileDirty && !paramsDirty &&
            (!activeProfilePath.empty() || !activeShaderProfilePath.empty()))
            autoSaveProfile();

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

        // Begin render pass
        VkRenderPassBeginInfo rpBegin = {};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = renderPass;
        rpBegin.framebuffer = framebuffer;
        rpBegin.renderArea.extent.width = width;
        rpBegin.renderArea.extent.height = height;

        logicalDevice->vkd.CmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        logicalDevice->vkd.CmdEndRenderPass(cmd);

        vr = logicalDevice->vkd.EndCommandBuffer(cmd);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to end ImGui command buffer: " + std::to_string(vr));
            return VK_NULL_HANDLE;
        }

        return cmd;
    }

    void ImGuiOverlay::collectCommandFences(std::vector<VkFence>& out) const
    {
        for (VkFence f : commandBufferFences)
            if (f != VK_NULL_HANDLE) out.push_back(f);
    }

} // namespace VkHook
