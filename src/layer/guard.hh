#pragma once

// Entry-point exception guards (AGENTS.md invariant 10): no exception crosses
// the C ABI. Layer work inside a hook runs under guardLayerWork(); a throw is
// caught, logged once, and soft-disables the device. The hook then still
// forwards the application's call, so the game keeps running without effects.

#include "diag/gpu_crash.hh"
#include "logical_device.hh"
#include "logger.hh"

#include <exception>
#include <string>

namespace VkHook
{
    // Logs the exception once per device and soft-disables it.
    inline void reportHookException(LogicalDevice* device, const char* entryPoint, const char* what) noexcept
    {
        try
        {
            if (device)
            {
                if (device->softDisabled.load(std::memory_order_acquire))
                    return;  // already off; the first exception was logged
                panicLayer(device, std::string(entryPoint) + " threw: " + (what ? what : "unknown exception"));
            }
            else
                Logger::err(std::string(entryPoint) + " threw: " + (what ? what : "unknown exception"));
        }
        catch (...)
        {
        }
    }

    // Runs `work`; true when it completed. `device` may be null.
    template <typename Work>
    bool guardLayerWork(LogicalDevice* device, const char* entryPoint, Work&& work) noexcept
    {
        try
        {
            work();
            return true;
        }
        catch (const std::exception& e)
        {
            reportHookException(device, entryPoint, e.what());
        }
        catch (...)
        {
            reportHookException(device, entryPoint, nullptr);
        }
        return false;
    }
} // namespace VkHook
