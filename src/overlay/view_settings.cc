#include "overlay_ui.hh"
#include "logger.hh"
#include "overlay/ui_theme.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "vendor/imgui/imgui.h"

namespace VkHook
{
    void OverlayUi::renderSettingsView(const KeyboardState& keyboard)
    {
        const AppSettings& cfg = settings();

        // Every control reads its value from the snapshot and answers with a
        // SetSetting; the range of a numeric control is the schema's. Settings
        // that are final (a toggle, a finished slider drag) also send
        // SettingsChanged, which persists them.
        const auto rangeOf = [](const char* key) { return Schema::find(key); };
        const auto commit = [&](const char* key, SettingValue value) {
            emitSetting(key, std::move(value));
            emit(SettingsChanged{});
        };

        ImGui::BeginChild("SettingsContent", ImVec2(0, 0), false);
        const float d = ImGui::GetFontSize() / 16.0f;

        // Renders a keybind button.
        auto renderKeyBind = [&](const char* label, const char* tooltip, const char* key,
                                 const std::string& currentKey, int bindingId) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tooltip);
            ImGui::SameLine(160.0f * d);

            bool isListening = (listeningForKey == bindingId);
            const char* buttonText = isListening ? "Press a key..." : currentKey.c_str();

            if (isListening)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                ImGui::PushStyleColor(ImGuiCol_Text, UI::OnSurface());
            }

            ImGui::PushID(bindingId);
            if (ImGui::Button(buttonText, ImVec2(140.0f * d, 0)))
                listeningForKey = isListening ? 0 : bindingId;
            ImGui::PopID();

            if (isListening)
                ImGui::PopStyleColor(2);

            if (isListening && !keyboard.lastKeyName.empty())
            {
                commit(key, keyboard.lastKeyName);
                listeningForKey = 0;
            }
        };

        // --- Controls ---
        ImGui::SeparatorText("Controls");
        ImGui::Spacing();
        renderKeyBind("Toggle Effects", "Key to enable/disable all effects", "toggleKey", cfg.toggleKey, 1);
        renderKeyBind("Reload Config", "Key to reload the configuration file", "reloadKey", cfg.reloadKey, 2);
        renderKeyBind("Toggle Overlay", "Key to show/hide this overlay", "overlayKey", cfg.overlayKey, 3);
        if (ImGui::Button("Reload config from disk"))
            emit(ReloadConfig{});
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Re-read the configuration files and rebuild the effect chain.");

        ImGui::Spacing();
        bool blockInput = cfg.overlayBlockInput;
        if (ImGui::Checkbox("Block input while the overlay is open", &blockInput))
            commit("overlayBlockInput", blockInput);
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("When enabled, keyboard and mouse input is captured by the overlay.");
            ImGui::EndTooltip();
        }

        // --- Behaviour ---
        ImGui::Spacing();
        ImGui::SeparatorText("Behaviour");

        bool enableOnLaunch = cfg.enableOnLaunch;
        if (ImGui::Checkbox("Enable effects on launch", &enableOnLaunch))
            commit("enableOnLaunch", enableOnLaunch);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("If enabled, effects are active when the game starts.");

        bool autoApply = cfg.autoApply;
        if (ImGui::Checkbox("Auto-apply changes", &autoApply))
            commit("autoApply", autoApply);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Automatically apply parameter changes after a short delay.");

        if (cfg.autoApply)
        {
            const SettingDesc* delayRow = rangeOf("autoApplyDelay");
            ImGui::Indent();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Delay");
            ImGui::SameLine(160.0f * d);
            ImGui::SetNextItemWidth(160.0f * d);
            int delayVal = cfg.autoApplyDelay;
            if (ImGui::SliderInt("##autoApplyDelay", &delayVal, static_cast<int>(delayRow->min),
                                 static_cast<int>(delayRow->max), "%d ms"))
                emitSetting("autoApplyDelay", delayVal);
            if (ImGui::IsItemDeactivatedAfterEdit())
                emit(SettingsChanged{});
            ImGui::Unindent();
        }

        bool usePortal = cfg.usePortalIntegration;
        if (ImGui::Checkbox("Use XDG Desktop Portal integrations when available", &usePortal))
            commit("usePortalIntegration", usePortal);
        ImGui::Spacing();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Max effects");
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("Maximum number of effects that can be added to the chain.");
            ImGui::EndTooltip();
        }
        ImGui::SameLine(160.0f * d);
        ImGui::SetNextItemWidth(120.0f * d);
        int maxEffectsVal = cfg.maxEffects;
        if (ImGui::InputInt("##maxEffects", &maxEffectsVal, 0, 0))
            emitSetting("maxEffects", maxEffectsVal);
        if (ImGui::IsItemDeactivatedAfterEdit())
            emit(SettingsChanged{});
        ImGui::TextDisabled("Requires restart");

        ImGui::Spacing();
        ImGui::SeparatorText("Appearance");
        bool automaticScale = cfg.uiScale == 0.0f;
        if (ImGui::Checkbox("Automatic UI scale", &automaticScale))
            commit("uiScale", automaticScale ? 0.0f : ImGui::GetFontSize() / 18.0f);
        if (!automaticScale)
        {
            const SettingDesc* scaleRow = rangeOf("uiScale");
            float uiScale = cfg.uiScale;
            ImGui::SetNextItemWidth(240.0f * d);
            if (ImGui::SliderFloat("UI scale", &uiScale, static_cast<float>(scaleRow->min),
                                   static_cast<float>(scaleRow->max), "%.2fx"))
                emitSetting("uiScale", uiScale);
            if (ImGui::IsItemDeactivatedAfterEdit())
                emit(SettingsChanged{});
        }

        ImGui::Spacing();
        bool separateParams = cfg.separateEffectParams;
        if (ImGui::Checkbox("Separate effect parameters into their own section", &separateParams))
            commit("separateEffectParams", separateParams);

        // --- Depth buffer: capture, decoding and selection all live here ---
        ImGui::Spacing();
        renderAdvancedView();

        // --- Developer: pages that live a level deeper, plus layout recovery ---
        ImGui::Spacing();
        ImGui::SeparatorText("Developer");
        if (ImGui::Button("Open diagnostics"))
            openSubView = SubView_Diagnostics;

        ImGui::EndChild();
    }

} // namespace VkHook
