#include "reshade_compile.hh"

#include <climits>
#include <exception>

#include "reshade/effect_parser.hh"
#include "reshade/effect_preprocessor.hh"
#include "reshade/reshade_depth_macros.hh"

#include "config_serializer.hh"
#include "logger.hh"

namespace VkHook
{
    namespace
    {
        // Compatibility shims for HLSL features this reshadefx snapshot lacks.
        const char* const kCompatibilityPrelude =
        "#define tex2DgatherR(s, coords) tex2Dgather(s, coords, 0)\n"
        "#define tex2DgatherG(s, coords) tex2Dgather(s, coords, 1)\n"
        "#define tex2DgatherB(s, coords) tex2Dgather(s, coords, 2)\n"
        "#define tex2DgatherA(s, coords) tex2Dgather(s, coords, 3)\n"
        "#define storage1D storage\n"
        "#define f32tof16 _vkhook_f32tof16\n"
        "#define f16tof32 _vkhook_f16tof32\n"
        "uint _vkhook_f32tof16(float v) {\n"
        "  uint x = asuint(v);\n"
        "  uint sign = (x >> 16) & 0x8000u;\n"
        "  uint exp = (x >> 23) & 0xFFu;\n"
        "  uint mant = x & 0x7FFFFFu;\n"
        "  if (exp == 0xFFu) {\n"
        "    if (mant == 0u) return sign | 0x7C00u;\n"
        "    uint nanMant = mant >> 13;\n"
        "    if (nanMant == 0u) nanMant = 1u;\n"
        "    return sign | 0x7C00u | (nanMant & 0x03FFu);\n"
        "  }\n"
        "  int newExp = int(exp) - 127 + 15;\n"
        "  if (newExp >= 31) return sign | 0x7C00u;\n"
        "  if (newExp <= 0) {\n"
        "    if (newExp < -10) return sign;\n"
        "    mant |= 0x800000u;\n"
        "    uint shift = uint(14 - newExp);\n"
        "    uint halfMant = mant >> shift;\n"
        "    uint roundMask = (1u << shift) - 1u;\n"
        "    uint roundBits = mant & roundMask;\n"
        "    uint halfway = 1u << (shift - 1u);\n"
        "    if (roundBits > halfway || (roundBits == halfway && (halfMant & 1u) != 0u))\n"
        "      halfMant += 1u;\n"
        "    return sign | (halfMant & 0x03FFu);\n"
        "  }\n"
        "  uint halfExp = uint(newExp) << 10;\n"
        "  uint halfMant = mant >> 13;\n"
        "  uint roundBits = mant & 0x1FFFu;\n"
        "  if (roundBits > 0x1000u || (roundBits == 0x1000u && (halfMant & 1u) != 0u)) {\n"
        "    halfMant += 1u;\n"
        "    if (halfMant == 0x0400u) {\n"
        "      halfMant = 0u;\n"
        "      halfExp += 0x0400u;\n"
        "      if (halfExp >= 0x7C00u) halfExp = 0x7C00u;\n"
        "    }\n"
        "  }\n"
        "  return sign | halfExp | (halfMant & 0x03FFu);\n"
        "}\n"
        "uint2 _vkhook_f32tof16(float2 v) { return uint2(_vkhook_f32tof16(v.x), _vkhook_f32tof16(v.y)); }\n"
        "uint3 _vkhook_f32tof16(float3 v) { return uint3(_vkhook_f32tof16(v.x), _vkhook_f32tof16(v.y), _vkhook_f32tof16(v.z)); }\n"
        "uint4 _vkhook_f32tof16(float4 v) { return uint4(_vkhook_f32tof16(v.x), _vkhook_f32tof16(v.y), _vkhook_f32tof16(v.z), _vkhook_f32tof16(v.w)); }\n"
        "float _vkhook_f16tof32(uint h) {\n"
        "  uint sign = (h & 0x8000u) << 16;\n"
        "  uint exp = (h >> 10) & 0x1Fu;\n"
        "  uint mant = h & 0x03FFu;\n"
        "  if (exp == 0u) {\n"
        "    if (mant == 0u) return asfloat(sign);\n"
        "    int shift = 0;\n"
        "    while ((mant & 0x0400u) == 0u) { mant <<= 1; shift++; }\n"
        "    mant &= 0x03FFu;\n"
        "    uint fullExp = uint(127 - 15 - shift);\n"
        "    return asfloat(sign | (fullExp << 23) | (mant << 13));\n"
        "  }\n"
        "  if (exp == 0x1Fu) return asfloat(sign | 0x7F800000u | (mant << 13));\n"
        "  return asfloat(sign | ((exp + 112u) << 23) | (mant << 13));\n"
        "}\n"
        "float2 _vkhook_f16tof32(uint2 h) { return float2(_vkhook_f16tof32(h.x), _vkhook_f16tof32(h.y)); }\n"
        "float3 _vkhook_f16tof32(uint3 h) { return float3(_vkhook_f16tof32(h.x), _vkhook_f16tof32(h.y), _vkhook_f16tof32(h.z)); }\n"
        "float4 _vkhook_f16tof32(uint4 h) { return float4(_vkhook_f16tof32(h.x), _vkhook_f16tof32(h.y), _vkhook_f16tof32(h.z), _vkhook_f16tof32(h.w)); }\n"
        "#define float2x3 matrix<float, 2, 3>\n"
        "#define float2x4 matrix<float, 2, 4>\n"
        "#define float3x2 matrix<float, 3, 2>\n"
        "#define float3x4 matrix<float, 3, 4>\n"
        "#define float4x2 matrix<float, 4, 2>\n"
        "#define float4x3 matrix<float, 4, 3>\n"
        "#define ddx_fine(x) ddx(x)\n"
        "#define ddy_fine(x) ddy(x)\n"
        "#define ddx_coarse(x) ddx(x)\n"
        "#define ddy_coarse(x) ddy(x)\n";

        void setupPreprocessor(reshadefx::preprocessor& pp, const ReshadePreprocessSetup& setup)
        {
            pp.add_macro_definition("__RESHADE__", std::to_string(INT_MAX));
            pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "1");
            pp.add_macro_definition("__RENDERER__", "0x20000");
            pp.add_macro_definition("BUFFER_WIDTH", std::to_string(setup.bufferWidth));
            pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(setup.bufferHeight));
            pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
            pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
            pp.add_macro_definition("BUFFER_COLOR_DEPTH", std::to_string(setup.colorBitDepth));
            pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "BUFFER_COLOR_DEPTH");

            // Only the static source encoding is a macro. The user's invert/flip/flop
            // transforms are runtime resolve push constants (see addReshadeDepthMacros).
            addReshadeDepthMacros(pp);

            pp.append_string(kCompatibilityPrelude);

            for (const auto& def : setup.userDefs)
                pp.add_macro_definition(def.name, def.value);

            for (const auto& path : ConfigSerializer::expandShaderIncludePaths(setup.includePaths))
                pp.add_include_path(path);
        }
    } // namespace

    bool hasFatalCompilerDiagnostics(const std::string& diagnostics)
    {
        return diagnostics.find(" error ") != std::string::npos ||
               diagnostics.find(": error ") != std::string::npos ||
               diagnostics.find("error X") != std::string::npos ||
               diagnostics.find("preprocessor error:") != std::string::npos;
    }

    bool preprocessReshadeFile(const std::string& path,
                               const ReshadePreprocessSetup& setup,
                               std::string& source,
                               std::string& error,
                               std::vector<std::pair<std::string, std::string>>* usedMacros)
    {
        error.clear();
        try
        {
            reshadefx::preprocessor preprocessor;
            setupPreprocessor(preprocessor, setup);

            if (path.empty() || !preprocessor.append_file(path))
            {
                const std::string& ppErrors = preprocessor.errors();
                error = "failed to load shader file: " + path + (ppErrors.empty() ? "" : ": " + ppErrors);
                return false;
            }

            const std::string& ppErrors = preprocessor.errors();
            if (!ppErrors.empty())
            {
                if (hasFatalCompilerDiagnostics(ppErrors))
                {
                    error = "preprocessor errors: " + ppErrors;
                    return false;
                }
                Logger::warn("reshade preprocessor warnings for " + path + ": " + ppErrors);
            }

            if (usedMacros)
                *usedMacros = preprocessor.used_macro_definitions();
            source = std::move(preprocessor.output());
            return true;
        }
        catch (const std::exception& e)
        {
            error = std::string("exception during preprocessing: ") + e.what();
        }
        catch (...)
        {
            error = "unknown exception during preprocessing";
        }
        return false;
    }

    bool compileReshadeSource(std::string source,
                              const ReshadeCodegenOptions& options,
                              reshadefx::module& module,
                              std::string& error)
    {
        error.clear();
        try
        {
            reshadefx::parser parser;
            std::unique_ptr<reshadefx::codegen> codegen(reshadefx::create_codegen_spirv(
                true /* vulkan semantics */,
                options.emitDebugInfo,
                false /* uniforms stay in the uniform buffer */,
                true /* flip vertex shader */,
                options.useLocalSizeId));

            if (!parser.parse(std::move(source), codegen.get()))
            {
                error = "parse errors: " + (parser.errors().empty() ? std::string("parse failed") : parser.errors());
                return false;
            }

            const std::string& diagnostics = parser.errors();
            if (!diagnostics.empty())
            {
                if (hasFatalCompilerDiagnostics(diagnostics))
                {
                    error = "parse errors: " + diagnostics;
                    return false;
                }
                Logger::warn("reshade parse warnings: " + diagnostics);
            }

            codegen->write_result(module);
            return true;
        }
        catch (const std::exception& e)
        {
            error = std::string("exception during compilation: ") + e.what();
        }
        catch (...)
        {
            error = "unknown exception during compilation";
        }
        return false;
    }
} // namespace VkHook
