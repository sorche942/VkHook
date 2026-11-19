#include "imgui_overlay.hh"

#include "control/preset_model.hh"
#include "depth/tracker.hh"
#include "effects/effect_registry.hh"
#include "layer/state.hh"
#include "logical_device.hh"
#include "config_serializer.hh"
#include "logger.hh"
#include "persist.hh"
#include "settings_manager.hh"

#include <algorithm>
#include <filesystem>
#include <future>
#include <mutex>
#include <set>

namespace VkHook
{
    ImGuiOverlay::ImGuiOverlay(LogicalDevice* device, VkFormat swapchainFormat, uint32_t imageCount,
                               OverlayPersistentState* persistentState)
        : logicalDevice(device)
    {
        ui = OverlayUi::create(device, persistentState, &commands, &slot);
        if (!ui)
            return;
        renderer = OverlayRenderer::create(*ui, device, swapchainFormat, imageCount);
    }

    ImGuiOverlay::~ImGuiOverlay()
    {
        // Auto-save profile on shutdown (before GPU cleanup), applying whatever
        // the user did since the last frame
        if (ui)
        {
            std::vector<Command> pending;
            commands.drain(pending);
            applyCommands(pending);
        }
        renderer.reset();
        ui.reset();
    }

    void ImGuiOverlay::toggle()
    {
        if (ui)
            ui->toggle();
    }

    void ImGuiOverlay::pushToast(LogLevel level, const std::string& message)
    {
        if (ui)
            ui->pushToast(level, message);
    }

    bool ImGuiOverlay::onSwapchainChanged(VkFormat swapchainFormat, uint32_t imageCount)
    {
        if (!ui || imageCount == 0)
            return false;
        if (renderer && renderer->format() == swapchainFormat && renderer->imageCount() == imageCount)
            return true;
        // the old renderer's work is waited out (bounded) when the new one
        // attaches and the backend has to be rebuilt; its resources are retired
        std::unique_ptr<OverlayRenderer> replacement = OverlayRenderer::create(*ui, logicalDevice, swapchainFormat, imageCount);
        if (!replacement)
            return false;
        renderer = std::move(replacement);
        return true;
    }

    VkFence ImGuiOverlay::getCommandBufferFence(uint32_t imageIndex)
    {
        return renderer ? renderer->fenceForSubmit(imageIndex) : VK_NULL_HANDLE;
    }

    void ImGuiOverlay::collectCommandFences(std::vector<VkFence>& out) const
    {
        if (renderer)
            renderer->collectFences(out);
    }

    VkCommandBuffer ImGuiOverlay::recordFrame(uint32_t imageIndex, VkImageView imageView, uint32_t width,
                                              uint32_t height)
    {
        if (!ui || !renderer)
            return VK_NULL_HANDLE;

        // the snapshot is only worth building while the views read it
        if (ui->isVisible())
            slot.publish(buildSnapshot());

        const VkCommandBuffer cmd = renderer->record(imageIndex, imageView, width, height);

        // what the user did this frame is applied at once, so the next frame's
        // snapshot already shows it
        std::vector<Command> pending;
        commands.drain(pending);
        if (!pending.empty())
            applyCommands(pending);
        return cmd;
    }

    // --- control/frontend.cc's side of the has/clear flags ----------------------

    void ImGuiOverlay::updateState(OverlayState newState)
    {
        // only what the add-effects page reads; a change here invalidates its cached list
        if (newState.currentConfigEffects != state.currentConfigEffects
            || newState.defaultConfigEffects != state.defaultConfigEffects
            || newState.effectPaths != state.effectPaths)
            ++availableVersion;

        state = std::move(newState);

        if (!effectRegistry)
            return;

        // Registry is already initialized from config at swapchain creation;
        // just make sure any newly added effects are in it.
        for (const auto& effectName : effectRegistry->getSelectedEffects())
        {
            if (!effectRegistry->hasEffect(effectName))
                effectRegistry->ensureEffect(effectName);
        }
    }

    void ImGuiOverlay::setGameProfile(const std::string& gameName, const std::string& profileName,
                                      const std::string& profilePath)
    {
        activeGameName = gameName;
        activeProfileName = profileName;
        activeProfilePath = profilePath;
        refreshShaderProfiles();
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

    void ImGuiOverlay::setSelectedEffects(const std::vector<std::string>& effects,
                                          const std::vector<std::string>& disabledEffects)
    {
        if (!effectRegistry)
            return;

        effectRegistry->setSelectedEffects(effects);
        const std::set<std::string> disabledSet(disabledEffects.begin(), disabledEffects.end());
        for (const auto& effectName : effects)
            effectRegistry->setEffectEnabled(effectName, disabledSet.find(effectName) == disabledSet.end());
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
        queueLastProfile();
    }

    void ImGuiOverlay::queueLastProfile()
    {
        if (activeGameName.empty() || activeShaderProfileName.empty())
            return;
        PersistenceQueue::instance().other(
            "last-profile:" + activeGameName,
            [game = activeGameName, profile = activeShaderProfileName] {
                return ConfigSerializer::setLastShaderProfile(game, profile);
            });
    }

    void ImGuiOverlay::setActiveProfile(const std::string& name)
    {
        activeShaderProfileName = name;
        activeShaderProfilePath = ConfigSerializer::getShaderProfilePath(activeGameName, activeShaderProfileName);
        queueLastProfile();
    }

    // Makes frontend.cc load the active preset on its next frame.
    void ImGuiOverlay::activateActiveProfile()
    {
        pendingShaderProfilePath = activeShaderProfilePath;
        pendingShaderProfile = true;
        applyRequested = true;
    }

    // Saves the outgoing preset while it is still active, then activates `name`.
    bool ImGuiOverlay::switchProfile(const std::string& name)
    {
        if (name.empty() || name == activeShaderProfileName)
            return true;
        if (!autoSaveProfile(true))
            return false;
        setActiveProfile(name);
        activateActiveProfile();
        return true;
    }

    // --- persistence: the active preset and per-game config -------------------

    bool ImGuiOverlay::autoSaveProfile(bool block)
    {
        if (!effectRegistry)
            return false;
        if (activeProfilePath.empty() && activeShaderProfilePath.empty())
            return true;

        // Snapshot the registry-derived data here: the write runs on the writer
        // thread and must not touch state the present thread mutates, and
        // EffectConfig owns unique_ptrs and cannot be copied.
        std::map<std::string, std::string> effectPaths;
        EffectStack stack;
        const auto& allEffects = effectRegistry->getAllEffects();
        for (const std::string& name : effectRegistry->getSelectedEffects())
        {
            const bool enabled = effectRegistry->isEffectEnabled(name);
            EffectInstance instance;
            instance.name = name;
            instance.enabled = enabled;

            const std::string path = effectRegistry->getEffectFilePath(name);
            if (!path.empty())
            {
                effectPaths[name] = path;
                const std::filesystem::path p(path);
                instance.type = p.extension() == ".fx" ? p.filename().string() : path;
            }
            const auto config = std::find_if(allEffects.begin(), allEffects.end(),
                                             [&name](const EffectConfig& item) { return item.name == name; });
            if (config != allEffects.end())
            {
                for (const auto& technique : config->techniqueNames)
                    instance.techniques.push_back({technique, enabled});
            }
            for (auto* param : effectRegistry->getParametersForEffect(name))
            {
                if (param->noSave)
                    continue;
                for (const auto& [suffix, value] : param->serialize())
                    instance.setParam(suffix.empty() ? param->name : suffix, value);
            }
            for (const auto& def : effectRegistry->getPreprocessorDefs(name))
                instance.setMacro(def.name, def.value);
            stack.instances.push_back(std::move(instance));
        }

        const std::string profilePath = activeProfilePath;
        const std::string shaderPath = activeShaderProfilePath;

        // Keyed by the preset file: repeated autosaves coalesce to the newest, and
        // a delete or rename of that preset cancels them instead of letting a
        // queued save bring the file back.
        const std::string queueKey = shaderPath.empty() ? profilePath : shaderPath;
        std::future<bool> saved = PersistenceQueue::instance().write(queueKey,
            [profilePath, shaderPath, effectPaths, stack = std::move(stack)]() mutable {
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
                    configSaved = ConfigSerializer::saveToPath(profilePath, {}, {}, {}, instancePaths, {});
                }

                bool shaderSaved = true;
                if (!shaderPath.empty())
                {
                    const SerializedPreset preset = serialize(stack);
                    shaderSaved = ConfigSerializer::writeShaderProfile(shaderPath, preset.ini, preset.sidecar);
                }

                if (shaderSaved && configSaved)
                    Logger::debug("Auto-saved profile: " + profilePath);
                else
                    Logger::err("Auto-save failed for profile: " + profilePath);
                return shaderSaved && configSaved;
            });

        if (block)
            return saved.get();
        return true;
    }

    // The "Save" of the config bar for an executable with no profile: every
    // effect, its values and its macros, to configs/<name>.conf.
    void ImGuiOverlay::saveConfig(const std::string& name)
    {
        if (!effectRegistry || name.empty() || !ConfigSerializer::isSafePathComponent(name))
            return;

        std::vector<std::string> effects = effectRegistry->getSelectedEffects();
        std::vector<std::string> disabledEffects;
        std::vector<ConfigParam> params;
        std::map<std::string, std::string> effectPaths;
        std::vector<PreprocessorDefinition> allDefs;
        for (const auto& effectName : effects)
        {
            if (!effectRegistry->isEffectEnabled(effectName))
                disabledEffects.push_back(effectName);
            for (auto* p : effectRegistry->getParametersForEffect(effectName))
            {
                if (p->noSave)
                    continue;
                for (const auto& [suffix, value] : p->serialize())
                    params.push_back({p->effectName, suffix.empty() ? p->name : suffix, value});
            }
            const std::string path = effectRegistry->getEffectFilePath(effectName);
            if (!path.empty())
                effectPaths[effectName] = path;
            for (const auto& def : effectRegistry->getPreprocessorDefs(effectName))
                allDefs.push_back(def);
        }

        const std::string path = ConfigSerializer::getConfigsDir() + "/" + name + ".conf";
        const bool saved = PersistenceQueue::instance().create(path,
            [name, effects, disabledEffects, params, effectPaths, allDefs] {
                return ConfigSerializer::saveConfig(name, effects, disabledEffects, params, effectPaths, allDefs);
            }).get();
        if (!saved)
            pushToast(LogLevel::Error, "Could not save the config.");
    }

    void ImGuiOverlay::importPreset(const std::string& sourcePath)
    {
        // the import picks a free name inside the job, so the queue is told
        // about the created file from there
        std::string imported;
        PersistenceQueue::instance().submit({PersistenceQueue::Kind::Other, "import:" + sourcePath, {},
            [&imported, sourcePath]() {
                imported = ConfigSerializer::importShaderProfile(sourcePath);
                if (imported.empty())
                    return false;
                PersistenceQueue::instance().noteCreated(ConfigSerializer::getImportedShaderProfilePath(imported));
                return true;
            }}).get();
        if (imported.empty())
        {
            pushToast(LogLevel::Error, "Could not import the shader preset.");
            return;
        }

        // save the outgoing profile, then activate the import. refresh can
        // already select the new name, so the reload is queued here directly.
        const bool saved = autoSaveProfile(true);
        refreshShaderProfiles();
        setActiveProfile(imported);
        activateActiveProfile();
        if (!saved)
            pushToast(LogLevel::Error, "Imported \"" + imported + "\", but the previous profile could not be saved.");
        else
            pushToast(LogLevel::Info, "Imported shader preset \"" + imported + "\".");
    }

    // --- the snapshot ------------------------------------------------------------

    void ImGuiOverlay::gatherDepth(DepthInfo& depthInfo, DeviceInfo& deviceInfo)
    {
        std::lock_guard<std::mutex> l(globalLock);

        depthInfo.supportedResolveModes = logicalDevice->supportedDepthResolveModes;
        depthInfo.depthCaptureEnabled = settingsManager.getDepthCapture();
        depthInfo.swapchainExtent = currentSwapchainExtentLocked(logicalDevice);

        deviceInfo.gpuName = logicalDevice->gpuName;
        deviceInfo.gpuDriverInfo = logicalDevice->gpuDriverInfo;
        deviceInfo.gpuPciSlot = logicalDevice->gpuPciSlot;
        deviceInfo.gpuApiVersion = logicalDevice->gpuApiVersion;
        deviceInfo.gpuVendorId = logicalDevice->gpuVendorId;

        const bool avgSupported = (logicalDevice->supportedDepthResolveModes & VK_RESOLVE_MODE_AVERAGE_BIT) != 0;
        depthInfo.depthResolveMode = settingsManager.getDepthResolveMode() == 2 && avgSupported
            ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;

        const auto& best = logicalDevice->bestDepthCandidate;
        auto describe = [&](DepthCandidateInfo& c, const DepthState& ds) {
            c.imageView = ds.imageView;
            c.format = ds.format;
            c.extent = ds.extent;
            c.samples = ds.samples;
            c.observedLayout = ds.observedLayout;
            c.transient = ds.transient;
            const bool isBest = best.valid && best.depthState.imageView == ds.imageView;
            c.drawCount = isBest ? best.drawCount : 0;
            c.hasPresentableSnapshotTarget = isBest ? best.hasPresentableSnapshotTarget : false;
        };

        // If the pinned view was destroyed by the render thread since the last
        // frame, clear the stale pin so the UI falls back to auto instead of
        // showing a ghost pin.
        depthInfo.pinnedView = logicalDevice->pinnedDepthImageView;
        depthInfo.depthIsPinned = depthInfo.pinnedView != VK_NULL_HANDLE;
        if (depthInfo.depthIsPinned)
        {
            const auto it = logicalDevice->depthViewStates.find(depthInfo.pinnedView);
            if (it != logicalDevice->depthViewStates.end())
            {
                describe(depthInfo.active, it->second);
            }
            else
            {
                Logger::debug("overlay snapshot: pinned depth view no longer tracked, clearing stale pin");
                logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
                depthInfo.depthIsPinned = false;
                depthInfo.pinnedView = VK_NULL_HANDLE;
            }
        }
        if (!depthInfo.depthIsPinned)
        {
            describe(depthInfo.active, logicalDevice->activeDepthState);
            // the active buffer is whatever the best candidate is, tracked or not
            depthInfo.active.drawCount = best.valid ? best.drawCount : 0;
            depthInfo.active.hasPresentableSnapshotTarget = best.hasPresentableSnapshotTarget;
        }
        depthInfo.depthResolveIsMsaa = depthInfo.active.samples != VK_SAMPLE_COUNT_1_BIT;

        for (const auto& [view, ds] : logicalDevice->depthViewStates)
        {
            DepthCandidateInfo c;
            describe(c, ds);
            c.imageView = view;
            depthInfo.candidates.push_back(c);
        }
        // busiest first, so the best candidate leads
        std::sort(depthInfo.candidates.begin(), depthInfo.candidates.end(),
                  [](const DepthCandidateInfo& a, const DepthCandidateInfo& b) { return a.drawCount > b.drawCount; });
    }

    CoreSnapshotPtr ImGuiOverlay::buildSnapshot()
    {
        auto snapshot = std::make_shared<CoreSnapshot>();
        snapshot->generation = ++snapshotGeneration;
        snapshot->settings = settingsManager.snapshot();

        EffectCatalog& catalog = snapshot->effects;
        catalog.effectsEnabled = state.effectsEnabled;
        catalog.currentConfigEffects = state.currentConfigEffects;
        catalog.defaultConfigEffects = state.defaultConfigEffects;
        catalog.effectPaths = state.effectPaths;
        catalog.availableVersion = availableVersion;

        if (effectRegistry)
        {
            const auto& allEffects = effectRegistry->getAllEffects();
            for (const std::string& name : effectRegistry->getSelectedEffects())
            {
                EffectView view;
                view.name = name;
                view.enabled = effectRegistry->isEffectEnabled(name);
                view.failed = effectRegistry->hasEffectFailed(name);
                if (view.failed)
                    view.error = effectRegistry->getEffectError(name);
                view.type = effectRegistry->getEffectType(name);
                view.filePath = effectRegistry->getEffectFilePath(name);
                view.macros = effectRegistry->getPreprocessorDefs(name);
                for (auto* param : effectRegistry->getParametersForEffect(name))
                    view.params.push_back(std::shared_ptr<const EffectParam>(param->clone()));
                const auto config = std::find_if(allEffects.begin(), allEffects.end(),
                                                 [&name](const EffectConfig& item) { return item.name == name; });
                if (config != allEffects.end())
                    view.techniques = config->techniqueNames;
                catalog.chain.push_back(std::move(view));
            }
        }

        ProfileInfo& profiles = snapshot->profiles;
        profiles.gameName = activeGameName;
        profiles.activeName = activeShaderProfileName;
        profiles.activePath = activeShaderProfilePath;
        profiles.names = shaderProfiles;
        profiles.configName = state.configName;

        gatherDepth(snapshot->depth, snapshot->device);
        return snapshot;
    }

    // --- the commands ----------------------------------------------------------

    // One member per Command alternative. Reference for core: what each user
    // action does, and which of them frontend.cc has to react to.
    struct ImGuiOverlay::CommandVisitor
    {
        ImGuiOverlay& self;
        EffectRegistry* registry() const { return self.effectRegistry; }

        void operator()(const SetParam& c) const
        {
            if (!registry() || !c.value)
                return;
            if (auto param = registry()->getParameter(c.effect, c.value->name))
                assignParamValue(**param, *c.value);
        }

        void operator()(const ResetParams& c) const
        {
            if (!registry())
                return;
            for (auto* param : registry()->getParametersForEffect(c.effect))
                param->resetToDefault();
        }

        void operator()(const SetMacro& c) const
        {
            if (registry())
                registry()->setPreprocessorDefValue(c.effect, c.name, c.value);
        }

        void operator()(const SetEffectEnabled& c) const
        {
            if (registry())
                registry()->setEffectEnabled(c.effect, c.enabled);
        }

        void operator()(const ToggleEffects&) const { self.toggleEffectsRequested = true; }

        void operator()(const EditChain& c) const
        {
            if (!registry())
                return;
            std::vector<std::string> selected = registry()->getSelectedEffects();
            // removals first, so the insert position lines up with what is left
            for (const std::string& removed : c.remove)
            {
                const auto it = std::find(selected.begin(), selected.end(), removed);
                if (it == selected.end())
                    continue;
                registry()->removeEffect(*it);
                selected.erase(it);
            }
            const size_t position = c.position >= 0 && static_cast<size_t>(c.position) <= selected.size()
                ? static_cast<size_t>(c.position) : selected.size();
            size_t at = position;
            for (const auto& [instanceName, effectType] : c.add)
            {
                selected.insert(selected.begin() + static_cast<std::ptrdiff_t>(at++), instanceName);
                registry()->ensureEffect(instanceName, effectType);
                registry()->setEffectEnabled(instanceName, true);
            }
            registry()->setSelectedEffects(selected);
        }

        void operator()(const ReorderEffects& c) const
        {
            if (registry())
                registry()->setSelectedEffects(c.order);
        }

        void operator()(const ClearEffects&) const
        {
            if (registry())
                registry()->clearSelectedEffects();
        }

        void operator()(const RebuildChain&) const { self.applyRequested = true; }

        void operator()(const SaveProfile&) const { self.autoSaveProfile(); }

        void operator()(const SwitchProfile& c) const
        {
            if (!self.switchProfile(c.name))
                self.pushToast(LogLevel::Error, "Could not save the active shader profile.");
        }

        void operator()(const CreateProfile& c) const
        {
            if (!ConfigSerializer::isValidShaderProfileName(c.name))
            {
                self.pushToast(LogLevel::Error, "Preset names can't contain /, \\ or @.");
                return;
            }
            if (!self.autoSaveProfile(true))
            {
                self.pushToast(LogLevel::Error, "Could not save the active shader profile.");
                return;
            }
            const bool created = PersistenceQueue::instance().create(
                ConfigSerializer::getGameShaderProfilePath(self.activeGameName, c.name),
                [game = self.activeGameName, name = c.name, from = self.activeShaderProfileName]() {
                    return ConfigSerializer::createShaderProfile(game, name, from);
                }).get();
            if (!created)
            {
                self.pushToast(LogLevel::Error, "Could not create the shader profile.");
                return;
            }
            self.switchProfile(c.name);
            self.refreshShaderProfiles();
        }

        void operator()(const RenameProfile& c) const
        {
            if (!ConfigSerializer::isValidShaderProfileName(c.to))
            {
                self.pushToast(LogLevel::Error, "Preset names can't contain /, \\ or @.");
                return;
            }
            if (!self.autoSaveProfile(true))
            {
                self.pushToast(LogLevel::Error, "Could not save the active shader profile.");
                return;
            }
            // run the rename on the writer thread so a queued save can't race
            // the files, then update the name and path only after it has landed
            const bool renamed = PersistenceQueue::instance().rename(
                ConfigSerializer::getShaderProfilePath(self.activeGameName, c.from),
                ConfigSerializer::getRenamedShaderProfilePath(self.activeGameName, c.from, c.to),
                [game = self.activeGameName, from = c.from, to = c.to]() {
                    return ConfigSerializer::renameShaderProfile(game, from, to);
                }).get();
            if (!renamed)
            {
                self.pushToast(LogLevel::Error, "Could not rename the shader profile.");
                return;
            }
            if (self.activeShaderProfileName == c.from)
                self.setActiveProfile(c.to);
            self.refreshShaderProfiles();
            self.activateActiveProfile();
            self.pushToast(LogLevel::Info, "Renamed shader preset to \"" + c.to + "\".");
        }

        void operator()(const DeleteProfile& c) const
        {
            // queued behind any pending autosave of this preset, which the
            // queue cancels, so the file cannot come back after the delete
            if (!PersistenceQueue::instance().remove(
                    ConfigSerializer::getShaderProfilePath(self.activeGameName, c.name),
                    [game = self.activeGameName, target = c.name]() {
                        return ConfigSerializer::deleteShaderProfile(game, target);
                    }).get())
            {
                self.pushToast(LogLevel::Error, "Could not delete the shader profile.");
                return;
            }
            if (c.name == self.activeShaderProfileName)
                self.activeShaderProfileName.clear();
            self.refreshShaderProfiles();
            if (self.shaderProfiles.empty())
            {
                self.activeShaderProfileName.clear();
                self.activeShaderProfilePath.clear();
            }
            self.activateActiveProfile();
        }

        void operator()(const ImportPreset& c) const { self.importPreset(c.path); }

        void operator()(const LoadConfig& c) const
        {
            self.pendingConfigPath = c.path;
            self.applyRequested = true;
        }

        void operator()(const SaveConfig& c) const { self.saveConfig(c.name); }

        void operator()(const SetDefaultConfig& c) const
        {
            if (!ConfigSerializer::isSafePathComponent(c.name))
                return;
            if (!PersistenceQueue::instance().write(ConfigSerializer::getDefaultConfigPath(), [name = c.name] {
                    return ConfigSerializer::setDefaultConfig(name);
                }).get())
                self.pushToast(LogLevel::Error, "Could not set the default config.");
        }

        void operator()(const DeleteConfig& c) const
        {
            if (!ConfigSerializer::isSafePathComponent(c.name))
                return;
            PersistenceQueue::instance().remove(ConfigSerializer::getConfigsDir() + "/" + c.name + ".conf",
                                                [name = c.name] { return ConfigSerializer::deleteConfig(name); }).wait();
        }

        // The settings file and everything derived from it are re-read here;
        // re-reading the game's .conf and preset from disk is control/frontend.cc's
        // reload-key path (Config::reload + applyShaderProfile).
        void operator()(const ReloadConfig&) const
        {
            settingsManager.reload();
            self.settingsSaved = true;
            self.shaderPathsChanged = true;
            self.applyRequested = true;
        }

        void operator()(const PinDepth& c) const
        {
            LogicalDevice* device = self.logicalDevice;
            std::lock_guard<std::mutex> l(globalLock);
            if (device->depthViewStates.find(c.view) == device->depthViewStates.end())
            {
                Logger::warn("depth pin requested but view not found (may have been destroyed)");
                return;
            }
            device->pinnedDepthImageView = c.view;
            Logger::info("depth manual pin set to view 0x" + std::to_string(reinterpret_cast<uint64_t>(c.view)));
            self.depthPinChanged = true;
            // force the deferred realloc so descriptor sets and MSAA framebuffers
            // are rebuilt for the new depth source view
            device->depthReallocPending = true;
        }

        void operator()(const ClearDepthPin&) const
        {
            LogicalDevice* device = self.logicalDevice;
            std::lock_guard<std::mutex> l(globalLock);
            device->pinnedDepthImageView = VK_NULL_HANDLE;
            self.depthPinChanged = true;
            // same: realloc so the just-unpinned view stops being used
            device->depthReallocPending = true;
            Logger::info("depth manual pin cleared (back to auto-promotion)");
        }

        void operator()(const RefreshDepth&) const
        {
            LogicalDevice* device = self.logicalDevice;
            std::lock_guard<std::mutex> l(globalLock);
            device->activeDepthState = DepthState{};
            device->bestDepthCandidate = LogicalDevice::DepthCandidateTrackingState{};
            device->pinnedDepthImageView = VK_NULL_HANDLE;
            self.depthPinChanged = true;
        }

        // the resolve owns the transforms (macros no longer carry them), so
        // re-recording it is all a toggle needs
        void operator()(const DepthTransformChanged&) const
        {
            std::lock_guard<std::mutex> l(globalLock);
            self.logicalDevice->depthReallocPending = true;
        }

        void operator()(const SetSetting& c) const
        {
            if (!settingsManager.set(c.key, c.value))
                Logger::warn("overlay: refused setting " + c.key);
        }

        void operator()(const SettingsChanged&) const
        {
            settingsManager.save();
            self.settingsSaved = true;
        }

        void operator()(const ShaderPathsChanged& c) const
        {
            PersistenceQueue::instance().write(ConfigSerializer::getBaseConfigDir() + "/shader_manager.conf",
                                               [config = c.config] { return ConfigSerializer::saveShaderManagerConfig(config); });
            self.shaderPathsChanged = true;
        }
    };

    void ImGuiOverlay::applyCommands(std::vector<Command>& pending)
    {
        const CommandVisitor visitor{*this};
        for (const Command& command : pending)
            std::visit(visitor, command);
    }

} // namespace VkHook
