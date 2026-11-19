#include "imgui_overlay.hh"
#include "persist.hh"
#include "effects/effect_registry.hh"
#include "settings_manager.hh"
#include "config_serializer.hh"
#include "params/field_editor.hh"
#include "logger.hh"
#include "overlay/ui_theme.hh"
#include "util.hh"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imfilebrowser.h"

namespace VkHook
{
    namespace
    {
        constexpr const char* kEffectReorderPayload = "VKHOOK_EFFECT_REORDER";

        // Render a single preprocessor definition input, returns true if value changed
        bool renderPreprocessorDef(PreprocessorDefinition& def, EffectRegistry* registry, const std::string& effectName)
        {
            bool changed = false;
            char valueBuf[64];
            strncpy(valueBuf, def.value.c_str(), sizeof(valueBuf) - 1);
            valueBuf[sizeof(valueBuf) - 1] = '\0';
            const bool modified = def.value != def.defaultValue;

            ImGui::PushID(def.name.c_str());

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(def.name.c_str());
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##value", valueBuf, sizeof(valueBuf)))
            {
                registry->setPreprocessorDefValue(effectName, def.name, valueBuf);
                changed = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Default: %s", def.defaultValue.c_str());

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!modified);
            if (ImGui::Button("Reset"))
            {
                registry->setPreprocessorDefValue(effectName, def.name, def.defaultValue);
                changed = true;
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(modified ? "Reset to default (%s)" : "Using default (%s)", def.defaultValue.c_str());

            if (modified)
            {
                ImGui::SameLine();
                ImGui::TextColored(UI::Warning(), "Modified");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Modified from default");
            }

            if (ImGui::BeginPopupContextItem("##preproc_reset"))
            {
                if (ImGui::MenuItem("Reset to default"))
                {
                    registry->setPreprocessorDefValue(effectName, def.name, def.defaultValue);
                    changed = true;
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
            return changed;
        }

    } // anonymous namespace

    // keyboard is part of the shared view signature; this particular view reads no
    // key state directly, so the name is omitted here.
    void ImGuiOverlay::renderMainView(const KeyboardState& /* keyboard */)
    {
        // Fallback picker when the xdg portal is unavailable; mirrors the
        // Shaders tab. only displayed while the main view is the active tab.
        if (!presetBrowser)
            presetBrowser = std::make_shared<ImGui::FileBrowser>(ImGuiFileBrowserFlags_CloseOnEsc);
        ImGui::FileBrowser& browser = *presetBrowser;

        if (!effectRegistry)
            return;

        auto openPresetBrowser = [&]() {
            browser.SetTitle("Import ReShade preset");
            browser.SetTypeFilters({".ini"});
            const char* home = std::getenv("HOME");
            browser.SetPwd(home ? home : "/");
            browser.Open();
        };

        // Harvest a completed portal import. a request started from another tab
        // is ignored by pollFileDialog's kind check and stays queued.
        {
            FileDialogResult result = FileDialogResult::Cancelled;
            std::string picked;
            if (pollFileDialog(FileDialogKind::OpenFile, result, picked))
            {
                if (result == FileDialogResult::Success && !picked.empty())
                    importShaderPreset(picked);
                else if (result == FileDialogResult::Unavailable)
                    openPresetBrowser();
            }
        }

        std::vector<std::string> selectedEffects = effectRegistry->getSelectedEffects();
        const bool splitParams = settingsManager.getSeparateEffectParams();

        // Renders one effect's preprocessor definitions and parameters, shared by
        // the inline list and the separate parameters card. Returns changed.
        auto renderEffectParams = [&](const std::string& effectName) -> bool {
            bool changed = false;
            if (effectRegistry->hasEffectFailed(effectName))
            {
                ImGui::PushStyleColor(ImGuiCol_Text, UI::Error());
                ImGui::TextWrapped("Failed to compile: %s",
                                   effectRegistry->getEffectError(effectName).c_str());
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            auto& defs = effectRegistry->getPreprocessorDefs(effectName);
            if (!defs.empty())
            {
                if (ImGui::TreeNode("Preprocessor"))
                {
                    if (ImGui::BeginTable("##definitions", 3, ImGuiTableFlags_Resizable))
                    {
                        ImGui::TableSetupColumn("Name");
                        ImGui::TableSetupColumn("Value");
                        ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed);
                        for (auto& def : defs)
                        {
                            if (renderPreprocessorDef(def, effectRegistry, effectName))
                            {
                                changed = true;
                                reloadNeeded = true;
                            }
                        }
                        ImGui::EndTable();
                    }
                    ImGui::TreePop();
                }
                ImGui::Spacing();
                ImGui::Spacing();
            }

            // ReShade uniforms are pushed every frame by updateEffect, so a value
            // edit needs no rebuild.
            auto effectParams = effectRegistry->getParametersForEffect(effectName);
            if (effectParams.empty() && defs.empty())
                UI::TextSupport("This effect has no adjustable parameters.");
            for (size_t paramIdx = 0; paramIdx < effectParams.size(); paramIdx++)
            {
                ImGui::PushID(static_cast<int>(paramIdx));
                if (renderFieldEditor(*effectParams[paramIdx]))
                    changed = true;
                ImGui::PopID();
            }
            return changed;
        };

        // Normal mode - show profile and effect controls

        // Shader profile operations retain their existing persistence behavior.
        if (!activeGameName.empty())
        {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
            const char* shaderLabel = activeShaderProfileName.empty() ? "None" : activeShaderProfileName.c_str();
            if (ImGui::BeginCombo("Shader preset", shaderLabel))
            {
                for (const auto& profile : shaderProfiles)
                {
                    const bool selected = profile == activeShaderProfileName;
                    if (ImGui::Selectable(profile.c_str(), selected) && !selected)
                    {
                        if (!switchShaderProfile(profile))
                            pushToast(LogLevel::Error, "Could not save the active shader profile.");
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("New preset"))
                ImGui::OpenPopup("New shader preset");
            if (!activeShaderProfileName.empty())
            {
                ImGui::SameLine();
                if (ImGui::Button("Rename"))
                {
                    renameProfileTarget = activeShaderProfileName;
                    std::snprintf(renameShaderProfileName, sizeof(renameShaderProfileName), "%s",
                                  activeShaderProfileName.c_str());
                    ImGui::OpenPopup("Rename shader preset");
                }

                if (ImGui::BeginPopupModal("Rename shader preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::TextDisabled("Name");
                    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
                    ImGui::InputText("##renameshaderprofilename", renameShaderProfileName,
                                     sizeof(renameShaderProfileName));
                    ImGui::Spacing();
                    ImGui::Spacing();
                    int action = -1;
                    if (ImGui::Button("Cancel"))
                        action = 0;
                    ImGui::SameLine();
                    ImGui::BeginDisabled(renameShaderProfileName[0] == '\0');
                    if (ImGui::Button("Rename"))
                        action = 1;
                    ImGui::EndDisabled();
                    if (action == 1)
                    {
                        const std::string newName = renameShaderProfileName;
                        const std::string oldName = renameProfileTarget;
                        if (newName == oldName)
                        {
                            // name unchanged: nothing to rename
                        }
                        else if (!ConfigSerializer::isValidShaderProfileName(newName))
                            pushToast(LogLevel::Error, "Preset names can't contain /, \\ or @.");
                        else if (std::find(shaderProfiles.begin(), shaderProfiles.end(), newName) != shaderProfiles.end())
                            pushToast(LogLevel::Error, "A shader preset named \"" + newName + "\" already exists.");
                        else if (!autoSaveProfile(true))
                            pushToast(LogLevel::Error, "Could not save the active shader profile.");
                        else
                        {
                            // run the rename on the writer thread so a queued
                            // save can't race the files, then update the UI's
                            // name/path only after it has landed
                            const bool renamed = PersistenceQueue::instance().rename(
                                ConfigSerializer::getShaderProfilePath(activeGameName, oldName),
                                ConfigSerializer::getRenamedShaderProfilePath(activeGameName, oldName, newName),
                                [game = activeGameName, oldName, newName]() {
                                    return ConfigSerializer::renameShaderProfile(game, oldName, newName);
                                }).get();
                            if (renamed)
                            {
                                if (activeShaderProfileName == oldName)
                                    setActiveShaderProfile(newName);
                                refreshShaderProfiles();
                                pendingShaderProfilePath = activeShaderProfilePath;
                                pendingShaderProfile = true;
                                applyRequested = true;
                                paramsDirty = false;
                                profileDirty = false;
                                pushToast(LogLevel::Info, "Renamed shader preset to \"" + newName + "\".");
                            }
                            else
                                pushToast(LogLevel::Error, "Could not rename the shader profile.");
                        }
                    }
                    if (action >= 0)
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }

                ImGui::SameLine();
                if (ImGui::Button("Delete"))
                {
                    deleteProfileTarget = activeShaderProfileName;
                    ImGui::OpenPopup("Delete shader preset");
                }

                if (ImGui::BeginPopupModal("Delete shader preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 28.0f);
                    ImGui::TextUnformatted(("This preset, \"" + deleteProfileTarget + "\", will be deleted. This can't be undone.").c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::Spacing();
                    ImGui::Spacing();
                    int action = -1;
                    if (ImGui::Button("Cancel"))
                        action = 0;
                    ImGui::SameLine();
                    if (ImGui::Button("Delete preset"))
                        action = 1;
                    if (action == 1)
                    {
                        // queued behind any pending autosave of this preset, which the
                        // queue cancels, so the file cannot come back after the delete
                        if (!PersistenceQueue::instance().remove(
                                ConfigSerializer::getShaderProfilePath(activeGameName, deleteProfileTarget),
                                [game = activeGameName, target = deleteProfileTarget]() {
                                    return ConfigSerializer::deleteShaderProfile(game, target);
                                }).get())
                            pushToast(LogLevel::Error, "Could not delete the shader profile.");
                        else
                        {
                            if (deleteProfileTarget == activeShaderProfileName)
                            {
                                profileDirty = false;
                                paramsDirty = false;
                                activeShaderProfileName.clear();
                            }
                            refreshShaderProfiles();
                            if (!shaderProfiles.empty())
                            {
                                pendingShaderProfilePath = activeShaderProfilePath;
                                pendingShaderProfile = true;
                                applyRequested = true;
                            }
                            else
                            {
                                activeShaderProfileName.clear();
                                activeShaderProfilePath.clear();
                                pendingShaderProfilePath.clear();
                                pendingShaderProfile = true;
                                applyRequested = true;
                            }
                        }
                    }
                    if (action >= 0)
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(fileDialogPending() || browser.IsOpened());
            if (ImGui::Button("Import preset"))
            {
                if (!settingsManager.getUsePortalIntegration()
                    || !startOpenFileDialog("Import ReShade preset", {"*.ini"}))
                    openPresetBrowser();
            }
            ImGui::EndDisabled();
            if (ImGui::BeginPopupModal("New shader preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextDisabled("Name");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
                ImGui::InputText("##newshaderprofilename", newShaderProfileName, sizeof(newShaderProfileName));
                ImGui::Spacing();
                ImGui::Spacing();
                int action = -1;
                if (ImGui::Button("Cancel"))
                    action = 0;
                ImGui::SameLine();
                ImGui::BeginDisabled(newShaderProfileName[0] == '\0');
                if (ImGui::Button("Create"))
                    action = 1;
                ImGui::EndDisabled();
                if (action == 1)
                {
                    if (!autoSaveProfile(true))
                        pushToast(LogLevel::Error, "Could not save the active shader profile.");
                    else if (PersistenceQueue::instance().create(
                                 ConfigSerializer::getGameShaderProfilePath(activeGameName, newShaderProfileName),
                                 [game = activeGameName, name = std::string(newShaderProfileName),
                                  from = activeShaderProfileName]() {
                                     return ConfigSerializer::createShaderProfile(game, name, from);
                                 }).get())
                    {
                        switchShaderProfile(newShaderProfileName);
                        refreshShaderProfiles();
                        newShaderProfileName[0] = '\0';
                    }
                    else
                        pushToast(LogLevel::Error, "Could not create the shader profile.");
                }
                if (action >= 0)
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

        }
        else
        {
            // Fallback: legacy config UI for unknown executables
            ImGui::Text("Config:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            ImGui::InputText("##configname", saveConfigName, sizeof(saveConfigName));
            ImGui::SameLine();
            ImGui::BeginDisabled(saveConfigName[0] == '\0');
            if (ImGui::Button("Save"))
                saveCurrentConfig();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Manage configs"))
            {
                inConfigManageMode = true;
                configListRefreshPending = true;
            }
        }

        UI::TextSupport(("Toggle keybind bound to " + settingsManager.getToggleKey()).c_str());
        bool effectsOn = state.effectsEnabled;
        if (ImGui::Checkbox("Enable effects", &effectsOn))
            toggleEffectsRequested = true;
        if (ImGui::Button("Add effects"))
        {
            inSelectionMode = true;
            addEffectsFocusSearch = true;
            insertPosition = -1;
            pendingAddEffects.clear();
            addEffectsDuplicateCount.clear();
            addEffectsRemoved.clear();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(selectedEffects.empty());
        if (ImGui::Button("Clear all"))
            ImGui::OpenPopup("Clear all effects");
        ImGui::EndDisabled();
        if (ImGui::BeginPopupModal("Clear all effects", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("Remove every effect from this preset? This cannot be undone.");
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::SameLine();
            if (ImGui::Button("Clear all"))
            {
                selectedEffects.clear();
                effectRegistry->clearSelectedEffects();
                paramsDirty = true;
                lastChangeTime = std::chrono::steady_clock::now();
                applyRequested = true;
                profileDirty = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        bool selectParamsTab = false;
        bool chainChanged = false;
        std::string removeEffect;
        std::string movingEffect;
        std::string targetEffect;
        ImGui::SeparatorText("Effects");
        if (selectedEffects.empty())
            UI::TextSupport("No effects in this preset.");
        for (const auto& effectName : selectedEffects)
        {
            ImGui::PushID(effectName.c_str());
            ImGui::Button("Drag");
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload(kEffectReorderPayload, effectName.c_str(), effectName.size() + 1);
                ImGui::TextUnformatted(effectName.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::SameLine();
            const bool failed = effectRegistry->hasEffectFailed(effectName);
            bool enabled = effectRegistry->isEffectEnabled(effectName);
            ImGui::BeginDisabled(failed);
            if (ImGui::Checkbox("##enabled", &enabled))
            {
                effectRegistry->setEffectEnabled(effectName, enabled);
                paramsDirty = true;
                profileDirty = true;
                reloadNeeded = true;
                lastChangeTime = std::chrono::steady_clock::now();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            bool open = false;
            if (splitParams)
            {
                if (ImGui::Selectable(effectName.c_str(), paramsActiveEffect == effectName))
                {
                    paramsActiveEffect = effectName;
                    selectParamsTab = true;
                }
            }
            else
                open = ImGui::CollapsingHeader(effectName.c_str());
            if (failed && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", effectRegistry->getEffectError(effectName).c_str());
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEffectReorderPayload))
                {
                    movingEffect = static_cast<const char*>(payload->Data);
                    targetEffect = effectName;
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem("effect_context"))
            {
                if (ImGui::MenuItem(enabled ? "Disable" : "Enable", nullptr, false, !failed))
                {
                    effectRegistry->setEffectEnabled(effectName, !enabled);
                    paramsDirty = true;
                    profileDirty = true;
                    reloadNeeded = true;
                    lastChangeTime = std::chrono::steady_clock::now();
                }
                if (ImGui::MenuItem("Reset to defaults"))
                {
                    for (auto* param : effectRegistry->getParametersForEffect(effectName))
                        param->resetToDefault();
                    paramsDirty = true;
                    profileDirty = true;
                    lastChangeTime = std::chrono::steady_clock::now();
                }
                if (ImGui::MenuItem("Insert effects here"))
                {
                    insertPosition = static_cast<int>(&effectName - selectedEffects.data());
                    inSelectionMode = true;
                    addEffectsFocusSearch = true;
                    pendingAddEffects.clear();
                    addEffectsDuplicateCount.clear();
                    addEffectsRemoved.clear();
                }
                if (ImGui::MenuItem("Remove"))
                    removeEffect = effectName;
                ImGui::EndPopup();
            }
            if (open && renderEffectParams(effectName))
            {
                paramsDirty = true;
                profileDirty = true;
                lastChangeTime = std::chrono::steady_clock::now();
            }
            ImGui::PopID();
        }
        if (!removeEffect.empty())
        {
            selectedEffects.erase(std::remove(selectedEffects.begin(), selectedEffects.end(), removeEffect),
                                  selectedEffects.end());
            effectRegistry->setSelectedEffects(selectedEffects);
            effectRegistry->removeEffect(removeEffect);
            chainChanged = true;
        }
        if (!movingEffect.empty() && movingEffect != targetEffect)
        {
            auto source = std::find(selectedEffects.begin(), selectedEffects.end(), movingEffect);
            auto target = std::find(selectedEffects.begin(), selectedEffects.end(), targetEffect);
            if (source != selectedEffects.end() && target != selectedEffects.end())
            {
                std::iter_swap(source, target);
                effectRegistry->setSelectedEffects(selectedEffects);
                chainChanged = true;
            }
        }
        if (chainChanged)
        {
            paramsDirty = true;
            profileDirty = true;
            applyRequested = true;
            lastChangeTime = std::chrono::steady_clock::now();
        }
        if (splitParams)
        {
            if (selectParamsTab)
                ImGui::SetNextItemOpen(true);
            if (ImGui::CollapsingHeader("Effect parameters"))
            {
                if (selectedEffects.empty())
                    UI::TextSupport("Add effects to edit their parameters.");
                if (ImGui::BeginTabBar("##effect_params", ImGuiTabBarFlags_FittingPolicyScroll))
                {
                    for (const auto& effectName : selectedEffects)
                    {
                        const ImGuiTabItemFlags flags = selectParamsTab && paramsActiveEffect == effectName
                            ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                        if (ImGui::BeginTabItem(effectName.c_str(), nullptr, flags))
                        {
                            paramsActiveEffect = effectName;
                            ImGui::PushID(effectName.c_str());
                            if (renderEffectParams(effectName))
                            {
                                paramsDirty = true;
                                profileDirty = true;
                                lastChangeTime = std::chrono::steady_clock::now();
                            }
                            ImGui::PopID();
                            ImGui::EndTabItem();
                        }
                    }
                    ImGui::EndTabBar();
                }
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Apply"))
        {
            applyRequested = true;
            paramsDirty = false;
            profileDirty = true;  // Mark for auto-save to profile
        }
        // Note: Auto-apply is handled globally in imgui_overlay.cpp

        // The fallback picker is modal and must be displayed every frame.
        browser.Display();
        if (browser.HasSelected())
        {
            importShaderPreset(browser.GetSelected().string());
            browser.ClearSelected();
        }
    }

    void ImGuiOverlay::importShaderPreset(const std::string& sourcePath)
    {
        // the import picks a free name inside the job, so the queue is told
        // about the created file from there
        const std::string imported = [&] {
            std::string name;
            PersistenceQueue::instance().submit({PersistenceQueue::Kind::Other, "import:" + sourcePath, {},
                [&name, sourcePath]() {
                    name = ConfigSerializer::importShaderProfile(sourcePath);
                    if (name.empty())
                        return false;
                    PersistenceQueue::instance().noteCreated(ConfigSerializer::getImportedShaderProfilePath(name));
                    return true;
                }}).get();
            return name;
        }();
        if (imported.empty())
        {
            pushToast(LogLevel::Error, "Could not import the shader preset.");
            return;
        }

        // save the outgoing profile, then activate the import. refresh can
        // already select the new name, so the reload is queued here directly.
        const bool saved = autoSaveProfile(true);
        refreshShaderProfiles();
        setActiveShaderProfile(imported);
        pendingShaderProfilePath = activeShaderProfilePath;
        pendingShaderProfile = true;
        applyRequested = true;
        paramsDirty = false;
        profileDirty = false;
        if (!saved)
            pushToast(LogLevel::Error, "Imported \"" + imported + "\", but the previous profile could not be saved.");
        else
            pushToast(LogLevel::Info, "Imported shader preset \"" + imported + "\".");
    }

} // namespace VkHook
