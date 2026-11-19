#include "overlay_ui.hh"
#include "reshade_parser.hh"
#include "config_serializer.hh"
#include "logger.hh"
#include "overlay/ui_theme.hh"

#include <filesystem>
#include <set>
#include <algorithm>

#ifdef __linux__
#include <malloc.h>  // malloc_trim — reclaim fragmented heap memory
#endif

#include "vendor/imgui/imgui.h"

namespace VkHook
{
    void OverlayUi::startShaderTest()
    {
        // Only an in-flight run blocks a new one: pressing the button again is an
        // explicit "refresh the paths and compile them all over".
        if (shaderTestRunning)
            return;

        // Ensure shader manager paths are loaded
        if (!shaderMgrInitialized)
        {
            ShaderManagerConfig config = ConfigSerializer::loadShaderManagerConfig();
            shaderMgrParentDirs = config.parentDirectories;
            shaderMgrShaderPaths = config.discoveredShaderPaths;
            shaderMgrTexturePaths = config.discoveredTexturePaths;
            shaderMgrInitialized = true;
        }

        // Build test queue from all .fx files in discovered shader paths
        shaderTestQueue.clear();
        shaderTestResults.clear();
        shaderTestCurrentIndex = 0;
        shaderTestComplete = false;
        shaderTestDuplicateCount = 0;
        // the set this run covers; a later availableVersion bump means new shaders
        shaderTestStateVersion = snapshot->effects.availableVersion;

        // Cache include paths once — avoids re-reading shader_manager.conf per shader
        ShaderManagerConfig smConfig = ConfigSerializer::loadShaderManagerConfig();
        shaderTestIncludePaths = smConfig.discoveredShaderPaths;

        std::set<std::string> seenCanonicalPaths;
        for (const auto& shaderPath : shaderMgrShaderPaths)
        {
            try
            {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(
                         shaderPath, std::filesystem::directory_options::skip_permission_denied))
                {
                    if (!entry.is_regular_file())
                        continue;
                    std::string ext = entry.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext != ".fx")
                        continue;

                    // Deduplicate by canonical path — catches symlinks pointing
                    // to the same nix store file across shader packs
                    std::string filePath = entry.path().string();
                    std::string canonical = filePath;
                    try { canonical = std::filesystem::canonical(entry.path()).string(); }
                    catch (...) {}
                    if (seenCanonicalPaths.count(canonical))
                    {
                        shaderTestDuplicateCount++;
                        continue;
                    }
                    seenCanonicalPaths.insert(canonical);

                    std::string effectName = entry.path().stem().string();
                    shaderTestQueue.emplace_back(effectName, filePath);
                }
            }
            catch (const std::filesystem::filesystem_error&)
            {
                // Skip inaccessible directories
            }
        }

        if (!shaderTestQueue.empty())
        {
            shaderTestRunning = true;
            Logger::info("Shader test auto-started: " + std::to_string(shaderTestQueue.size()) + " shaders");
        }
    }

    void OverlayUi::processShaderTest()
    {
        if (!shaderTestRunning)
            return;

        if (shaderTestCurrentIndex < shaderTestQueue.size())
        {
            const auto& [name, path] = shaderTestQueue[shaderTestCurrentIndex];
            ShaderTestResult result = testShaderCompilation(name, path, shaderTestIncludePaths);
            shaderTestResults.emplace_back(result.effectName, result.filePath,
                result.success, result.errorMessage);
            if (result.usesDepth)
                depthShaders.insert(result.effectName);
            shaderTestCurrentIndex++;

            // Reclaim fragmented heap memory every 25 shaders to prevent
            // OOM from accumulating freed-but-unreturned allocations
#ifdef __linux__
            if (shaderTestCurrentIndex % 25 == 0)
                malloc_trim(0);
#endif
        }
        else
        {
            shaderTestRunning = false;
            shaderTestComplete = true;
#ifdef __linux__
            malloc_trim(0);  // Final cleanup after all tests
#endif
            Logger::info("Shader test complete: tested " +
                std::to_string(shaderTestResults.size()) + " shaders");
        }
    }

    void OverlayUi::renderShaderTestSection()
    {
        // The action never greys out on its own: a finished run just changes what
        // it offers, from the first compile-test to a re-run over refreshed paths.
        // Only an in-flight run blocks it.
        if (shaderTestRunning)
        {
            const float progress = shaderTestQueue.empty() ? 1.0f :
                static_cast<float>(shaderTestCurrentIndex) / static_cast<float>(shaderTestQueue.size());
            ImGui::ProgressBar(progress);
            UI::TextSupport(("Testing " + std::to_string(shaderTestCurrentIndex) + "/" +
                             std::to_string(shaderTestQueue.size()) + " shaders...").c_str());
        }
        else
        {
            const bool alreadyTested = shaderTestComplete && snapshot->effects.availableVersion == shaderTestStateVersion;
            const char* label = alreadyTested
                ? "Refresh paths and compile-test again"
                : "Compile-test shaders";
            if (ImGui::Button(label))
                startShaderTest();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(alreadyTested
                    ? "Re-scan every shader path and compile-test them all again."
                    : "Test every .fx shader for compilation errors.");
        }

        // summary line
        if (shaderTestComplete && !shaderTestResults.empty())
        {
            int passCount = 0, failCount = 0;
            for (const auto& [name, path, success, error] : shaderTestResults)
            {
                if (success)
                    passCount++;
                else
                    failCount++;
            }
            const int depthCount = static_cast<int>(depthShaders.size());
            if (failCount == 0)
                ImGui::TextColored(UI::Success(), "All %d passed", passCount);
            else
                ImGui::TextColored(UI::Warning(), "%d passed, %d failed", passCount, failCount);
            if (depthCount > 0)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("\xC2\xB7 %d use depth", depthCount);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("These shaders require depth buffer access.");
            }
            if (shaderTestDuplicateCount > 0)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("\xC2\xB7 %d duplicates skipped", shaderTestDuplicateCount);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Identical files (symlinks or copies) found in multiple shader paths.\nEach unique file is tested exactly once.");
            }
        }

        ImGui::Spacing();
        UI::TextSupport("A clean compile does not guarantee that a shader looks or behaves correctly.");
    }

    // Render detailed test results in collapsible sections
    // Call this separately after the main shader manager content
    void renderShaderTestResults(
        const std::vector<std::tuple<std::string, std::string, bool, std::string>>& results,
        const std::set<std::string>& depthShaderNames)
    {
        if (results.empty())
            return;

        // Group results by compilation status and depth requirements.
        std::vector<size_t> failed, depth, safe;
        for (size_t i = 0; i < results.size(); ++i)
        {
            const auto& [name, path, success, error] = results[i];
            if (!success)
                failed.push_back(i);
            else if (depthShaderNames.count(name))
                depth.push_back(i);
            else
                safe.push_back(i);
        }

        auto renderGroup = [&](const char* label, const std::vector<size_t>& indices,
                               const char* tooltip, ImVec4 nameColor) {
            if (indices.empty())
                return;
            if (!ImGui::CollapsingHeader(label))
                return;
            ImGui::PushID(label);
            ImGui::TextDisabled("%zu shaders", indices.size());
            ImGui::TextWrapped("%s", tooltip);
            for (size_t index : indices)
            {
                const auto& [name, path, success, error] = results[index];
                ImGui::PushID(static_cast<int>(index));
                ImGui::PushStyleColor(ImGuiCol_Text, nameColor);
                const bool expanded = ImGui::TreeNode("Result", "%s%s", name.c_str(),
                                                     error.empty() ? "" : (success ? " (warnings)" : " (errors)"));
                ImGui::PopStyleColor();
                if (expanded)
                {
                    ImGui::TextWrapped("%s", path.c_str());
                    if (!error.empty())
                    {
                        ImGui::PushTextWrapPos();
                        ImGui::TextColored(UI::Attention(), "%s", error.c_str());
                        ImGui::PopTextWrapPos();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        };

        renderGroup("Failed shaders", failed, "These shaders did not compile.", UI::Error());
        renderGroup("Depth shaders", depth, "These shaders require depth buffer access.", UI::Secondary());
        renderGroup("Safe shaders", safe, "Compiled successfully and need no depth buffer.", UI::Success());
    }

} // namespace VkHook
