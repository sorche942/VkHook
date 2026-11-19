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

        if (logicalDevice->depth)
            logicalDevice->depth->refreshDetection();
        Logger::debug("smoke hook: forced depth re-detect");
    }

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
