#include "imgui_overlay.hh"
#include "config_serializer.hh"
#include "logger.hh"
#include "overlay/ui_theme.hh"
#include "util.hh"

#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <set>
#include <string>

#include "vendor/imgui/imgui.h"
#include "vendor/imgui/imfilebrowser.h"

namespace VkHook
{
    // Defined in view_shader_test.cpp
    void renderShaderTestResultsUI(
        const std::vector<std::tuple<std::string, std::string, bool, std::string>>& results,
        const std::set<std::string>& depthShaderNames);
}

namespace VkHook
{
    // Static file browser for adding directories
    static ImGui::FileBrowser dirBrowser(
        ImGuiFileBrowserFlags_SelectDirectory |
        ImGuiFileBrowserFlags_HideRegularFiles |
        ImGuiFileBrowserFlags_CloseOnEsc |
        ImGuiFileBrowserFlags_CreateNewDir);

    // Case-insensitive string comparison
    static bool equalsIgnoreCase(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++)
        {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    }

    // Recursively scan directory for Shaders/ and Textures/ subdirectories
    static void scanDirectory(
        const std::filesystem::path& dir,
        std::set<std::string>& shaderPaths,
        std::set<std::string>& texturePaths)
    {
        try
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                dir, std::filesystem::directory_options::skip_permission_denied))
            {
                if (!entry.is_directory())
                    continue;

                std::string dirName = entry.path().filename().string();
                if (equalsIgnoreCase(dirName, "Shaders"))
                    shaderPaths.insert(entry.path().string());
                else if (equalsIgnoreCase(dirName, "Textures"))
                    texturePaths.insert(entry.path().string());
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            Logger::err("Shader Manager: Error scanning " + dir.string() + ": " + e.what());
        }
    }

    void ImGuiOverlay::renderShaderManagerView()
    {
        // Load config on first open
        if (!shaderMgrInitialized)
        {
            ShaderManagerConfig config = ConfigSerializer::loadShaderManagerConfig();
            shaderMgrParentDirs = config.parentDirectories;
            shaderMgrShaderPaths = config.discoveredShaderPaths;
            shaderMgrTexturePaths = config.discoveredTexturePaths;
            shaderMgrInitialized = true;
        }

        // Helper to save config (auto-save on any change)
        auto saveConfig = [&]() {
            ShaderManagerConfig config;
            config.parentDirectories = shaderMgrParentDirs;
            config.discoveredShaderPaths = shaderMgrShaderPaths;
            config.discoveredTexturePaths = shaderMgrTexturePaths;
            ConfigSerializer::saveShaderManagerConfig(config);
            shaderPathsChanged = true;
        };

        ImGui::BeginChild("ShaderMgrContent", ImVec2(0, 0), false);

        UI::TextSupport("Add shader packs and scan them for ReShade effects.");
        ImGui::Spacing();

        // Harvest any completed portal request (non-blocking), then offer Browse.
        {
            FileDialogResult result = FileDialogResult::Cancelled;
            std::string picked;
            if (pollFileDialog(FileDialogKind::OpenDirectory, result, picked))
            {
                if (result == FileDialogResult::Success && !picked.empty())
                {
                    if (std::find(shaderMgrParentDirs.begin(), shaderMgrParentDirs.end(), picked) == shaderMgrParentDirs.end())
                    {
                        shaderMgrParentDirs.push_back(picked);
                        saveConfig();
                    }
                }
                else if (result == FileDialogResult::Unavailable)
                {
                    // No portal: fall back to the in-overlay browser.
                    dirBrowser.SetTitle("Select Parent Directory");
                    const char* home = std::getenv("HOME");
                    dirBrowser.SetPwd(home ? home : "/");
                    dirBrowser.Open();
                }
            }
        }

        // --- Directories ---
        ImGui::SeparatorText("Shader Directories");
        ImGui::TextWrapped("Each folder is scanned for its Shaders/ and Textures/ subfolders.");
        ImGui::Spacing();

        const bool portalPending = fileDialogPending();
        ImGui::BeginDisabled(portalPending);
        if (ImGui::Button(portalPending ? "Opening..." : "Browse..."))
        {
            // If the portal cannot even be started (no GIO, no session bus),
            // open the in-overlay browser right away.
            if (!settingsManager.getUsePortalIntegration()
                || !startOpenDirectoryDialog("Select Parent Directory"))
            {
                dirBrowser.SetTitle("Select Parent Directory");
                const char* home = std::getenv("HOME");
                dirBrowser.SetPwd(home ? home : "/");
                dirBrowser.Open();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Rescan all"))
        {
            std::set<std::string> shaderSet, textureSet;
            for (const auto& parentDir : shaderMgrParentDirs)
            {
                if (std::filesystem::exists(parentDir) && std::filesystem::is_directory(parentDir))
                    scanDirectory(parentDir, shaderSet, textureSet);
            }
            shaderMgrShaderPaths.assign(shaderSet.begin(), shaderSet.end());
            shaderMgrTexturePaths.assign(textureSet.begin(), textureSet.end());
            Logger::info("Shader Manager: Found " + std::to_string(shaderMgrShaderPaths.size()) +
                " shader paths, " + std::to_string(shaderMgrTexturePaths.size()) + " texture paths");
            saveConfig();
        }

        ImGui::Spacing();
        {
            int removeIdx = -1;
            for (size_t i = 0; i < shaderMgrParentDirs.size(); i++)
            {
                ImGui::PushID(static_cast<int>(i));
                ImGui::TextWrapped("%s", shaderMgrParentDirs[i].c_str());
                if (ImGui::Button("Remove directory"))
                    removeIdx = static_cast<int>(i);
                ImGui::Separator();
                ImGui::PopID();
            }
            if (shaderMgrParentDirs.empty())
                UI::TextSupport("No directories added yet.");

            if (removeIdx >= 0)
            {
                shaderMgrParentDirs.erase(shaderMgrParentDirs.begin() + removeIdx);
                saveConfig();
            }
        }

        // --- Discovered paths ---
        ImGui::Spacing();
        ImGui::SeparatorText("Discovered Paths");
        struct PathGroup
        {
            const char* label;
            std::vector<std::string>* paths;
        };
        PathGroup groups[] = {
            { "Shader paths", &shaderMgrShaderPaths },
            { "Texture paths", &shaderMgrTexturePaths },
        };
        for (PathGroup& group : groups)
        {
            ImGui::PushID(group.label);
            if (ImGui::CollapsingHeader(group.label))
            {
                ImGui::TextDisabled("%zu paths", group.paths->size());
                if (group.paths->empty())
                    UI::TextSupport("None yet - press Rescan all.");
                int removeIdx = -1;
                if (ImGui::BeginTable("Paths", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
                {
                    ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed);
                    for (size_t i = 0; i < group.paths->size(); ++i)
                    {
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%s", (*group.paths)[i].c_str());
                        ImGui::TableNextColumn();
                        if (ImGui::Button("Remove"))
                            removeIdx = static_cast<int>(i);
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                if (removeIdx >= 0)
                {
                    group.paths->erase(group.paths->begin() + removeIdx);
                    saveConfig();
                }
            }
            ImGui::PopID();
        }

        // --- Shader test ---
        ImGui::Spacing();
        ImGui::SeparatorText("Shader Test");
        renderShaderTestSection();
        if (shaderTestComplete)
            renderShaderTestResultsUI(shaderTestResults, depthShaders);

        ImGui::EndChild();

        // Display file browser (must be called every frame when open)
        dirBrowser.Display();
        if (dirBrowser.HasSelected())
        {
            std::string selectedPath = dirBrowser.GetSelected().string();
            // Avoid duplicates
            bool exists = false;
            for (const auto& dir : shaderMgrParentDirs)
            {
                if (dir == selectedPath)
                {
                    exists = true;
                    break;
                }
            }
            if (!exists)
            {
                shaderMgrParentDirs.push_back(selectedPath);
                saveConfig();
            }
            dirBrowser.ClearSelected();
        }

    }

} // namespace VkHook
