#pragma once

// The shader-preset model: an ordered stack of effect instances and the pure
// functions that read it from / write it to ReShade-compatible preset text.
//
// A preset is two files. The `.ini` is a ReShade preset (Techniques,
// TechniqueSorting, one `[file.fx]` section per shader file). The hidden
// sidecar carries what ReShade has no place for: the exact ordered instance
// stack (`[VKHOOK] Effects=`, `*` marks a disabled instance, `name@type` names
// the shader of a renamed/duplicated instance) and per-instance values for
// instances the shared `.ini` section cannot describe (disabled instances and
// several instances of one shader file).
//
// Nothing here touches Config, the filesystem or global state.

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

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

    struct PresetTechnique
    {
        std::string name;
        bool enabled = true;
        bool operator==(const PresetTechnique&) const = default;
    };

    struct EffectInstance
    {
        std::string name;  // VkHook instance name: "Bloom", "Bloom.2"
        std::string type;  // shader filename ("bloom.fx") or built-in id ("DLSS5")
        bool enabled = true;
        // every technique of the shader file, in preset order. empty means the
        // file's default technique (the file stem). built-in types have none.
        std::vector<PresetTechnique> techniques;
        std::vector<PresetParam> params;
        std::vector<PresetMacro> macros;  // per-instance preprocessor definitions

        // Sets a value by its Config-style key: "Strength" or "Tint[1]" (one
        // component of a vector, which creates or grows the vector).
        void setParam(std::string_view key, std::string value);
        void setMacro(std::string_view name, std::string value);

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

    // Maps an instance name, shader filename or built-in id to the resolved
    // effect path ("DLSS5" for the built-in). Empty means unknown.
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
        // names of the instances the caller already has for a shader file
        // (argument: lowercase filename); lets a foreign preset's techniques
        // land on the game's own instance names. optional.
        std::function<std::vector<std::string>(std::string_view)> configuredNames;
    };

    // Entries naming no known instance are dropped and reported, never kept.
    EffectStack parse(std::string_view iniText, std::string_view sidecarText, const PathResolver& resolve,
                      Diagnostics* diagnostics = nullptr, const ParseOptions& options = {});

    SerializedPreset serialize(const EffectStack& stack);
} // namespace VkHook
