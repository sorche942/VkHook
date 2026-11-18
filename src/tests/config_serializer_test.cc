#include "config.hh"
#include "config_serializer.hh"
#include "default_config.hh"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
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

    // A missing global config inherits launch effects enabled and writes that default.
    VkHook::ConfigSerializer::ensureConfigExists();
    expect(VkHook::ConfigSerializer::loadSettings().enableOnLaunch,
           "missing global config defaults enableOnLaunch to true");
    expect(readFile(VkHook::ConfigSerializer::getBaseConfigDir() + "/VkHook.conf")
                   .find("enableOnLaunch = true") != std::string::npos,
           "generated global config writes enableOnLaunch as true");

    // Accept the capitalized boolean spelling used by older bundled configs.
    {
        std::ofstream legacy(VkHook::ConfigSerializer::getBaseConfigDir() + "/VkHook.conf");
        legacy << "enableOnLaunch = True\n";
    }
    expect(VkHook::ConfigSerializer::loadSettings().enableOnLaunch,
           "settings load capitalized True for enableOnLaunch");
    std::filesystem::remove(VkHook::ConfigSerializer::getBaseConfigDir() + "/VkHook.conf");

    // Settings are read back from an isolated XDG config directory.
    VkHook::VkBasaltSettings settings = VkHook::DefaultConfig::appSettings();
    settings.maxEffects = 23;
    settings.overlayBlockInput = false;
    settings.usePortalIntegration = false;
    settings.toggleKey = "F8";
    settings.reloadKey = "F9";
    settings.overlayKey = "F11";
    settings.enableOnLaunch = true;
    settings.depthCapture = true;
    settings.autoApply = false;
    settings.autoApplyDelay = 375;
    settings.showDebugWindow = true;
    settings.separateEffectParams = false;
    settings.depthResolveMode = 1;
    settings.depthManualPin = "image-view-42";
    settings.depthTransientWorkaround = false;
    settings.depthCaptureMethod = 1;
    settings.depthSourceChannel = 4;
    settings.depthInvert = false;
    settings.depthFlip = true;
    settings.depthFlop = true;

    expect(VkHook::ConfigSerializer::saveSettings(settings), "save global settings");
    const auto loaded = VkHook::ConfigSerializer::loadSettings();
    expect(loaded.maxEffects == settings.maxEffects, "settings preserve maxEffects");
    expect(loaded.overlayBlockInput == settings.overlayBlockInput, "settings preserve overlayBlockInput");
    expect(loaded.usePortalIntegration == settings.usePortalIntegration, "settings preserve usePortalIntegration");
    expect(loaded.toggleKey == settings.toggleKey && loaded.reloadKey == settings.reloadKey &&
               loaded.overlayKey == settings.overlayKey,
           "settings preserve key bindings");
    expect(loaded.enableOnLaunch == settings.enableOnLaunch, "settings preserve enableOnLaunch");
    expect(loaded.depthCapture == settings.depthCapture, "settings preserve depthCapture");
    expect(loaded.autoApply == settings.autoApply && loaded.autoApplyDelay == settings.autoApplyDelay,
           "settings preserve auto-apply options");
    expect(loaded.showDebugWindow == settings.showDebugWindow, "settings preserve showDebugWindow");
    expect(loaded.separateEffectParams == settings.separateEffectParams, "settings preserve separateEffectParams");
    expect(loaded.depthResolveMode == settings.depthResolveMode && loaded.depthManualPin == settings.depthManualPin &&
               loaded.depthTransientWorkaround == settings.depthTransientWorkaround &&
               loaded.depthCaptureMethod == settings.depthCaptureMethod &&
               loaded.depthSourceChannel == settings.depthSourceChannel && loaded.depthInvert == settings.depthInvert &&
               loaded.depthFlip == settings.depthFlip && loaded.depthFlop == settings.depthFlop,
           "settings preserve depth options");

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

    // The experimental @default config name is migrated to the fixed game path.
    const std::string gamePath = VkHook::ConfigSerializer::getProfilePath("migration-game");
    const std::string oldGamePath = VkHook::ConfigSerializer::getConfigsDir() + "/migration-game@default.conf";
    std::filesystem::create_directories(VkHook::ConfigSerializer::getConfigsDir());
    {
        std::ofstream legacy(oldGamePath);
        legacy << "effects = Legacy\n";
    }
    expect(VkHook::ConfigSerializer::ensureGameProfile("migration-game") == gamePath,
           "game config migration returns canonical path");
    expect(std::filesystem::exists(gamePath) && !std::filesystem::exists(oldGamePath),
           "game config migration renames legacy path");
    expect(readFile(gamePath) == "effects = Legacy\n", "game config migration preserves contents");

    // Shader-profile serialization covers lists, scalar/vector params, and macros.
    const std::string shaderPath = VkHook::ConfigSerializer::getShaderProfilePath("roundtrip-game", "source");
    const std::vector<VkHook::ConfigParam> shaderParams = {
        {"Simple", "Strength", "0.75"},
        {"Simple", "Tint[0]", "0.1"},
        {"Simple", "Tint[1]", "0.2"},
        {"Simple", "@QUALITY", "high,fast"},
    };
    const std::vector<std::string> shaderEffects = {"Simple", "Custom"};
    const std::vector<std::string> shaderDisabled = {"Custom"};
    const std::vector<std::string> techniques = {"SimplePass@simple.fx"};
    const std::vector<std::string> sorting = {"SimplePass@simple.fx", "CustomPass@custom.fx"};
    const std::vector<VkHook::ConfigParam> disabledParams = {
        {"Custom", "Strength", "0.25"},
        {"Custom", "@QUALITY", "low"},
    };
    expect(VkHook::ConfigSerializer::saveShaderProfile(shaderPath, shaderParams, shaderEffects, shaderDisabled,
                                                         effectPaths, techniques, sorting, disabledParams),
           "save shader profile");
    const auto shaderData = VkHook::ConfigSerializer::loadShaderProfileData(shaderPath);
    expect(shaderData.hasTechniques && shaderData.techniques == techniques && shaderData.techniqueSorting == sorting,
           "shader profile preserves ReShade techniques");
    expect(!shaderData.hasEffectList, "shader profile does not require VkHook-only effect-list keys");
    const std::string shaderText = readFile(shaderPath);
    expect(shaderText.find("[simple.fx]\n") != std::string::npos &&
               shaderText.find("\n\n[simple.fx]\n") != std::string::npos,
           "shader preset separates parameter sections with a blank line");
    expect(shaderText.find("VkHookEffects=") == std::string::npos &&
               shaderText.find("VkHookDisabledEffects=") == std::string::npos,
           "shader profile only writes ReShade-compatible keys");
    expect(shaderText.find("[simple.fx]\n") != std::string::npos,
           "shader parameters use ReShade shader-filename section names");
    expect(shaderData.params.size() == shaderParams.size(), "shader profile preserves parameter count");
    const auto presetPath = std::filesystem::path(shaderPath);
    const auto disabledProfilePath = (presetPath.parent_path() /
        ("." + presetPath.filename().string() + "_disabled-effectvalues")).string();
    const auto savedDisabledParams = VkHook::ConfigSerializer::loadShaderProfile(disabledProfilePath);
    expect(savedDisabledParams.size() == disabledParams.size(), "disabled effect values saved in sidecar");
    expect(readFile(disabledProfilePath).find("PreprocessorDefinitions=QUALITY=low") != std::string::npos,
           "disabled sidecar writes preprocessor definitions in normal INI syntax");
    expect(readFile(shaderPath).find("PreprocessorDefinitions=QUALITY=high,,fast") != std::string::npos,
           "escaped commas in macro values retain ReShade syntax");
    expect(readFile(shaderPath).find("Strength=0.25") == std::string::npos,
           "disabled effect values are omitted from main preset");
    expect(std::find_if(savedDisabledParams.begin(), savedDisabledParams.end(), [](const auto& param) {
               return param.effectName == "custom.fx" && param.paramName == "Strength" && param.value == "0.25";
           }) != savedDisabledParams.end(),
           "disabled effect value retains ReShade shader section in sidecar");
    bool foundMacro = false, foundStrength = false, foundVector0 = false, foundVector1 = false;
    for (const auto& param : shaderData.params)
    {
        foundMacro |= param.paramName == "@QUALITY" && param.value == "high,fast";
        foundStrength |= param.paramName == "Strength" && param.value == "0.75";
        foundVector0 |= param.paramName == "Tint[0]" && param.value == "0.1";
        foundVector1 |= param.paramName == "Tint[1]" && param.value == "0.2";
    }
    expect(foundMacro, "shader profile preserves comma-containing preprocessor values");
    expect(foundStrength && foundVector0 && foundVector1, "shader profile preserves scalar and vector values");

    // Two instances of one shader file collapse into a single filename section,
    // so their values are mirrored into the sidecar by instance name.
    const std::string dupPath = VkHook::ConfigSerializer::getShaderProfilePath("roundtrip-game", "dups");
    const std::map<std::string, std::string> dupPaths = {
        {"Bloom", "/shaders/bloom.fx"},
        {"Bloom.2", "/shaders/bloom.fx"},
    };
    const std::vector<VkHook::ConfigParam> dupParams = {
        {"Bloom", "Strength", "0.5"},
        {"Bloom", "@QUALITY", "high"},
        {"Bloom.2", "Strength", "0.9"},
        {"Bloom.2", "@QUALITY", "low"},
    };
    expect(VkHook::ConfigSerializer::saveShaderProfile(dupPath, dupParams, {"Bloom", "Bloom.2"}, {},
                                                        dupPaths, {}, {}, {}),
           "save duplicate-instance shader profile");
    const auto dupSidecar = VkHook::ConfigSerializer::loadShaderProfileData(
        VkHook::ConfigSerializer::getShaderProfileSidecarPath(dupPath));
    const auto hasParam = [](const std::vector<VkHook::ConfigParam>& params, const std::string& instance,
                             const std::string& name, const std::string& value) {
        return std::find_if(params.begin(), params.end(), [&](const auto& p) {
                   return p.effectName == instance && p.paramName == name && p.value == value;
               }) != params.end();
    };
    expect(hasParam(dupSidecar.params, "Bloom", "Strength", "0.5") &&
               hasParam(dupSidecar.params, "Bloom.2", "Strength", "0.9"),
           "duplicate instances keep per-instance values in the sidecar");
    expect(hasParam(dupSidecar.params, "Bloom", "@QUALITY", "high") &&
               hasParam(dupSidecar.params, "Bloom.2", "@QUALITY", "low"),
           "duplicate instances keep per-instance preprocessor definitions");
    expect(dupSidecar.instances.size() == 2, "duplicate instance list round-trips");

    // The sidecar carries VkHook's authoritative instance list: order,
    // kinds, and per-instance enable state, including built-ins.
    const std::string ownedPath = VkHook::ConfigSerializer::getShaderProfilePath("roundtrip-game", "owned");
    expect(VkHook::ConfigSerializer::saveShaderProfile(ownedPath, {}, {"Simple", "cas", "Custom"}, {"Custom"},
                                                        effectPaths, {}, {}, {}),
           "save vkhook-owned profile");
    const auto ownedData = VkHook::ConfigSerializer::loadShaderProfileData(
        VkHook::ConfigSerializer::getShaderProfileSidecarPath(ownedPath));
    expect(ownedData.owned && ownedData.instances.size() == 3,
           "sidecar marks the profile owned and lists every instance");
    expect(ownedData.instances[0].name == "Simple" && ownedData.instances[0].type == "simple.fx" &&
               ownedData.instances[0].enabled,
           "reshade instance round-trips with its filename");
    expect(ownedData.instances[1].name == "cas" && ownedData.instances[1].type == "cas",
           "built-in instance round-trips as its bare type");
    expect(ownedData.instances[2].name == "Custom" && !ownedData.instances[2].enabled,
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
    expect(!VkHook::ConfigSerializer::loadShaderProfileData(shaderPath).owned &&
               !VkHook::ConfigSerializer::loadShaderProfileData(ownedPath).owned,
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
    const auto importedData = VkHook::ConfigSerializer::loadShaderProfileData(importedPath);
    expect(!importedData.owned, "imported preset without a sidecar stays foreign");
    expect(importedData.hasTechniques && importedData.techniques.size() == 2 &&
               importedData.techniqueSorting.size() == 2,
           "ReShade preset lists parse with standard comma-separated spacing");
    expect(std::find_if(importedData.params.begin(), importedData.params.end(), [](const auto& param) {
               return param.effectName == "Example.fx" && param.paramName == "Strength" && param.value == "0.5";
           }) != importedData.params.end(),
           "ReShade shader-filename sections parse");
    expect(std::find_if(importedData.params.begin(), importedData.params.end(), [](const auto& param) {
               return param.effectName == "Example.fx" && param.paramName == "@QUALITY" && param.value == "2,4";
           }) != importedData.params.end(),
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
    const auto disabledTechniqueData = VkHook::ConfigSerializer::loadShaderProfileData(disabledTechniquePath);
    expect(disabledTechniqueData.hasTechniques && disabledTechniqueData.techniques.size() == 1 &&
               disabledTechniqueData.techniqueSorting.size() == 2,
           "disabled techniques remain represented in sorting while absent from enabled list");

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

    std::filesystem::remove_all(root);
    if (failures != 0)
    {
        std::cerr << failures << " config serializer test(s) failed\n";
        return 1;
    }

    std::cout << "Config serializer tests passed\n";
    return 0;
}
