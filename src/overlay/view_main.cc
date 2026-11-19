#include "overlay_ui.hh"
#include "effect_names.hh"
#include "config_serializer.hh"
#include "params/field_editor.hh"
#include "logger.hh"
#include "overlay/ui_theme.hh"
#include "util.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imfilebrowser.h"

namespace VkHook
{
    namespace
    {
        constexpr const char* kEffectReorderPayload = "VKHOOK_EFFECT_REORDER";

        // One preprocessor definition row. Returns the new value when the user
        // edited or reset it; the caller turns that into a SetMacro command.
        std::optional<std::string> renderPreprocessorDef(const PreprocessorDefinition& def)
        {
            std::optional<std::string> changed;
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
                changed = std::string(valueBuf);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Default: %s", def.defaultValue.c_str());

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!modified);
            if (ImGui::Button("Reset"))
                changed = def.defaultValue;
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
                    changed = def.defaultValue;
                ImGui::EndPopup();
            }

            ImGui::PopID();
            return changed;
        }

    } // anonymous namespace

    void OverlayUi::renderMainView()
    {
        // Fallback picker when the xdg portal is unavailable; mirrors the
        // Shaders tab. only displayed while the main view is the active tab.
        if (!presetBrowser)
            presetBrowser = std::make_shared<ImGui::FileBrowser>(ImGuiFileBrowserFlags_CloseOnEsc);
        ImGui::FileBrowser& browser = *presetBrowser;

        const std::vector<EffectView>& chain = snapshot->effects.chain;
        const ProfileInfo& profiles = snapshot->profiles;

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
                {
                    dropPendingEdits();
                    emit(ImportPreset{picked});
                }
                else if (result == FileDialogResult::Unavailable)
                    openPresetBrowser();
            }
        }

        const bool splitParams = settings().separateEffectParams;

        // Renders one effect's preprocessor definitions and parameters, shared by
        // the inline list and the separate parameters card. Every edit leaves as
        // a command; the widgets work on edit buffers, never on the layer's state.
        auto renderEffectParams = [&](const EffectView& effect) {
            if (effect.failed)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, UI::Error());
                ImGui::TextWrapped("Failed to compile: %s", effect.error.c_str());
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }

            if (!effect.macros.empty())
            {
                if (ImGui::TreeNode("Preprocessor"))
                {
                    if (ImGui::BeginTable("##definitions", 3, ImGuiTableFlags_Resizable))
                    {
                        ImGui::TableSetupColumn("Name");
                        ImGui::TableSetupColumn("Value");
                        ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed);
                        for (const PreprocessorDefinition& def : effect.macros)
                        {
                            if (const auto value = renderPreprocessorDef(def))
                            {
                                emit(SetMacro{effect.name, def.name, *value});
                                noteEdit(true);
                            }
                        }
                        ImGui::EndTable();
                    }
                    ImGui::TreePop();
                }
                ImGui::Spacing();
                ImGui::Spacing();
            }

            // ReShade uniforms are pushed every frame by updateFrame, so a value
            // edit needs no rebuild.
            if (effect.params.empty() && effect.macros.empty())
                UI::TextSupport("This effect has no adjustable parameters.");
            for (size_t paramIdx = 0; paramIdx < effect.params.size(); paramIdx++)
            {
                ImGui::PushID(static_cast<int>(paramIdx));
                EffectParam& edit = editableParam(effect.name, *effect.params[paramIdx]);
                if (renderFieldEditor(edit))
                {
                    emit(SetParam{effect.name, std::shared_ptr<const EffectParam>(edit.clone())});
                    noteEdit(false);
                }
                ImGui::PopID();
            }
        };

        // Normal mode - show profile and effect controls

        if (!profiles.gameName.empty())
        {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
            const char* shaderLabel = profiles.activeName.empty() ? "None" : profiles.activeName.c_str();
            if (ImGui::BeginCombo("Shader preset", shaderLabel))
            {
                for (const auto& profile : profiles.names)
                {
                    const bool selected = profile == profiles.activeName;
                    if (ImGui::Selectable(profile.c_str(), selected) && !selected)
                    {
                        dropPendingEdits();
                        emit(SwitchProfile{profile});
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("New preset"))
                ImGui::OpenPopup("New shader preset");
            if (!profiles.activeName.empty())
            {
                ImGui::SameLine();
                if (ImGui::Button("Rename"))
                {
                    renameProfileTarget = profiles.activeName;
                    std::snprintf(renameShaderProfileName, sizeof(renameShaderProfileName), "%s",
                                  profiles.activeName.c_str());
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
                        if (newName == renameProfileTarget)
                        {
                            // name unchanged: nothing to rename
                        }
                        else if (!ConfigSerializer::isValidShaderProfileName(newName))
                            pushToast(LogLevel::Error, "Preset names can't contain /, \\ or @.");
                        else if (std::find(profiles.names.begin(), profiles.names.end(), newName) != profiles.names.end())
                            pushToast(LogLevel::Error, "A shader preset named \"" + newName + "\" already exists.");
                        else
                        {
                            dropPendingEdits();
                            emit(RenameProfile{renameProfileTarget, newName});
                        }
                    }
                    if (action >= 0)
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }

                ImGui::SameLine();
                if (ImGui::Button("Delete"))
                {
                    deleteProfileTarget = profiles.activeName;
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
                        dropPendingEdits();
                        emit(DeleteProfile{deleteProfileTarget});
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
                if (!settings().usePortalIntegration
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
                    dropPendingEdits();
                    emit(CreateProfile{newShaderProfileName});
                    newShaderProfileName[0] = '\0';
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
            {
                emit(SaveConfig{saveConfigName});
                configListRefreshPending = true;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Manage configs"))
            {
                inConfigManageMode = true;
                configListRefreshPending = true;
            }
        }

        UI::TextSupport(("Toggle keybind bound to " + settings().toggleKey).c_str());
        bool effectsOn = snapshot->effects.effectsEnabled;
        if (ImGui::Checkbox("Enable effects", &effectsOn))
            emit(ToggleEffects{});
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
        ImGui::BeginDisabled(chain.empty());
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
                emit(ClearEffects{});
                chainEdited();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        bool selectParamsTab = false;
        std::string removeEffect;
        std::string movingEffect;
        std::string targetEffect;
        ImGui::SeparatorText("Effects");
        if (chain.empty())
            UI::TextSupport("No effects in this preset.");
        for (size_t effectIndex = 0; effectIndex < chain.size(); ++effectIndex)
        {
            const EffectView& effect = chain[effectIndex];
            const std::string& effectName = effect.name;
            const std::string label = effectLabel(snapshot->effects, effect);
            ImGui::PushID(effectName.c_str());
            ImGui::Button("Drag");
            if (ImGui::BeginDragDropSource())
            {
                ImGui::SetDragDropPayload(kEffectReorderPayload, effectName.c_str(), effectName.size() + 1);
                ImGui::TextUnformatted(label.c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::SameLine();
            bool enabled = effect.enabled;
            ImGui::BeginDisabled(effect.failed);
            if (ImGui::Checkbox("##enabled", &enabled))
            {
                emit(SetEffectEnabled{effectName, enabled});
                noteEdit(true);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            bool open = false;
            if (splitParams)
            {
                if (ImGui::Selectable(label.c_str(), paramsActiveEffect == effectName))
                {
                    paramsActiveEffect = effectName;
                    selectParamsTab = true;
                }
            }
            else
                open = ImGui::CollapsingHeader(label.c_str());
            if (effect.failed && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", effect.error.c_str());
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
                if (ImGui::MenuItem(enabled ? "Disable" : "Enable", nullptr, false, !effect.failed))
                {
                    emit(SetEffectEnabled{effectName, !enabled});
                    noteEdit(true);
                }
                if (ImGui::MenuItem("Reset to defaults"))
                {
                    emit(ResetParams{effectName});
                    noteEdit(false);
                }
                if (ImGui::MenuItem("Insert effects here"))
                {
                    insertPosition = static_cast<int>(effectIndex);
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
            if (open)
                renderEffectParams(effect);
            ImGui::PopID();
        }
        if (!removeEffect.empty())
        {
            EditChain edit;
            edit.remove.push_back(removeEffect);
            emit(std::move(edit));
            chainEdited();
        }
        if (!movingEffect.empty() && movingEffect != targetEffect)
        {
            std::vector<std::string> order;
            order.reserve(chain.size());
            for (const EffectView& effect : chain)
                order.push_back(effect.name);
            auto source = std::find(order.begin(), order.end(), movingEffect);
            auto target = std::find(order.begin(), order.end(), targetEffect);
            if (source != order.end() && target != order.end())
            {
                std::iter_swap(source, target);
                emit(ReorderEffects{std::move(order)});
                chainEdited();
            }
        }
        if (splitParams)
        {
            if (selectParamsTab)
                ImGui::SetNextItemOpen(true);
            if (ImGui::CollapsingHeader("Effect parameters"))
            {
                if (chain.empty())
                    UI::TextSupport("Add effects to edit their parameters.");
                if (ImGui::BeginTabBar("##effect_params", ImGuiTabBarFlags_FittingPolicyScroll))
                {
                    for (const EffectView& effect : chain)
                    {
                        const ImGuiTabItemFlags flags = selectParamsTab && paramsActiveEffect == effect.name
                            ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                        if (ImGui::BeginTabItem((effectLabel(snapshot->effects, effect) + "###" + effect.name).c_str(), nullptr, flags))
                        {
                            paramsActiveEffect = effect.name;
                            ImGui::PushID(effect.name.c_str());
                            renderEffectParams(effect);
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
            emit(RebuildChain{});
            paramsDirty = false;
            rebuildNeeded = false;
            saveDue = true;
        }
        // Auto-apply is OverlayUi::runAutoApply, after the frame's views.

        // The fallback picker is modal and must be displayed every frame.
        browser.Display();
        if (browser.HasSelected())
        {
            dropPendingEdits();
            emit(ImportPreset{browser.GetSelected().string()});
            browser.ClearSelected();
        }
    }

} // namespace VkHook
