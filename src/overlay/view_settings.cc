#include "imgui_overlay.hh"
#include "settings_manager.hh"
#include "logger.hh"
#include "overlay/ui_theme.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>

#include "vendor/imgui/imgui.h"

namespace VkHook
{
    void ImGuiOverlay::renderSettingsView(const KeyboardState& keyboard)
    {
        // Helper to save settings to file
        auto saveSettings = [&]() {
            settingsManager.save();
            settingsSaved = true;
        };

        ImGui::BeginChild("SettingsContent", ImVec2(0, 0), false);
        const float d = ImGui::GetFontSize() / 16.0f;

        // Helper lambda to render a keybind button.
        auto renderKeyBind = [&](const char* label, const char* tooltip,
                                 const std::string& currentKey,
                                 std::function<void(const std::string&)> setter,
                                 int bindingId) {
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
                setter(keyboard.lastKeyName);
                listeningForKey = 0;
                saveSettings();
            }
        };

        // --- Controls ---
        ImGui::SeparatorText("Controls");
        ImGui::Spacing();
        renderKeyBind("Toggle Effects", "Key to enable/disable all effects",
                      settingsManager.getToggleKey(),
                      [](const std::string& key) { settingsManager.setToggleKey(key); }, 1);
        renderKeyBind("Reload Config", "Key to reload the configuration file",
                      settingsManager.getReloadKey(),
                      [](const std::string& key) { settingsManager.setReloadKey(key); }, 2);
        renderKeyBind("Toggle Overlay", "Key to show/hide this overlay",
                      settingsManager.getOverlayKey(),
                      [](const std::string& key) { settingsManager.setOverlayKey(key); }, 3);

        ImGui::Spacing();
        bool blockInput = settingsManager.getOverlayBlockInput();
        if (ImGui::Checkbox("Block input while the overlay is open", &blockInput))
        {
            settingsManager.setOverlayBlockInput(blockInput);
            saveSettings();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("When enabled, keyboard and mouse input is captured by the overlay.");
            ImGui::EndTooltip();
        }

        // --- Behaviour ---
        ImGui::Spacing();
        ImGui::SeparatorText("Behaviour");

        bool enableOnLaunch = settingsManager.getEnableOnLaunch();
        if (ImGui::Checkbox("Enable effects on launch", &enableOnLaunch))
        {
            settingsManager.setEnableOnLaunch(enableOnLaunch);
            saveSettings();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("If enabled, effects are active when the game starts.");

        bool autoApply = settingsManager.getAutoApply();
        if (ImGui::Checkbox("Auto-apply changes", &autoApply))
        {
            settingsManager.setAutoApply(autoApply);
            saveSettings();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Automatically apply parameter changes after a short delay.");

        if (autoApply)
        {
            ImGui::Indent();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Delay");
            ImGui::SameLine(160.0f * d);
            ImGui::SetNextItemWidth(160.0f * d);
            int delayVal = settingsManager.getAutoApplyDelay();
            if (ImGui::SliderInt("##autoApplyDelay", &delayVal, 20, 1000, "%d ms"))
                settingsManager.setAutoApplyDelay(delayVal);
            if (ImGui::IsItemDeactivatedAfterEdit())
                saveSettings();
            ImGui::Unindent();
        }

        bool usePortal = settingsManager.getUsePortalIntegration();
        if (ImGui::Checkbox("Use XDG Desktop Portal integrations when available", &usePortal))
        {
            settingsManager.setUsePortalIntegration(usePortal);
            saveSettings();
        }
        ImGui::Spacing();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Max effects");
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("Maximum number of effects that can be active simultaneously.");
            ImGui::TextColored(UI::Warning(), "High values use significant VRAM.");
            ImGui::EndTooltip();
        }
        ImGui::SameLine(160.0f * d);
        ImGui::SetNextItemWidth(120.0f * d);
        int maxEffectsVal = settingsManager.getMaxEffects();
        if (ImGui::InputInt("##maxEffects", &maxEffectsVal, 0, 0))
        {
            maxEffectsVal = std::clamp(maxEffectsVal, 1, 200);
            settingsManager.setMaxEffects(maxEffectsVal);
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
            saveSettings();
        float bytesPerSlot = 2.0f * currentWidth * currentHeight * 4.0f;
        int estimatedVramMB = static_cast<int>((maxEffectsVal * bytesPerSlot) / (1024.0f * 1024.0f));
        ImGui::SameLine();
        if (maxEffectsVal > 20)
            ImGui::TextColored(UI::Attention(), "~%d MB @ %ux%u", estimatedVramMB, currentWidth, currentHeight);
        else
            ImGui::TextDisabled("~%d MB @ %ux%u", estimatedVramMB, currentWidth, currentHeight);
        ImGui::TextDisabled("Requires restart");

        ImGui::Spacing();
        ImGui::SeparatorText("Appearance");
        float uiScale = settingsManager.getUiScale();
        bool automaticScale = uiScale == 0.0f;
        if (ImGui::Checkbox("Automatic UI scale", &automaticScale))
        {
            settingsManager.setUiScale(automaticScale ? 0.0f : ImGui::GetFontSize() / 18.0f);
            saveSettings();
        }
        if (!automaticScale)
        {
            uiScale = settingsManager.getUiScale();
            ImGui::SetNextItemWidth(240.0f * d);
            if (ImGui::SliderFloat("UI scale", &uiScale, 0.75f, 3.0f, "%.2fx"))
                settingsManager.setUiScale(uiScale);
            if (ImGui::IsItemDeactivatedAfterEdit())
                saveSettings();
        }

        ImGui::Spacing();
        bool separateParams = settingsManager.getSeparateEffectParams();
        if (ImGui::Checkbox("Separate effect parameters into their own section", &separateParams))
        {
            settingsManager.setSeparateEffectParams(separateParams);
            saveSettings();
        }


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
