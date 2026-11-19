#include "control/commands_apply.hh"

#include "control/frontend.hh"
#include "control/preset_model.hh"
#include "depth/tracker.hh"
#include "effects/effect_registry.hh"
#include "config_serializer.hh"
#include "logical_device.hh"
#include "logger.hh"
#include "persist.hh"
#include "store.hh"

#include <algorithm>
#include <filesystem>
#include <future>
#include <mutex>
#include <set>
#include <variant>

namespace VkHook
{
    OverlayControl::OverlayControl(LogicalDevice* device)
        : logicalDevice(device), effectRegistry(&VkHook::effectRegistry)
    {
    }

    OverlayControl::~OverlayControl()
    {
        // The last frame's edits are applied before the UI goes.
        applyCommands();
        uiPtr.store(nullptr, std::memory_order_release);
        uiOwner.reset();
    }

    OverlayUi* OverlayControl::ensureUi()
    {
        if (OverlayUi* existing = ui())
            return existing;

        // Built with no lock held: it reads fonts and the saved layout from disk.
        std::unique_ptr<OverlayUi> created = OverlayUi::create(logicalDevice, &persistent, &commands, &slot);
        if (!created)
            return nullptr;

        OverlayUi* expected = nullptr;
        if (uiPtr.compare_exchange_strong(expected, created.get(), std::memory_order_acq_rel))
        {
            uiOwner = std::move(created);
            return uiOwner.get();
        }
        return expected;  // another thread's UI won; this one is discarded
    }

    void OverlayControl::pushToast(LogLevel level, const std::string& message)
    {
        if (OverlayUi* current = ui())
            current->pushToast(level, message);
    }

    void OverlayControl::initProfiles()
    {
        if (profilesInitialized)
            return;
        profilesInitialized = true;
        activeGameName = detectedGameName;
        activeProfileName = VkHook::activeProfileName;
        activeProfilePath = VkHook::activeProfilePath;
        refreshShaderProfiles();
    }

    void OverlayControl::publishSnapshot()
    {
        slot.publish(buildSnapshot());
    }

    void OverlayControl::setAvailable(OverlayState newState)
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

    void OverlayControl::refreshShaderProfiles()
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
        activeShaderProfilePathValue = ConfigSerializer::getShaderProfilePath(activeGameName, activeShaderProfileName);
        queueLastProfile();
    }

    void OverlayControl::queueLastProfile()
    {
        if (activeGameName.empty() || activeShaderProfileName.empty())
            return;
        PersistenceQueue::instance().other(
            "last-profile:" + activeGameName,
            [game = activeGameName, profile = activeShaderProfileName] {
                return ConfigSerializer::setLastShaderProfile(game, profile);
            });
    }

    void OverlayControl::setActiveProfile(const std::string& name)
    {
        activeShaderProfileName = name;
        activeShaderProfilePathValue = ConfigSerializer::getShaderProfilePath(activeGameName, activeShaderProfileName);
        queueLastProfile();
    }

    // Makes frontend.cc load the active preset on its next frame.
    void OverlayControl::activateActiveProfile()
    {
        requests.pendingShaderProfilePath = activeShaderProfilePathValue;
        requests.pendingShaderProfile = true;
        requests.rebuild = true;
    }

    // Saves the outgoing preset while it is still active, then activates `name`.
    bool OverlayControl::switchProfile(const std::string& name)
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

    bool OverlayControl::autoSaveProfile(bool block)
    {
        if (!effectRegistry)
            return false;
        if (activeProfilePath.empty() && activeShaderProfilePathValue.empty())
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
        const std::string shaderPath = activeShaderProfilePathValue;

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
    void OverlayControl::saveConfig(const std::string& name)
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

    void OverlayControl::importPreset(const std::string& sourcePath)
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

    void OverlayControl::gatherDepth(DepthInfo& depthInfo, DeviceInfo& deviceInfo)
    {
        const auto settings = ConfigStore::instance().snapshot();

        // a short read of the tracker's maps; the copy leaves the lock before anything else runs
        std::lock_guard<std::mutex> l(logicalDevice->stateLock);

        depthInfo.supportedResolveModes = logicalDevice->supportedDepthResolveModes;
        depthInfo.depthCaptureEnabled = settings->depthCapture;
        depthInfo.swapchainExtent = currentSwapchainExtentLocked(logicalDevice);

        deviceInfo.gpuName = logicalDevice->gpuName;
        deviceInfo.gpuDriverInfo = logicalDevice->gpuDriverInfo;
        deviceInfo.gpuPciSlot = logicalDevice->gpuPciSlot;
        deviceInfo.gpuApiVersion = logicalDevice->gpuApiVersion;
        deviceInfo.gpuVendorId = logicalDevice->gpuVendorId;

        const bool avgSupported = (logicalDevice->supportedDepthResolveModes & VK_RESOLVE_MODE_AVERAGE_BIT) != 0;
        depthInfo.depthResolveMode = settings->depthResolveMode == 2 && avgSupported
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

        // A pin whose view the render thread destroyed since the last frame is
        // shown as no pin (the depth hooks clear the field itself).
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

    CoreSnapshotPtr OverlayControl::buildSnapshot()
    {
        auto snapshot = std::make_shared<CoreSnapshot>();
        snapshot->generation = ++snapshotGeneration;
        snapshot->settings = ConfigStore::instance().snapshot();

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
        profiles.activePath = activeShaderProfilePathValue;
        profiles.names = shaderProfiles;
        profiles.configName = state.configName;

        gatherDepth(snapshot->depth, snapshot->device);
        return snapshot;
    }

    // --- the commands ----------------------------------------------------------

    // One member per Command alternative: what each user action does, and which
    // of them the control step has to react to (OverlayRequests).
    struct OverlayControl::CommandVisitor
    {
        OverlayControl& self;
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

        void operator()(const ToggleEffects&) const { self.requests.toggleEffects = true; }

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

        void operator()(const RebuildChain&) const { self.requests.rebuild = true; }

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
                self.activeShaderProfilePathValue.clear();
            }
            self.activateActiveProfile();
        }

        void operator()(const ImportPreset& c) const { self.importPreset(c.path); }

        void operator()(const LoadConfig& c) const
        {
            self.requests.pendingConfigPath = c.path;
            self.requests.rebuild = true;
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

        // The reload key's path: the settings file is re-read here, and the
        // control step re-reads the game's .conf and preset (Config::reload plus
        // applyShaderProfile) before it rebuilds the chain.
        void operator()(const ReloadConfig&) const
        {
            ConfigStore::instance().reload();
            self.requests.settingsSaved = true;
            self.requests.shaderPathsChanged = true;
            self.requests.reloadFromDisk = true;
            self.requests.rebuild = true;
        }

        void operator()(const PinDepth& c) const
        {
            if (!pinDepthView(self.logicalDevice, c.view))
            {
                Logger::warn("depth pin requested but view not found (may have been destroyed)");
                return;
            }
            Logger::info("depth manual pin set to view 0x" + std::to_string(reinterpret_cast<uint64_t>(c.view)));
        }

        void operator()(const ClearDepthPin&) const
        {
            clearDepthPin(self.logicalDevice);
            Logger::info("depth manual pin cleared (back to auto-promotion)");
        }

        void operator()(const RefreshDepth&) const { refreshDepthDetection(self.logicalDevice); }

        // the resolve owns the transforms (macros no longer carry them), so
        // re-recording it is all a toggle needs
        void operator()(const DepthTransformChanged&) const { requestDepthRealloc(self.logicalDevice); }

        void operator()(const SetSetting& c) const
        {
            if (!ConfigStore::instance().set(c.key, c.value))
                Logger::warn("overlay: refused setting " + c.key);
        }

        void operator()(const SettingsChanged&) const
        {
            ConfigStore::instance().save();
            self.requests.settingsSaved = true;
        }

        void operator()(const ShaderPathsChanged& c) const
        {
            PersistenceQueue::instance().write(ConfigSerializer::getBaseConfigDir() + "/shader_manager.conf",
                                               [config = c.config] { return ConfigSerializer::saveShaderManagerConfig(config); });
            self.requests.shaderPathsChanged = true;
        }
    };

    OverlayRequests OverlayControl::applyCommands()
    {
        std::vector<Command> pending;
        commands.drain(pending);
        const CommandVisitor visitor{*this};
        for (const Command& command : pending)
            std::visit(visitor, command);
        return std::exchange(requests, OverlayRequests{});
    }
} // namespace VkHook
