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

    // Parse a ReShade .fx file and extract its parameters without creating Vulkan resources.
    // effectName: display name for the effect (used in EffectParam.effectName)
    // effectPath: full path to the .fx file
    // config: config for current param values
    // setup: the shared preprocess setup (extent, user macros, include paths)
    // outError, when given, is set to a non-empty message on any fatal
    // preprocessor/parse failure, including a throwing compiler. That lets a
    // caller use this parse as its compile check.
    std::vector<std::unique_ptr<EffectParam>> parseReshadeEffect(
        const std::string& effectName,
        const std::string& effectPath,
        Config* config,
        const ReshadePreprocessSetup& setup,
        std::vector<std::string>* techniqueNames = nullptr,
        std::string* outError = nullptr);

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
