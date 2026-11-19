// Preset model: parse/serialize of ReShade presets and VkHook's sidecar.
//
// argv[1] is tests/fixtures. Inputs live in fixtures/presets/*.ini (+ .sidecar);
// the checked-in outputs are *.golden.ini / *.golden.sidecar. After an
// intentional format change, regenerate them with VKHOOK_UPDATE_GOLDEN=1 and
// review the diff.

#include "control/preset_model.hh"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace VkHook;

namespace
{
    int failures = 0;
    std::filesystem::path presets;
    bool updateGolden = false;

    void expect(bool condition, const std::string& message)
    {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }

    std::string readFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    std::string lowerCase(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
        return text;
    }

    // The installed shaders of the fake machine. Like EffectRegistry::resolveEffectPath
    // a key resolves by filename or stem, case-insensitively; instance names
    // that are not a shader resolve to nothing.
    std::string resolveFake(std::string_view key)
    {
        static const std::vector<std::string> files = {
            "qUINT_mxao.fx", "qUINT_bloom.fx", "LumaSharpen.fx", "Tonemap.fx",
            "Vibrance.fx",   "SMAA.fx",        "LumaStack.fx",
        };
        if (key == "DLSS5")
            return "DLSS5";
        const std::string wanted = lowerCase(std::string(key));
        for (const auto& file : files)
        {
            const std::string name = lowerCase(file);
            if (name == wanted || name.substr(0, name.size() - 3) == wanted)
                return "/shaders/" + file;
        }
        return {};
    }

    std::string input(const std::string& file)
    {
        const auto path = presets / file;
        const bool exists = std::filesystem::exists(path);
        expect(exists, "fixture exists: " + file);
        return exists ? readFile(path) : std::string();
    }

    std::string optionalInput(const std::string& file)
    {
        const auto path = presets / file;
        return std::filesystem::exists(path) ? readFile(path) : std::string();
    }

    struct Parsed
    {
        EffectStack stack;
        Diagnostics diagnostics;
    };

    Parsed parseFixture(const std::string& name, const ParseOptions& options = {})
    {
        Parsed parsed;
        parsed.stack = parse(input(name + ".ini"), optionalInput(name + ".sidecar"), resolveFake,
                             &parsed.diagnostics, options);
        return parsed;
    }

    void writeFile(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

    // serialize(x) is the checked-in golden pair, and reading the golden back
    // gives x again: the format loses nothing it writes.
    void checkGolden(const std::string& name, const EffectStack& stack)
    {
        const SerializedPreset out = serialize(stack);
        const auto goldenIni = presets / (name + ".golden.ini");
        const auto goldenSidecar = presets / (name + ".golden.sidecar");
        if (updateGolden)
        {
            writeFile(goldenIni, out.ini);
            writeFile(goldenSidecar, out.sidecar);
        }
        expect(out.ini == readFile(goldenIni), name + ": serialized .ini matches the golden file");
        expect(out.sidecar == readFile(goldenSidecar), name + ": serialized sidecar matches the golden file");

        const EffectStack reread = parse(readFile(goldenIni), readFile(goldenSidecar), resolveFake);
        expect(reread == stack, name + ": parse(golden) equals the model");
        const SerializedPreset again = serialize(reread);
        expect(again.ini == out.ini && again.sidecar == out.sidecar, name + ": serialize(parse(golden)) is the golden");
        const EffectStack roundTrip = parse(out.ini, out.sidecar, resolveFake);
        expect(roundTrip == stack, name + ": parse(serialize(x)) == x");
    }

    const EffectInstance* find(const EffectStack& stack, const std::string& name)
    {
        for (const auto& instance : stack.instances)
            if (instance.name == name)
                return &instance;
        return nullptr;
    }

    std::vector<std::string> names(const EffectStack& stack)
    {
        std::vector<std::string> result;
        for (const auto& instance : stack.instances)
            result.push_back(instance.name);
        return result;
    }

    const PresetParam* param(const EffectInstance& instance, const std::string& key)
    {
        for (const auto& p : instance.params)
            if (p.name == key)
                return &p;
        return nullptr;
    }

    std::string macro(const std::vector<PresetMacro>& macros, const std::string& key)
    {
        for (const auto& m : macros)
            if (m.name == key)
                return m.value;
        return "<missing>";
    }

    std::vector<std::string> lines(const std::string& text)
    {
        std::vector<std::string> result;
        size_t begin = 0;
        while (begin < text.size())
        {
            size_t end = text.find('\n', begin);
            if (end == std::string::npos)
                end = text.size();
            result.push_back(text.substr(begin, end - begin));
            begin = end + 1;
        }
        return result;
    }

    // ReShade reads PreprocessorDefinitions, Techniques and TechniqueSorting
    // before the first section, then `[file.fx]` sections of key=value lines.
    void checkReShadeShape(const std::string& name, const std::string& ini)
    {
        const auto all = lines(ini);
        std::vector<std::string> head;
        size_t i = 0;
        for (; i < all.size() && (all[i].empty() || all[i].front() != '['); ++i)
            if (!all[i].empty())
                head.push_back(all[i].substr(0, all[i].find('=')));
        const std::vector<std::string> withMacros = {"PreprocessorDefinitions", "Techniques", "TechniqueSorting"};
        const std::vector<std::string> withoutMacros = {"Techniques", "TechniqueSorting"};
        expect(head == withMacros || head == withoutMacros, name + ": ini starts with ReShade's key set in order");
        expect(ini.find("[VKHOOK]") == std::string::npos && ini.find("\r") == std::string::npos,
               name + ": ini carries no VkHook state");
        for (; i < all.size(); ++i)
        {
            if (all[i].empty())
                continue;
            if (all[i].front() == '[')
                expect(all[i].substr(all[i].size() - 4) == ".fx]" || all[i] == "[DLSS5]",
                       name + ": section is keyed by shader filename (built-ins by name): " + all[i]);
            else
                expect(all[i].find('=') != std::string::npos && all[i].find(", ") == std::string::npos,
                       name + ": plain key=value line without spaces: " + all[i]);
        }
    }

    void testForeignPack()
    {
        const auto parsed = parseFixture("qUINT_pack");
        const auto& stack = parsed.stack;
        expect(stack.authoritative, "a preset with techniques is authoritative");
        expect(names(stack) == std::vector<std::string>({"qUINT_mxao", "qUINT_bloom", "LumaSharpen", "Vibrance"}),
               "instances follow the preset's sorting; sorting-only Tonemap is not imported");
        expect(parsed.diagnostics.messages.empty(), "a clean preset reports nothing");
        expect(stack.globalMacros.size() == 4 &&
                   macro(stack.globalMacros, "RESHADE_DEPTH_LINEARIZATION_FAR_PLANE") == "1000",
               "global definitions parse");

        const auto* mxao = find(stack, "qUINT_mxao");
        expect(mxao && mxao->type == "qUINT_mxao.fx" && mxao->enabled, "MXAO lands on the installed file");
        expect(mxao && mxao->techniques == std::vector<PresetTechnique>{{"MXAO", true}},
               "technique name is kept apart from the instance name");
        expect(mxao && macro(mxao->macros, "MXAO_MIPLEVEL_IL") == "2", "per-effect definitions parse");
        expect(mxao && param(*mxao, "MXAO_SAMPLE_RADIUS") &&
                   param(*mxao, "MXAO_SAMPLE_RADIUS")->values == std::vector<std::string>{"2.500000"},
               "scalar parameter parses");

        const auto* luma = find(stack, "LumaSharpen");
        expect(luma && macro(luma->macros, "TINT") == "float3(1,0.9,0.8)" && macro(luma->macros, "LS_QUALITY") == "2",
               "doubled commas in a macro value read back as literal commas");

        const auto* vibrance = find(stack, "Vibrance");
        expect(vibrance && param(*vibrance, "Vibrance_RGB_balance") &&
                   param(*vibrance, "Vibrance_RGB_balance")->values.size() == 3,
               "vector parameter keeps its components");

        const SerializedPreset out = serialize(stack);
        checkReShadeShape("qUINT_pack", out.ini);
        expect(out.ini.find("TINT=float3(1,,0.9,,0.8),LS_QUALITY=2") != std::string::npos,
               "literal commas in macro values are written doubled");
        expect(out.ini.find("Tonemap") == std::string::npos, "sorting-only techniques are not invented");
        checkGolden("qUINT_pack", stack);
    }

    void testCrlfAndGeneralSection()
    {
        const auto lf = parseFixture("sweetfx");
        const auto crlf = parseFixture("sweetfx_crlf");
        expect(input("sweetfx_crlf.ini").find("\r\n") != std::string::npos, "the CRLF fixture is CRLF");
        expect(lf.stack == crlf.stack, "CRLF and LF presets parse to the same stack");
        expect(names(lf.stack) == std::vector<std::string>({"SMAA", "Tonemap"}),
               "ReShade 4 [GENERAL] keys are read; Vibrance (sorting only) is not imported");
        const auto* tonemap = find(lf.stack, "Tonemap");
        expect(tonemap && param(*tonemap, "FogColor") && param(*tonemap, "FogColor")->values.size() == 3 &&
                   param(*tonemap, "Exposure")->values == std::vector<std::string>{"-0.100000"},
               "CRLF values carry no stray carriage return");
        const SerializedPreset a = serialize(lf.stack);
        const SerializedPreset b = serialize(crlf.stack);
        expect(a.ini == b.ini && a.sidecar == b.sidecar, "the written preset is LF whatever the input was");
        checkReShadeShape("sweetfx", a.ini);
        checkGolden("sweetfx", lf.stack);
    }

    void testMultipleTechniquesPerFile()
    {
        const auto parsed = parseFixture("multi_technique");
        const auto& stack = parsed.stack;
        expect(names(stack) == std::vector<std::string>({"LumaStack", "Vibrance"}),
               "several techniques of one file are one instance");
        const auto* luma = find(stack, "LumaStack");
        expect(luma && luma->techniques ==
                           std::vector<PresetTechnique>({{"LumaPre", true}, {"LumaPost", true}, {"LumaDebug", false}}),
               "every technique of the file is kept, with its own enabled flag");
        const SerializedPreset out = serialize(stack);
        expect(out.ini.find("Techniques=LumaPre@LumaStack.fx,LumaPost@LumaStack.fx,Vibrance@Vibrance.fx\n") !=
                   std::string::npos,
               "enabled techniques are all written");
        expect(out.ini.find("LumaDebug@LumaStack.fx") != std::string::npos &&
                   out.ini.find("Techniques=LumaPre@LumaStack.fx,LumaPost@LumaStack.fx,Vibrance@Vibrance.fx,LumaDebug") ==
                       std::string::npos,
               "the disabled technique stays in the sorting list only");
        checkReShadeShape("multi_technique", out.ini);
        checkGolden("multi_technique", stack);

        // an owned preset whose instance is disabled keeps every technique
        EffectStack disabled = stack;
        disabled.instances[0].enabled = false;
        for (auto& technique : disabled.instances[0].techniques)
            technique.enabled = true;
        const SerializedPreset off = serialize(disabled);
        expect(off.ini.find("Techniques=Vibrance@Vibrance.fx\n") != std::string::npos,
               "a disabled instance contributes no enabled technique");
        expect(parse(off.ini, off.sidecar, resolveFake) == disabled, "disabled multi-technique instance round-trips");
    }

    void testUnknownEntriesAreDropped()
    {
        const auto parsed = parseFixture("unknown_entries");
        expect(names(parsed.stack) == std::vector<std::string>({"qUINT_mxao", "Vibrance"}),
               "a technique naming no installed shader does not become an instance");
        expect(parsed.diagnostics.messages.size() == 1 &&
                   parsed.diagnostics.messages[0].find("Ghost@NoSuchShader.fx") != std::string::npos,
               "the dropped technique is reported");
        const SerializedPreset out = serialize(parsed.stack);
        expect(out.ini.find("NoSuchShader") == std::string::npos && out.ini.find("Ghost") == std::string::npos &&
                   out.sidecar.find("Ghost") == std::string::npos,
               "nothing of the dropped entry is written back");
        checkGolden("unknown_entries", parsed.stack);

        // a sidecar entry no installed shader backs is dropped too
        Diagnostics diagnostics;
        const EffectStack owned = parse("Techniques=\nTechniqueSorting=\n",
                                        "[VKHOOK]\nEffects=Vibrance, MXAO, *cas, Bloom.2@qUINT_bloom.fx\n", resolveFake,
                                        &diagnostics);
        expect(names(owned) == std::vector<std::string>({"Vibrance", "Bloom.2"}),
               "owned instances naming nothing (the ghost MXAO, a removed built-in) are dropped");
        expect(diagnostics.messages.size() == 2, "each dropped owned instance is reported");
        expect(find(owned, "Bloom.2") && find(owned, "Bloom.2")->type == "qUINT_bloom.fx",
               "a renamed instance resolves through its shader file");
    }

    void testLegacyEffectKeys()
    {
        const auto parsed = parseFixture("legacy_vkhook_effects");
        const auto& stack = parsed.stack;
        expect(stack.authoritative, "the legacy list is authoritative");
        expect(names(stack) == std::vector<std::string>({"qUINT_mxao", "Vibrance"}),
               "legacy VkHookEffects entries resolve; the ghost MXAO and the removed cas are dropped");
        expect(parsed.diagnostics.messages.size() == 2, "both dropped legacy entries are reported");
        expect(find(stack, "Vibrance") && !find(stack, "Vibrance")->enabled && find(stack, "qUINT_mxao")->enabled,
               "legacy VkHookDisabledEffects disables");
        expect(find(stack, "Vibrance") && param(*find(stack, "Vibrance"), "Vibrance")->values.front() == "0.300000",
               "legacy presets keep their parameter sections");
        // writing migrates the preset to the one current format
        const SerializedPreset out = serialize(stack);
        expect(out.ini.find("VkHook") == std::string::npos, "the legacy keys are never written again");
        checkGolden("legacy_vkhook_effects", stack);
    }

    void testOldWriterOutput()
    {
        const auto parsed = parseFixture("old_writer");
        const auto& stack = parsed.stack;
        expect(names(stack) == std::vector<std::string>({"Bloom", "Bloom.2", "Vibrance", "DLSS5"}),
               "the sidecar is the complete ordered stack; the removed built-in cas is dropped");
        expect(!find(stack, "Vibrance")->enabled && find(stack, "Bloom")->enabled && find(stack, "DLSS5")->enabled,
               "'*' marks a disabled instance");
        expect(find(stack, "DLSS5")->type == "DLSS5" && find(stack, "DLSS5")->techniques.empty(),
               "a built-in has no technique entries");
        expect(param(*find(stack, "DLSS5"), "skinStructure")->values.front() == "-1",
               "built-in parameters live in a section named after the instance");
        expect(param(*find(stack, "Bloom"), "BLOOM_INT")->values.front() == "0.500000" &&
                   param(*find(stack, "Bloom.2"), "BLOOM_INT")->values.front() == "1.500000",
               "duplicate instances of one file keep their own values from the sidecar");
        const auto* vibrance = find(stack, "Vibrance");
        expect(param(*vibrance, "Vibrance")->values.front() == "0.750000" &&
                   param(*vibrance, "Vibrance_RGB_balance")->values ==
                       std::vector<std::string>({"1.000000", "0.900000", "0.800000"}),
               "a disabled instance's values come from the filename-keyed sidecar section");
        expect(parsed.diagnostics.messages.size() == 1, "only the removed built-in is reported");
        checkGolden("old_writer", stack);

        const SerializedPreset out = serialize(stack);
        expect(out.sidecar.find("[Bloom]\n") != std::string::npos && out.sidecar.find("[Bloom.2]\n") != std::string::npos &&
                   out.sidecar.find("[Vibrance]\n") != std::string::npos,
               "instances that the shared section cannot describe get their own sidecar section");
        expect(out.sidecar.find("[DLSS5]") == std::string::npos, "an enabled lone instance stays in the ini only");
    }

    void testEarlierSidecarSectionName()
    {
        const auto parsed = parseFixture("legacy_vkintox");
        expect(names(parsed.stack) == std::vector<std::string>({"DLSS5"}) && parsed.stack.authoritative,
               "the [VKINTOX] sidecar of an earlier release is still read");
        const auto* dlss = find(parsed.stack, "DLSS5");
        expect(dlss && dlss->params.size() == 7, "its built-in parameters are applied");
        checkGolden("legacy_vkintox", parsed.stack);
    }

    // Everything the format has to carry, built in code, checked against the golden.
    void testFullStack()
    {
        EffectStack stack;
        stack.globalMacros = {{"RESHADE_DEPTH_INPUT_IS_REVERSED", "1"}, {"GLOBAL_TINT", "float3(0,1,0)"}};

        EffectInstance bloom;
        bloom.name = "Bloom";
        bloom.type = "qUINT_bloom.fx";
        bloom.techniques = {{"Bloom", true}};
        bloom.params = {{"BLOOM_INT", {"0.500000"}}, {"BLOOM_TINT", {"1.000000", "0.500000", "0.250000"}}};
        bloom.macros = {{"BLOOM_QUALITY", "2"}};

        EffectInstance bloom2 = bloom;
        bloom2.name = "Bloom.2";
        bloom2.params = {{"BLOOM_INT", {"1.500000"}}, {"BLOOM_TINT", {"0.000000", "0.000000", "1.000000"}}};
        bloom2.macros = {{"BLOOM_QUALITY", "float2(1,2)"}};

        EffectInstance luma;
        luma.name = "LumaStack";
        luma.type = "LumaStack.fx";
        luma.techniques = {{"LumaPre", true}, {"LumaPost", false}};
        luma.params = {{"Strength", {"0.400000"}}};

        EffectInstance vibrance;
        vibrance.name = "Vibrance";
        vibrance.type = "Vibrance.fx";
        vibrance.enabled = false;
        vibrance.techniques = {{"Vibrance", true}};
        vibrance.params = {{"Vibrance", {"0.750000"}}, {"Vibrance_RGB_balance", {"1.000000", "0.900000", "0.800000"}}};
        vibrance.macros = {{"VIB_TINT", "float3(1,0,0)"}, {"VIB_MODE", "1"}};

        EffectInstance dlss;
        dlss.name = "DLSS5";
        dlss.type = "DLSS5";
        dlss.params = {{"intensity", {"1"}}, {"skinStructure", {"-1"}}};

        EffectInstance noValues;
        noValues.name = "Tonemap";
        noValues.type = "Tonemap.fx";
        noValues.techniques = {{"Tonemap", true}};

        stack.instances = {bloom, luma, bloom2, vibrance, dlss, noValues};
        const SerializedPreset out = serialize(stack);
        checkReShadeShape("full_stack", out.ini);
        expect(out.ini.find("GLOBAL_TINT=float3(0,,1,,0)") != std::string::npos,
               "global macro commas are doubled");
        expect(out.ini.find("[Vibrance.fx]") == std::string::npos,
               "a disabled instance's values stay out of the ReShade ini");
        expect(out.sidecar.find("VIB_TINT=float3(1,,0,,0)") != std::string::npos,
               "a disabled instance's macro commas survive the sidecar");
        expect(out.sidecar.find("Effects=Bloom@qUINT_bloom.fx, LumaStack@LumaStack.fx, Bloom.2@qUINT_bloom.fx, "
                                "*Vibrance@Vibrance.fx, DLSS5, Tonemap@Tonemap.fx\n") !=
                   std::string::npos,
               "the sidecar lists the full ordered stack");
        checkGolden("full_stack", stack);

        const EffectStack empty;
        const SerializedPreset blank = serialize(empty);
        expect(blank.ini == "Techniques=\nTechniqueSorting=\n" && blank.sidecar == "[VKHOOK]\nEffects=\n",
               "an empty stack serializes to an empty preset");
        const EffectStack reread = parse(blank.ini, blank.sidecar, resolveFake);
        expect(reread.authoritative && reread.instances.empty(),
               "an empty owned preset still clears the effect list");
    }

    void testCallerPolicy()
    {
        // the cap is the caller's
        ParseOptions capped;
        capped.maxEffects = 2;
        Parsed limited = parseFixture("qUINT_pack", capped);
        expect(names(limited.stack) == std::vector<std::string>({"qUINT_mxao", "qUINT_bloom"}),
               "instances beyond maxEffects are dropped");
        expect(!limited.diagnostics.messages.empty(), "the cap is reported");

        // a technique lands on the instance name the caller already has for the
        // shader file, but only one that really is that file
        ParseOptions configured;
        configured.configuredNames = [](std::string_view file) {
            return file == "quint_mxao.fx" ? std::vector<std::string>{"Occlusion"} : std::vector<std::string>{};
        };
        const PathResolver withOcclusion = [](std::string_view key) {
            return key == "Occlusion" ? std::string("/shaders/qUINT_mxao.fx") : resolveFake(key);
        };
        const EffectStack named = parse(input("qUINT_pack.ini"), "", withOcclusion, nullptr, configured);
        expect(names(named) == std::vector<std::string>({"Occlusion", "qUINT_bloom", "LumaSharpen", "Vibrance"}) &&
                   find(named, "Occlusion") && param(*find(named, "Occlusion"), "MXAO_SAMPLE_RADIUS"),
               "a foreign preset applies to the caller's own instance of the shader");

        // a technique name that resolves to some OTHER shader is not an instance
        const PathResolver collides = [](std::string_view key) {
            return key == "MXAO" ? std::string("/shaders/MXAO.fx") : resolveFake(key);
        };
        const EffectStack safe = parse(input("qUINT_pack.ini"), "", collides);
        expect(find(safe, "qUINT_mxao") && !find(safe, "MXAO"),
               "the bare technique name never becomes an effect-list entry");

        // a preset that lists nothing leaves the caller's list alone
        const EffectStack nothing = parse("Techniques=\nTechniqueSorting=\n", "", resolveFake);
        expect(!nothing.authoritative && nothing.instances.empty(), "an empty foreign preset is not authoritative");

        // no filesystem or global state: the same inputs give the same stack
        expect(parseFixture("qUINT_pack").stack == parseFixture("qUINT_pack").stack, "parse is deterministic");
    }

    void testInstanceSetters()
    {
        EffectInstance instance;
        instance.setParam("Strength", "0.5");
        instance.setParam("Tint[1]", "0.2");
        instance.setParam("Tint[0]", "0.1");
        instance.setParam("Strength", "0.75");
        instance.setMacro("QUALITY", "high,fast");
        instance.setMacro("QUALITY", "low");
        expect(instance.params.size() == 2 && instance.params[0].values == std::vector<std::string>{"0.75"} &&
                   instance.params[1].values == std::vector<std::string>({"0.1", "0.2"}),
               "setParam replaces scalars and fills vector components by index");
        expect(instance.macros.size() == 1 && instance.macros[0].value == "low", "setMacro replaces by name");

        // values with a comma round-trip as vector components, never as a macro
        EffectStack stack;
        instance.name = "Simple";
        instance.type = "Vibrance.fx";
        instance.techniques = {{"Vibrance", true}};
        stack.instances = {instance};
        const SerializedPreset out = serialize(stack);
        expect(out.ini.find("Tint=0.1,0.2\n") != std::string::npos && out.ini.find("QUALITY=low") != std::string::npos,
               "vector components are comma-joined without spaces, like ReShade");
        expect(parse(out.ini, out.sidecar, resolveFake) == stack, "setter-built stack round-trips");
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: preset_model_test <fixtures dir>\n";
        return 2;
    }
    presets = std::filesystem::path(argv[1]) / "presets";
    updateGolden = std::getenv("VKHOOK_UPDATE_GOLDEN") != nullptr;

    testForeignPack();
    testCrlfAndGeneralSection();
    testMultipleTechniquesPerFile();
    testUnknownEntriesAreDropped();
    testLegacyEffectKeys();
    testOldWriterOutput();
    testEarlierSidecarSectionName();
    testFullStack();
    testCallerPolicy();
    testInstanceSetters();

    if (failures != 0)
    {
        std::cerr << failures << " preset model test(s) failed\n";
        return 1;
    }
    std::cout << "Preset model tests passed\n";
    return 0;
}
