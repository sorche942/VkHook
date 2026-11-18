#include "imgui_overlay.hh"
#include "config_serializer.hh"
#include "overlay/ui_theme.hh"

#include <cstring>

#include "vendor/imgui/imgui.h"

namespace VkHook
{
    void ImGuiOverlay::renderConfigManagerView()
    {
        ImGui::SeparatorText("Manage Configs");
        UI::TextSupport("Click a config to load it. The default one is applied on launch.");
        ImGui::Spacing();

        // Refresh config list and get current default
        if (configListRefreshPending)
        {
            configList = ConfigSerializer::listConfigs();
            configListDefault = ConfigSerializer::getDefaultConfig();
            configListRefreshPending = false;
        }

        const float footerH = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("ConfigList", ImVec2(0, -footerH), false);
        for (size_t i = 0; i < configList.size(); i++)
        {
            ImGui::PushID(static_cast<int>(i));
            const std::string& cfg = configList[i];
            const bool isDefault = (cfg == configListDefault);

            const bool load = ImGui::Selectable(cfg.c_str());
            if (isDefault)
                ImGui::TextDisabled("Default on launch");
            ImGui::BeginDisabled(isDefault);
            if (ImGui::Button("Set default"))
            {
                if (ConfigSerializer::setDefaultConfig(cfg))
                    configListDefault = cfg;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Delete"))
            {
                ConfigSerializer::deleteConfig(cfg);
                configListRefreshPending = true;
                ImGui::PopID();
                continue;
            }
            ImGui::Separator();

            if (load)
            {
                // Signal to vkhook.cpp to load this config
                pendingConfigPath = ConfigSerializer::getConfigsDir() + "/" + cfg + ".conf";
                strncpy(saveConfigName, cfg.c_str(), sizeof(saveConfigName) - 1);
                applyRequested = true;
                inConfigManageMode = false;
            }

            ImGui::PopID();
        }
        if (configList.empty())
        {
            UI::TextSupport("No saved configs yet. Use Save As in the config bar to create one.");
        }
        ImGui::EndChild();

        if (ImGui::Button("Back"))
        {
            inConfigManageMode = false;
        }
    }

} // namespace VkHook
