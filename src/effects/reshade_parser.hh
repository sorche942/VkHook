#ifndef RESHADE_PARSER_HPP_INCLUDED
#define RESHADE_PARSER_HPP_INCLUDED

#include <string>
#include <vector>
#include <memory>

#include "effects/effect_config.hh"
#include "effects/reshade_compile.hh"
#include "effects/params/effect_param.hh"
#include "config.hh"

namespace VkHook
{
    // Result of testing a shader for compilation errors
    struct ShaderTestResult
    {
        std::string effectName;     // Effect name (filename without extension)
        std::string filePath;       // Full path to .fx file
        bool success = false;       // True if shader compiled without errors
        bool usesDepth = false;     // True if shader uses depth buffer (DEPTH semantic texture)
        std::string errorMessage;   // Error message if failed
    };

    // The user-facing parameters of a compiled module (one per non-internal
    // uniform), with their values read from `config`. Never writes to `config`.
    // effectName: the effect's id text (EffectId::format()); stored in
    //   EffectParam.effectName and the first key looked up
    //   (`<effectName>.<uniform>`, then `<effectName>@<uniform>`)
    // legacyName: when non-empty and config has no value under effectName, the
    //   value is read under this pre-EffectId name instead
    std::vector<std::unique_ptr<EffectParam>> parametersFromModule(
        const reshadefx::module& module,
        const std::string& effectName,
        Config* config,
        const std::string& legacyName = {});

    // Test a ReShade .fx shader for compilation errors without creating Vulkan resources.
    // Returns a ShaderTestResult with success status and any error messages.
    // Overload with includePaths avoids re-reading shader_manager.conf (use for batch testing).
    ShaderTestResult testShaderCompilation(
        const std::string& effectName,
        const std::string& effectPath);
    ShaderTestResult testShaderCompilation(
        const std::string& effectName,
        const std::string& effectPath,
        const std::vector<std::string>& includePaths);

    // Extract user-configurable preprocessor definitions from a ReShade shader.
    // These are macros used via #ifndef/#ifdef that aren't built-in (like __RESHADE__).
    // Returns empty vector for built-in effects or if no user macros are found.
    std::vector<PreprocessorDefinition> extractPreprocessorDefinitions(
        const std::string& effectName,
        const std::string& effectPath,
        const ReshadePreprocessSetup& setup);

} // namespace VkHook

#endif // RESHADE_PARSER_HPP_INCLUDED
