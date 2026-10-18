#include "../field_editor.hh"
#include "../../../vendor/imgui/imgui.h"
#include <cstring>
#include <cstdio>
#include <string>

namespace VKIntox
{
    // Robust int field editor with custom value input via right-click
    // Supports both slider mode and combo/dropdown mode
    class IntFieldEditor : public FieldEditor
    {
    public:
        bool render(EffectParam& param) override
        {
            auto& p = static_cast<IntParam&>(param);
            bool changed = false;

            // Generate stable unique IDs using parameter address
            const void* paramAddr = static_cast<const void*>(&param);
            char contextId[64];
            char modalId[64];
            char inputId[64];
            snprintf(contextId, sizeof(contextId), "##intctx_%p", paramAddr);
            snprintf(modalId, sizeof(modalId), "Custom value###intmodal_%p", paramAddr);
            snprintf(inputId, sizeof(inputId), "##intinput_%p", paramAddr);

            // === Render the control (combo or slider) ===
            if (!p.items.empty())
            {
                // Combo box mode for enumerated values
                const int itemCount = static_cast<int>(p.items.size());
                const int previewIdx = (p.value >= 0 && p.value < itemCount) ? p.value : 0;
                if (ImGui::BeginCombo(p.label.c_str(), p.items[previewIdx].c_str()))
                {
                    for (int i = 0; i < itemCount; ++i)
                    {
                        if (ImGui::Selectable(p.items[i].c_str(), i == p.value))
                        {
                            p.value = i;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            else
            {
                // Slider mode for numeric range
                if (ImGui::SliderInt(p.label.c_str(), &p.value, p.minValue, p.maxValue))
                {
                    if (p.step > 0.0f)
                    {
                        int step = static_cast<int>(p.step);
                        if (step > 0)
                            p.value = (p.value / step) * step;
                    }
                    changed = true;
                }
            }

            // === Right-click context menu ===
            if (ImGui::BeginPopupContextItem(contextId))
            {
                if (!p.items.empty())
                {
                    // Combo mode: only show reset option
                    if (ImGui::MenuItem("Reset to default"))
                    {
                        resetToDefault(param);
                        changed = true;
                    }
                }
                else
                {
                    // Slider mode: show custom value AND reset
                    if (ImGui::MenuItem("Type in manually..."))
                    {
                        activeModalParam = paramAddr;
                        snprintf(customValueBuf, sizeof(customValueBuf), "%d", p.value);
                        focusCustomValueInput = true;
                        modalOpenRequested = true;
                    }
                    if (ImGui::MenuItem("Reset to default"))
                    {
                        resetToDefault(param);
                        changed = true;
                    }
                }
                ImGui::EndPopup();
            }

            // === Deferred modal opening (outside popup context) ===
            if (modalOpenRequested && activeModalParam == paramAddr)
            {
                modalOpenRequested = false;
                ImGui::OpenPopup(modalId);
            }

            // === Render custom value popup (only for slider mode) ===
            if (p.items.empty() &&
                activeModalParam == paramAddr &&
                ImGui::BeginPopupModal(modalId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted(p.label.c_str());
                ImGui::TextDisabled("Current range: [%d, %d]", p.minValue, p.maxValue);
                ImGui::TextDisabled("(values outside range are allowed)");
                ImGui::Spacing();

                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
                if (focusCustomValueInput)
                {
                    ImGui::SetKeyboardFocusHere();
                    focusCustomValueInput = false;
                }
                bool submit = ImGui::InputText(inputId, customValueBuf,
                    sizeof(customValueBuf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

                ImGui::Spacing();
                const bool cancel = ImGui::Button("Cancel");
                ImGui::SameLine();
                const bool ok = ImGui::Button("OK");

                if (submit || ok)
                {
                    int newVal;
                    if (sscanf(customValueBuf, "%d", &newVal) == 1)
                    {
                        p.value = newVal;
                        changed = true;
                    }
                    ImGui::CloseCurrentPopup();
                    activeModalParam = nullptr;
                }

                if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    ImGui::CloseCurrentPopup();
                    activeModalParam = nullptr;
                }

                ImGui::EndPopup();
            }

            return changed;
        }

        void resetToDefault(EffectParam& param) override
        {
            param.resetToDefault();
        }

    private:
        static constexpr size_t BUF_SIZE = 128;
        char customValueBuf[BUF_SIZE] = "";
        const void* activeModalParam = nullptr;
        bool modalOpenRequested = false;
        bool focusCustomValueInput = false;
    };

    REGISTER_FIELD_EDITOR(ParamType::Int, IntFieldEditor)

} // namespace VKIntox
