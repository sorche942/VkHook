#pragma once

#include "gpu/retire.hh"

namespace VkHook
{
    struct LogicalDevice;

    // Binds the device's RetireQueue to the Vulkan destroy calls of `device`.
    // Called once while the device is created.
    void bindRetireDestroyer(LogicalDevice& device);
} // namespace VkHook
