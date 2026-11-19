#ifndef RESHADE_COMPILE_HPP_INCLUDED
#define RESHADE_COMPILE_HPP_INCLUDED

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "effect_config.hh"
#include "reshade/effect_codegen.hh"
#include "reshade/effect_module.hh"

namespace VkHook
{
    // The single preprocessing and compile setup for ReShade effects. The
    // registry parse (parameter discovery), the shader check and the runtime
    // compile all go through it, so they see the same prelude, the same macros
    // and therefore the same set of uniforms.
    struct ReshadePreprocessSetup
    {
        uint32_t bufferWidth = 1920;   // BUFFER_WIDTH / BUFFER_HEIGHT
        uint32_t bufferHeight = 1080;
        uint32_t colorBitDepth = 8;    // BUFFER_COLOR_DEPTH
        int depthSourceMode = 0;       // see addReshadeDepthMacros
        std::vector<PreprocessorDefinition> userDefs;  // per-effect user macros
        std::vector<std::string> includePaths;          // shader roots; expanded to include dirs
    };

    // Uniforms always stay in the uniform buffer: turning them into
    // specialization constants bakes their values into the pipeline, so slider
    // edits would stop having any effect.
    struct ReshadeCodegenOptions
    {
        bool emitDebugInfo = true;
        bool useLocalSizeId = true;
    };

    // Preprocess `path`. Returns false with `error` set when the file cannot be
    // read or the preprocessor reports an error; never throws. `usedMacros`, when
    // given, receives the macros the shader read.
    bool preprocessReshadeFile(const std::string& path,
                               const ReshadePreprocessSetup& setup,
                               std::string& source,
                               std::string& error,
                               std::vector<std::pair<std::string, std::string>>* usedMacros = nullptr);

    // Parse and generate code for preprocessed `source`. Returns false with
    // `error` set on any compiler failure, including a thrown exception.
    bool compileReshadeSource(std::string source,
                              const ReshadeCodegenOptions& options,
                              reshadefx::module& module,
                              std::string& error);

    bool hasFatalCompilerDiagnostics(const std::string& diagnostics);
}

#endif // RESHADE_COMPILE_HPP_INCLUDED
