#ifndef CONFIG_SERIALIZER_HPP_INCLUDED
#define CONFIG_SERIALIZER_HPP_INCLUDED

#include <string>
#include <vector>
#include <map>

#include "effects/effect_config.hh"

namespace VkHook
{
    // Serialized parameter format for config files
    struct ConfigParam
    {
        std::string effectName;
        std::string paramName;
        std::string value;
    };

    // Shader Manager configuration (from shader_manager.conf)
    struct ShaderManagerConfig
    {
        std::vector<std::string> parentDirectories;       // User-added parent dirs to scan
        std::vector<std::string> discoveredShaderPaths;   // Auto-discovered Shaders/ dirs
        std::vector<std::string> discoveredTexturePaths;  // Auto-discovered Textures/ dirs
    };

    class ConfigSerializer
    {
    public:
        // Save a game-specific config to ~/.config/VkHook/configs/<name>.conf
        // effects: all effects in the list (enabled + disabled)
        // disabledEffects: effects that are unchecked (won't be rendered)
        // params: all effect parameters
        // effectPaths: map of effect name to shader file path (for ReShade effects with custom names)
        // preprocessorDefs: preprocessor definitions to save (format: effectName#MACRO = value)
        static bool saveConfig(
            const std::string& configName,
            const std::vector<std::string>& effects,
            const std::vector<std::string>& disabledEffects,
            const std::vector<ConfigParam>& params,
            const std::map<std::string, std::string>& effectPaths = {},
            const std::vector<PreprocessorDefinition>& preprocessorDefs = {});

        // Get the base config directory path (~/.config/VkHook/)
        static std::string getBaseConfigDir();

        // Get the configs directory path (~/.config/VkHook/configs/)
        static std::string getConfigsDir();

        // List available config files
        static std::vector<std::string> listConfigs();

        // Delete a config file
        static bool deleteConfig(const std::string& configName);

        // Default config management
        static bool setDefaultConfig(const std::string& configName);
        static std::string getDefaultConfig();
        static std::string getDefaultConfigPath();

        // Shader Manager config (shader_manager.conf). Loading never writes.
        static ShaderManagerConfig loadShaderManagerConfig();
        static bool saveShaderManagerConfig(const ShaderManagerConfig& config);

        // Write the running layer version to <config>/version. Keeps the config
        // dir identifiable when the setup script did not seed it, and refreshes
        // a stale file after an upgrade.
        static bool ensureVersionFile();

        // Expand shader root directories to include their subdirectories, so a
        // shader can include a header that lives inside another package's folder.
        static std::vector<std::string> expandShaderIncludePaths(const std::vector<std::string>& roots);

        // Detect the game executable name from /proc/self/exe
        static std::string detectGameName();

        // Check if a per-game config exists and return the game name if so.
        // Returns empty string if no per-game config was found.
        static std::string autoDetectConfig();

        // --- Per-app profile system ---

        // Get the fixed config path for a game: configs/<gameName>.conf
        static std::string getProfilePath(const std::string& gameName);

        // Ensure the default profile for a game exists (creates with empty
        // effects list if missing). Returns the profile path.
        static std::string ensureGameProfile(const std::string& gameName);

        static std::string getShaderProfilePath(const std::string& gameName, const std::string& profileName);

        // Hidden sidecar beside a shader profile holding VkHook-only state:
        // the ordered instance list and values for disabled effects. ReShade
        // never reads it, so the .ini stays cross-compatible.
        static std::string getShaderProfileSidecarPath(const std::string& path);

        // Path createShaderProfile would write for this game and name; unlike
        // getShaderProfilePath it never resolves to an imported global preset.
        static std::string getGameShaderProfilePath(const std::string& gameName, const std::string& profileName);

        // Where renameShaderProfile moves a preset: the same kind (game-specific
        // or imported global), under the new name. Empty when oldName is unknown.
        static std::string getRenamedShaderProfilePath(const std::string& gameName, const std::string& oldName,
                                                       const std::string& newName);

        static std::vector<std::string> listShaderProfilesForGame(const std::string& gameName);
        static bool setLastShaderProfile(const std::string& gameName, const std::string& profileName);
        static std::string getLastShaderProfile(const std::string& gameName);
        static bool createShaderProfile(const std::string& gameName, const std::string& profileName,
                                        const std::string& copyFromProfile = "");
        static bool deleteShaderProfile(const std::string& gameName, const std::string& profileName);

        // One path component: non-empty, not "." or "..", no '/' or '\\' and no
        // control bytes. The single check every game/profile name goes through.
        static bool isSafePathComponent(const std::string& name);

        // A usable preset name: a safe path component without '@' (the
        // game/profile separator).
        static bool isValidShaderProfileName(const std::string& name);

        // Rename a preset in place, keeping its game-specific or imported-global
        // kind, carrying its sidecar, and repointing the game's last-used file.
        // Refuses an invalid or already-taken name and rolls back a partial
        // rename, so on failure every file is exactly where it started.
        static bool renameShaderProfile(const std::string& gameName, const std::string& oldName,
                                        const std::string& newName);

        // Copy an external ReShade preset into configs/shaders as a global
        // (game-agnostic) <name>.ini. never overwrites: an existing name gets a
        // numeric suffix. returns the imported profile name, or empty on failure.
        static std::string importShaderProfile(const std::string& sourcePath);
        // Path importShaderProfile writes for an imported preset called `name`
        static std::string getImportedShaderProfilePath(const std::string& name);

        // The two files of a preset (.ini plus hidden sidecar), as written by
        // preset_model.hh's serialize(). Creates the directory; both files are
        // written atomically. The caller routes the call through the PersistenceQueue.
        static bool writeShaderProfile(const std::string& path, const std::string& ini,
                                       const std::string& sidecar);
        // Raw text of both files; a missing file reads as empty.
        static void readShaderProfile(const std::string& path, std::string& ini, std::string& sidecar);

        // Save directly to a profile path (bypasses config name lookup)
        static bool saveToPath(
            const std::string& filePath,
            const std::vector<std::string>& effects,
            const std::vector<std::string>& disabledEffects,
            const std::vector<ConfigParam>& params,
            const std::map<std::string, std::string>& effectPaths = {},
            const std::vector<PreprocessorDefinition>& preprocessorDefs = {});
    };

} // namespace VkHook

#endif // CONFIG_SERIALIZER_HPP_INCLUDED
