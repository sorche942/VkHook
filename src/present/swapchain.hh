#pragma once

// Swapchain lifetime: the hooks themselves are reached through
// swapchainInterceptTable(); this header is what the rest of the layer calls.
//
// A rebuild of a swapchain's chain is a generation swap (AGENTS.md invariant 6,
// formal/VkHookFormal/ChainRebuild.lean): the next generation is built with no
// lock held and no GPU wait, swapped into LogicalDevice::swapchains under the
// device's stateLock, and the old generation retires through the RetireQueue
// when the last present thread drops its reference.

#include "vulkan_include.hh"
#include "logical_device.hh"
#include "logical_swapchain.hh"

#include <memory>
#include <string>
#include <vector>

namespace VkHook
{
    // Rebuilds the chain of every faked swapchain of `logicalDevice` with
    // `activeEffects`. No layer lock may be held; never waits for the GPU.
    void reloadAllSwapchains(LogicalDevice* logicalDevice, const std::vector<std::string>& activeEffects);

    // Starts the rebuild of one swapchain's chain from `current` (same effects when
    // `effects` is null) against the device's current depth state. The effects'
    // shaders compile on the device's registry worker; advanceChainBuild swaps the
    // new generation in at a later present. A chain with nothing to compile is
    // swapped in before this returns. False when nothing was started or swapped in:
    // another rebuild of that swapchain is finishing, the swapchain was replaced
    // meanwhile, or the build failed. A newer request replaces one still compiling.
    // No lock may be held.
    bool rebuildSwapchainChain(LogicalDevice* logicalDevice,
                               const std::shared_ptr<LogicalSwapchain>& current,
                               const std::vector<std::string>* effects);

    // Present-boundary step of one swapchain's chain: requests the first chain once
    // the registry has its selection, and finishes a compiled one (constructs the
    // effects from the worker's results and swaps the generation in, with no lock
    // held). Cheap when nothing is pending. Never waits.
    void advanceChainBuild(LogicalDevice* logicalDevice, const std::shared_ptr<LogicalSwapchain>& current);

    // Extent of a live, non-passthrough swapchain for this device (the window
    // size), for UI defaults such as the manual depth pin. Caller holds stateLock.
    VkExtent2D currentSwapchainExtentLocked(LogicalDevice* logicalDevice);
} // namespace VkHook
