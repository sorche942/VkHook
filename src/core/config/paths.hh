#ifndef VKHOOK_CONFIG_PATHS_HH_INCLUDED
#define VKHOOK_CONFIG_PATHS_HH_INCLUDED

#include <array>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

#include "config_paths.hh"

// The one XDG resolver. Config, ConfigSerializer and Logger all ask here, so a
// given environment always yields the same file. Header-only because the
// logger is linked into nearly every test binary.
namespace VkHook::Paths
{
    // An unset or empty variable is "unset", per the XDG base directory spec.
    inline std::string env(const char* name)
    {
        const char* value = std::getenv(name);
        return value && *value ? std::string(value) : std::string();
    }

    inline std::string configHome()
    {
        if (std::string xdg = env("XDG_CONFIG_HOME"); !xdg.empty())
            return xdg;
        if (std::string home = env("HOME"); !home.empty())
            return home + "/.config";
        return "";
    }

    inline std::string dataHome()
    {
        if (std::string xdg = env("XDG_DATA_HOME"); !xdg.empty())
            return xdg;
        if (std::string home = env("HOME"); !home.empty())
            return home + "/.local/share";
        return "";
    }

    // Regenerable data: the shader and pipeline caches live under it
    inline std::string cacheHome()
    {
        if (std::string xdg = env("XDG_CACHE_HOME"); !xdg.empty())
            return xdg;
        if (std::string home = env("HOME"); !home.empty())
            return home + "/.cache";
        return "";
    }

    // ~/.config/VkHook, where every user-owned file lives
    inline std::string baseConfigDir()
    {
        const std::string home = configHome();
        return home.empty() ? std::string() : home + "/VkHook";
    }

    // The file VkHook writes settings to
    inline std::string userSettingsFile()
    {
        const std::string base = baseConfigDir();
        return base.empty() ? std::string() : base + "/VkHook.conf";
    }

    // Every place VkHook.conf may be read from, most specific first
    inline std::array<std::string, 5> settingsSearchPaths()
    {
        const std::string data = dataHome();
        return {
            userSettingsFile(),
            data.empty() ? std::string() : data + "/VkHook/VkHook.conf",
            std::string(SYSCONFDIR) + "/VkHook.conf",
            std::string(SYSCONFDIR) + "/VkHook/VkHook.conf",
            std::string(DATADIR) + "/VkHook/VkHook.conf",
        };
    }

    // First existing candidate, or empty
    inline std::string findSettingsFile()
    {
        for (const std::string& path : settingsSearchPaths())
        {
            struct stat st;
            if (!path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
                return path;
        }
        return "";
    }

    inline std::string logFile()
    {
        if (std::string file = env("VKHOOK_LOG_FILE"); !file.empty())
            return file;
        const std::string base = baseConfigDir();
        return base.empty() ? std::string("stderr") : base + "/vkhook.log";
    }
} // namespace VkHook::Paths

#endif // VKHOOK_CONFIG_PATHS_HH_INCLUDED
