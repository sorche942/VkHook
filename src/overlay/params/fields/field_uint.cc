#include "../field_editor.hh"
#include "../../../vendor/imgui/imgui.h"
#include <cstring>
#include <cstdio>
#include <string>

namespace VkHook
{
    // Robust unsigned int field editor with custom value input via right-click
    class UintFieldEditor : public FieldEditor
    {
    public:
        bool render(EffectParam& param) override
        {
            auto& p = static_cast<UintParam&>(param);
            bool changed = false;

            // Generate stable unique IDs using parameter address
            const void* paramAddr = static_cast<const void*>(&param);
            char contextId[64];
            char modalId[64];
            char inputId[64];
            snprintf(contextId, sizeof(contextId), "##uintctx_%p", paramAddr);
            snprintf(modalId, sizeof(modalId), "Custom value###uintmodal_%p", paramAddr);
            snprintf(inputId, sizeof(inputId), "##uintinput_%p", paramAddr);

            // === Render the slider (using SliderScalar for unsigned) ===
            if (ImGui::SliderScalar(p.label.c_str(), ImGuiDataType_U32, 
                &p.value, &p.minValue, &p.maxValue))
            {
                if (p.step > 0.0f)
                {
                    uint32_t step = static_cast<uint32_t>(p.step);
                    if (step > 0)
                        p.value = (p.value / step) * step;
                }
                changed = true;
            }

            // === Right-click context menu ===
            if (ImGui::BeginPopupContextItem(contextId))
            {
                if (ImGui::MenuItem("Type in manually..."))
                {
                    activeModalParam = paramAddr;
                    snprintf(customValueBuf, sizeof(customValueBuf), "%u", p.value);
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
                ImGui::TextDisabled("Current range: [%u, %u]", p.minValue, p.maxValue);
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
                    unsigned int newVal;
                    if (sscanf(customValueBuf, "%u", &newVal) == 1)
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

    REGISTER_FIELD_EDITOR(ParamType::Uint, UintFieldEditor)

} // namespace VkHook
