#ifndef CONFIG_HPP_INCLUDED
#define CONFIG_HPP_INCLUDED
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <sys/stat.h>

#include "vulkan_include.hh"

namespace VkHook
{
    // A key/value file in the format ini.hh describes, plus in-memory overrides
    // and an optional fallback config for keys this one does not define.
    class Config
    {
    public:
        Config();  // Finds and loads VkHook.conf
        Config(const std::string& path);  // Loads specific config file
        Config(const Config&) = delete;
        Config& operator=(const Config&) = delete;

        // Set a fallback config for options not found in this config
        void setFallback(Config* fallbackConfig) { fallback = fallbackConfig; }
        void setOption(const std::string& option, const std::string& value) { options[option] = value; }

        template<typename T>
        T getOption(const std::string& option, const T& defaultValue = {}) const
        {
            // In-memory overrides win, then this file, then the fallback
            if (const std::string* text = findLocal(option))
            {
                T result = defaultValue;
                parseValue(*text, result, option);
                return result;
            }
            if (fallback)
                return fallback->getOption(option, defaultValue);
            return defaultValue;
        }

        // Effect parameter lookup: looks for "effectName.paramName"
        template<typename T>
        T getInstanceOption(const std::string& effectName, const std::string& paramName, const T& defaultValue = {}) const
        {
            const std::string dotKey = effectName + "." + paramName;
            if (hasOptionKey(dotKey))
                return getOption<T>(dotKey, defaultValue);

            const std::string atKey = effectName + "@" + paramName;
            if (hasOptionKey(atKey))
                return getOption<T>(atKey, defaultValue);

            return defaultValue;
        }

        // In-memory override support (does not modify config file)
        void setOverride(const std::string& option, const std::string& value);
        void clearOverrides();
        bool hasOverrides() const { return !overrides.empty(); }

        // Hot-reload support. VkHook's own writes (see isOwnWrite) are not
        // changes: the in-memory state they came from is already current.
        bool        hasConfigChanged();
        void        reload();
        std::string getConfigFilePath() const { return configFilePath; }

        // Get all effect definitions (keys whose values are .fx file paths)
        std::unordered_map<std::string, std::string> getEffectDefinitions() const;

        // Whether this config or its fallback defines the given option.
        bool hasOption(const std::string& option) const { return hasOptionKey(option); }

    private:
        std::unordered_map<std::string, std::string> options;
        std::unordered_map<std::string, std::string> overrides;  // In-memory overrides
        std::string                                  configFilePath;
        timespec                                     lastModified{};  // st_mtim, nanosecond resolution
        Config*                                      fallback = nullptr;
        std::chrono::steady_clock::time_point        lastConfigCheckTime{};  // Throttle stat() calls

        bool load(const std::string& path);
        void updateLastModifiedTime();
        bool hasOptionKey(const std::string& option) const;
        const std::string* findLocal(const std::string& option) const;

        static void parseValue(const std::string& text, int32_t& result, const std::string& option);
        static void parseValue(const std::string& text, uint32_t& result, const std::string& option);
        static void parseValue(const std::string& text, float& result, const std::string& option);
        static void parseValue(const std::string& text, bool& result, const std::string& option);
        static void parseValue(const std::string& text, std::string& result, const std::string& option);
        static void parseValue(const std::string& text, std::vector<std::string>& result, const std::string& option);
    };
} // namespace VkHook

#endif // CONFIG_HPP_INCLUDED
