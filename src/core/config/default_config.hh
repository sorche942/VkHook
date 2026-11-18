#ifndef DEFAULT_CONFIG_HH_INCLUDED
#define DEFAULT_CONFIG_HH_INCLUDED

#include <cstdint>
#include <string>

#include "config_serializer.hh"

// every shipped default lives here so config_serializer and the overlay can
// read them instead of carrying their own literals.
namespace VkHook::DefaultConfig
{
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
}

#endif // DEFAULT_CONFIG_HH_INCLUDED
