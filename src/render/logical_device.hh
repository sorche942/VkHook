#ifndef LOGICAL_DEVICE_HPP_INCLUDED
#define LOGICAL_DEVICE_HPP_INCLUDED
#include <atomic>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "vulkan_include.hh"
#include "vkdispatch.hh"
#include "gpu/queue.hh"
#include "gpu/retire.hh"
#include "gpu/timeline.hh"
#include <chrono>
#include <mutex>

namespace VkHook
{
    struct LogicalSwapchain;  // Forward declaration

    class DepthTracker;  // depth/tracker.hh

    struct DeviceControlState;  // control/frontend.hh

    struct LogicalDevice
    {
        DeviceDispatch           vkd;
        InstanceDispatch         vki;
        VkDevice                 device;
        VkPhysicalDevice         physicalDevice;
        VkInstance               instance;
        VkQueue                  queue;
        uint32_t                 queueFamilyIndex;
        // The layer's own graphics queue (family queueFamilyIndex). Submits go
        // through `queues`, which holds the queue's mutex around every call.
        // Effect command buffers are recorded for this family; a present on a
        // queue of another family submits them here instead.
        // Shared driver pipeline cache. Rebuilt effects reuse compiled pipelines
        // instead of making the driver recompile them on every chain reload.
        VkPipelineCache          pipelineCache = VK_NULL_HANDLE;
        PFN_vkGetInstanceProcAddr downstreamGIPA = nullptr;
        PFN_vkGetDeviceProcAddr downstreamGDPA = nullptr;
        // The loader's callback that gives a layer-created dispatchable handle its
        // dispatch table pointer (VK_LOADER_DATA_CALLBACK). Null only in tests.
        VkResult (VKAPI_PTR* setLoaderData)(VkDevice, void*) = nullptr;
        bool dlss5Supported = false;
        std::string dlss5UnavailableReason;
        VkQueue dlss5OpticalFlowQueue = VK_NULL_HANDLE;
        uint32_t dlss5OpticalFlowQueueFamily = VK_QUEUE_FAMILY_IGNORED;
        // Incremented even when effects/presents bypass; gaps invalidate history.
        std::atomic<uint64_t> presentSerial{0};
        bool                     supportsMutableFormat;
        bool                     isNvidiaGpu;
        // Real identity of the device the game is actually running on. The
        // sysfs DRM scan can't tell hybrid GPUs apart, so the UI reports these
        // instead of guessing a vendor from card enumeration order.
        std::string              gpuName;
        std::string              gpuDriverInfo;
        std::string              gpuPciSlot;  // "0000:01:00.0", empty if none
        uint32_t                 gpuApiVersion = 0;
        uint32_t                 gpuVendorId = 0;
        // Physical device properties, filled at vkCreateDevice (pipeline cache key)
        VkPhysicalDeviceProperties gpuProperties = {};
        // Memory types never change; read from the driver on the first allocation (render/memory.cc)
        std::once_flag             memoryPropertiesOnce;
        VkPhysicalDeviceMemoryProperties memoryProperties = {};
        bool                     gpuCrashDiagnosticsEnabled = false;
        bool                     supportsNvDiagnosticCheckpoints = false;
        bool                     supportsNvDiagnosticsConfig = false;
        bool                     supportsDeviceFaultExt = false;
        // Queried once at device init via VK_KHR_depth_resolve_mode / core 1.2.
        // OR of VK_RESOLVE_MODE_SAMPLE_ZERO_BIT / AVERAGE_BIT / MIN/MAX. Effects
        // the MSAA depth resolve path and the Advanced UI mode selector.
        VkResolveModeFlags       supportedDepthResolveModes = 0;
        // The device's depth tracker (depth/tracker.hh); null while depth capture is off.
        std::shared_ptr<DepthTracker> depth;
        // The device was created with separateDepthStencilLayouts (device plan):
        // only then may the layer use DEPTH_READ_ONLY_OPTIMAL-style layouts.
        bool separateDepthStencilLayouts = false;

        // ring of the last submitted batches (layer and app) so a queue that
        // stops completing can be traced to the submission that wedged it
        struct SubmitJournalEntry
        {
            uint64_t    seq             = 0;
            const char* kind            = nullptr;
            uint32_t    imageIndex      = 0;
            VkSemaphore waitSemaphore   = VK_NULL_HANDLE;
            VkSemaphore signalSemaphore = VK_NULL_HANDLE;
            VkFence     fence           = VK_NULL_HANDLE;
        };
        static constexpr uint32_t kSubmitJournalSize = 32;
        SubmitJournalEntry    submitJournal[kSubmitJournalSize];
        std::atomic<uint64_t> submitJournalSeq{0};

        // A chain reload requested by the UI, deferred until every swapchain's
        // in-flight effect submissions have completed. Replaces the global
        // QueueWaitIdle that used to stall the present thread on every toggle.
        bool pendingChainReload = false;
        std::vector<std::string> pendingChainEffects;

        // Hotkeys, overlay UI and its command/snapshot hand-off for this device
        // (control/frontend.hh). Created with the device, reset in vkDestroyDevice
        // after every swapchain (whose overlay renderers draw with its UI) is gone.
        std::shared_ptr<DeviceControlState> control;

        // Set by panicLayer() when the layer hits a fatal error. While true,
        // interceptors take the pass-through fast path: the swapchain keeps
        // presenting, the toast stays visible, but no effect work is done.
        // Cleared only by DestroyDevice — the user must restart the game to
        // re-enable effects after a panic.
        std::atomic<bool> softDisabled{false};

        // ---- layer runtime (gpu/*, layer/registry) ----

        // Serials, queue mutexes and deferred destruction for this device.
        Timeline    timeline;
        QueueTable  queues;
        RetireQueue retireQueue{timeline};

        // Retire a layer-owned object the GPU may still read. Destroyed by
        // RetireQueue::collect once its tagged serial completed.
        template <typename Handle>
        void retire(VkObjectType type, Handle handle, uint64_t extra = 0)
        {
            retireQueue.retire(type, handleToU64(handle), extra);
        }

        // The short per-device lock: guards `swapchains` and the pending-reload
        // flags. Held for a few map operations at a time. NEVER across a GPU
        // wait, file I/O, a shader compile or a call into the next layer
        // (AGENTS.md invariant 1).
        std::mutex stateLock;
        std::unordered_map<VkSwapchainKHR, std::shared_ptr<LogicalSwapchain>> swapchains;
        // What the device was created with (after the device plan): vkGetDeviceProcAddr
        // returns null for commands whose extension was not enabled. Set once at
        // vkCreateDevice, read-only afterwards.
        std::unordered_set<std::string> enabledExtensions;
        uint32_t                        deviceApiVersion = 0;  // min(instance API the app asked for, physical device)

        // Present threads of one device run the control step one at a time:
        // the loser skips it (it does file I/O, so it cannot wait on a mutex).
        std::atomic<bool> frontendBusy{false};

        // set by panicLayer() when the driver reported VK_ERROR_DEVICE_LOST
        std::atomic<bool> deviceLost{false};
    };

    // vkAllocateCommandBuffers for the layer's own buffers. A command buffer the
    // layer allocates never passed through the loader trampoline, so it must be
    // handed its loader data here; without it a layer below (validation) cannot
    // find the buffer's dispatch table and aborts.
    inline VkResult allocateLayerCommandBuffers(LogicalDevice* logicalDevice, const VkCommandBufferAllocateInfo* info,
                                                VkCommandBuffer* buffers)
    {
        const VkResult result = logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, info, buffers);
        if (result != VK_SUCCESS || !logicalDevice->setLoaderData)
            return result;
        for (uint32_t i = 0; i < info->commandBufferCount; ++i)
        {
            const VkResult loaderResult = logicalDevice->setLoaderData(logicalDevice->device, buffers[i]);
            if (loaderResult != VK_SUCCESS)
            {
                logicalDevice->vkd.FreeCommandBuffers(logicalDevice->device, info->commandPool, info->commandBufferCount, buffers);
                return loaderResult;
            }
        }
        return VK_SUCCESS;
    }
} // namespace VkHook

#endif // LOGICAL_DEVICE_HPP_INCLUDED