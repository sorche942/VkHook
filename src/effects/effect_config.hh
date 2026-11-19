#ifndef EFFECT_CONFIG_HPP_INCLUDED
#define EFFECT_CONFIG_HPP_INCLUDED

#include <string>
#include <vector>
#include <memory>
#include <filesystem>

#include "effect_id.hh"
#include "params/effect_param.hh"

namespace VkHook
{
    enum class EffectType
    {
        ReShade,   // .fx files
        DLSS5
    };

    // Preprocessor definition extracted from ReShade shader
    // These are user-configurable compile-time constants (#define macros)
    struct PreprocessorDefinition
    {
        std::string name;           // Macro name, e.g., "ENABLE_SCANLINES"
        std::string value;          // Current value (will be passed to compiler)
        std::string defaultValue;   // Default from shader or "1"
        std::string effectName;     // Which effect this belongs to
    };

    // What the registry knows about one chain entry, minus its parameter
    // objects. Plain data, so it is copyable and the registry's snapshots
    // (getAllEffects) can outlive its lock.
    struct EffectConfig
    {
        std::string name;       // registry key: EffectId::format(), e.g. "Clarity.fx|Clarity"
        EffectId    id;         // the entry: file, technique, copy number
        std::string effectType; // the file stem ("Clarity"), or "DLSS5"
        std::string filePath;   // Path to the .fx file
        EffectType type = EffectType::ReShade;
        bool enabled = true;
        // Every technique of the file in declaration order (reflection of the
        // whole file; `id.technique` is the one this entry runs).
        std::vector<std::string> techniqueNames;
        std::vector<PreprocessorDefinition> preprocessorDefs;  // ReShade: user-configurable macros
        std::string compileError;  // Empty if compiled successfully, error message if failed
        std::filesystem::file_time_type fileModTime{};  // Last modification time of .fx file when parsed
        // The name this entry had before the EffectId migration ("Clarity.2");
        // values saved under it are still read as a fallback. Empty for an entry
        // that was always an id.
        std::string legacyName;
        bool hasFailed() const { return !compileError.empty(); }
    };

    // The registry's record of one entry: the plain data plus the parameter
    // objects, which are owned and so make the record move-only.
    struct EffectEntry
    {
        EffectConfig info;
        std::vector<std::unique_ptr<EffectParam>> parameters;
        // Bumped by every macro edit; a re-parse result is applied only while it
        // still matches, so a superseded one is dropped.
        uint64_t macroVersion = 0;
    };

} // namespace VkHook

#endif // EFFECT_CONFIG_HPP_INCLUDED
