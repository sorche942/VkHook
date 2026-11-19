#include "config_serializer.hh"
#include "default_config.hh"
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
#include <charconv>
#include <cmath>

namespace VkHook
{
    namespace
    {
        constexpr int MIN_MAX_EFFECTS = 1;
        constexpr int MAX_MAX_EFFECTS = 200;

    } // namespace

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
        if (configsDir.empty() || configName.empty() ||
            configName.find('/') != std::string::npos ||
            configName.find('\\') != std::string::npos ||
            configName.find("..") != std::string::npos)
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

    namespace
    {
        bool parseInt(const std::string& text, int& out)
        {
            int parsed = 0;
            const char* end = text.data() + text.size();
            auto [ptr, ec] = std::from_chars(text.data(), end, parsed);
            if (ec != std::errc() || ptr != end)
                return false;
            out = parsed;
            return true;
        }

        void applyBool(const std::string& key, const std::string& value, bool& target)
        {
            if (const auto parsed = Ini::parseBool(value))
                target = *parsed;
            else
                Logger::err("invalid " + key + " value: " + value);
        }

        void applyInt(const std::string& key, const std::string& value, int lo, int hi, int& target)
        {
            int parsed = 0;
            if (parseInt(value, parsed))
                target = std::clamp(parsed, lo, hi);
            else
                Logger::err("invalid " + key + " value: " + value);
        }

        // One owned key, in file order, with the comment that explains it.
        struct SettingEntry
        {
            const char* heading;  // section banner written before this key in a fresh file
            const char* comment;  // lines written right above the key; may be empty
            const char* key;
            std::string value;
        };

        std::string flag(bool value) { return value ? "true" : "false"; }

        std::vector<SettingEntry> settingEntries(const VkBasaltSettings& settings)
        {
            const int clampedMaxEffects = std::clamp(settings.maxEffects, MIN_MAX_EFFECTS, MAX_MAX_EFFECTS);
            std::ostringstream scale;
            scale << (std::isfinite(settings.uiScale) && settings.uiScale > 0.0f
                          ? std::clamp(settings.uiScale, 0.75f, 3.0f) : 0.0f);
            return {
                {"Overlay settings", "", "overlayBlockInput", flag(settings.overlayBlockInput)},
                {"", "", "usePortalIntegration", flag(settings.usePortalIntegration)},
                {"", "# uiScale: 0 = automatic; manual range 0.75 to 3.0\n", "uiScale", scale.str()},
                {"", "", "maxEffects", std::to_string(clampedMaxEffects)},
                {"", "", "autoApply", flag(settings.autoApply)},
                {"", "", "autoApplyDelay", std::to_string(settings.autoApplyDelay)},
                {"Key bindings", "", "toggleKey", Ini::formatValue(settings.toggleKey)},
                {"", "", "reloadKey", Ini::formatValue(settings.reloadKey)},
                {"", "", "overlayKey", Ini::formatValue(settings.overlayKey)},
                {"Startup behavior", "", "enableOnLaunch", flag(settings.enableOnLaunch)},
                {"", "", "depthCapture", settings.depthCapture ? "on" : "off"},
                {"Debug", "", "showDebugWindow", flag(settings.showDebugWindow)},
                {"Overlay layout", "", "separateEffectParams", flag(settings.separateEffectParams)},
                {"Advanced (depth buffer)",
                 "# depthResolveMode: 0=auto (prefer average), 1=sample-zero, 2=average\n",
                 "depthResolveMode", std::to_string(settings.depthResolveMode)},
                {"", "# depthRebuildMode: 0=safe (idle the queue before rebuilding), 1=fast (fence-gated)\n",
                 "depthRebuildMode", std::to_string(settings.depthRebuildMode)},
                {"", "", "depthManualPin", Ini::formatValue(settings.depthManualPin)},
                {"", "", "depthTransientWorkaround", flag(settings.depthTransientWorkaround)},
                {"", "# depthCaptureMethod: 0=off, 1=renderpass-end (recommended), 2=queue-submit (robust fallback)\n",
                 "depthCaptureMethod", std::to_string(settings.depthCaptureMethod)},
                {"Alternative depth buffer handling",
                 "# depthSourceChannel mode selection:\n"
                 "#   0 = Luminance/Red (standard Vulkan depth)\n"
                 "#   1 = Alpha (alpha-encoded depth)\n"
                 "#   2 = Packed RGB (depth in RGB channels)\n"
                 "#   3 = Logarithmic (log-encoded depth)\n"
                 "#   4 = View-space Z (raw view-space Z)\n"
                 "#   5 = NDC (Normalized Device Coordinates)\n"
                 "#   6 = Reversed-Z (inverted for precision)\n",
                 "depthSourceChannel", std::to_string(settings.depthSourceChannel)},
                {"", "# depthInvert: invert depth values (flips near/far planes)\n",
                 "depthInvert", flag(settings.depthInvert)},
                {"", "# depthFlip/depthFlop: mirror the depth image vertically/horizontally\n",
                 "depthFlip", flag(settings.depthFlip)},
                {"", "", "depthFlop", flag(settings.depthFlop)},
            };
        }

        std::string freshSettingsText(const std::vector<SettingEntry>& entries)
        {
            std::string text = "# VkHook configuration\n";
            for (const SettingEntry& entry : entries)
            {
                if (*entry.heading)
                    text += std::string("\n# ") + entry.heading + "\n";
                text += entry.comment;
                text += std::string(entry.key) + " = " + entry.value + "\n";
            }
            return text;
        }

        // Rewrites the owned keys of `existing` in place and appends the ones it
        // lacks. Everything else (unknown keys, comments, blank lines, order,
        // CRLF endings) is copied through untouched.
        std::string mergeSettingsText(const std::string& existing, const std::vector<SettingEntry>& entries)
        {
            std::map<std::string, const SettingEntry*> owned;
            for (const SettingEntry& entry : entries)
                owned[entry.key] = &entry;

            std::set<std::string> written;
            std::string out;
            size_t pos = 0;
            while (pos < existing.size())
            {
                size_t eol = existing.find('\n', pos);
                const bool hasNewline = eol != std::string::npos;
                if (!hasNewline)
                    eol = existing.size();
                std::string line = existing.substr(pos, eol - pos);
                pos = hasNewline ? eol + 1 : eol;

                const Ini::Line parsed = Ini::tokenize(line);
                const auto it = parsed.kind == Ini::LineKind::Pair ? owned.find(parsed.key) : owned.end();
                if (it == owned.end())
                {
                    out += line;
                }
                else
                {
                    out += std::string(it->second->key) + " = " + it->second->value;
                    if (!line.empty() && line.back() == '\r')
                        out += '\r';
                    written.insert(it->first);
                }
                if (hasNewline)
                    out += '\n';
            }

            std::string added;
            for (const SettingEntry& entry : entries)
            {
                if (written.count(entry.key))
                    continue;
                added += entry.comment;
                added += std::string(entry.key) + " = " + entry.value + "\n";
            }
            if (!added.empty())
            {
                if (!out.empty() && out.back() != '\n')
                    out += '\n';
                out += "\n# Added by VkHook\n" + added;
            }
            return out;
        }

        std::string slurp(const std::string& path)
        {
            std::ifstream file(path, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }
    } // namespace

    VkBasaltSettings ConfigSerializer::loadSettings()
    {
        VkBasaltSettings settings = DefaultConfig::appSettings();

        const std::string configPath = Paths::findSettingsFile();
        if (configPath.empty())
            return settings;

        Logger::info("SettingsManager loading from: " + configPath);

        std::vector<std::pair<std::string, std::string>> entries;
        if (!Ini::readFile(configPath, entries))
            return settings;

        for (const auto& [key, value] : entries)
        {
            if (key == "maxEffects")
            {
                int parsed = 0;
                if (!parseInt(value, parsed) || parsed < 0)
                {
                    Logger::err("invalid maxEffects value: " + value);
                    continue;
                }
                settings.maxEffects = std::clamp(parsed, MIN_MAX_EFFECTS, MAX_MAX_EFFECTS);
                if (settings.maxEffects != parsed)
                    Logger::warn("clamped maxEffects from " + std::to_string(parsed) + " to "
                                 + std::to_string(settings.maxEffects));
            }
            else if (key == "overlayBlockInput")
                applyBool(key, value, settings.overlayBlockInput);
            else if (key == "usePortalIntegration")
                applyBool(key, value, settings.usePortalIntegration);
            else if (key == "uiScale")
            {
                float scale = 0.0f;
                const char* end = value.data() + value.size();
                auto [ptr, ec] = std::from_chars(value.data(), end, scale);
                if (ec != std::errc() || ptr != end || !std::isfinite(scale) || scale < 0.0f)
                    Logger::err("invalid uiScale value: " + value);
                else
                    settings.uiScale = scale == 0.0f ? 0.0f : std::clamp(scale, 0.75f, 3.0f);
            }
            else if (key == "toggleKey")
                settings.toggleKey = value;
            else if (key == "reloadKey")
                settings.reloadKey = value;
            else if (key == "overlayKey")
                settings.overlayKey = value;
            else if (key == "enableOnLaunch")
                applyBool(key, value, settings.enableOnLaunch);
            else if (key == "depthCapture")
                applyBool(key, value, settings.depthCapture);
            else if (key == "autoApply")
                applyBool(key, value, settings.autoApply);
            else if (key == "autoApplyDelay")
            {
                int parsed = 0;
                if (parseInt(value, parsed))
                    settings.autoApplyDelay = parsed;
                else
                    Logger::err("invalid autoApplyDelay value: " + value);
            }
            else if (key == "showDebugWindow")
                applyBool(key, value, settings.showDebugWindow);
            else if (key == "separateEffectParams")
                applyBool(key, value, settings.separateEffectParams);
            else if (key == "depthResolveMode")
            {
                int parsed = 0;
                if (parseInt(value, parsed))
                    settings.depthResolveMode = parsed;
                else
                    Logger::err("invalid depthResolveMode value: " + value);
            }
            else if (key == "depthRebuildMode")
                applyInt(key, value, 0, 1, settings.depthRebuildMode);
            else if (key == "depthManualPin")
                settings.depthManualPin = value;
            else if (key == "depthTransientWorkaround")
                applyBool(key, value, settings.depthTransientWorkaround);
            else if (key == "depthCaptureMethod")
                applyInt(key, value, 0, 2, settings.depthCaptureMethod);
            else if (key == "depthSourceChannel")
                applyInt(key, value, 0, 6, settings.depthSourceChannel);
            else if (key == "depthInvert")
                applyBool(key, value, settings.depthInvert);
            else if (key == "depthFlip")
                applyBool(key, value, settings.depthFlip);
            else if (key == "depthFlop")
                applyBool(key, value, settings.depthFlop);
        }

        return settings;
    }

    bool ConfigSerializer::saveSettings(const VkBasaltSettings& settings)
    {
        const std::string baseDir = getBaseConfigDir();
        if (baseDir.empty())
        {
            Logger::err("Could not determine config directory");
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(baseDir, ec);

        // read-modify-write: the file belongs to the user, who may keep keys of
        // their own in it (reshadeIncludePath, extra effect definitions, notes)
        const std::string configPath = Paths::userSettingsFile();
        const bool userFileExists = std::filesystem::exists(configPath, ec);
        const std::string loadedFrom = userFileExists ? configPath : Paths::findSettingsFile();
        const std::string existing = loadedFrom.empty() ? std::string() : slurp(loadedFrom);

        const std::vector<SettingEntry> entries = settingEntries(settings);
        const std::string text = loadedFrom.empty() ? freshSettingsText(entries) : mergeSettingsText(existing, entries);

        if (userFileExists && text == existing)
            return true;  // nothing changed: leave the file and its mtime alone

        if (!writeAtomically(configPath, text))
        {
            Logger::err("Could not write VkHook.conf: " + configPath);
            return false;
        }

        Logger::debug("Saved settings to: " + configPath);
        return true;
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

    // Effect packages used to install under reshade/packages/. Merge that tree
    // into reshade/ once, keeping any file already at the destination so a newer
    // install is never clobbered, then drop the legacy tree only if every file
    // made it across. Safe to run on every launch: an incomplete copy is retried.
    void ConfigSerializer::migrateLegacyReshadePackages()
    {
        const std::string base = getBaseConfigDir();
        if (base.empty())
            return;

        const std::filesystem::path reshade = std::filesystem::path(base) / "reshade";
        const std::filesystem::path legacy = reshade / "packages";

        std::error_code ec;
        if (!std::filesystem::is_directory(legacy, ec) || ec)
            return;

        bool complete = true;
        std::filesystem::recursive_directory_iterator it(
            legacy, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec))
        {
            std::error_code entryError;
            if (it->is_symlink(entryError) || !it->is_regular_file(entryError) || entryError)
                continue;

            std::error_code relError;
            const auto relative = std::filesystem::relative(it->path(), legacy, relError);
            if (relError || relative.empty())
            {
                complete = false;
                continue;
            }

            const auto dest = reshade / relative;
            std::filesystem::create_directories(dest.parent_path(), entryError);
            if (entryError)
            {
                complete = false;
                continue;
            }
            if (std::filesystem::exists(dest, entryError))
                continue;  // the new layout wins

            std::filesystem::copy_file(it->path(), dest, std::filesystem::copy_options::none, entryError);
            if (entryError)
                complete = false;
        }

        if (!complete)
        {
            Logger::warn("VkHook: reshade/packages migration incomplete; leaving it in place");
            return;
        }

        std::error_code removeError;
        std::filesystem::remove_all(legacy, removeError);
        if (removeError)
            Logger::warn("VkHook: could not remove the legacy reshade/packages directory");
        else
            Logger::info("VkHook: migrated reshade/packages into reshade/");
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
            // Older setup versions wrote all discovered paths on one
            // comma-separated line, so accept that and one path per line.
            for (const auto& [key, value] : entries)
            {
                if (value.empty())
                    continue;
                if (key == "parentDir")
                    config.parentDirectories.push_back(value);
                else if (key == "shaderPath")
                {
                    for (auto& path : Ini::splitList(value, ','))
                        config.discoveredShaderPaths.push_back(std::move(path));
                }
                else if (key == "texturePath")
                {
                    for (auto& path : Ini::splitList(value, ','))
                        config.discoveredTexturePaths.push_back(std::move(path));
                }
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
