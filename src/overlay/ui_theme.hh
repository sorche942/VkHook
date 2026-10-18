#pragma once

#include "vendor/imgui/imgui.h"
#include "logger.hh"

namespace VKIntox::UI
{
    inline ImVec4 Primary() { return ImGui::GetStyleColorVec4(ImGuiCol_CheckMark); }
    inline ImVec4 Secondary() { return ImGui::GetStyleColorVec4(ImGuiCol_SliderGrab); }
    inline ImVec4 Tertiary() { return ImGui::GetStyleColorVec4(ImGuiCol_PlotLines); }
    inline ImVec4 Error() { return ImVec4(1.0f, 0.40f, 0.40f, 1.0f); }
    inline ImVec4 OnError() { return ImGui::GetStyleColorVec4(ImGuiCol_Text); }
    inline ImVec4 Outline() { return ImGui::GetStyleColorVec4(ImGuiCol_Border); }
    inline ImVec4 OnSurface() { return ImGui::GetStyleColorVec4(ImGuiCol_Text); }
    inline ImVec4 Muted() { return ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled); }
    inline ImVec4 Container() { return ImGui::GetStyleColorVec4(ImGuiCol_FrameBg); }
    inline ImVec4 ContainerLow() { return ImGui::GetStyleColorVec4(ImGuiCol_ChildBg); }
    inline ImVec4 Surface() { return ImGui::GetStyleColorVec4(ImGuiCol_WindowBg); }
    inline ImVec4 Success() { return ImVec4(0.40f, 0.85f, 0.50f, 1.0f); }
    inline ImVec4 Warning() { return ImVec4(1.0f, 0.78f, 0.30f, 1.0f); }
    inline ImVec4 Attention() { return Warning(); }

    inline ImVec4 LogLevelColor(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Error: return Error();
        case LogLevel::Warn: return Warning();
        default: return Primary();
        }
    }

    inline void TextSupport(const char* text) { ImGui::TextDisabled("%s", text); }
}
