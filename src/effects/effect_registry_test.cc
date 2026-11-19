// EffectRegistry on real shaders: legacy-name migration to EffectIds,
// technique-level entries, enabled state surviving a re-parse, macro edits
// re-parsing the parameter list through the compile worker, snapshots, and two
// registries being independent.
//
// usage: effect_registry_test <fixtures dir>

#include "effect_registry.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace VkHook;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

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

    const FloatParam* floatParam(EffectRegistry& registry, const std::string& effect, const std::string& name)
    {
        const auto param = registry.getParameter(effect, name);
        return param ? dynamic_cast<const FloatParam*>(*param) : nullptr;
    }

    bool hasParam(EffectRegistry& registry, const std::string& effect, const std::string& name)
    {
        return registry.getParameter(effect, name).has_value();
    }

    const EffectConfig* find(const std::vector<EffectConfig>& all, const std::string& name)
    {
        for (const auto& effect : all)
            if (effect.name == name)
                return &effect;
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
    const fs::path root = fs::temp_directory_path() / ("vkhook-registry-test-" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "shaders");
    fs::create_directories(root / "xdg");
    // user config and shader caches stay inside the sandbox
    ::setenv("XDG_CONFIG_HOME", (root / "xdg").c_str(), 1);
    ::setenv("XDG_CACHE_HOME", (root / "xdg").c_str(), 1);
    const fs::path shader = root / "shaders" / "MultiTechnique.fx";
    fs::copy_file(fs::path(argv[1]) / "compile" / "MultiTechnique.fx", shader);

    const std::string single = "MultiTechnique.fx|SinglePass";
    const std::string single2 = "MultiTechnique.fx|SinglePass|2";
    const std::string triple = "MultiTechnique.fx|TriplePass";
    const std::string doubled = "MultiTechnique.fx|DoublePass";

    // a config written by the old version: instance names, "name.param", "name@MACRO"
    const fs::path confPath = root / "legacy.conf";
    std::ofstream(confPath) << "reshadeIncludePath = " << (root / "shaders").string() << "\n"
                            << "effects = MultiTechnique:MultiTechnique.2\n"
                            << "disabledEffects = MultiTechnique.2\n"
                            << "MultiTechnique.BaseGain = 0.8\n"
                            << "MultiTechnique@MULTI_EXTRA = 1\n"
                            << "MultiTechnique.2.BaseGain = 0.1\n";
    const auto configOwner = std::make_shared<Config>(confPath.string());
    Config& config = *configOwner;

    {
        EffectRegistry registry;
        registry.setCompileExtent(640, 360);
        registry.initialize(configOwner);
        // the build runs on the registry's worker; the entries are there once it is idle
        expect(registry.waitForParses(std::chrono::seconds(120)) && !registry.initializing() && registry.isInitializedFromConfig(),
               "initialize finishes and seeds the selection");

        // migration: the first technique of the file, copy numbers kept
        const auto all = registry.getAllEffects();
        expect(all.size() == 2, "both legacy instances load");
        const EffectConfig* first = find(all, single);
        const EffectConfig* second = find(all, single2);
        expect(first && second, "legacy names become ids of the file's first technique");
        if (first && second)
        {
            expect(first->id.technique == "SinglePass" && first->id.canonicalPath == "MultiTechnique.fx",
                   "id carries file and technique");
            expect(second->id.ordinal == 2, "the legacy .2 copy keeps its number");
            expect(first->techniqueNames.size() == 3, "every technique of the file is reflected");
            expect(first->enabled && !second->enabled, "disabledEffects survives the migration");
            expect(first->legacyName == "MultiTechnique" && second->legacyName == "MultiTechnique.2",
                   "the legacy name is remembered");
        }

        // legacy names still resolve, and the old keys supply the saved values
        expect(registry.hasEffect("MultiTechnique") && registry.canonicalName("MultiTechnique.2") == single2,
               "legacy names alias their ids");
        expect(registry.getEffectFilePath("MultiTechnique") == shader.string(), "file path through the alias");
        const FloatParam* gain = floatParam(registry, single, "BaseGain");
        const FloatParam* gain2 = floatParam(registry, single2, "BaseGain");
        expect(gain && gain->value == 0.8f, "value saved under the legacy name");
        expect(gain2 && gain2->value == 0.1f, "value of the second legacy copy");
        expect(gain && gain->effectName == single, "parameters are named by the id");
        expect(hasParam(registry, single, "ExtraGain") && !hasParam(registry, single2, "ExtraGain"),
               "a legacy macro value gates the uniform of its own copy only");
        const auto defs = registry.getPreprocessorDefs(single);
        expect(std::any_of(defs.begin(), defs.end(),
                           [&](const PreprocessorDefinition& d) { return d.name == "MULTI_EXTRA" && d.value == "1" && d.effectName == single; }),
               "macro definitions are named by the id");

        // a selected list of legacy names reads back as ids
        registry.setSelectedEffects({"MultiTechnique", "MultiTechnique.2"});
        const std::vector<std::string> selected = registry.getSelectedEffects();
        expect(selected == std::vector<std::string>({single, single2}), "getSelectedEffects maps legacy names to ids");

        // technique-level entries: each technique of one file is its own entry
        registry.ensureEffect(triple, "MultiTechnique.fx");
        registry.ensureEffect(doubled, "MultiTechnique");
        const auto after = registry.getAllEffects();
        expect(after.size() == 4, "two more techniques of the same file");
        const EffectConfig* three = find(after, triple);
        expect(three && three->id.technique == "TriplePass" && three->id.ordinal == 1, "the selected technique, not techniques[0]");
        expect(three && three->techniqueNames == std::vector<std::string>({"SinglePass", "DoublePass", "TriplePass"}),
               "technique names in file order");

        // an id without a technique means the file's first
        registry.ensureEffect("MultiTechnique.fx", "");
        expect(registry.hasEffect("MultiTechnique.fx") && registry.canonicalName("MultiTechnique.fx") != "MultiTechnique.fx",
               "a bare file reference becomes an entry of the first technique");

        // an unknown technique is an error on its entry, not a silent techniques[0]
        registry.ensureEffect("MultiTechnique.fx|NoSuchTechnique", "MultiTechnique.fx");
        expect(registry.hasEffectFailed("MultiTechnique.fx|NoSuchTechnique") &&
                   registry.getEffectError("MultiTechnique.fx|NoSuchTechnique").find("NoSuchTechnique") != std::string::npos,
               "a missing technique fails that entry");

        // enabled survives a re-parse (the shader file changed on disk)
        registry.setEffectEnabled(doubled, false);
        const uint64_t generation = registry.parameterGeneration();
        fs::last_write_time(shader, fs::last_write_time(shader) + 10s);
        registry.ensureEffect(doubled, "MultiTechnique.fx");
        expect(registry.parameterGeneration() > generation, "the re-parse replaced the parameters");
        expect(!registry.isEffectEnabled(doubled), "a re-parse does not turn the entry back on");
        expect(registry.isEffectEnabled(single) && !registry.isEffectEnabled(single2), "other entries keep their state");

        // enabled set before the entry exists is applied when it is created
        registry.setEffectEnabled("MultiTechnique.fx|TriplePass|2", false);
        registry.ensureEffect("MultiTechnique.fx|TriplePass|2", "MultiTechnique.fx");
        expect(registry.hasEffect("MultiTechnique.fx|TriplePass|2") && !registry.isEffectEnabled("MultiTechnique.fx|TriplePass|2"),
               "pending enabled state is applied on creation");

        // snapshots do not alias the registry
        auto snapshot = registry.getAllEffects();
        const size_t countBefore = snapshot.size();
        registry.removeEffect(triple);
        expect(countBefore == 7 && snapshot.size() == countBefore && find(snapshot, triple) &&
                   registry.getAllEffects().size() == countBefore - 1,
               "a snapshot outlives later mutations");
        expect(!registry.hasEffect(triple), "removeEffect removes the entry");
        auto defsCopy = registry.getPreprocessorDefs(single);
        registry.setPreprocessorDefValue(single, "MULTI_EXTRA", "1");  // same value: nothing to re-parse
        expect(registry.waitForParses(10s), "an unchanged macro starts no re-parse");

        // a macro edit re-parses the parameter list on the worker
        {
            auto param = registry.getParameter(single, "BaseGain");
            if (param)
                static_cast<FloatParam*>(*param)->value = 0.33f;  // the user's edit
        }
        registry.setPreprocessorDefValue(single, "MULTI_EXTRA", "0");
        expect(registry.waitForParses(30s), "the re-parse finishes");
        expect(!hasParam(registry, single, "ExtraGain"), "macro off: the gated uniform disappears");
        const FloatParam* kept = floatParam(registry, single, "BaseGain");
        expect(kept && kept->value == 0.33f, "an edited value survives the re-parse");
        registry.setPreprocessorDefValue(single, "MULTI_EXTRA", "1");
        expect(registry.waitForParses(30s), "the second re-parse finishes");
        expect(hasParam(registry, single, "ExtraGain"), "macro on: the gated uniform appears without a restart");
        expect(floatParam(registry, single, "BaseGain")->value == 0.33f, "still kept");
        expect(!registry.hasEffectFailed(single), "the re-parse leaves the entry healthy");
        expect(defsCopy.size() == registry.getPreprocessorDefs(single).size(), "the macro list is unchanged");

        // superseded edits: only the last one lands
        registry.setPreprocessorDefValue(single, "MULTI_EXTRA", "0");
        registry.setPreprocessorDefValue(single, "MULTI_EXTRA", "1");
        registry.setPreprocessorDefValue(single, "MULTI_EXTRA", "0");
        expect(registry.waitForParses(30s) && !hasParam(registry, single, "ExtraGain"),
               "rapid edits settle on the last value");

        // a macro value that breaks the compile marks the entry failed until a good one
        registry.setPreprocessorDefValue(single, "MULTI_EXTRA", "(");
        expect(registry.waitForParses(30s), "a failing macro value finishes");
        expect(registry.hasEffectFailed(single), "a failing macro value fails the entry");
        registry.setPreprocessorDefValue(single, "MULTI_EXTRA", "1");
        expect(registry.waitForParses(30s) && !registry.hasEffectFailed(single) && hasParam(registry, single, "ExtraGain"),
               "a good value recovers it");

        // the catalog answers by stem without walking the disk
        expect(registry.catalog().size() == 1 && registry.resolveEffectPath("multitechnique", &config) == shader.string(),
               "catalog lookup by stem");
        expect(registry.resolveEffectPath("MultiTechnique.fx|TriplePass|3", &config) == shader.string(),
               "an id resolves by its file");
        expect(registry.resolveEffectPath("DLSS5||2", &config) == "DLSS5", "the built-in resolves");
        expect(registry.resolveEffectPath("NoSuchShader", &config).empty(), "a miss is empty");

        // DLSS5 legacy copy names migrate too
        registry.ensureEffect("DLSS5", "");
        registry.ensureEffect("DLSS5.2", "");
        expect(registry.hasEffect("DLSS5") && registry.canonicalName("DLSS5.2") == "DLSS5||2", "built-in copies");

        // removal drops the alias
        registry.removeEffect("MultiTechnique.2");
        expect(!registry.hasEffect(single2) && registry.canonicalName("MultiTechnique.2") == "MultiTechnique.2",
               "removing by legacy name removes the entry and its alias");

        // a second registry is independent: its own entries, catalog, cache and worker
        EffectRegistry other;
        expect(other.getAllEffects().empty() && other.catalog().size() == 0 && &other.shaderCache() != &registry.shaderCache() &&
                   &other.compiler() != &registry.compiler(),
               "registries share no state");
    }

    // a registry destroyed with a re-parse in flight must not hang or crash
    {
        const auto again = std::make_shared<Config>(confPath.string());
        EffectRegistry registry;
        registry.initialize(again);
        registry.setPreprocessorDefValue(single, "MULTI_EXTRA", "0");
    }

    fs::remove_all(root);
    if (failures == 0)
        std::printf("effect registry tests passed\n");
    return failures == 0 ? 0 : 1;
}
