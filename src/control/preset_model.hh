#pragma once

// The shader-preset model: an ordered stack of effect instances and the pure
// functions that read it from / write it to ReShade-compatible preset text.
//
// An instance is one TECHNIQUE of a shader file (see effects/effect_id.hh), so
// a file with three techniques contributes up to three instances.
//
// A preset is two files. The `.ini` is a ReShade preset: `Techniques=` is
// exactly the enabled instances (`technique@file.fx`), `TechniqueSorting=`
// every instance in stack order, and one `[file.fx]` section per shader file
// (written from the file's last enabled instance). The hidden sidecar carries
// what ReShade has no place for: the exact ordered stack (`[VKHOOK] Effects=`,
// each item an EffectId text, `*` marks a disabled instance) and the values of
// instances the shared `.ini` section cannot describe (disabled instances,
// extra copies and instances that disagree with their file's section), under
// `[<EffectId text>]`.
//
// Reading accepts three generations: an owned sidecar of EffectId items; a
// sidecar (or `VkHookEffects=` key) of legacy instance names (`name`,
// `name@file.fx`, `name.2`), which migrate to ids; and a foreign ReShade
// preset, where each enabled technique becomes one instance.
//
// Nothing here touches Config, the filesystem or global state.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "effects/effect_id.hh"

namespace VkHook
{
    struct PresetMacro
    {
        std::string name;
        std::string value;
        bool operator==(const PresetMacro&) const = default;
    };

    // One shader uniform. A scalar has one value; a vector has one per component.
    struct PresetParam
    {
        std::string name;
        std::vector<std::string> values;
        bool operator==(const PresetParam&) const = default;
    };

    struct EffectInstance
    {
        EffectId id;
        bool enabled = true;
        std::vector<PresetParam> params;
        std::vector<PresetMacro> macros;  // per-instance preprocessor definitions

        // The registry / config key of this instance.
        std::string name() const { return id.format(); }

        // Sets a value by its Config-style key: "Strength" or "Tint[1]" (one
        // component of a vector, which creates or grows the vector).
        void setParam(std::string_view key, std::string value);
        void setMacro(std::string_view macroName, std::string value);

        bool operator==(const EffectInstance&) const = default;
    };

    struct EffectStack
    {
        std::vector<EffectInstance> instances;
        std::vector<PresetMacro> globalMacros;  // preset-wide PreprocessorDefinitions
        // set by parse(): the preset declares an effect list (possibly an empty
        // one). false for a foreign preset that lists no technique at all, which
        // must leave the caller's own effect list alone. Not part of equality.
        bool authoritative = false;

        bool operator==(const EffectStack& other) const
        {
            return instances == other.instances && globalMacros == other.globalMacros;
        }
    };

    struct SerializedPreset
    {
        std::string ini;
        std::string sidecar;
    };

    // Maps a shader filename, legacy instance name or built-in id to the
    // resolved effect path ("DLSS5" for the built-in). Empty means unknown.
    using PathResolver = std::function<std::string(std::string_view)>;

    // Everything parse() refused or rewrote, for the caller to log.
    struct Diagnostics
    {
        std::vector<std::string> messages;
    };

    struct ParseOptions
    {
        // instances beyond this many are dropped (the caller's clamp)
        size_t maxEffects = static_cast<size_t>(-1);
    };

    // Entries naming no installed shader are dropped and reported, never kept.
    EffectStack parse(std::string_view iniText, std::string_view sidecarText, const PathResolver& resolve,
                      Diagnostics* diagnostics = nullptr, const ParseOptions& options = {});

    SerializedPreset serialize(const EffectStack& stack);
} // namespace VkHook
