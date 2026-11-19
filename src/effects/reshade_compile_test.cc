#include "reshade_compile.hh"
#include "reshade_parser.hh"

#include <cstdio>
#include <string>

#include "config.hh"

using namespace VkHook;

namespace
{
    int failures = 0;

    void expect(bool condition, const char* message)
    {
        if (!condition)
        {
            ++failures;
            std::printf("FAILED: %s\n", message);
        }
    }

    const FloatParam* findFloat(const std::vector<std::unique_ptr<EffectParam>>& params, const std::string& name)
    {
        for (const auto& param : params)
            if (param->name == name)
                return dynamic_cast<const FloatParam*>(param.get());
        return nullptr;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: %s <fixtures dir>\n", argv[0]);
        return 2;
    }
    const std::string dir = std::string(argv[1]) + "/compile/";
    const std::string gated = dir + "MacroGatedUniform.fx";
    Config config("/nonexistent/vkhook-test.conf");

    // Without the macro the gated uniform does not exist; the extent defaults
    // come from the setup, not from a hard-coded 1920x1080.
    {
        ReshadePreprocessSetup setup;
        std::string error;
        auto params = parseReshadeEffect("MacroGatedUniform", gated, &config, setup, nullptr, &error);
        expect(error.empty(), "plain parse succeeds");
        expect(findFloat(params, "AlwaysStrength") != nullptr, "ungated uniform is discovered");
        expect(findFloat(params, "ExtraStrength") == nullptr, "gated uniform is absent without its macro");
        const FloatParam* width = findFloat(params, "WidthProbe");
        const FloatParam* height = findFloat(params, "HeightProbe");
        expect(width && width->defaultValue == 1920.0f, "default extent width comes from the setup");
        expect(height && height->defaultValue == 1080.0f, "default extent height comes from the setup");
    }

    {
        ReshadePreprocessSetup setup;
        setup.userDefs.push_back({"ENABLE_EXTRA", "1", "1", "MacroGatedUniform"});
        std::string error;
        auto params = parseReshadeEffect("MacroGatedUniform", gated, &config, setup, nullptr, &error);
        expect(error.empty(), "parse with the macro succeeds");
        const FloatParam* extra = findFloat(params, "ExtraStrength");
        expect(extra != nullptr, "gated uniform appears when its macro is set");
        expect(extra && extra->defaultValue == 0.25f, "gated uniform keeps its declared default");
    }

    {
        ReshadePreprocessSetup setup;
        setup.bufferWidth = 2560;
        setup.bufferHeight = 1440;
        std::string error;
        auto params = parseReshadeEffect("MacroGatedUniform", gated, &config, setup, nullptr, &error);
        const FloatParam* width = findFloat(params, "WidthProbe");
        const FloatParam* height = findFloat(params, "HeightProbe");
        expect(width && width->defaultValue == 2560.0f, "passed-in width reaches BUFFER_WIDTH");
        expect(height && height->defaultValue == 1440.0f, "passed-in height reaches BUFFER_HEIGHT");
    }

    // The macro the shader reads is offered to the user.
    {
        ReshadePreprocessSetup setup;
        auto defs = extractPreprocessorDefinitions("MacroGatedUniform", gated, setup);
        bool found = false;
        for (const auto& def : defs)
            found |= def.name == "ENABLE_EXTRA";
        expect(found, "ENABLE_EXTRA is extracted as a user macro");
    }

    // Compiler failures are error results, never crashes or exceptions.
    {
        ReshadePreprocessSetup setup;
        std::string error;
        auto params = parseReshadeEffect("BrokenSyntax", dir + "BrokenSyntax.fx", &config, setup, nullptr, &error);
        expect(!error.empty() && params.empty(), "a shader with a syntax error returns an error");

        error.clear();
        params = parseReshadeEffect("Missing", dir + "DoesNotExist.fx", &config, setup, nullptr, &error);
        expect(!error.empty() && params.empty(), "a missing shader returns an error");
    }

    // Uniforms stay in the uniform buffer, so editing one never needs a recompile.
    {
        ReshadePreprocessSetup setup;
        std::string source, error;
        reshadefx::module module;
        const bool ok = preprocessReshadeFile(gated, setup, source, error) &&
                        compileReshadeSource(std::move(source), ReshadeCodegenOptions{}, module, error);
        expect(ok, "fixture compiles");
        bool inBuffer = false;
        for (const auto& uniform : module.uniforms)
            inBuffer |= uniform.name == "AlwaysStrength";
        expect(inBuffer, "uniform lives in the uniform buffer");
        bool userSpecConstant = false;
        for (const auto& spec : module.spec_constants)
            userSpecConstant |= spec.name.rfind("__vkhook_", 0) != 0;
        expect(!userSpecConstant, "no uniform became a specialization constant");
    }

    if (failures == 0)
        std::printf("reshade compile setup checks passed\n");
    return failures == 0 ? 0 : 1;
}
