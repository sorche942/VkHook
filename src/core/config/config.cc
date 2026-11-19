#include "config.hh"

#include "ini.hh"
#include "logger.hh"
#include "paths.hh"
#include "util.hh"

#include <charconv>
#include <climits>

namespace VkHook
{
    Config::Config()
    {
        const std::string path = Paths::findSettingsFile();
        if (path.empty())
        {
            Logger::err("no VkHook.conf found");
            return;
        }
        Logger::info("base config: " + path);
        load(path);
    }

    Config::Config(const std::string& path)
    {
        if (!load(path))
        {
            Logger::err("failed to load config: " + path);
            return;
        }
        Logger::info("config: " + path);
    }

    bool Config::load(const std::string& path)
    {
        std::vector<std::pair<std::string, std::string>> entries;
        if (!Ini::readFile(path, entries))
            return false;
        configFilePath = path;
        options.clear();
        for (auto& [key, value] : entries)
            options[std::move(key)] = std::move(value);
        updateLastModifiedTime();
        return true;
    }

    void Config::updateLastModifiedTime()
    {
        if (configFilePath.empty())
            return;

        struct stat fileStat;
        if (stat(configFilePath.c_str(), &fileStat) == 0)
            lastModified = fileStat.st_mtim;
    }

    const std::string* Config::findLocal(const std::string& option) const
    {
        if (auto it = overrides.find(option); it != overrides.end())
            return &it->second;
        if (auto it = options.find(option); it != options.end())
            return &it->second;
        return nullptr;
    }

    bool Config::hasOptionKey(const std::string& option) const
    {
        if (findLocal(option))
            return true;
        return fallback ? fallback->hasOptionKey(option) : false;
    }

    bool Config::hasConfigChanged()
    {
        if (configFilePath.empty())
            return false;

        // Throttle stat() syscall to every 500ms instead of every frame.
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastConfigCheckTime).count() < 500)
            return false;
        lastConfigCheckTime = now;

        struct stat fileStat;
        if (stat(configFilePath.c_str(), &fileStat) != 0)
            return false;

        if (fileStat.st_mtim.tv_sec == lastModified.tv_sec && fileStat.st_mtim.tv_nsec == lastModified.tv_nsec)
            return false;

        if (isOwnWrite(configFilePath))
        {
            lastModified = fileStat.st_mtim;
            return false;
        }
        return true;
    }

    void Config::reload()
    {
        if (configFilePath.empty())
            return;

        // Parse into a scratch map first so a failed read keeps what we have
        std::vector<std::pair<std::string, std::string>> entries;
        if (!Ini::readFile(configFilePath, entries))
        {
            Logger::err("failed to reload config: " + configFilePath);
            return;
        }

        Logger::info("reloading config: " + configFilePath);
        if (entries.empty() && !options.empty())
        {
            Logger::warn("config reload produced empty options, keeping previous");
            updateLastModifiedTime();
            return;
        }

        options.clear();
        for (auto& [key, value] : entries)
            options[std::move(key)] = std::move(value);
        updateLastModifiedTime();
    }

    void Config::parseValue(const std::string& text, int32_t& result, const std::string& option)
    {
        const std::string value = Ini::trim(text);
        int32_t parsed = 0;
        const char* end = value.data() + value.size();
        auto [ptr, ec] = std::from_chars(value.data(), end, parsed);
        if (ec == std::errc() && ptr == end)
            result = parsed;
        else
            Logger::warn("invalid int32_t value for: " + option);
    }

    void Config::parseValue(const std::string& text, uint32_t& result, const std::string& option)
    {
        const std::string value = Ini::trim(text);
        uint32_t parsed = 0;
        const char* end = value.data() + value.size();
        auto [ptr, ec] = std::from_chars(value.data(), end, parsed);
        if (ec == std::errc() && ptr == end)
            result = parsed;
        else
            Logger::warn("invalid uint32_t value for: " + option);
    }

    void Config::parseValue(const std::string& text, float& result, const std::string& option)
    {
        std::string value = Ini::trim(text);
        // ReShade-style literals carry an optional 'f' suffix
        if (!value.empty() && (value.back() == 'f' || value.back() == 'F'))
            value.pop_back();
        float parsed = 0.0f;
        const char* end = value.data() + value.size();
        auto [ptr, ec] = std::from_chars(value.data(), end, parsed);
        if (ec == std::errc() && ptr == end)
            result = parsed;
        else
            Logger::warn("invalid float value for: " + option);
    }

    void Config::parseValue(const std::string& text, bool& result, const std::string& option)
    {
        if (const auto parsed = Ini::parseBool(text))
            result = *parsed;
        else
            Logger::warn("invalid bool value for: " + option);
    }

    void Config::parseValue(const std::string& text, std::string& result, const std::string&)
    {
        result = text;
    }

    void Config::parseValue(const std::string& text, std::vector<std::string>& result, const std::string&)
    {
        result = Ini::splitList(text, ':');
    }

    void Config::setOverride(const std::string& option, const std::string& value)
    {
        overrides[option] = value;
    }

    void Config::clearOverrides()
    {
        overrides.clear();
    }

    std::unordered_map<std::string, std::string> Config::getEffectDefinitions() const
    {
        std::unordered_map<std::string, std::string> effects = fallback
            ? fallback->getEffectDefinitions()
            : std::unordered_map<std::string, std::string>{};
        for (const auto& [key, value] : options)
        {
            if (value.size() >= 3 && value.substr(value.size() - 3) == ".fx")
                effects[key] = value;
        }
        return effects;
    }

} // namespace VkHook
