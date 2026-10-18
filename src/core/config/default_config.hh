#ifndef DEFAULT_CONFIG_HH_INCLUDED
#define DEFAULT_CONFIG_HH_INCLUDED

#include <cstdint>
#include <fstream>
#include <string>

#include "config_serializer.hh"

// every shipped default lives here so config_serializer and the overlay can
// read them instead of carrying their own literals.
namespace VKIntox::DefaultConfig
{
    // the flatpak app id this process runs inside, or empty when not in a
    // flatpak sandbox. every process in a sandbox can read /.flatpak-info.
    inline std::string flatpakAppId()
    {
        std::ifstream info("/.flatpak-info");
        if (!info.is_open())
            return {};

        std::string line;
        bool inApplication = false;
        while (std::getline(info, line))
        {
            if (!line.empty() && line.front() == '[')
            {
                inApplication = (line == "[Application]");
                continue;
            }
            if (inApplication && line.rfind("name=", 0) == 0)
                return line.substr(5);
        }
        return {};
    }

    // depth capture is on by default so depth effects work on a game's first
    // launch instead of needing a restart.
    inline VkBasaltSettings appSettings()
    {
        VkBasaltSettings settings;
        settings.maxEffects = 10;
        settings.overlayBlockInput = true;
        settings.usePortalIntegration = true;
        settings.uiScale = 0.0f;
        settings.toggleKey = "End";
        settings.reloadKey = "F10";
        settings.overlayKey = "Home";
        settings.enableOnLaunch = true;
        settings.depthCapture = true;
        settings.autoApply = true;
        settings.autoApplyDelay = 200;
        settings.showDebugWindow = false;
        settings.separateEffectParams = true;
        settings.depthResolveMode = 0;
        settings.depthRebuildMode = 0;
        settings.depthManualPin.clear();
        settings.depthTransientWorkaround = true;
        settings.depthCaptureMethod = 2;
        settings.depthSourceChannel = 0;
        settings.depthInvert = false;
        settings.depthFlip = false;
        settings.depthFlop = false;
        return settings;
    }

    // per-game depth orientation. sober's depth reads inverted, and so does
    // the mac client butter runs through rosemary; macoblox (mldr) renders
    // upside down, so it starts flipped. flop stays off for every game.
    inline bool depthInvertForGame(const std::string& executableName)
    {
        return executableName == "sober" || executableName == "rosemary"
            || flatpakAppId() == "io.github.microlift.butter";
    }

    inline bool depthFlipForGame(const std::string& executableName)
    {
        return executableName == "mldr";
    }

    inline void applyGameDefaults(VkBasaltSettings& settings, const std::string& executableName)
    {
        settings.depthInvert = depthInvertForGame(executableName);
        settings.depthFlip = depthFlipForGame(executableName);
        settings.depthFlop = false;
    }
}

#endif // DEFAULT_CONFIG_HH_INCLUDED
