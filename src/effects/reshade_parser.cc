#include "reshade_parser.hh"

#include <climits>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <queue>
#include <set>
#include <unordered_map>
#include <cstdlib>

#include "reshade/effect_parser.hh"
#include "reshade/effect_codegen.hh"
#include "reshade_compile.hh"

#include "logger.hh"
#include "config_serializer.hh"

namespace VkHook
{
    namespace
    {
        // Helper to find annotation by name
        template<typename T>
        auto findAnnotation(const T& annotations, const std::string& name)
        {
            return std::find_if(annotations.begin(), annotations.end(),
                [&name](const auto& a) { return a.name == name; });
        }

        // Helper to check if annotation exists
        template<typename T>
        bool hasAnnotation(const T& annotations, const std::string& name)
        {
            return findAnnotation(annotations, name) != annotations.end();
        }

        // Helper to get float value from annotation (handles int->float conversion)
        template<typename T>
        float getAnnotationFloat(const T& annotation)
        {
            return annotation.type.is_floating_point()
                ? annotation.value.as_float[0]
                : static_cast<float>(annotation.value.as_int[0]);
        }

        // Helper to get int value from annotation (handles float->int conversion)
        template<typename T>
        int getAnnotationInt(const T& annotation)
        {
            return annotation.type.is_integral()
                ? annotation.value.as_int[0]
                : static_cast<int>(annotation.value.as_float[0]);
        }

        // Parse null-separated string into vector
        std::vector<std::string> parseNullSeparatedString(const std::string& str)
        {
            std::vector<std::string> items;
            size_t start = 0;

            for (size_t i = 0; i <= str.size(); i++)
            {
                bool atEnd = (i == str.size() || str[i] == '\0');
                if (!atEnd)
                    continue;

                if (i > start)
                    items.push_back(str.substr(start, i - start));
                start = i + 1;
            }

            return items;
        }

        void applyFloatRange(FloatParam& p, const auto& annotations)
        {
            auto minIt = findAnnotation(annotations, "ui_min");
            auto maxIt = findAnnotation(annotations, "ui_max");

            if (minIt != annotations.end())
                p.minValue = getAnnotationFloat(*minIt);
            if (maxIt != annotations.end())
                p.maxValue = getAnnotationFloat(*maxIt);
        }

        void applyIntRange(IntParam& p, const auto& annotations)
        {
            auto minIt = findAnnotation(annotations, "ui_min");
            auto maxIt = findAnnotation(annotations, "ui_max");

            if (minIt != annotations.end())
                p.minValue = getAnnotationInt(*minIt);
            if (maxIt != annotations.end())
                p.maxValue = getAnnotationInt(*maxIt);
        }

        std::unique_ptr<EffectParam> convertUniform(
            const reshadefx::uniform_info& spec,
            const std::string& effectName,
            Config* config)
        {
            // Label (common to all types)
            auto labelIt = findAnnotation(spec.annotations, "ui_label");
            std::string label = (labelIt != spec.annotations.end()) ? labelIt->value.string_data : spec.name;

            // Tooltip (common to all types)
            auto tooltipIt = findAnnotation(spec.annotations, "ui_tooltip");
            std::string tooltip = (tooltipIt != spec.annotations.end()) ? tooltipIt->value.string_data : "";

            // UI type (common to all types)
            auto typeIt = findAnnotation(spec.annotations, "ui_type");
            std::string uiType = (typeIt != spec.annotations.end()) ? typeIt->value.string_data : "";

            // Helper lambda to populate float vector parameters
            auto populateFloatVector = [&](FloatVecParam& p, uint32_t componentCount) {
                p.effectName = effectName;
                p.name = spec.name;
                p.label = label;
                p.tooltip = tooltip;
                p.uiType = uiType;
                p.componentCount = componentCount;

                auto minIt = findAnnotation(spec.annotations, "ui_min");
                auto maxIt = findAnnotation(spec.annotations, "ui_max");
                for (uint32_t c = 0; c < componentCount; c++)
                {
                    std::string suffix = "[" + std::to_string(c) + "]";
                    p.defaultValue[c] = spec.initializer_value.as_float[c];
                    p.value[c] = config->getInstanceOption<float>(effectName, spec.name + suffix, p.defaultValue[c]);
                    if (minIt != spec.annotations.end())
                        p.minValue[c] = getAnnotationFloat(*minIt);
                    if (maxIt != spec.annotations.end())
                        p.maxValue[c] = getAnnotationFloat(*maxIt);
                }

                auto stepIt = findAnnotation(spec.annotations, "ui_step");
                if (stepIt != spec.annotations.end())
                    p.step = getAnnotationFloat(*stepIt);
            };

            // Helper lambda to populate int vector parameters
            auto populateIntVector = [&](IntVecParam& p, uint32_t componentCount) {
                p.effectName = effectName;
                p.name = spec.name;
                p.label = label;
                p.tooltip = tooltip;
                p.uiType = uiType;
                p.componentCount = componentCount;

                auto minIt = findAnnotation(spec.annotations, "ui_min");
                auto maxIt = findAnnotation(spec.annotations, "ui_max");
                for (uint32_t c = 0; c < componentCount; c++)
                {
                    std::string suffix = "[" + std::to_string(c) + "]";
                    p.defaultValue[c] = spec.initializer_value.as_int[c];
                    p.value[c] = config->getInstanceOption<int32_t>(effectName, spec.name + suffix, p.defaultValue[c]);
                    if (minIt != spec.annotations.end())
                        p.minValue[c] = getAnnotationInt(*minIt);
                    if (maxIt != spec.annotations.end())
                        p.maxValue[c] = getAnnotationInt(*maxIt);
                }

                auto stepIt = findAnnotation(spec.annotations, "ui_step");
                if (stepIt != spec.annotations.end())
                    p.step = getAnnotationFloat(*stepIt);
            };

            // Helper lambda to populate uint vector parameters
            auto populateUintVector = [&](UintVecParam& p, uint32_t componentCount) {
                p.effectName = effectName;
                p.name = spec.name;
                p.label = label;
                p.tooltip = tooltip;
                p.uiType = uiType;
                p.componentCount = componentCount;

                auto minIt = findAnnotation(spec.annotations, "ui_min");
                auto maxIt = findAnnotation(spec.annotations, "ui_max");
                for (uint32_t c = 0; c < componentCount; c++)
                {
                    std::string suffix = "[" + std::to_string(c) + "]";
                    p.defaultValue[c] = spec.initializer_value.as_uint[c];
                    p.value[c] = config->getInstanceOption<uint32_t>(effectName, spec.name + suffix, p.defaultValue[c]);
                    if (minIt != spec.annotations.end())
                        p.minValue[c] = static_cast<uint32_t>(getAnnotationInt(*minIt));
                    if (maxIt != spec.annotations.end())
                        p.maxValue[c] = static_cast<uint32_t>(getAnnotationInt(*maxIt));
                }

                auto stepIt = findAnnotation(spec.annotations, "ui_step");
                if (stepIt != spec.annotations.end())
                    p.step = getAnnotationFloat(*stepIt);
            };

            // Create appropriate subclass based on spec type
            if (spec.type.is_floating_point() && spec.type.rows >= 2 && spec.type.rows <= 4)
            {
                // float2/float3/float4 vector types
                auto p = std::make_unique<FloatVecParam>();
                populateFloatVector(*p, spec.type.rows);
                return p;
            }
            else if (spec.type.is_floating_point() && spec.type.rows == 1)
            {
                // scalar float
                auto p = std::make_unique<FloatParam>();
                p->effectName = effectName;
                p->name = spec.name;
                p->label = label;
                p->tooltip = tooltip;
                p->uiType = uiType;
                p->defaultValue = spec.initializer_value.as_float[0];
                p->value = config->getInstanceOption<float>(effectName, spec.name, p->defaultValue);
                applyFloatRange(*p, spec.annotations);

                auto stepIt = findAnnotation(spec.annotations, "ui_step");
                if (stepIt != spec.annotations.end())
                    p->step = getAnnotationFloat(*stepIt);

                return p;
            }
            else if (spec.type.is_boolean())
            {
                auto p = std::make_unique<BoolParam>();
                p->effectName = effectName;
                p->name = spec.name;
                p->label = label;
                p->tooltip = tooltip;
                p->uiType = uiType;
                p->defaultValue = (spec.initializer_value.as_uint[0] != 0);
                p->value = config->getInstanceOption<bool>(effectName, spec.name, p->defaultValue);
                return p;
            }
            else if (spec.type.is_integral() && spec.type.is_signed() && spec.type.rows >= 2 && spec.type.rows <= 4)
            {
                // int2/int3/int4 vector types
                auto p = std::make_unique<IntVecParam>();
                populateIntVector(*p, spec.type.rows);
                return p;
            }
            else if (spec.type.is_integral() && spec.type.is_signed() && spec.type.rows == 1)
            {
                // Scalar signed int
                auto p = std::make_unique<IntParam>();
                p->effectName = effectName;
                p->name = spec.name;
                p->label = label;
                p->tooltip = tooltip;
                p->uiType = uiType;
                p->defaultValue = spec.initializer_value.as_int[0];
                p->value = config->getInstanceOption<int32_t>(effectName, spec.name, p->defaultValue);
                applyIntRange(*p, spec.annotations);

                auto stepIt = findAnnotation(spec.annotations, "ui_step");
                if (stepIt != spec.annotations.end())
                    p->step = getAnnotationFloat(*stepIt);

                auto itemsIt = findAnnotation(spec.annotations, "ui_items");
                if (itemsIt != spec.annotations.end())
                    p->items = parseNullSeparatedString(itemsIt->value.string_data);

                return p;
            }
            else if (spec.type.is_integral() && !spec.type.is_signed() && spec.type.rows >= 2 && spec.type.rows <= 4)
            {
                // uint2/uint3/uint4 vector types
                auto p = std::make_unique<UintVecParam>();
                populateUintVector(*p, spec.type.rows);
                return p;
            }
            else if (spec.type.is_integral() && !spec.type.is_signed() && spec.type.rows == 1)
            {
                // Scalar unsigned int
                auto p = std::make_unique<UintParam>();
                p->effectName = effectName;
                p->name = spec.name;
                p->label = label;
                p->tooltip = tooltip;
                p->uiType = uiType;
                p->defaultValue = spec.initializer_value.as_uint[0];
                p->value = config->getInstanceOption<uint32_t>(effectName, spec.name, p->defaultValue);

                auto minIt = findAnnotation(spec.annotations, "ui_min");
                auto maxIt = findAnnotation(spec.annotations, "ui_max");
                if (minIt != spec.annotations.end())
                    p->minValue = static_cast<uint32_t>(getAnnotationInt(*minIt));
                if (maxIt != spec.annotations.end())
                    p->maxValue = static_cast<uint32_t>(getAnnotationInt(*maxIt));

                auto stepIt = findAnnotation(spec.annotations, "ui_step");
                if (stepIt != spec.annotations.end())
                    p->step = getAnnotationFloat(*stepIt);

                return p;
            }

            return nullptr;
        }

        bool shouldSkipUniform(const reshadefx::uniform_info& spec)
        {
            if (spec.name.empty())
                return true;
            if (spec.name.rfind("__vkhook_", 0) == 0)
                return true;
            if (hasAnnotation(spec.annotations, "source"))
                return true;
            return false;
        }
    } // anonymous namespace

    std::vector<std::unique_ptr<EffectParam>> parseReshadeEffect(
        const std::string& effectName,
        const std::string& effectPath,
        Config* config,
        const ReshadePreprocessSetup& setup,
        std::vector<std::string>* techniqueNames,
        std::string* outError)
    {
        std::vector<std::unique_ptr<EffectParam>> params;
        if (outError)
            outError->clear();

        std::string source;
        std::string error;
        reshadefx::module module;
        if (!preprocessReshadeFile(effectPath, setup, source, error) ||
            !compileReshadeSource(std::move(source), ReshadeCodegenOptions{}, module, error))
        {
            Logger::err("reshade_parser: " + effectName + ": " + error);
            if (outError)
                *outError = error;
            return params;
        }

        if (techniqueNames)
        {
            techniqueNames->clear();
            for (const auto& technique : module.techniques)
                techniqueNames->push_back(technique.name);
        }

        // Uniforms stay in the uniform buffer (no specialization constants), so
        // every user-facing parameter is a module uniform.
        for (const auto& uniform : module.uniforms)
        {
            if (shouldSkipUniform(uniform))
                continue;

            auto param = convertUniform(uniform, effectName, config);
            if (param)
            {
                param->noSave = hasAnnotation(uniform.annotations, "nosave");
                params.push_back(std::move(param));
            }
        }

        return params;
    }

    ShaderTestResult testShaderCompilation(
        const std::string& effectName,
        const std::string& effectPath)
    {
        // Convenience overload: loads include paths from shader_manager.conf
        ShaderManagerConfig smConfig = ConfigSerializer::loadShaderManagerConfig();
        return testShaderCompilation(effectName, effectPath, smConfig.discoveredShaderPaths);
    }

    ShaderTestResult testShaderCompilation(
        const std::string& effectName,
        const std::string& effectPath,
        const std::vector<std::string>& includePaths)
    {
        ShaderTestResult result;
        result.effectName = effectName;
        result.filePath = effectPath;

        try
        {
            ReshadePreprocessSetup setup;
            setup.includePaths = includePaths;

            std::string source;
            std::string error;
            if (!preprocessReshadeFile(effectPath, setup, source, error))
            {
                result.errorMessage = error;
                return result;
            }

            reshadefx::module module;
            if (!compileReshadeSource(std::move(source), ReshadeCodegenOptions{}, module, error))
            {
                result.errorMessage = error;
                return result;
            }

            if (const char* dumpPath = std::getenv("VKHOOK_DUMP_SPIRV");
                dumpPath != nullptr && *dumpPath != '\0' && !module.spirv.empty())
            {
                std::ofstream dump(dumpPath, std::ios::binary);
                if (dump.is_open())
                {
                    dump.write(reinterpret_cast<const char*>(module.spirv.data()), static_cast<std::streamsize>(module.spirv.size() * sizeof(uint32_t)));
                }
            }

            // Check if shader actually uses the depth buffer at runtime.
            // ReShade.fxh always declares a DEPTH texture + "DepthBuffer" sampler +
            // GetLinearizedDepth function, but most shaders never call them.
            // Libraries like qUINT_common.fxh also declare depth samplers that
            // may go unused (e.g. qUINT_bloom includes the header but never uses depth).
            //
            // Strategy: find depth samplers, then verify they're actually used
            // in the compiled SPIR-V code (not just declared). A sampler that is
            // declared but never loaded/sampled in any entry point is dead code.
            {
                std::string depthTexName;
                for (const auto& tex : module.textures)
                {
                    if (tex.semantic == "DEPTH")
                    {
                        depthTexName = tex.unique_name;
                        break;
                    }
                }

                if (!depthTexName.empty())
                {
                    // Check if any entry point transitively uses the depth sampler.
                    //
                    // ReShade.fxh/qUINT_common.fxh declare depth functions that
                    // compile into the SPIR-V even when unreachable from entry
                    // points. A raw OpLoad scan would produce false positives.
                    //
                    // Algorithm: build a per-function call graph from the SPIR-V,
                    // then BFS from entry points to check reachability.
                    auto isSamplerUsedInSpirv = [&](uint32_t samplerId) -> bool {
                        const auto& code = module.spirv;
                        if (code.size() < 5)
                            return false;

                        // Pass 1: collect entry point function IDs
                        std::set<uint32_t> entryFuncIds;
                        size_t i = 5;  // Skip SPIR-V header
                        while (i < code.size())
                        {
                            uint32_t wc = code[i] >> 16;
                            uint32_t op = code[i] & 0xFFFF;
                            if (wc == 0)
                                break;
                            // OpEntryPoint: [wc|15, execModel, funcId, name..., interface...]
                            if (op == 15 && wc >= 3 && (i + 2) < code.size())
                                entryFuncIds.insert(code[i + 2]);
                            i += wc;
                        }

                        // Pass 2: scan function bodies for depth sampler loads and call edges
                        struct FuncInfo
                        {
                            bool loadsDepthSampler = false;
                            std::vector<uint32_t> callees;
                        };
                        std::unordered_map<uint32_t, FuncInfo> funcs;
                        uint32_t currentFunc = 0;
                        i = 5;
                        while (i < code.size())
                        {
                            uint32_t wc = code[i] >> 16;
                            uint32_t op = code[i] & 0xFFFF;
                            if (wc == 0)
                                break;
                            // OpFunction: [wc|54, resultType, funcId, control, funcType]
                            if (op == 54 && wc >= 3 && (i + 2) < code.size())
                            {
                                currentFunc = code[i + 2];
                                funcs[currentFunc];
                            }
                            // OpFunctionEnd: [wc|56]
                            else if (op == 56)
                                currentFunc = 0;
                            else if (currentFunc != 0)
                            {
                                // OpLoad: [wc|61, resultType, resultId, pointer]
                                if (op == 61 && wc >= 4 && (i + 3) < code.size())
                                {
                                    if (code[i + 3] == samplerId)
                                        funcs[currentFunc].loadsDepthSampler = true;
                                }
                                // OpFunctionCall: [wc|57, resultType, resultId, funcId, args...]
                                if (op == 57 && wc >= 4 && (i + 3) < code.size())
                                    funcs[currentFunc].callees.push_back(code[i + 3]);
                            }
                            i += wc;
                        }

                        // BFS from entry points through call graph
                        std::queue<uint32_t> worklist;
                        std::set<uint32_t> visited;
                        for (uint32_t ep : entryFuncIds)
                        {
                            worklist.push(ep);
                            visited.insert(ep);
                        }
                        while (!worklist.empty())
                        {
                            uint32_t fid = worklist.front();
                            worklist.pop();
                            auto it = funcs.find(fid);
                            if (it == funcs.end())
                                continue;
                            if (it->second.loadsDepthSampler)
                                return true;
                            for (uint32_t callee : it->second.callees)
                            {
                                if (visited.insert(callee).second)
                                    worklist.push(callee);
                            }
                        }
                        return false;
                    };

                    bool depthUsed = false;
                    for (const auto& samp : module.samplers)
                    {
                        if (samp.texture_name != depthTexName)
                            continue;
                        if (!isSamplerUsedInSpirv(samp.id))
                            continue;  // Declared but never used — dead code
                        depthUsed = true;
                        break;
                    }
                    result.usesDepth = depthUsed;
                }
            }

            result.success = true;
        }
        catch (const std::exception& e)
        {
            result.success = false;
            result.errorMessage = "Exception: " + std::string(e.what());
        }
        catch (...)
        {
            result.success = false;
            result.errorMessage = "Unknown exception during compilation";
        }

        return result;
    }

    // Built-in macros that should not be exposed to users
    static const std::set<std::string> builtInMacros = {
        "__RESHADE__",
        "__RESHADE_PERFORMANCE_MODE__",
        "__RENDERER__",
        "BUFFER_WIDTH",
        "BUFFER_HEIGHT",
        "BUFFER_RCP_WIDTH",
        "BUFFER_RCP_HEIGHT",
        "BUFFER_COLOR_DEPTH",
        "__FILE__",
        "__LINE__",
        "__DATE__",
        "__TIME__",
        "__VENDOR__",
        "__APPLICATION__",
    };

    std::vector<PreprocessorDefinition> extractPreprocessorDefinitions(
        const std::string& effectName,
        const std::string& effectPath,
        const ReshadePreprocessSetup& setup)
    {
        std::vector<PreprocessorDefinition> defs;

        std::string source;
        std::string error;
        std::vector<std::pair<std::string, std::string>> usedMacros;
        if (!preprocessReshadeFile(effectPath, setup, source, error, &usedMacros))
        {
            Logger::err("extractPreprocessorDefinitions: " + error);
            return defs;
        }

        for (const auto& [name, value] : usedMacros)
        {
            // Skip built-in macros
            if (builtInMacros.count(name))
                continue;

            // Skip macros that start with underscore (internal/compiler)
            if (!name.empty() && name[0] == '_')
                continue;

            PreprocessorDefinition def;
            def.name = name;
            def.effectName = effectName;
            def.defaultValue = value.empty() ? "1" : value;
            def.value = def.defaultValue;
            defs.push_back(def);
        }

        if (!defs.empty())
        {
            Logger::debug("extractPreprocessorDefinitions: found " + std::to_string(defs.size()) +
                " user macros in " + effectName);
            for (const auto& def : defs)
                Logger::debug("  " + def.name + " = " + def.defaultValue);
        }

        return defs;
    }

} // namespace VkHook
