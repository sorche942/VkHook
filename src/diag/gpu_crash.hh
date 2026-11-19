#pragma once

// GPU crash diagnostics: queue checkpoints, VK_EXT_device_fault, depth dumps,
// and panicLayer(), the one way the layer soft-disables a device.

#include "vulkan_include.hh"
#include "logical_device.hh"
#include "logical_swapchain.hh"

#include <chrono>
#include <cstdint>
#include <string>

namespace VkHook
{
    // Per-device state of the developer/test hooks below (nothing here is
    // process-wide, so two devices do not share one-shot flags).
    struct DiagState
    {
        std::chrono::steady_clock::time_point lastSmokeRefresh{};
        bool                                  depthDumped = false;
    };

    std::string formatHexU64(uint64_t value);

    // Logs checkpoint / device-fault data when `result` is VK_ERROR_DEVICE_LOST
    // and the device has crash diagnostics enabled.
    void reportDeviceLostDiagnostics(LogicalDevice* logicalDevice, VkQueue queue, const char* context, VkResult result);

    // Soft-disable the device: toast on its overlay, no more effect work. Presents
    // of faked swapchains keep going through the layer's copy-only path. Nothing
    // is flushed or destroyed here.
    void panicLayer(LogicalDevice* logicalDevice, const std::string& reason);

    // True once panicLayer() recorded a device loss for this device.
    bool isDeviceLost(LogicalDevice* logicalDevice);

    // Records one submitted batch in the device's small submit journal; the
    // image index is a label for the dump, not a real image when unknown.
    void recordSubmittedBatch(LogicalDevice* logicalDevice, const char* kind, uint32_t imageIndex,
                              VkSemaphore waitSemaphore, VkSemaphore signalSemaphore, VkFence fence);
    // Logs the journal, oldest first, with each batch's fence state.
    void logSubmitJournal(LogicalDevice* logicalDevice, const char* reason);

    // Smoke-test hook (VKHOOK_SMOKE_DEPTH_REFRESH=<ms>). Takes the device's stateLock.
    void maybeForceSmokeDepthRefresh(LogicalDevice* logicalDevice, DiagState& state);

    // Developer-only depth-resolve dump (VKHOOK_DEBUG builds); no-op otherwise.
    void maybeDumpDepthResolveImage(LogicalDevice* logicalDevice, LogicalSwapchain* logicalSwapchain,
                                    uint32_t imageIndex, VkQueue queue, DiagState& state);
} // namespace VkHook
