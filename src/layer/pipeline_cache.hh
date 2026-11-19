#pragma once

// The device's shared VkPipelineCache, persisted across runs.
//
// The file is keyed by vendor ID, device ID and pipelineCacheUUID, so a driver
// update or another GPU never reads a blob that is not its own. Writes go
// through PersistenceQueue, never on the calling thread.

#include "logical_device.hh"

#include <string>

namespace VkHook
{
    // <XDG cache>/VkHook/pipeline-<vendor>-<device>-<uuid>.bin; empty when no
    // cache directory can be resolved.
    std::string pipelineCachePath(const VkPhysicalDeviceProperties& properties);

    // Creates device.pipelineCache, seeded from the file when it exists and its
    // header matches this device. Failure leaves the handle null; effect builds
    // then create an empty cache themselves.
    void loadPipelineCache(LogicalDevice& device, const VkPhysicalDeviceProperties& properties);

    // Snapshots the cache and queues the file write. No-op without a cache.
    void savePipelineCache(LogicalDevice& device);

    // Header check of a cached blob against the device (VkPipelineCacheHeaderVersionOne).
    bool pipelineCacheBlobMatches(const void* data, size_t size, const VkPhysicalDeviceProperties& properties);
} // namespace VkHook
