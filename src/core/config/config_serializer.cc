#include "config_serializer.hh"
#include "settings_schema.hh"
#include "ini.hh"
#include "paths.hh"
#include "c_resource.hh"
#include "logger.hh"
#include "util.hh"
#include "version.hh"

#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <sys/stat.h>
#include <dirent.h>
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include <climits>
#include <cerrno>
#include <cctype>

namespace VkHook
{
    std::string ConfigSerializer::getBaseConfigDir()
    {
        return Paths::baseConfigDir();
    }

    std::string ConfigSerializer::getConfigsDir()
    {
        std::string baseDir = getBaseConfigDir();
        if (baseDir.empty())
            return "";
        return baseDir + "/configs";
    }

    std::vector<std::string> ConfigSerializer::listConfigs()
    {
        std::vector<std::string> configs;
        std::string dir = getConfigsDir();

        UniqueDir d(opendir(dir.c_str()));
        if (!d)
            return configs;

        struct dirent* entry;
        while ((entry = readdir(d.get())) != nullptr)
        {
            std::string name = entry->d_name;
            if (name.size() > 5 && name.substr(name.size() - 5) == ".conf")
                configs.push_back(name.substr(0, name.size() - 5));
        }

        std::sort(configs.begin(), configs.end());
        return configs;
    }

    static std::string joinEffects(const std::vector<std::string>& effects)
    {
        std::string result;
        for (size_t i = 0; i < effects.size(); i++)
        {
            if (i > 0)
                result += ":";
            result += effects[i];
        }
        return Ini::formatValue(result);
    }

    bool ConfigSerializer::saveConfig(
        const std::string& configName,
        const std::vector<std::string>& effects,
        const std::vector<std::string>& disabledEffects,
        const std::vector<ConfigParam>& params,
        const std::map<std::string, std::string>& effectPaths,
        const std::vector<PreprocessorDefinition>& preprocessorDefs)
    {
        std::string configsDir = getConfigsDir();
        if (configsDir.empty())
        {
            Logger::err("Could not determine configs directory");
            return false;
        }

        mkdir(configsDir.c_str(), 0755);

        std::string filePath = configsDir + "/" + configName + ".conf";
        bool result = saveToPath(filePath, effects, disabledEffects, params, effectPaths, preprocessorDefs);
        if (result)
            Logger::info("Saved config to: " + filePath);
        return result;
    }

    bool ConfigSerializer::deleteConfig(const std::string& configName)
    {
        std::string configsDir = getConfigsDir();
        if (configsDir.empty() || !isSafePathComponent(configName))
            return false;

        std::string filePath = configsDir + "/" + configName + ".conf";
        if (std::remove(filePath.c_str()) == 0)
        {
            Logger::info("Deleted config: " + filePath);
            return true;
        }
        Logger::err("Failed to delete config: " + filePath);
        return false;
    }

    std::string ConfigSerializer::getDefaultConfigPath()
    {
        const std::string baseDir = getBaseConfigDir();
        if (baseDir.empty())
            return "";
        return baseDir + "/default_config";
    }

    bool ConfigSerializer::setDefaultConfig(const std::string& configName)
    {
        std::string path = getDefaultConfigPath();
        if (path.empty())
            return false;

        if (!writeAtomically(path, configName))
        {
            Logger::err("Could not write default config file: " + path);
            return false;
        }

        Logger::info("Set default config: " + configName);
        return true;
    }

    std::string ConfigSerializer::getDefaultConfig()
    {
        std::string path = getDefaultConfigPath();
        if (path.empty())
            return "";

        std::ifstream file(path);
        if (!file.is_open())
            return "";

        std::string configName;
        std::getline(file, configName);
        return configName;
    }

    bool ConfigSerializer::ensureVersionFile()
    {
        const std::string version = VKHOOK_VERSION;
        if (version.empty() || version == "unknown")
            return false;

        const std::string baseDir = getBaseConfigDir();
        if (baseDir.empty())
            return false;

        const std::string path = baseDir + "/version";

        // leave a correct file alone so a read-only config dir stays quiet
        std::ifstream existing(path);
        std::string current;
        if (existing.is_open() && std::getline(existing, current))
        {
            if (!current.empty() && current.back() == '\r')
                current.pop_back();
            if (current == version)
                return true;
        }

        mkdir(baseDir.c_str(), 0755);
        if (!writeAtomically(path, version + "\n"))
        {
            Logger::err("Could not write version file: " + path);
            return false;
        }

        return true;
    }

    std::string ConfigSerializer::detectGameName()
    {
        char buf[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len <= 0)
            return "";

        buf[len] = '\0';
        std::string exePath(buf);

        // Extract basename
        size_t lastSlash = exePath.rfind('/');
        if (lastSlash == std::string::npos)
            return exePath;

        return exePath.substr(lastSlash + 1);
    }

    std::string ConfigSerializer::autoDetectConfig()
    {
        std::string gameName = detectGameName();
        if (gameName.empty())
            return "";

        std::string configsDir = getConfigsDir();
        if (configsDir.empty())
            return "";

        std::string configPath = configsDir + "/" + gameName + ".conf";
        struct stat st;
        if (stat(configPath.c_str(), &st) != 0)
            return "";

        Logger::info("Auto-loaded config for: " + gameName);
        return gameName;
    }

    // Case-insensitive string comparison helper
    static bool equalsIgnoreCaseLocal(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++)
        {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        }
        return true;
    }

    // Scan a directory recursively for Shaders/ and Textures/ subdirectories.
    // For each Shaders/ folder we also add its parent, so shaders that live in
    // a subfolder and `#include "ReShade.fxh"` (with the .fxh sitting next to
    // the Shaders/ directory) resolve correctly. Without the parent entry the
    // preprocessor only knows about the Shaders/ subfolder itself.
    static void scanDirectoryForShaders(
        const std::string& dir,
        std::vector<std::string>& shaderPaths,
        std::vector<std::string>& texturePaths)
    {
        try
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                dir, std::filesystem::directory_options::skip_permission_denied))
            {
                if (!entry.is_directory())
                    continue;

                std::string dirName = entry.path().filename().string();
                if (equalsIgnoreCaseLocal(dirName, "Shaders"))
                {
                    shaderPaths.push_back(entry.path().string());
                    // Add the parent directory too — this is where shared
                    // headers like ReShade.fxh typically live. De-duplicated
                    // below so we don't accumulate duplicates across packages.
                    std::string parent = entry.path().parent_path().string();
                    if (std::find(shaderPaths.begin(), shaderPaths.end(), parent) == shaderPaths.end())
                        shaderPaths.push_back(parent);
                }
                else if (equalsIgnoreCaseLocal(dirName, "Textures"))
                {
                    texturePaths.push_back(entry.path().string());
                }
            }
        }
        catch (const std::filesystem::filesystem_error& e)
        {
            Logger::err("Error scanning directory " + dir + ": " + e.what());
        }
    }

    std::vector<std::string> ConfigSerializer::expandShaderIncludePaths(const std::vector<std::string>& roots)
    {
        // one expansion per compile would mean rescanning the whole shader tree
        // each time, so memoise by the root set
        static std::mutex cacheMutex;
        static std::map<std::string, std::vector<std::string>> cache;

        std::string key;
        for (const std::string& root : roots)
            key += root + "\n";

        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            auto it = cache.find(key);
            if (it != cache.end())
                return it->second;
        }

        std::vector<std::string> result;
        std::set<std::string> seen;
        auto addExisting = [&](const std::string& path) {
            if (path.empty())
                return;
            std::error_code ec;
            if (!std::filesystem::is_directory(path, ec) || ec)
                return;
            const std::string normalized = std::filesystem::path(path).lexically_normal().string();
            if (seen.insert(normalized).second)
                result.push_back(normalized);
        };

        for (const std::string& root : roots)
        {
            addExisting(root);
            // headers live inside the per-package directories, so they have to be
            // searchable even when the shader sits in a different package
            std::error_code ec;
            std::filesystem::recursive_directory_iterator it(
                root, std::filesystem::directory_options::skip_permission_denied, ec);
            const std::filesystem::recursive_directory_iterator end;
            for (; !ec && it != end; it.increment(ec))
            {
                std::error_code entryError;
                if (it->is_directory(entryError) && !entryError)
                    addExisting(it->path().string());
            }
        }

        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            cache[key] = result;
        }
        return result;
    }

    ShaderManagerConfig ConfigSerializer::loadShaderManagerConfig()
    {
        // Loading is a pure read: it never writes the file or creates
        // directories. Callers that want a refreshed scan persisted call
        // saveShaderManagerConfig through the PersistenceQueue.
        struct Cache
        {
            std::mutex mutex;
            ShaderManagerConfig config;
            std::filesystem::file_time_type modifiedTime{};
            std::string path;
            bool exists = false;
            bool valid = false;
        };
        static Cache cache;

        const std::string configPath = getBaseConfigDir() + "/shader_manager.conf";
        std::error_code timestampError;
        const auto modifiedTime = std::filesystem::last_write_time(configPath, timestampError);
        const bool exists = !timestampError;

        {
            std::lock_guard<std::mutex> lock(cache.mutex);
            if (cache.valid && cache.path == configPath && cache.exists == exists
                && (!exists || cache.modifiedTime == modifiedTime))
                return cache.config;
        }

        ShaderManagerConfig config;

        auto refreshDiscoveredPaths = [&]() {
            std::set<std::string> shaderPaths;
            std::set<std::string> texturePaths;

            auto addExistingPaths = [](const std::vector<std::string>& paths, std::set<std::string>& output) {
                for (const auto& path : paths)
                {
                    std::error_code ec;
                    if (std::filesystem::is_directory(path, ec))
                        output.insert(std::filesystem::path(path).lexically_normal().string());
                }
            };

            addExistingPaths(config.discoveredShaderPaths, shaderPaths);
            addExistingPaths(config.discoveredTexturePaths, texturePaths);

            // Refresh paths on load so newly installed or moved shader packs
            // are available immediately, without requiring the UI rescan.
            for (const auto& parentDir : config.parentDirectories)
            {
                std::error_code ec;
                if (!std::filesystem::is_directory(parentDir, ec))
                    continue;

                std::vector<std::string> foundShaders;
                std::vector<std::string> foundTextures;
                scanDirectoryForShaders(parentDir, foundShaders, foundTextures);
                addExistingPaths(foundShaders, shaderPaths);
                addExistingPaths(foundTextures, texturePaths);
            }

            config.discoveredShaderPaths.assign(shaderPaths.begin(), shaderPaths.end());
            config.discoveredTexturePaths.assign(texturePaths.begin(), texturePaths.end());
        };

        std::vector<std::pair<std::string, std::string>> entries;
        if (!exists || !Ini::readFile(configPath, entries))
        {
            // No file yet: the default reshade directory is the only parent
            config.parentDirectories.push_back(getBaseConfigDir() + "/reshade");
        }
        else
        {
            // one path per line; the migration to schema v1 split the
            // comma-separated form older setups wrote
            for (const auto& [key, value] : entries)
            {
                if (value.empty())
                    continue;
                if (key == "parentDir")
                    config.parentDirectories.push_back(value);
                else if (key == "shaderPath")
                    config.discoveredShaderPaths.push_back(value);
                else if (key == "texturePath")
                    config.discoveredTexturePaths.push_back(value);
            }
        }
        refreshDiscoveredPaths();

        {
            std::lock_guard<std::mutex> lock(cache.mutex);
            cache.config = config;
            cache.path = configPath;
            cache.exists = exists;
            cache.modifiedTime = exists ? modifiedTime : std::filesystem::file_time_type{};
            cache.valid = true;
        }
        return config;
    }

    bool ConfigSerializer::saveShaderManagerConfig(const ShaderManagerConfig& config)
    {
        std::string baseDir = getBaseConfigDir();
        if (baseDir.empty())
        {
            Logger::err("Could not determine config directory");
            return false;
        }

        mkdir(baseDir.c_str(), 0755);

        std::string configPath = baseDir + "/shader_manager.conf";
        std::ostringstream file;

        file << "# Shader Manager configuration\n";
        file << "# " << Schema::kVersionKey << ": the file layout generation\n";
        file << Schema::kVersionKey << " = " << Schema::kCurrentVersion << "\n";
        file << "# Parent directories are scanned recursively for Shaders/ and Textures/ subdirs\n\n";

        file << "# Parent directories (user-added)\n";
        for (const auto& dir : config.parentDirectories)
            file << "parentDir = " << Ini::formatValue(dir) << "\n";

        file << "\n# Discovered shader paths (auto-generated on scan)\n";
        for (const auto& path : config.discoveredShaderPaths)
            file << "shaderPath = " << Ini::formatValue(path) << "\n";

        file << "\n# Discovered texture paths (auto-generated on scan)\n";
        for (const auto& path : config.discoveredTexturePaths)
            file << "texturePath = " << Ini::formatValue(path) << "\n";

        if (!file.good() || !writeAtomically(configPath, file.str()))
        {
            Logger::err("Could not write shader_manager.conf: " + configPath);
            return false;
        }

        Logger::debug("Saved shader manager config to: " + configPath);
        return true;
    }

    bool ConfigSerializer::saveToPath(
        const std::string& filePath,
        const std::vector<std::string>& effects,
        const std::vector<std::string>& disabledEffects,
        const std::vector<ConfigParam>& params,
        const std::map<std::string, std::string>& effectPaths,
        const std::vector<PreprocessorDefinition>& preprocessorDefs)
    {
        std::ostringstream file;

        // Group params by effect
        std::map<std::string, std::vector<const ConfigParam*>> paramsByEffect;
        for (const auto& param : params)
            paramsByEffect[param.effectName].push_back(&param);

        // Group preprocessor defs by effect
        std::map<std::string, std::vector<const PreprocessorDefinition*>> defsByEffect;
        for (const auto& def : preprocessorDefs)
            defsByEffect[def.effectName].push_back(&def);

        // Write params grouped by effect
        for (const auto& [effectName, effectParams] : paramsByEffect)
        {
            file << "# " << effectName << "\n";
            auto pathIt = effectPaths.find(effectName);
            if (pathIt != effectPaths.end() && !pathIt->second.empty())
                file << effectName << " = " << Ini::formatValue(pathIt->second) << "\n";
            for (const auto* param : effectParams)
                file << param->effectName << "." << param->paramName << " = " << Ini::formatValue(param->value) << "\n";
            auto defsIt = defsByEffect.find(effectName);
            if (defsIt != defsByEffect.end())
            {
                for (const auto* def : defsIt->second)
                    file << def->effectName << "@" << def->name << " = " << Ini::formatValue(def->value) << "\n";
            }
            file << "\n";
        }

        // Write preprocessor defs for effects that have defs but no params
        for (const auto& [effectName, effectDefs] : defsByEffect)
        {
            if (paramsByEffect.find(effectName) != paramsByEffect.end())
                continue;
            file << "# " << effectName << "\n";
            auto pathIt = effectPaths.find(effectName);
            if (pathIt != effectPaths.end() && !pathIt->second.empty())
                file << effectName << " = " << Ini::formatValue(pathIt->second) << "\n";
            for (const auto* def : effectDefs)
                file << def->effectName << "@" << def->name << " = " << Ini::formatValue(def->value) << "\n";
            file << "\n";
        }

        // Write paths for effects that have no params or defs
        for (const auto& [effectName, path] : effectPaths)
        {
            if (!path.empty() &&
                paramsByEffect.find(effectName) == paramsByEffect.end() &&
                defsByEffect.find(effectName) == defsByEffect.end())
            {
                file << "# " << effectName << "\n";
                file << effectName << " = " << Ini::formatValue(path) << "\n\n";
            }
        }

        // Write effects list
        file << "effects = " << joinEffects(effects) << "\n";

        if (!disabledEffects.empty())
            file << "disabledEffects = " << joinEffects(disabledEffects) << "\n";

        if (!file.good() || !writeAtomically(filePath, file.str()))
        {
            Logger::err("Failed to write config to: " + filePath);
            return false;
        }

        return true;
    }

} // namespace VkHook
