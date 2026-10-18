#include "../field_editor.hh"
#include "../../../vendor/imgui/imgui.h"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>

namespace VKIntox
{
    // Robust float vector (vec2/3/4) field editor with custom value input via right-click
    class FloatVecFieldEditor : public FieldEditor
    {
    public:
        bool render(EffectParam& param) override
        {
            auto& p = static_cast<FloatVecParam&>(param);
            bool changed = false;

            // Generate stable unique IDs using parameter address
            const void* paramAddr = static_cast<const void*>(&param);
            char contextId[64];
            char modalId[64];
            char inputId[64];
            snprintf(contextId, sizeof(contextId), "##fvcctx_%p", paramAddr);
            snprintf(modalId, sizeof(modalId), "Custom values###fvcmodal_%p", paramAddr);
            snprintf(inputId, sizeof(inputId), "##fvcinput_%p", paramAddr);

            // === Render the vector slider ===
            switch (p.componentCount)
            {
                case 2:
                    changed = ImGui::SliderFloat2(p.label.c_str(), p.value, 
                        p.minValue[0], p.maxValue[0]);
                    break;
                case 3:
                    changed = ImGui::SliderFloat3(p.label.c_str(), p.value,
                        p.minValue[0], p.maxValue[0]);
                    break;
                case 4:
                    changed = ImGui::SliderFloat4(p.label.c_str(), p.value,
                        p.minValue[0], p.maxValue[0]);
                    break;
                default:
                    break;
            }

            if (changed)
            {
                const float step = p.step > 0.0f ? p.step : 0.01f;
                for (uint32_t i = 0; i < p.componentCount; i++)
                    p.value[i] = std::round(p.value[i] / step) * step;
            }

            // === Right-click context menu ===
            if (ImGui::BeginPopupContextItem(contextId))
            {
                if (ImGui::MenuItem("Type in manually..."))
                {
                    activeModalParam = paramAddr;
                    // Format current values into buffer based on component count
                    formatValues(p);
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

            // === Deferred modal opening ===
            if (modalOpenRequested && activeModalParam == paramAddr)
            {
                modalOpenRequested = false;
                ImGui::OpenPopup(modalId);
            }

            // === Render custom value popup ===
            if (activeModalParam == paramAddr &&
                ImGui::BeginPopupModal(modalId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::TextUnformatted(p.label.c_str());
                ImGui::TextDisabled("Components: %u (comma-separated)",
                    static_cast<unsigned int>(p.componentCount));
                ImGui::TextDisabled("(values outside range are allowed)");
                ImGui::Spacing();

                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
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
                    changed |= parseAndApply(p);
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
        // Format current vector values into the buffer as comma-separated
        void formatValues(const FloatVecParam& p)
        {
            switch (p.componentCount)
            {
                case 2:
                    snprintf(customValueBuf, sizeof(customValueBuf),
                        "%.6g, %.6g", p.value[0], p.value[1]);
                    break;
                case 3:
                    snprintf(customValueBuf, sizeof(customValueBuf),
                        "%.6g, %.6g, %.6g", p.value[0], p.value[1], p.value[2]);
                    break;
                case 4:
                    snprintf(customValueBuf, sizeof(customValueBuf),
                        "%.6g, %.6g, %.6g, %.6g", 
                        p.value[0], p.value[1], p.value[2], p.value[3]);
                    break;
                default:
                    snprintf(customValueBuf, sizeof(customValueBuf), "%.6g", p.value[0]);
                    break;
            }
        }

        // Parse comma-separated values and apply to parameter
        bool parseAndApply(FloatVecParam& p)
        {
            float vals[4] = { p.value[0], p.value[1], p.value[2], p.value[3] };
            int count = sscanf(customValueBuf, "%f, %f, %f, %f",
                &vals[0], &vals[1], &vals[2], &vals[3]);

            if (count >= static_cast<int>(p.componentCount))
            {
                for (uint32_t i = 0; i < p.componentCount; i++)
                    p.value[i] = vals[i];
                return true;
            }
            return false;
        }

        static constexpr size_t BUF_SIZE = 256;
        char customValueBuf[BUF_SIZE] = "";
        const void* activeModalParam = nullptr;
        bool modalOpenRequested = false;
        bool focusCustomValueInput = false;
    };

    REGISTER_FIELD_EDITOR(ParamType::FloatVec, FloatVecFieldEditor)

} // namespace VKIntox
