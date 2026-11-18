#include "../field_editor.hh"
#include "../../../vendor/imgui/imgui.h"
#include <cstring>
#include <cstdio>
#include <string>

namespace VkHook
{
    // Robust unsigned int vector (uvec2/3/4) field editor with custom value input via right-click
    class UintVecFieldEditor : public FieldEditor
    {
    public:
        bool render(EffectParam& param) override
        {
            auto& p = static_cast<UintVecParam&>(param);
            bool changed = false;

            // Generate stable unique IDs using parameter address
            const void* paramAddr = static_cast<const void*>(&param);
            char contextId[64];
            char modalId[64];
            char inputId[64];
            snprintf(contextId, sizeof(contextId), "##uvcctx_%p", paramAddr);
            snprintf(modalId, sizeof(modalId), "Custom values###uvcmodal_%p", paramAddr);
            snprintf(inputId, sizeof(inputId), "##uvcinput_%p", paramAddr);

            // === Render the vector slider (using SliderScalarN for unsigned) ===
            if (ImGui::SliderScalarN(p.label.c_str(), ImGuiDataType_U32,
                p.value, p.componentCount, &p.minValue[0], &p.maxValue[0]))
            {
                if (p.step > 0.0f)
                {
                    uint32_t step = static_cast<uint32_t>(p.step);
                    if (step > 0)
                    {
                        for (uint32_t i = 0; i < p.componentCount; i++)
                            p.value[i] = (p.value[i] / step) * step;
                    }
                }
                changed = true;
            }

            // === Right-click context menu ===
            if (ImGui::BeginPopupContextItem(contextId))
            {
                if (ImGui::MenuItem("Type in manually..."))
                {
                    activeModalParam = paramAddr;
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
                ImGui::TextDisabled("Components: %u (comma-separated unsigned integers)",
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
        void formatValues(const UintVecParam& p)
        {
            switch (p.componentCount)
            {
                case 2:
                    snprintf(customValueBuf, sizeof(customValueBuf),
                        "%u, %u", p.value[0], p.value[1]);
                    break;
                case 3:
                    snprintf(customValueBuf, sizeof(customValueBuf),
                        "%u, %u, %u", p.value[0], p.value[1], p.value[2]);
                    break;
                case 4:
                    snprintf(customValueBuf, sizeof(customValueBuf),
                        "%u, %u, %u, %u",
                        p.value[0], p.value[1], p.value[2], p.value[3]);
                    break;
                default:
                    snprintf(customValueBuf, sizeof(customValueBuf), "%u", p.value[0]);
                    break;
            }
        }

        bool parseAndApply(UintVecParam& p)
        {
            unsigned int vals[4] = { p.value[0], p.value[1], p.value[2], p.value[3] };
            int count = sscanf(customValueBuf, "%u, %u, %u, %u",
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

    REGISTER_FIELD_EDITOR(ParamType::UintVec, UintVecFieldEditor)

} // namespace VkHook
