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

    struct InterceptEntry
    {
        const char*       name;
        PFN_vkVoidFunction fn;
        InterceptGate     gate;
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

    // one per module that owns hooks
    InterceptTable instanceInterceptTable();   // layer/instance.cc
    InterceptTable deviceInterceptTable();     // layer/device.cc
    InterceptTable swapchainInterceptTable();  // present/swapchain.cc
    InterceptTable presentInterceptTable();    // present/present.cc
    InterceptTable submitInterceptTable();     // present/submit.cc
    InterceptTable depthInterceptTable();      // depth/hooks.cc
} // namespace VkHook
