#include "../field_editor.hh"
#include "../../../vendor/imgui/imgui.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>

namespace VkHook
{
    // Robust float field editor with custom value input via right-click
    // Uses deferred modal opening to avoid ImGui nested-popup issues
    class FloatFieldEditor : public FieldEditor
    {
    public:
        bool render(EffectParam& param) override
        {
            auto& p = static_cast<FloatParam&>(param);
            bool changed = false;

            // Generate stable unique IDs using parameter address (avoids label collisions)
            const void* paramAddr = static_cast<const void*>(&param);
            char contextId[64];
            char modalId[64];
            char inputId[64];
            snprintf(contextId, sizeof(contextId), "##flctx_%p", paramAddr);
            snprintf(modalId, sizeof(modalId), "Custom value###flmodal_%p", paramAddr);
            snprintf(inputId, sizeof(inputId), "##flinput_%p", paramAddr);

            // === Render the slider ===
            if (ImGui::SliderFloat(p.label.c_str(), &p.value, p.minValue, p.maxValue))
            {
                const float step = p.step > 0.0f ? p.step : 0.01f;
                p.value = std::round(p.value / step) * step;
                changed = true;
            }

            // === Right-click context menu ===
            if (ImGui::BeginPopupContextItem(contextId))
            {
                if (ImGui::MenuItem("Type in manually..."))
                {
                    // Mark modal as pending for THIS parameter, initialize buffer
                    activeModalParam = paramAddr;
                    snprintf(customValueBuf, sizeof(customValueBuf), "%.6g", p.value);
                    // Don't call OpenPopup here - defer to after context menu closes
                    focusCustomValueInput = true;
                    modalOpenRequested = true;
                }
                if (ImGui::MenuItem("Reset to default"))
                {
                    resetToDefault(param);
                    changed = true;
                }
                ImGui::EndPopup();
            }

            // === Deferred modal opening (must happen outside any popup context) ===
            if (modalOpenRequested && activeModalParam == paramAddr)
            {
                modalOpenRequested = false;
                ImGui::OpenPopup(modalId);
            }

            // === Render custom value popup for THIS parameter only ===
            if (activeModalParam == paramAddr &&
                ImGui::BeginPopupModal(modalId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted(p.label.c_str());
                ImGui::TextDisabled("Current range: [%.4g, %.4g]", p.minValue, p.maxValue);
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

                // Handle submission
                if (submit || ok)
                {
                    float newVal;
                    if (sscanf(customValueBuf, "%f", &newVal) == 1)
                    {
                        p.value = newVal;
                        changed = true;
                    }
                    ImGui::CloseCurrentPopup();
                    activeModalParam = nullptr;  // Clear active state
                }

                // Handle cancellation
                if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape))
                {
                    ImGui::CloseCurrentPopup();
                    activeModalParam = nullptr;  // Clear active state
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
        const void* activeModalParam = nullptr;  // Which param has modal open
        bool modalOpenRequested = false;
        bool focusCustomValueInput = false;
    };

    REGISTER_FIELD_EDITOR(ParamType::Float, FloatFieldEditor)

} // namespace VkHook
