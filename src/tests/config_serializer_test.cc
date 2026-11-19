#include "config.hh"
#include "config_serializer.hh"
#include "control/preset_model.hh"
#include "ini.hh"
#include "util.hh"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <chrono>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
    int failures = 0;

    void expect(bool condition, const std::string& message)
    {
        if (condition)
            return;
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }

    std::string readFile(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
}

int main()
{
    char temporary[] = "/tmp/vkhook-config-tests-XXXXXX";
    char* tempDir = mkdtemp(temporary);
    if (!tempDir)
    {
        std::cerr << "Could not create temporary config directory\n";
        return 1;
    }

    const std::filesystem::path root(tempDir);
    setenv("XDG_CONFIG_HOME", root.c_str(), 1);
    setenv("HOME", root.c_str(), 1);


    // Saving a per-game config and loading it through Config preserves options.
    std::filesystem::create_directories(VkHook::ConfigSerializer::getConfigsDir());
    const std::string configPath = VkHook::ConfigSerializer::getConfigsDir() + "/serialization-test.conf";
    const std::vector<std::string> effects = {"Simple", "Custom"};
    const std::vector<std::string> disabled = {"Custom"};
    const std::vector<VkHook::ConfigParam> params = {
        {"Simple", "Strength", "0.75"},
        {"Custom", "@QUALITY", "high"},
    };
    const std::map<std::string, std::string> effectPaths = {
        {"Simple", "simple.fx"},
        {"Custom", "/shaders/custom.fx"},
    };
    const std::vector<VkHook::PreprocessorDefinition> definitions = {
        {"QUALITY", "high", "low", "Custom"},
    };
    expect(VkHook::ConfigSerializer::saveToPath(configPath, effects, disabled, params, effectPaths, definitions),
           "save game config");
    VkHook::Config config(configPath);
    expect(config.getOption<std::string>("Simple") == "simple.fx", "config preserves effect paths");
    expect(config.getOption<std::string>("Simple.Strength") == "0.75", "config preserves effect parameters");
    expect(config.getOption<std::string>("Custom@QUALITY") == "high", "config preserves preprocessor definitions");
    expect(config.getOption<std::vector<std::string>>("effects") == effects, "config preserves enabled effect list");
    expect(config.getOption<std::vector<std::string>>("disabledEffects") == disabled,
           "config preserves disabled effect list");

    // Shader-profile files: serialize() (covered in preset_model_test) writes
    // both files; the serializer places, reads, copies and deletes them.
    const auto resolveAny = [](std::string_view key) {
        return key == "cas" ? std::string("cas") : "/shaders/" + std::string(key);
    };
    const auto hasInstance = [](const VkHook::EffectStack& stack, const std::string& name) {
        return std::any_of(stack.instances.begin(), stack.instances.end(),
                           [&](const VkHook::EffectInstance& instance) { return instance.name == name; });
    };
    VkHook::EffectInstance simple;
    simple.name = "Simple";
    simple.type = "simple.fx";
    simple.techniques = {{"SimplePass", true}};
    simple.params = {{"Strength", {"0.75"}}, {"Tint", {"0.1", "0.2"}}};
    simple.macros = {{"QUALITY", "high,fast"}};
    VkHook::EffectInstance custom;
    custom.name = "Custom";
    custom.type = "custom.fx";
    custom.enabled = false;
    custom.techniques = {{"CustomPass", true}};
    custom.params = {{"Strength", {"0.25"}}};
    custom.macros = {{"QUALITY", "low"}};
    VkHook::EffectInstance builtin;
    builtin.name = "cas";
    builtin.type = "cas";

    const std::string shaderPath = VkHook::ConfigSerializer::getShaderProfilePath("roundtrip-game", "source");
    VkHook::EffectStack sourceStack;
    sourceStack.instances = {simple, custom};
    {
        const auto preset = VkHook::serialize(sourceStack);
        expect(VkHook::ConfigSerializer::writeShaderProfile(shaderPath, preset.ini, preset.sidecar),
               "write shader profile");
    }
    std::string shaderIni, shaderSidecar;
    VkHook::ConfigSerializer::readShaderProfile(shaderPath, shaderIni, shaderSidecar);
    const std::string shaderText = readFile(shaderPath);
    expect(shaderIni == shaderText && !shaderIni.empty(), "readShaderProfile returns the .ini text");
    const auto presetPath = std::filesystem::path(shaderPath);
    const auto disabledProfilePath = (presetPath.parent_path() /
        ("." + presetPath.filename().string() + "_disabled-effectvalues")).string();
    expect(VkHook::ConfigSerializer::getShaderProfileSidecarPath(shaderPath) == disabledProfilePath &&
               shaderSidecar == readFile(disabledProfilePath) && !shaderSidecar.empty(),
           "the sidecar is the hidden file beside the preset");
    expect(shaderText.find("\n\n[simple.fx]\n") != std::string::npos,
           "shader preset separates parameter sections with a blank line");
    expect(shaderText.find("VkHookEffects=") == std::string::npos &&
               shaderText.find("VkHookDisabledEffects=") == std::string::npos &&
               shaderText.find("[VKHOOK]") == std::string::npos,
           "shader profile only writes ReShade-compatible keys");
    expect(shaderText.find("Tint=0.1,0.2") != std::string::npos, "vector values are written as one ReShade line");
    expect(shaderText.find("PreprocessorDefinitions=QUALITY=high,,fast") != std::string::npos,
           "escaped commas in macro values retain ReShade syntax");
    expect(shaderText.find("Strength=0.25") == std::string::npos && shaderText.find("custom.fx]") == std::string::npos &&
               shaderText.find("QUALITY=low") == std::string::npos,
           "disabled effect values are omitted from main preset");
    expect(shaderSidecar.find("[Custom]\nPreprocessorDefinitions=QUALITY=low\n") != std::string::npos &&
               shaderSidecar.find("Strength=0.25") != std::string::npos,
           "disabled effect values are saved in the sidecar in normal INI syntax");
    expect(VkHook::parse(shaderIni, shaderSidecar, resolveAny) == sourceStack,
           "the written preset reads back as the stack that was saved");

    // Two instances of one shader file share a filename section, so their
    // values live in the sidecar by instance name.
    const std::string dupPath = VkHook::ConfigSerializer::getShaderProfilePath("roundtrip-game", "dups");
    VkHook::EffectInstance bloom;
    bloom.name = "Bloom";
    bloom.type = "bloom.fx";
    bloom.techniques = {{"Bloom", true}};
    bloom.params = {{"Strength", {"0.5"}}};
    bloom.macros = {{"QUALITY", "high"}};
    VkHook::EffectInstance bloom2 = bloom;
    bloom2.name = "Bloom.2";
    bloom2.params = {{"Strength", {"0.9"}}};
    bloom2.macros = {{"QUALITY", "low"}};
    VkHook::EffectStack dupStack;
    dupStack.instances = {bloom, bloom2};
    {
        const auto preset = VkHook::serialize(dupStack);
        expect(VkHook::ConfigSerializer::writeShaderProfile(dupPath, preset.ini, preset.sidecar),
               "write duplicate-instance shader profile");
    }
    std::string dupIni, dupSidecar;
    VkHook::ConfigSerializer::readShaderProfile(dupPath, dupIni, dupSidecar);
    const auto dupRead = VkHook::parse(dupIni, dupSidecar, resolveAny);
    expect(dupRead == dupStack && dupRead.instances.size() == 2 &&
               dupRead.instances[0].params[0].values[0] == "0.5" && dupRead.instances[1].params[0].values[0] == "0.9",
           "duplicate instances keep per-instance values and preprocessor definitions");

    // The sidecar carries VkHook's authoritative instance list: order,
    // kinds, and per-instance enable state, including built-ins.
    const std::string ownedPath = VkHook::ConfigSerializer::getShaderProfilePath("roundtrip-game", "owned");
    VkHook::EffectStack ownedStack;
    ownedStack.instances = {simple, builtin, custom};
    ownedStack.instances[0].params.clear();
    ownedStack.instances[0].macros.clear();
    ownedStack.instances[2].params.clear();
    ownedStack.instances[2].macros.clear();
    {
        const auto preset = VkHook::serialize(ownedStack);
        expect(VkHook::ConfigSerializer::writeShaderProfile(ownedPath, preset.ini, preset.sidecar),
               "write vkhook-owned profile");
    }
    std::string ownedIni, ownedSidecar;
    VkHook::ConfigSerializer::readShaderProfile(ownedPath, ownedIni, ownedSidecar);
    const auto ownedRead = VkHook::parse(ownedIni, ownedSidecar, resolveAny);
    expect(ownedRead.authoritative && ownedRead.instances.size() == 3,
           "sidecar marks the profile owned and lists every instance");
    expect(ownedRead.instances[0].name == "Simple" && ownedRead.instances[0].type == "simple.fx" &&
               ownedRead.instances[0].enabled,
           "reshade instance round-trips with its filename");
    expect(ownedRead.instances[1].name == "cas" && ownedRead.instances[1].type == "cas",
           "built-in instance round-trips as its bare type");
    expect(ownedRead.instances[2].name == "Custom" && !ownedRead.instances[2].enabled,
           "disabled flag survives the sidecar round-trip");
    expect(readFile(ownedPath).find("[VKHOOK]") == std::string::npos,
           "the main ini stays vkhook-free for ReShade");

    // inheritance copies the sidecar; deletion removes both files
    expect(VkHook::ConfigSerializer::createShaderProfile("roundtrip-game", "copy", "owned"),
           "create copies an owned profile");
    const std::string copyPath = VkHook::ConfigSerializer::getShaderProfilePath("roundtrip-game", "copy");
    expect(readFile(VkHook::ConfigSerializer::getShaderProfileSidecarPath(copyPath)) ==
               readFile(VkHook::ConfigSerializer::getShaderProfileSidecarPath(ownedPath)),
           "inherited profile copies the sidecar verbatim");
    expect(VkHook::ConfigSerializer::deleteShaderProfile("roundtrip-game", "copy") &&
               !std::filesystem::exists(copyPath) &&
               !std::filesystem::exists(VkHook::ConfigSerializer::getShaderProfileSidecarPath(copyPath)),
           "deleting a profile also deletes its sidecar");
    expect(!hasInstance(VkHook::parse(ownedIni, "", resolveAny), "cas") &&
               !hasInstance(VkHook::parse(shaderIni, "", resolveAny), "Custom"),
           "a preset's main ini is never mistaken for the vkhook instance list");

    // Imported, unprefixed ReShade presets should be discoverable and readable.
    const std::string importedPath = VkHook::ConfigSerializer::getBaseConfigDir() +
                                     "/configs/shaders/reshade-import.ini";
    std::filesystem::create_directories(std::filesystem::path(importedPath).parent_path());
    {
        std::ofstream imported(importedPath);
        imported << "Techniques=BloomPass@Example.fx,ColorPass@Example.fx\n"
                    "TechniqueSorting=BloomPass@Example.fx, ColorPass@Example.fx\n\n"
                    "[Example.fx]\n"
                    "PreprocessorDefinitions=QUALITY=2,,4\n"
                    "Strength = 0.5\n";
    }
    const auto listedProfiles = VkHook::ConfigSerializer::listShaderProfilesForGame("roundtrip-game");
    expect(std::find(listedProfiles.begin(), listedProfiles.end(), "reshade-import") != listedProfiles.end(),
           "unprefixed ReShade preset appears in the profile list");
    expect(VkHook::ConfigSerializer::setLastShaderProfile("roundtrip-game", "reshade-import") &&
               VkHook::ConfigSerializer::getLastShaderProfile("roundtrip-game") == "reshade-import",
           "last shader profile persists by game name");
    expect(VkHook::ConfigSerializer::getShaderProfilePath("roundtrip-game", "reshade-import") == importedPath,
           "unprefixed ReShade preset resolves to its imported path");
    std::string importedIni, importedSidecar;
    VkHook::ConfigSerializer::readShaderProfile(importedPath, importedIni, importedSidecar);
    expect(importedSidecar.empty(), "imported preset without a sidecar stays foreign");
    const auto importedStack = VkHook::parse(importedIni, importedSidecar, resolveAny);
    expect(importedStack.instances.size() == 1 && importedStack.instances[0].name == "Example" &&
               importedStack.instances[0].techniques.size() == 2,
           "ReShade preset lists parse with standard comma-separated spacing");
    expect(importedStack.instances.size() == 1 && importedStack.instances[0].params.size() == 1 &&
               importedStack.instances[0].params[0].name == "Strength" &&
               importedStack.instances[0].params[0].values == std::vector<std::string>{"0.5"},
           "ReShade shader-filename sections parse");
    expect(importedStack.instances.size() == 1 && importedStack.instances[0].macros.size() == 1 &&
               importedStack.instances[0].macros[0].name == "QUALITY" &&
               importedStack.instances[0].macros[0].value == "2,4",
           "ReShade preprocessor definitions preserve escaped commas");

    // Creating a game profile with an imported preset's name must not overwrite
    // or alias the shared preset; it should copy its contents into a game file.
    const auto gameImportPath = VkHook::ConfigSerializer::getBaseConfigDir() +
                                "/configs/shaders/roundtrip-game@reshade-import.ini";
    expect(VkHook::ConfigSerializer::createShaderProfile("roundtrip-game", "reshade-import", "reshade-import"),
           "create game profile with same name as imported preset");
    expect(VkHook::ConfigSerializer::getShaderProfilePath("roundtrip-game", "reshade-import") == gameImportPath,
           "game-specific profile takes precedence over imported preset");
    expect(readFile(gameImportPath) == readFile(importedPath),
           "same-named game profile inherits imported preset contents");
    expect(readFile(importedPath).find("TechniqueSorting=BloomPass@Example.fx") != std::string::npos,
           "creating same-named game profile leaves imported preset intact");

    const std::string disabledTechniquePath = VkHook::ConfigSerializer::getBaseConfigDir() +
                                               "/configs/shaders/disabled-technique.ini";
    {
        std::ofstream preset(disabledTechniquePath);
        preset << "Techniques=EnabledPass@Example.fx\n"
                  "TechniqueSorting=EnabledPass@Example.fx,DisabledPass@Other.fx\n";
    }
    std::string disabledTechniqueIni, disabledTechniqueSidecar;
    VkHook::ConfigSerializer::readShaderProfile(disabledTechniquePath, disabledTechniqueIni, disabledTechniqueSidecar);
    const auto disabledTechniqueStack = VkHook::parse(disabledTechniqueIni, disabledTechniqueSidecar, resolveAny);
    expect(disabledTechniqueStack.instances.size() == 1 && disabledTechniqueStack.instances[0].name == "Example" &&
               disabledTechniqueStack.instances[0].techniques.size() == 1,
           "a technique listed only in the sorting is not imported");

    // New shader profiles inherit the exact active profile contents.
    const std::string inheritedPath = VkHook::ConfigSerializer::getShaderProfilePath("roundtrip-game", "inherited");
    expect(VkHook::ConfigSerializer::createShaderProfile("roundtrip-game", "inherited", "source"),
           "create shader profile from active profile");
    expect(readFile(inheritedPath) == readFile(shaderPath), "new shader profile inherits source contents");
    expect(!VkHook::ConfigSerializer::createShaderProfile("roundtrip-game", "inherited", "source"),
           "duplicate shader profile creation fails");

    // Renaming a preset moves the ini, its sidecar, and the last-used pointer.
    expect(VkHook::ConfigSerializer::createShaderProfile("roundtrip-game", "rename-src", "owned"),
           "create rename source");
    const std::string renameSrcPath = VkHook::ConfigSerializer::getShaderProfilePath("roundtrip-game", "rename-src");
    const std::string renameSrcSidecar = VkHook::ConfigSerializer::getShaderProfileSidecarPath(renameSrcPath);
    expect(VkHook::ConfigSerializer::setLastShaderProfile("roundtrip-game", "rename-src") &&
               VkHook::ConfigSerializer::renameShaderProfile("roundtrip-game", "rename-src", "rename-dst"),
           "rename a game-specific preset");
    const std::string renameDstPath = VkHook::ConfigSerializer::getBaseConfigDir() +
                                      "/configs/shaders/roundtrip-game@rename-dst.ini";
    expect(std::filesystem::exists(renameDstPath) && !std::filesystem::exists(renameSrcPath),
           "rename moves the game-specific ini");
    expect(!std::filesystem::exists(renameSrcSidecar) &&
               std::filesystem::exists(VkHook::ConfigSerializer::getShaderProfileSidecarPath(renameDstPath)),
           "rename carries the sidecar");
    expect(readFile(renameDstPath) == readFile(ownedPath), "rename preserves preset contents");
    expect(VkHook::ConfigSerializer::getLastShaderProfile("roundtrip-game") == "rename-dst",
           "rename repoints the last-used name");
    expect(VkHook::ConfigSerializer::getShaderProfilePath("roundtrip-game", "rename-dst") == renameDstPath,
           "renamed preset resolves by its new name");

    // Refusals must leave every file exactly where it started.
    expect(!VkHook::ConfigSerializer::renameShaderProfile("roundtrip-game", "rename-dst", "owned"),
           "rename onto an existing preset is refused");
    expect(std::filesystem::exists(renameDstPath) &&
               !std::filesystem::exists(VkHook::ConfigSerializer::getBaseConfigDir() +
                                        "/configs/shaders/roundtrip-game@owned-renamed.ini"),
           "refused rename leaves the source in place");
    expect(VkHook::ConfigSerializer::isValidShaderProfileName("good name") &&
               !VkHook::ConfigSerializer::isValidShaderProfileName("") &&
               !VkHook::ConfigSerializer::isValidShaderProfileName(".") &&
               !VkHook::ConfigSerializer::isValidShaderProfileName("bad/name") &&
               !VkHook::ConfigSerializer::isValidShaderProfileName("bad\\name") &&
               !VkHook::ConfigSerializer::isValidShaderProfileName("bad@name"),
           "profile-name validation rejects separators, '@' and empties");
    expect(!VkHook::ConfigSerializer::renameShaderProfile("roundtrip-game", "rename-dst", "bad/name"),
           "rename refuses an invalid target name");
    expect(!VkHook::ConfigSerializer::renameShaderProfile("roundtrip-game", "missing-source", "whatever"),
           "rename refuses a missing source");
    expect(std::filesystem::exists(renameDstPath), "invalid rename leaves the source in place");
    expect(VkHook::ConfigSerializer::renameShaderProfile("roundtrip-game", "rename-dst", "rename-dst") &&
               std::filesystem::exists(renameDstPath),
           "rename to the same name is a no-op success");

    // An imported global preset keeps its global kind when renamed.
    const std::string globalRenameSrc = VkHook::ConfigSerializer::getBaseConfigDir() +
                                        "/configs/shaders/global-rename-src.ini";
    {
        std::ofstream preset(globalRenameSrc);
        preset << "Techniques=\nTechniqueSorting=\n";
    }
    expect(VkHook::ConfigSerializer::renameShaderProfile("roundtrip-game", "global-rename-src", "global-rename-dst"),
           "rename an imported global preset");
    const std::string globalRenameDst = VkHook::ConfigSerializer::getBaseConfigDir() +
                                        "/configs/shaders/global-rename-dst.ini";
    expect(std::filesystem::exists(globalRenameDst) && !std::filesystem::exists(globalRenameSrc),
           "global rename keeps the file global (no game prefix)");


    // One tokenizer: values with blanks, '#', quotes, '=' and backslashes survive write->read.
    {
        const std::vector<std::string> values = {
            "plain", "with space", "has # hash", "say \"hi\"", "back\\slash", " leading", "trailing ",
            "a=b=c", "", "\"quoted\"", "tab\there", "/path with spaces/#1/file \"x\".fx",
        };
        for (const auto& value : values)
        {
            const auto line = VkHook::Ini::tokenize("key = " + VkHook::Ini::formatValue(value) + "   # trailing comment");
            expect(line.kind == VkHook::Ini::LineKind::Pair && line.key == "key" && line.value == value,
                   "ini round trip: [" + value + "] came back as [" + line.value + "]");
        }
        expect(VkHook::Ini::tokenize("  # hello").kind == VkHook::Ini::LineKind::Comment, "comment line");
        expect(VkHook::Ini::tokenize("   ").kind == VkHook::Ini::LineKind::Blank, "blank line");
        expect(VkHook::Ini::tokenize("a = b = c").value == "b = c", "second '=' belongs to the value");
        expect(VkHook::Ini::tokenize("a = x y").value == "x y", "unquoted inner blank kept");
        expect(VkHook::Ini::parseBool("True") == true && VkHook::Ini::parseBool("off") == false &&
                   !VkHook::Ini::parseBool("maybe").has_value(),
               "bool parser");

        // Config reads the same quoting rules the settings store does (config_store_test)
        const std::string confPath = VkHook::ConfigSerializer::getBaseConfigDir() + "/VkHook.conf";
        {
            std::ofstream f(confPath);
            f << "maxEffects = \"17\"   # quoted number\n"
                 "toggleKey = \"Home\"\n"
                 "enableOnLaunch = Off # comment\n"
                 "depthManualPin = \"pin # one\"\n";
        }
        VkHook::Config config(confPath);
        expect(config.getOption<int>("maxEffects") == 17, "Config reads a quoted number with a comment");
        expect(config.getOption<std::string>("toggleKey") == "Home", "Config reads a quoted string");
        expect(config.getOption<bool>("enableOnLaunch") == false, "Config reads a bool");
        expect(config.getOption<std::string>("depthManualPin") == "pin # one",
               "a '#' inside quotes is not a comment");

        // The hot-reload watcher ignores VkHook's own writes but sees external edits.
        VkHook::Config watched(confPath);
        expect(!watched.hasConfigChanged(), "fresh config reports no change");
        std::this_thread::sleep_for(std::chrono::milliseconds(650));
        expect(VkHook::writeAtomically(confPath, "maxEffects = 9\n"), "own write");
        expect(!watched.hasConfigChanged(), "the watcher ignores VkHook's own write");
        std::this_thread::sleep_for(std::chrono::milliseconds(650));
        {
            std::ofstream external(confPath);
            external << "maxEffects = 10\n";
        }
        expect(watched.hasConfigChanged(), "the watcher sees an external edit");
        std::filesystem::remove(confPath);
    }

    // A disabled effect's macro value containing commas survives the sidecar.
    {
        const std::string path = VkHook::ConfigSerializer::getShaderProfilePath("macro-game", "commas");
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        VkHook::EffectInstance tint;
        tint.name = "Tint";
        tint.type = "tint.fx";
        tint.enabled = false;
        tint.techniques = {{"Tint", true}};
        tint.params = {{"Strength", {"0.5"}}};
        tint.macros = {{"COLOR", "float3(1,0,0)"}};
        VkHook::EffectStack stack;
        stack.instances = {tint};
        const auto preset = VkHook::serialize(stack);
        expect(VkHook::ConfigSerializer::writeShaderProfile(path, preset.ini, preset.sidecar),
               "save profile with a disabled comma macro");
        std::string ini, sidecar;
        VkHook::ConfigSerializer::readShaderProfile(path, ini, sidecar);
        const auto data = VkHook::parse(ini, sidecar, resolveAny);
        const bool ok = data.instances.size() == 1 && data.instances[0].macros.size() == 1 &&
                        data.instances[0].macros[0].value == "float3(1,0,0)";
        expect(ok, "disabled macro value float3(1,0,0) round-trips through the sidecar");
    }

    // Loading the shader-manager config never writes or creates anything.
    {
        const std::string managerConf = VkHook::ConfigSerializer::getBaseConfigDir() + "/shader_manager.conf";
        std::filesystem::remove(managerConf);
        const auto before = std::filesystem::exists(VkHook::ConfigSerializer::getBaseConfigDir() + "/reshade");
        (void)VkHook::ConfigSerializer::loadShaderManagerConfig();
        expect(!std::filesystem::exists(managerConf), "loading shader manager config does not write it");
        expect(std::filesystem::exists(VkHook::ConfigSerializer::getBaseConfigDir() + "/reshade") == before,
               "loading shader manager config does not create directories");
    }

    // The shader-path caches are shared by compile workers, the present thread
    // and the persistence writer: hammer load, expand and save together. Run
    // under TSan this is the data-race check; here it also proves every reader
    // sees a complete, self-consistent result while saves invalidate the caches.
    {
        const std::string base = VkHook::ConfigSerializer::getBaseConfigDir();
        const std::filesystem::path packs = std::filesystem::path(base) / "cache-hammer";
        std::filesystem::create_directories(packs / "Shaders" / "a");
        std::filesystem::create_directories(packs / "Shaders" / "b");
        VkHook::ShaderManagerConfig seed;
        seed.parentDirectories = {packs.string()};
        expect(VkHook::ConfigSerializer::saveShaderManagerConfig(seed), "seed shader manager config for the hammer");

        std::atomic<int> bad{0};
        std::vector<std::thread> workers;
        for (int t = 0; t < 6; ++t)
        {
            workers.emplace_back([&, t] {
                for (int i = 0; i < 200; ++i)
                {
                    if (t == 0 && i % 10 == 0)
                    {
                        if (!VkHook::ConfigSerializer::saveShaderManagerConfig(seed))
                            ++bad;
                        continue;
                    }
                    const auto config = VkHook::ConfigSerializer::loadShaderManagerConfig();
                    if (config.parentDirectories.size() != 1 || config.parentDirectories[0] != packs.string())
                        ++bad;
                    const auto paths = VkHook::ConfigSerializer::expandShaderIncludePaths({(packs / "Shaders").string()});
                    // the root and its two subdirectories, never a partial list
                    if (paths.size() != 3)
                        ++bad;
                }
            });
        }
        for (std::thread& worker : workers)
            worker.join();
        expect(bad == 0, "shader-path caches stay consistent under concurrent load/expand/save");
    }

    std::filesystem::remove_all(root);
    if (failures != 0)
    {
        std::cerr << failures << " config serializer test(s) failed\n";
        return 1;
    }

    std::cout << "Config serializer tests passed\n";
    return 0;
}
