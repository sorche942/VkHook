#pragma once

// Environment toggles read by the interceptors, and the one create-info helper
// shared by CreateDevice and CreateSwapchainKHR.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

#include "vulkan_include.hh"

namespace VkHook
{
    // true when the pNext chain already carries a struct of type `sType`
    inline bool pNextChainContainsSType(const void* pNext, VkStructureType sType)
    {
        const VkBaseInStructure* base = reinterpret_cast<const VkBaseInStructure*>(pNext);
        while (base)
        {
            if (base->sType == sType)
                return true;
            base = base->pNext;
        }
        return false;
    }

    inline bool parseBoolEnvValue(const std::string& value, bool defaultValue)
    {
        std::string s(value);
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (s == "1" || s == "true" || s == "yes" || s == "on")
            return true;
        if (s == "0" || s == "false" || s == "no" || s == "off")
            return false;
        return defaultValue;
    }

    inline bool getMutableSwapchainEnvOverride(bool& hasOverride, bool defaultValue)
    {
        const char* value = std::getenv("VKHOOK_ENABLE_MUTABLE_SWAPCHAIN");
        if (value == nullptr)
        {
            hasOverride = false;
            return defaultValue;
        }

        hasOverride = true;
        return parseBoolEnvValue(value, defaultValue);
    }

    inline bool isSwapchainDiagEnabled()
    {
        const char* value = std::getenv("VKHOOK_DIAG_SWAPCHAIN");
        if (value == nullptr)
            return false;
        return parseBoolEnvValue(value, false);
    }

    inline bool isGpuCrashDiagEnabled()
    {
        const char* value = std::getenv("VKHOOK_GPU_CRASH_DIAGNOSTICS");
        if (value == nullptr)
            return false;
        return parseBoolEnvValue(value, false);
    }
} // namespace VkHook
