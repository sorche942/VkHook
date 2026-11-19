#pragma once

#include "gpu/retire.hh"

namespace VkHook
{
    struct LogicalDevice;

    // Binds the device's RetireQueue to the Vulkan destroy calls of `device`.
    // Called once while the device is created.
    void bindRetireDestroyer(LogicalDevice& device);

    // Destroys a layer-owned object now, for a caller that knows the GPU is done with
    // it (`extra` is the pool of a descriptor set or command buffer). The same
    // per-type destroy the RetireQueue runs.
    void destroyObjectNow(LogicalDevice& device, VkObjectType type, uint64_t handle, uint64_t extra = 0);
} // namespace VkHook
