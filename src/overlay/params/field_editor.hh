#ifndef FIELD_EDITOR_HPP_INCLUDED
#define FIELD_EDITOR_HPP_INCLUDED

#include "effects/params/effect_param.hh"

namespace VkHook
{
    // Renders the editor for a parameter and returns true if its value changed.
    // Numeric types share one implementation (field_editor.cc); any popup state
    // lives in ImGui's per-window storage, so nothing is shared between overlays.
    bool renderFieldEditor(EffectParam& param);

} // namespace VkHook

#endif // FIELD_EDITOR_HPP_INCLUDED
