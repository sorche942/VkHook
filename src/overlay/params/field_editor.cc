#include "field_editor.hh"

#include "field_parse.hh"

#include "../../vendor/imgui/imgui.h"
#include "../../vendor/imgui/imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace VkHook
{
    namespace
    {
        constexpr size_t kTextBufferSize = 128;

        template<typename T>
        struct NumericTraits;

        template<>
        struct NumericTraits<float>
        {
            static constexpr ImGuiDataType dataType = ImGuiDataType_Float;
            static constexpr const char* format = "%.3f";
            static constexpr const char* rangeFormat = "[%.4g, %.4g]";
            static constexpr const char* hint = "numbers";
            static void quantize(float& value, float step)
            {
                const float s = step > 0.0f ? step : 0.01f;
                value = std::round(value / s) * s;
            }
        };

        template<>
        struct NumericTraits<int>
        {
            static constexpr ImGuiDataType dataType = ImGuiDataType_S32;
            static constexpr const char* format = "%d";
            static constexpr const char* rangeFormat = "[%d, %d]";
            static constexpr const char* hint = "integers";
            static void quantize(int& value, float step)
            {
                const int s = static_cast<int>(step);
                if (s > 0)
                    value = (value / s) * s;
            }
        };

        template<>
        struct NumericTraits<uint32_t>
        {
            static constexpr ImGuiDataType dataType = ImGuiDataType_U32;
            static constexpr const char* format = "%u";
            static constexpr const char* rangeFormat = "[%u, %u]";
            static constexpr const char* hint = "unsigned integers";
            static void quantize(uint32_t& value, float step)
            {
                const uint32_t s = static_cast<uint32_t>(step);
                if (s > 0)
                    value = (value / s) * s;
            }
        };

        // Popup state: a "requested" flag and the text buffer, kept in the
        // storage of the window the editor is drawn in and keyed off the current
        // ID stack, so every parameter (and every ImGui context) has its own.
        struct PopupKeys
        {
            ImGuiID request = ImGui::GetID("##numeric_request");
            ImGuiID buffer = ImGui::GetID("##numeric_buffer");
            ImGuiID focus = ImGui::GetID("##numeric_focus");
            ImGuiID invalid = ImGui::GetID("##numeric_invalid");
        };

        // The numeric editor for 1..4 components of T. Each component carries
        // its own range; `values`, `mins` and `maxs` are arrays of `count`.
        template<typename T>
        bool renderNumeric(EffectParam& param, T* values, const T* mins, const T* maxs, uint32_t count, float step)
        {
            using Traits = NumericTraits<T>;
            bool changed = false;
            ImGuiStorage* storage = ImGui::GetStateStorage();
            const PopupKeys keys;

            // --- the control: one slider per component, each with its own range ---
            const char* label = param.label.c_str();
            if (count == 1)
            {
                changed = ImGui::SliderScalar(label, Traits::dataType, &values[0], &mins[0], &maxs[0], Traits::format);
            }
            else
            {
                ImGui::BeginGroup();
                ImGui::PushID(label);
                ImGui::PushMultiItemsWidths(static_cast<int>(count), ImGui::CalcItemWidth());
                for (uint32_t i = 0; i < count; ++i)
                {
                    if (i > 0)
                        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                    ImGui::PushID(static_cast<int>(i));
                    changed |= ImGui::SliderScalar("##component", Traits::dataType, &values[i], &mins[i], &maxs[i],
                                                   Traits::format);
                    ImGui::PopID();
                    ImGui::PopItemWidth();
                }
                ImGui::PopID();
                const char* labelEnd = ImGui::FindRenderedTextEnd(label);
                if (label != labelEnd)
                {
                    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                    ImGui::TextEx(label, labelEnd);
                }
                ImGui::EndGroup();
            }
            if (changed)
            {
                for (uint32_t i = 0; i < count; ++i)
                    Traits::quantize(values[i], step);
            }

            // --- right-click: type a value, or reset ---
            if (ImGui::BeginPopupContextItem("##numeric_context"))
            {
                if (ImGui::MenuItem("Type in manually..."))
                {
                    char* buffer = static_cast<char*>(storage->GetVoidPtr(keys.buffer));
                    if (!buffer)
                    {
                        buffer = static_cast<char*>(ImGui::MemAlloc(kTextBufferSize));
                        storage->SetVoidPtr(keys.buffer, buffer);
                    }
                    const std::string text = FieldParse::formatComponents(values, count);
                    std::snprintf(buffer, kTextBufferSize, "%s", text.c_str());
                    storage->SetBool(keys.focus, true);
                    storage->SetBool(keys.invalid, false);
                    // opened below, outside this popup's scope
                    storage->SetBool(keys.request, true);
                }
                if (ImGui::MenuItem("Reset to default"))
                {
                    param.resetToDefault();
                    changed = true;
                }
                ImGui::EndPopup();
            }

            static constexpr const char* modalName = "Custom value###numeric_modal";
            if (storage->GetBool(keys.request, false))
            {
                storage->SetBool(keys.request, false);
                ImGui::OpenPopup(modalName);
            }

            char* buffer = static_cast<char*>(storage->GetVoidPtr(keys.buffer));
            if (buffer && ImGui::BeginPopupModal(modalName, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            {
                auto closeEditor = [&] {
                    ImGui::CloseCurrentPopup();
                    ImGui::MemFree(buffer);
                    storage->SetVoidPtr(keys.buffer, nullptr);
                };

                ImGui::TextUnformatted(label);
                if (count == 1)
                {
                    char range[96];
                    // Variadic promotion makes float→double for %g; int/uint32_t match %d/%u as-is.
                    std::snprintf(range, sizeof(range), Traits::rangeFormat, mins[0], maxs[0]);
                    ImGui::TextDisabled("Current range: %s", range);
                }
                else
                    ImGui::TextDisabled("%u components (comma-separated %s)", count, Traits::hint);
                ImGui::TextDisabled("(values outside range are allowed)");
                ImGui::Spacing();

                ImGui::SetNextItemWidth(ImGui::GetFontSize() * (count == 1 ? 20.0f : 24.0f));
                if (storage->GetBool(keys.focus, false))
                {
                    ImGui::SetKeyboardFocusHere();
                    storage->SetBool(keys.focus, false);
                }
                const bool submit = ImGui::InputText("##numeric_input", buffer, kTextBufferSize,
                                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if (storage->GetBool(keys.invalid, false))
                    ImGui::TextDisabled("Not a valid value");

                ImGui::Spacing();
                const bool cancel = ImGui::Button("Cancel");
                ImGui::SameLine();
                const bool ok = ImGui::Button("OK");

                if (submit || ok)
                {
                    if (FieldParse::parseComponents<T>(buffer, values, count))
                    {
                        changed = true;
                        closeEditor();
                    }
                    else
                        storage->SetBool(keys.invalid, true);
                }
                else if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape))
                    closeEditor();

                ImGui::EndPopup();
            }

            return changed;
        }

        bool renderInt(IntParam& p)
        {
            if (p.items.empty())
                return renderNumeric<int>(p, &p.value, &p.minValue, &p.maxValue, 1, p.step);

            // enumerated values: a combo, with only "reset" in the context menu
            bool changed = false;
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
            if (ImGui::BeginPopupContextItem("##enum_context"))
            {
                if (ImGui::MenuItem("Reset to default"))
                {
                    p.resetToDefault();
                    changed = true;
                }
                ImGui::EndPopup();
            }
            return changed;
        }
    } // namespace

    bool renderFieldEditor(EffectParam& param)
    {
        bool changed = false;
        switch (param.getType())
        {
            case ParamType::Float:
            {
                auto& p = static_cast<FloatParam&>(param);
                changed = renderNumeric<float>(p, &p.value, &p.minValue, &p.maxValue, 1, p.step);
                break;
            }
            case ParamType::FloatVec:
            {
                auto& p = static_cast<FloatVecParam&>(param);
                changed = renderNumeric<float>(p, p.value, p.minValue, p.maxValue, std::clamp(p.componentCount, 2u, 4u), p.step);
                break;
            }
            case ParamType::Int:
                changed = renderInt(static_cast<IntParam&>(param));
                break;
            case ParamType::IntVec:
            {
                auto& p = static_cast<IntVecParam&>(param);
                changed = renderNumeric<int>(p, p.value, p.minValue, p.maxValue, std::clamp(p.componentCount, 2u, 4u), p.step);
                break;
            }
            case ParamType::Uint:
            {
                auto& p = static_cast<UintParam&>(param);
                changed = renderNumeric<uint32_t>(p, &p.value, &p.minValue, &p.maxValue, 1, p.step);
                break;
            }
            case ParamType::UintVec:
            {
                auto& p = static_cast<UintVecParam&>(param);
                changed = renderNumeric<uint32_t>(p, p.value, p.minValue, p.maxValue, std::clamp(p.componentCount, 2u, 4u), p.step);
                break;
            }
            case ParamType::Bool:
                changed = ImGui::Checkbox(param.label.c_str(), &static_cast<BoolParam&>(param).value);
                break;
        }

        if (!param.tooltip.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", param.tooltip.c_str());

        return changed;
    }

} // namespace VkHook
