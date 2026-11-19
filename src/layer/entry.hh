#pragma once

// The intercept table. Every module that owns hooks exports a table of
// {name, function, gate}; entry.cc concatenates them behind
// vkGetInstanceProcAddr / vkGetDeviceProcAddr.

#include "vulkan_include.hh"

#include <cstddef>

namespace VkHook
{
    enum class InterceptGate
    {
        Unconditional,
        // Only returned while depth capture is on. The latch is fixed for the
        // process lifetime (see depthCaptureActive), so a game that does not use
        // depth capture never calls through these hooks at all.
        DepthCapture,
        // Only returned when the next layer / driver resolves the same name, so an
        // extension entry point the device did not enable stays null.
        Downstream,
    };

    // `extension` / `coreVersion` say what makes the command exist: the device
    // extension that provides it and/or the core Vulkan version that promoted it
    // (VK_API_VERSION_*). vkGetDeviceProcAddr returns null for the command
    // unless the device enabled that extension or its API version is at least
    // `coreVersion`. Entries with neither are always available.
    struct InterceptEntry
    {
        const char*        name;
        PFN_vkVoidFunction fn;
        InterceptGate      gate;
        const char*        extension   = nullptr;
        uint32_t           coreVersion = 0;
    };

    struct InterceptTable
    {
        const InterceptEntry* entries;
        size_t                count;
    };

    template <size_t N>
    constexpr InterceptTable makeInterceptTable(const InterceptEntry (&entries)[N])
    {
        return InterceptTable{entries, N};
    }

#define VKHOOK_INTERCEPT(gate, Name) \
    ::VkHook::InterceptEntry{"vk" #Name, reinterpret_cast<PFN_vkVoidFunction>(&VkHook_##Name), ::VkHook::InterceptGate::gate}

    // `ext` is a device extension name string (e.g. VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) or nullptr,
    // `core` a VK_API_VERSION_* constant or 0.
#define VKHOOK_INTERCEPT_EXT(gate, Name, ext, core) \
    ::VkHook::InterceptEntry{"vk" #Name, reinterpret_cast<PFN_vkVoidFunction>(&VkHook_##Name), ::VkHook::InterceptGate::gate, ext, core}

    // one per module that owns hooks
    InterceptTable instanceInterceptTable();   // layer/instance.cc
    InterceptTable deviceInterceptTable();     // layer/device.cc
    InterceptTable swapchainInterceptTable();  // present/swapchain.cc
    InterceptTable presentInterceptTable();    // present/present.cc
    InterceptTable submitInterceptTable();     // present/submit.cc
    InterceptTable depthInterceptTable();      // depth/hooks.cc
} // namespace VkHook
