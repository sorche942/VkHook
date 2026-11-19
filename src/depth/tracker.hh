#pragma once

// Depth tracker: follows the application's depth attachment as commands are
// recorded, picks a presentable snapshot target for it, resolves it, and retries
// when the attachment is not yet usable. Functions with a `Locked` suffix need
// globalLock; the rest document their own locking.

#include "vulkan_include.hh"
#include "logical_device.hh"
#include "logical_swapchain.hh"

#include <chrono>
#include <cstdint>
#include <unordered_map>

namespace VkHook
{
    struct DepthRetryState
    {
        bool                                  disabled     = false;
        bool                                  retryPending = false;
        std::chrono::steady_clock::time_point retryAt{};
        VkExtent2D                            blockedExtent{0, 0};
    };
    // keyed by device; guarded by globalLock
    extern std::unordered_map<LogicalDevice*, DepthRetryState> depthRetryStates;

    inline constexpr auto DEPTH_RETRY_DELAY        = std::chrono::seconds(5);
    inline constexpr auto DEPTH_REBUILD_POLL_DELAY = std::chrono::milliseconds(100);

    bool hasDepthState(const DepthState& state);
    bool sameDepthState(const DepthState& a, const DepthState& b);
    bool hasPresentableSnapshotTarget(const DepthSnapshotTarget& target);
    bool isDepthStencilAttachmentFormat(VkFormat format);
    bool validateDepthStateForResolve(LogicalDevice* logicalDevice, const DepthState& depth);
    bool depthMatchesSwapchainExtent(const DepthState& depth, const LogicalSwapchain* sc);

    // Force the depth attachment's storeOp to STORE; true if the description changed.
    bool forceDepthAttachmentStoreOp(VkAttachmentDescription& attachment);
    bool forceDepthAttachmentStoreOp(VkAttachmentDescription2& attachment);
    bool forceDepthAttachmentStoreOp(VkRenderingAttachmentInfo& attachment, bool hasStencilAspect);

    bool selectDepthCandidateForSwapchainLocked(LogicalDevice* logicalDevice,
                                                const LogicalSwapchain* logicalSwapchain,
                                                DepthState& candidate);
    DepthState selectDepthStateFromRenderPassBegin(LogicalDevice* logicalDevice, const VkRenderPassBeginInfo* pRenderPassBegin);
    DepthState selectDepthStateFromRenderingInfo(LogicalDevice* logicalDevice, const VkRenderingInfo* pRenderingInfo);
    DepthSnapshotTarget selectDepthSnapshotTargetFromImage(LogicalDevice* logicalDevice, VkImage image);
    DepthSnapshotTarget selectDepthSnapshotTargetFromImageViews(LogicalDevice* logicalDevice,
                                                                const VkImageView* imageViews,
                                                                uint32_t imageViewCount);
    DepthSnapshotTarget selectDepthSnapshotTargetFromRenderPassBegin(LogicalDevice* logicalDevice,
                                                                     const VkRenderPassBeginInfo* pRenderPassBegin);
    DepthSnapshotTarget selectDepthSnapshotTargetFromRenderingInfo(LogicalDevice* logicalDevice,
                                                                   const VkRenderingInfo* pRenderingInfo);

    void beginTrackedDepthScope(LogicalDevice* logicalDevice,
                                VkCommandBuffer commandBuffer,
                                const DepthState& depthState,
                                DepthSnapshotTarget snapshotTarget,
                                VkImageLayout depthFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED);
    void countTrackedDepthDraw(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer, uint32_t drawCount = 1);
    void accumulateExecutedCommandBufferDraws(LogicalDevice* logicalDevice,
                                              VkCommandBuffer primaryCommandBuffer,
                                              const VkCommandBuffer* pCommandBuffers,
                                              uint32_t commandBufferCount);
    bool endTrackedDepthScope(LogicalDevice* logicalDevice,
                              VkCommandBuffer commandBuffer,
                              const char* reason,
                              DepthState* pPromotedDepthState,
                              DepthSnapshotTarget* snapshotTarget = nullptr,
                              VkImageLayout* pDepthFinalLayout = nullptr);
    void tryActivatePendingTransferLinkedDepthScope(LogicalDevice* logicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    VkImage destinationImage,
                                                    const char* reason);

    void recordDepthResolveSnapshotForCommandBuffer(LogicalDevice* logicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    const DepthState& depthState,
                                                    const DepthSnapshotTarget* snapshotTarget);
    void recordDepthResolveSnapshotForAllSwapchains(LogicalDevice* logicalDevice,
                                                    VkCommandBuffer commandBuffer,
                                                    const DepthState& depthState);

    void updateDeviceDepthStateLocked(LogicalDevice* logicalDevice, const DepthState& depth, const char* reason);
    DepthState getDepthState(LogicalDevice* logicalDevice);
    VkImageView getOrCreateTrackedDepthSampleViewLocked(LogicalDevice* logicalDevice, VkImage image, VkFormat format);
    void reallocateCommandBuffers(LogicalDevice* logicalDevice, LogicalSwapchain* logicalSwapchain, const DepthState& depth);

    void destroyDepthResolveResources(LogicalSwapchain* logicalSwapchain);
    void ensureDepthResolveResources(LogicalSwapchain* logicalSwapchain, const DepthState& depth);
    void destroyPersistentDepthStorage(LogicalDevice* logicalDevice);
    void ensurePersistentDepthStorage(LogicalDevice* logicalDevice, VkFormat format, const VkExtent3D& extent);

    // wipes scope tracking, the best candidate and any pending copy; the hold
    // lifts on the first observation from the new generation
    void armDepthTransitionHoldLocked(LogicalDevice* logicalDevice, const char* reason);
    void armDepthRetryLocked(LogicalDevice* logicalDevice, const LogicalSwapchain* sc, const char* reason);
    bool depthRetryDueLocked(LogicalDevice* logicalDevice);
    void scheduleDepthRetryLocked(LogicalDevice* logicalDevice, bool fastPoll = false);
    bool depthRebuildFencesReady(LogicalDevice* logicalDevice, const LogicalSwapchain* sc);

    // Extent of a live, non-passthrough swapchain for this device (the window
    // size), for UI defaults such as the manual depth pin. Defined in
    // present/swapchain.cc. Callers must hold globalLock.
    VkExtent2D currentSwapchainExtentLocked(LogicalDevice* logicalDevice);

    // Drops every tracked scope whose depth state matches `predicate`.
    template <typename Predicate>
    void clearTrackedDepthScopesLocked(LogicalDevice* logicalDevice, Predicate predicate)
    {
        for (auto it = logicalDevice->commandBufferDepthStates.begin(); it != logicalDevice->commandBufferDepthStates.end();)
        {
            if (predicate(it->second.depthState))
                it = logicalDevice->commandBufferDepthStates.erase(it);
            else
                ++it;
        }

        for (auto it = logicalDevice->pendingTransferLinkedDepthScopes.begin();
             it != logicalDevice->pendingTransferLinkedDepthScopes.end();)
        {
            if (predicate(it->second.depthState))
                it = logicalDevice->pendingTransferLinkedDepthScopes.erase(it);
            else
                ++it;
        }

        if (logicalDevice->bestDepthCandidate.valid && predicate(logicalDevice->bestDepthCandidate.depthState))
            logicalDevice->bestDepthCandidate = {};
    }
} // namespace VkHook
