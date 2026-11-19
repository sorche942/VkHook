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

    // Push a fatal-error toast onto the device's overlay and switch the layer
    // into pass-through mode until the device is destroyed.
    void panicLayer(LogicalDevice* logicalDevice, const std::string& reason);

    // True once panicLayer() recorded a device loss for this device.
    bool isDeviceLost(LogicalDevice* logicalDevice);
    // DestroyDevice: forget the device.
    void forgetDeviceLoss(LogicalDevice* logicalDevice);

    // Smoke-test hook (VKHOOK_SMOKE_DEPTH_REFRESH=<ms>). Takes globalLock.
    void maybeForceSmokeDepthRefresh(LogicalDevice* logicalDevice, DiagState& state);

    // Developer-only depth-resolve dump (VKHOOK_DEBUG builds); no-op otherwise.
    void maybeDumpDepthResolveImage(LogicalDevice* logicalDevice, LogicalSwapchain* logicalSwapchain,
                                    uint32_t imageIndex, VkQueue queue, DiagState& state);
} // namespace VkHook
