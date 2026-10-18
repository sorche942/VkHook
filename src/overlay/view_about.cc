#include "imgui_overlay.hh"
#include "version.hh"

#include "vendor/imgui/imgui.h"

namespace VKIntox
{
    namespace
    {
        // compiled in from the top-level VERSION file, so the label and the
        // footer agree and neither depends on a file the installer may not have
        // written.
        const char* runtimeVersion()
        {
            return VKINTOX_VERSION;
        }
    }

    void ImGuiOverlay::renderAboutView()
    {
        ImGui::BeginChild("about_content");
        ImGui::SeparatorText("VKIntox");
        ImGui::TextWrapped("ReShade-like Vulkan post processing layer, made Linux-first.");
        ImGui::TextDisabled("Version %s - beta", runtimeVersion());
        ImGui::TextWrapped("The overlay uses standard Dear ImGui controls.");
        ImGui::Spacing();
        ImGui::SeparatorText("Credits");
        auto credit = [](const char* what, const char* handle, const char* url) {
            ImGui::TextUnformatted(what);
            ImGui::SameLine();
            ImGui::TextLinkOpenURL(handle, url);
        };
        credit("VKIntox by", "@buwryme", "https://github.com/buwryme");
        credit("vkShade by", "@slobodaapl", "https://github.com/slobodaapl");
        credit("vkBasalt by", "@DadSchoorse", "https://github.com/DadSchoorse/vkBasalt");
        credit("Overlay fork by", "@Boux", "https://github.com/Boux/vkBasalt_overlay");
        credit("Wayland overlay by", "@Daaboulex", "https://github.com/Daaboulex/vkBasalt_overlay_wayland");
        credit("ReShade FX support by", "@crosire", "https://github.com/crosire/reshade");
        credit("Dear ImGui by", "@ocornut", "https://github.com/ocornut/imgui");
        ImGui::Spacing();
        ImGui::TextDisabled("VKIntox by buwryme  \xC2\xB7  version %s", runtimeVersion());
        ImGui::TextDisabled("Report issues:");
        ImGui::SameLine();
        ImGui::TextLinkOpenURL("github.com/buwryme/VKIntox/issues", "https://github.com/buwryme/VKIntox/issues");
        ImGui::EndChild();
    }

} // namespace VKIntox
