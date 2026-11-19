#include "diag/gpu_crash.hh"

#include "layer/registry.hh"
#include "layer/env.hh"
#include "control/frontend.hh"

#include "format.hh"
#include "buffer.hh"
#include "memory.hh"
#include "util.hh"
#include "logger.hh"
#include "depth/tracker.hh"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <mutex>
#include <sstream>

namespace VkHook
{

    std::string formatHexU64(uint64_t value)
    {
        std::ostringstream oss;
        oss << std::hex << value;
        return oss.str();
    }

    // Smoke-test hook: VKHOOK_SMOKE_DEPTH_REFRESH=<ms> drops the depth
    // selection on that interval, exactly what the Advanced tab's refresh does,
    // so a run covers the rebuild path while the queue is loaded. Off unless set.
    DiagState::DiagState()
    {
        const char* value = std::getenv("VKHOOK_SMOKE_DEPTH_REFRESH");
        if (value == nullptr)
            return;
        try
        {
            smokeRefreshIntervalMs = std::max(0, std::stoi(value));
        }
        catch (...)
        {
            smokeRefreshIntervalMs = 0;
        }
    }

    void maybeForceSmokeDepthRefresh(LogicalDevice* logicalDevice, DiagState& state)
    {
        const int intervalMs = state.smokeRefreshIntervalMs;
        if (intervalMs <= 0 || logicalDevice == nullptr)
            return;

        auto& lastFired = state.lastSmokeRefresh;
        const auto now = std::chrono::steady_clock::now();
        if (!lastFired.time_since_epoch().count())
        {
            lastFired = now;
            return;
        }
        if (now - lastFired < std::chrono::milliseconds(intervalMs))
            return;
        lastFired = now;

        refreshDepthDetection(logicalDevice);
        Logger::debug("smoke hook: forced depth re-detect");
    }

#ifdef VKHOOK_DEBUG
    static bool isDepthCopyDumpEnabled()
    {
        const char* value = std::getenv("VKHOOK_DEBUG_DUMP_DEPTH_COPY");
        if (value == nullptr)
            return false;
        return parseBoolEnvValue(value, false);
    }

    static VkImageLayout getInternalDepthReadOnlyLayoutForDebug(VkFormat format)
    {
        return isStencilFormat(format) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                       : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    }

    static VkImageLayout getDepthResolveReadOnlyLayoutForDebug(const LogicalSwapchain* logicalSwapchain)
    {
        if (!logicalSwapchain)
            return VK_IMAGE_LAYOUT_UNDEFINED;

        // the fused msaa pass leaves its raw intermediate in GENERAL
        if (logicalSwapchain->depthResolveMsaaTransform)
            return VK_IMAGE_LAYOUT_GENERAL;

        return logicalSwapchain->depthResolveUsesShader
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : getInternalDepthReadOnlyLayoutForDebug(logicalSwapchain->depthResolveFormat);
    }

    static VkImageAspectFlags getDepthResolveAspectMaskForDebug(const LogicalSwapchain* logicalSwapchain)
    {
        if (!logicalSwapchain)
            return VK_IMAGE_ASPECT_COLOR_BIT;

        return logicalSwapchain->depthResolveUsesShader ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    // Developer-only debug helper: dumps the depth-resolve image to
    // /tmp/vkhook-depth-copy-<i>.f32 so we can inspect what the layer is
    // feeding into effects. Compiled out of release builds — it does a
    // synchronous QueueWaitIdle + MapMemory on the present path.
    void maybeDumpDepthResolveImage(LogicalDevice* logicalDevice, LogicalSwapchain* logicalSwapchain, uint32_t imageIndex, VkQueue queue, DiagState& state)
    {
        if (state.depthDumped || !isDepthCopyDumpEnabled() || !logicalDevice || !logicalSwapchain)
            return;
        if (imageIndex >= logicalSwapchain->depthResolvePerImage.size())
            return;
        if (logicalSwapchain->depthResolveFormat != VK_FORMAT_D32_SFLOAT
            && logicalSwapchain->depthResolveFormat != VK_FORMAT_R32_SFLOAT)
        {
            Logger::warn("depth copy dump only supports VK_FORMAT_D32_SFLOAT or VK_FORMAT_R32_SFLOAT right now, got format="
                         + convertToString(logicalSwapchain->depthResolveFormat));
            state.depthDumped = true;
            return;
        }

        VkExtent3D extent = logicalSwapchain->depthResolveExtent;
        if (extent.width == 0 || extent.height == 0)
            return;

        state.depthDumped = true;

        // One-shot debug path: wait for prior work so the sampled backup image is stable.
        logicalDevice->queues.waitIdle(queue);

        const VkDeviceSize dumpSize = static_cast<VkDeviceSize>(extent.width) * static_cast<VkDeviceSize>(extent.height) * sizeof(float);
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        createBuffer(logicalDevice,
                     dumpSize,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuffer,
                     stagingMemory);

        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = logicalDevice->queueFamilyIndex;
        VkCommandPool dumpPool = VK_NULL_HANDLE;
        VkResult vr = logicalDevice->vkd.CreateCommandPool(logicalDevice->device, &poolInfo, nullptr, &dumpPool);
        ASSERT_VULKAN(vr);

        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = dumpPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        vr = logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &allocInfo, &commandBuffer);
        ASSERT_VULKAN(vr);
        initializeDispatchTable(commandBuffer, logicalDevice->device);

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vr = logicalDevice->vkd.BeginCommandBuffer(commandBuffer, &beginInfo);
        ASSERT_VULKAN(vr);

        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.image = logicalSwapchain->depthResolvePerImage[imageIndex].image;
        barrier.oldLayout = getDepthResolveReadOnlyLayoutForDebug(logicalSwapchain);
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = getDepthResolveAspectMaskForDebug(logicalSwapchain);
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &barrier);

        VkBufferImageCopy region = {};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = getDepthResolveAspectMaskForDebug(logicalSwapchain);
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = extent.width;
        region.imageExtent.height = extent.height;
        region.imageExtent.depth = 1;

        logicalDevice->vkd.CmdCopyImageToBuffer(commandBuffer,
                                                 logicalSwapchain->depthResolvePerImage[imageIndex].image,
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 stagingBuffer,
                                                 1,
                                                 &region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = getDepthResolveReadOnlyLayoutForDebug(logicalSwapchain);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        logicalDevice->vkd.CmdPipelineBarrier(commandBuffer,
                                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                               0,
                                               0,
                                               nullptr,
                                               0,
                                               nullptr,
                                               1,
                                               &barrier);

        vr = logicalDevice->vkd.EndCommandBuffer(commandBuffer);
        ASSERT_VULKAN(vr);

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        vr = logicalDevice->queues.submitAndWait(queue, submitInfo, 5000);
        ASSERT_VULKAN(vr);

        void* mapped = nullptr;
        vr = logicalDevice->vkd.MapMemory(logicalDevice->device, stagingMemory, 0, dumpSize, 0, &mapped);
        ASSERT_VULKAN(vr);

        const float* values = static_cast<const float*>(mapped);
        const size_t pixelCount = static_cast<size_t>(extent.width) * static_cast<size_t>(extent.height);
        float minValue = std::numeric_limits<float>::infinity();
        float maxValue = -std::numeric_limits<float>::infinity();
        size_t finiteCount = 0;
        size_t zeroishCount = 0;
        size_t oneishCount = 0;
        for (size_t i = 0; i < pixelCount; ++i)
        {
            const float v = values[i];
            if (!std::isfinite(v))
                continue;
            finiteCount++;
            minValue = std::min(minValue, v);
            maxValue = std::max(maxValue, v);
            if (std::abs(v) < 1e-6f)
                zeroishCount++;
            if (std::abs(v - 1.0f) < 1e-6f)
                oneishCount++;
        }

        auto sampleAt = [&](uint32_t x, uint32_t y) -> float {
            return values[static_cast<size_t>(y) * static_cast<size_t>(extent.width) + x];
        };

        const uint32_t centerX = extent.width / 2;
        const uint32_t centerY = extent.height / 2;
        Logger::warn("depth copy dump: imageIndex=" + std::to_string(imageIndex)
                     + " extent=" + std::to_string(extent.width) + "x" + std::to_string(extent.height)
                     + " finite=" + std::to_string(finiteCount)
                     + " min=" + std::to_string(minValue)
                     + " max=" + std::to_string(maxValue)
                     + " zeroish=" + std::to_string(zeroishCount)
                     + " oneish=" + std::to_string(oneishCount)
                     + " sample(0,0)=" + std::to_string(sampleAt(0, 0))
                     + " sample(center)=" + std::to_string(sampleAt(centerX, centerY))
                     + " sample(last,last)=" + std::to_string(sampleAt(extent.width - 1, extent.height - 1)));

        const std::string dumpPath = "/tmp/vkhook-depth-copy-" + std::to_string(imageIndex) + ".f32";
        {
            std::ofstream dump(dumpPath, std::ios::binary | std::ios::trunc);
            dump.write(reinterpret_cast<const char*>(values), static_cast<std::streamsize>(dumpSize));
        }
        Logger::warn("depth copy dump written to " + dumpPath);

        logicalDevice->vkd.UnmapMemory(logicalDevice->device, stagingMemory);
        logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, dumpPool, nullptr);
        logicalDevice->vkd.DestroyBuffer(logicalDevice->device, stagingBuffer, nullptr);
        logicalDevice->vkd.FreeMemory(logicalDevice->device, stagingMemory, nullptr);
    }
#else
    void maybeDumpDepthResolveImage(LogicalDevice*, LogicalSwapchain*, uint32_t, VkQueue, DiagState&) {}
#endif

    static void logNvQueueCheckpointData(LogicalDevice* logicalDevice, VkQueue queue, const char* context)
    {
        if (!logicalDevice || !logicalDevice->supportsNvDiagnosticCheckpoints)
            return;

        if (logicalDevice->vkd.GetQueueCheckpointData2NV)
        {
            uint32_t checkpointCount = 0;
            logicalDevice->vkd.GetQueueCheckpointData2NV(queue, &checkpointCount, nullptr);
            if (checkpointCount == 0)
            {
                Logger::warn(std::string(context) + ": no VK_NV checkpoint data available");
                return;
            }

            std::vector<VkCheckpointData2NV> checkpoints(checkpointCount);
            for (auto& checkpoint : checkpoints)
            {
                checkpoint = {};
                checkpoint.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV;
            }
            logicalDevice->vkd.GetQueueCheckpointData2NV(queue, &checkpointCount, checkpoints.data());

            for (uint32_t i = 0; i < checkpointCount; ++i)
            {
                const auto& checkpoint = checkpoints[i];
                Logger::warn(
                    std::string(context) + ": checkpoint[" + std::to_string(i) +
                    "] stage=0x" + formatHexU64(static_cast<uint64_t>(checkpoint.stage)) +
                    " marker=0x" + formatHexU64(reinterpret_cast<uint64_t>(checkpoint.pCheckpointMarker)));
            }
            return;
        }

        if (logicalDevice->vkd.GetQueueCheckpointDataNV)
        {
            uint32_t checkpointCount = 0;
            logicalDevice->vkd.GetQueueCheckpointDataNV(queue, &checkpointCount, nullptr);
            if (checkpointCount == 0)
            {
                Logger::warn(std::string(context) + ": no VK_NV checkpoint data available");
                return;
            }

            std::vector<VkCheckpointDataNV> checkpoints(checkpointCount);
            for (auto& checkpoint : checkpoints)
            {
                checkpoint = {};
                checkpoint.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
            }
            logicalDevice->vkd.GetQueueCheckpointDataNV(queue, &checkpointCount, checkpoints.data());

            for (uint32_t i = 0; i < checkpointCount; ++i)
            {
                const auto& checkpoint = checkpoints[i];
                Logger::warn(
                    std::string(context) + ": checkpoint[" + std::to_string(i) +
                    "] stage=0x" + formatHexU64(static_cast<uint64_t>(checkpoint.stage)) +
                    " marker=0x" + formatHexU64(reinterpret_cast<uint64_t>(checkpoint.pCheckpointMarker)));
            }
        }
    }

    static void logDeviceFaultInfo(LogicalDevice* logicalDevice, const char* context)
    {
        if (!logicalDevice || !logicalDevice->supportsDeviceFaultExt || !logicalDevice->vkd.GetDeviceFaultInfoEXT)
            return;

        VkDeviceFaultCountsEXT faultCounts = {};
        faultCounts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
        VkResult countsResult = logicalDevice->vkd.GetDeviceFaultInfoEXT(logicalDevice->device, &faultCounts, nullptr);
        if (countsResult != VK_SUCCESS && countsResult != VK_INCOMPLETE)
        {
            Logger::warn(std::string(context) + ": vkGetDeviceFaultInfoEXT(counts) failed: " + std::to_string(countsResult));
            return;
        }

        std::vector<VkDeviceFaultAddressInfoEXT> addressInfos(faultCounts.addressInfoCount);
        std::vector<VkDeviceFaultVendorInfoEXT> vendorInfos(faultCounts.vendorInfoCount);
        std::vector<uint8_t> vendorBinary(faultCounts.vendorBinarySize);

        VkDeviceFaultInfoEXT faultInfo = {};
        faultInfo.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
        faultInfo.pAddressInfos = addressInfos.empty() ? nullptr : addressInfos.data();
        faultInfo.pVendorInfos = vendorInfos.empty() ? nullptr : vendorInfos.data();
        faultInfo.pVendorBinaryData = vendorBinary.empty() ? nullptr : vendorBinary.data();

        VkResult infoResult = logicalDevice->vkd.GetDeviceFaultInfoEXT(logicalDevice->device, &faultCounts, &faultInfo);
        if (infoResult != VK_SUCCESS && infoResult != VK_INCOMPLETE)
        {
            Logger::warn(std::string(context) + ": vkGetDeviceFaultInfoEXT(info) failed: " + std::to_string(infoResult));
            return;
        }

        Logger::err(
            std::string(context) + ": device fault description=\"" + faultInfo.description +
            "\", addresses=" + std::to_string(faultCounts.addressInfoCount) +
            ", vendorInfos=" + std::to_string(faultCounts.vendorInfoCount) +
            ", vendorBinaryBytes=" + std::to_string(faultCounts.vendorBinarySize));

        for (uint32_t i = 0; i < faultCounts.addressInfoCount; ++i)
        {
            const auto& address = addressInfos[i];
            Logger::warn(
                std::string(context) + ": faultAddress[" + std::to_string(i) +
                "] type=" + std::to_string(address.addressType) +
                " reported=0x" + formatHexU64(address.reportedAddress) +
                " precision=0x" + formatHexU64(address.addressPrecision));
        }
    }

    void reportDeviceLostDiagnostics(LogicalDevice* logicalDevice, VkQueue queue, const char* context, VkResult result)
    {
        if (result != VK_ERROR_DEVICE_LOST || !logicalDevice || !logicalDevice->gpuCrashDiagnosticsEnabled)
            return;

        Logger::err(std::string(context) + ": VK_ERROR_DEVICE_LOST (gathering diagnostics)");
        logNvQueueCheckpointData(logicalDevice, queue, context);
        logDeviceFaultInfo(logicalDevice, context);
    }

    // Soft-disable the device: push a fatal-error toast onto its overlay and
    // stop running effects. Presents of faked swapchains keep going through the
    // layer's copy-only path (AGENTS.md invariant 5); nothing is destroyed or
    // flushed here, because a panic does not make a GPU object any less in use.
    void panicLayer(LogicalDevice* logicalDevice, const std::string& reason)
    {
        Logger::err("VkHook panic: " + reason);
        if (logicalDevice)
        {
            if (reason.find("Device lost") != std::string::npos
                || reason.find("device lost") != std::string::npos
                || reason.find("VK_ERROR_DEVICE_LOST") != std::string::npos)
                logicalDevice->deviceLost.store(true, std::memory_order_release);

            logicalDevice->softDisabled.store(true, std::memory_order_release);
            if (logicalDevice->control)
                logicalDevice->control->overlay.pushToast(
                    LogLevel::Error,
                    "VkHook disabled itself to keep the game alive.\nReason: " + reason +
                    "\nEffects are off until you restart the game.");
        }
    }

    bool isDeviceLost(LogicalDevice* logicalDevice)
    {
        return logicalDevice->deviceLost.load(std::memory_order_acquire);
    }

    void recordSubmittedBatch(LogicalDevice* logicalDevice, const char* kind, uint32_t imageIndex,
                              VkSemaphore waitSemaphore, VkSemaphore signalSemaphore, VkFence fence)
    {
        if (!logicalDevice)
            return;

        const uint64_t seq = logicalDevice->submitJournalSeq.fetch_add(1, std::memory_order_relaxed);
        auto& entry = logicalDevice->submitJournal[seq % LogicalDevice::kSubmitJournalSize];
        entry.seq             = seq;
        entry.kind            = kind;
        entry.imageIndex      = imageIndex;
        entry.waitSemaphore   = waitSemaphore;
        entry.signalSemaphore = signalSemaphore;
        entry.fence           = fence;
    }

    void logSubmitJournal(LogicalDevice* logicalDevice, const char* reason)
    {
        // the batches a stuck queue might be waiting on, oldest first
        const uint64_t journalSeq = logicalDevice->submitJournalSeq.load(std::memory_order_relaxed);
        const uint32_t journalCount = journalSeq < LogicalDevice::kSubmitJournalSize
            ? static_cast<uint32_t>(journalSeq) : LogicalDevice::kSubmitJournalSize;
        Logger::warn(std::string(reason ? reason : "submit journal") + ": submit journal, newest last ("
                     + std::to_string(journalCount) + " entries):");
        for (uint32_t i = 0; i < journalCount; i++)
        {
            const auto& entry = logicalDevice->submitJournal[(journalSeq - journalCount + i) % LogicalDevice::kSubmitJournalSize];
            const VkResult fenceStatus = entry.fence != VK_NULL_HANDLE
                ? logicalDevice->vkd.GetFenceStatus(logicalDevice->device, entry.fence)
                : VK_SUCCESS;
            Logger::warn("  #" + std::to_string(entry.seq)
                         + " " + std::string(entry.kind ? entry.kind : "?")
                         + " img=" + std::to_string(entry.imageIndex)
                         + " wait=" + convertToString(entry.waitSemaphore)
                         + " signal=" + convertToString(entry.signalSemaphore)
                         + " fence=" + convertToString(entry.fence)
                         + " fenceStatus=" + std::to_string(fenceStatus));
        }
    }
} // namespace VkHook
