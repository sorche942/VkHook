#pragma once

// Swapchain lifetime: the hooks themselves are reached through
// swapchainInterceptTable(); this header is what the rest of the layer calls.

#include "vulkan_include.hh"
#include "logical_device.hh"

#include <string>
#include <vector>

namespace VkHook
{
    // Rebuild the effect chain of every faked swapchain of `logicalDevice` (all
    // devices when null) with `activeEffects`. Caller holds globalLock and has
    // established that the swapchains' in-flight effect submissions completed.
    void reloadAllSwapchains(LogicalDevice* logicalDevice, const std::vector<std::string>& activeEffects);
} // namespace VkHook
