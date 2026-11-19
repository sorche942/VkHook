#include "layer/entry.hh"
#include "layer/state.hh"
#include "layer/env.hh"

#include "control/frontend.hh"
#include "depth/tracker.hh"
#include "diag/gpu_crash.hh"

#include "keyboard_input_wayland.hh"
#include "mouse_input_wayland.hh"
#include "vk_handle.hh"
#include "util.hh"
#include "logger.hh"
#include "effects/dlss5_config.hh"
#ifdef VKHOOK_DLSS5
#include "effects/effect_dlss5.hh"
#include "render/dlss5_device.hh"
#endif
#include "imgui_overlay.hh"

#include <cstring>
#include <string>
#include <vector>

namespace VkHook
{
namespace
{
    VkResult VKAPI_CALL VkHook_CreateDevice(VkPhysicalDevice             physicalDevice,
                                              const VkDeviceCreateInfo*    pCreateInfo,
                                              const VkAllocationCallbacks* pAllocator,
                                              VkDevice*                    pDevice)
    {
        Logger::trace("vkCreateDevice");
        VkLayerDeviceCreateInfo* layerCreateInfo = (VkLayerDeviceCreateInfo*) pCreateInfo->pNext;

        // step through the chain of pNext until we get to the link info
        while (layerCreateInfo
               && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO))
        {
            layerCreateInfo = (VkLayerDeviceCreateInfo*) layerCreateInfo->pNext;
        }

        if (layerCreateInfo == nullptr)
        {
            // No loader instance create info
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // everything below uses a copy of the instance state: no lock is held
        // while the next layer creates the device
        const InstanceSnapshot instSnap = snapshotInstance(GetKey(physicalDevice));
        if (!instSnap.found)
            return failUnknownObject("vkCreateDevice", VK_ERROR_INITIALIZATION_FAILED);

        PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        PFN_vkGetDeviceProcAddr   gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
        // move chain on for next layer
        layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

        PFN_vkCreateDevice createFunc = (PFN_vkCreateDevice) gipa(VK_NULL_HANDLE, "vkCreateDevice");

        // check and activate extentions
        uint32_t extensionCount = 0;

        std::vector<VkExtensionProperties> extensionProperties;

        {
            {
                VkResult res = instSnap.dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
                if (res != VK_SUCCESS || extensionCount == 0)
                {
                    Logger::warn("EnumerateDeviceExtensionProperties failed or no extensions");
                }
                else
                {
                    extensionProperties.resize(extensionCount);
                    res = instSnap.dispatch.EnumerateDeviceExtensionProperties(
                        physicalDevice, nullptr, &extensionCount, extensionProperties.data());
                    if (res != VK_SUCCESS)
                        Logger::warn("EnumerateDeviceExtensionProperties second call failed");
                }
            }
        }

        auto hasDeviceExtension = [&](const char* extensionName) {
            for (const VkExtensionProperties& properties : extensionProperties)
            {
                if (std::strcmp(properties.extensionName, extensionName) == 0)
                    return true;
            }
            return false;
        };

        const bool supportsMutableFormat = hasDeviceExtension("VK_KHR_swapchain_mutable_format");
        const bool supportsNvCheckpointExt = hasDeviceExtension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        const bool supportsNvDiagnosticsConfigExt = hasDeviceExtension(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
        const bool supportsDeviceFaultExt = hasDeviceExtension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        const bool gpuCrashDiagnosticsRequested = isGpuCrashDiagEnabled();
        if (supportsMutableFormat)
            Logger::debug("device supports VK_KHR_swapchain_mutable_format");

        bool hasMutableEnvOverride = false;
        bool mutableRequested = getMutableSwapchainEnvOverride(hasMutableEnvOverride, true);

        VkPhysicalDeviceProperties deviceProps;
        instSnap.dispatch.GetPhysicalDeviceProperties(physicalDevice, &deviceProps);

        VkDeviceCreateInfo       modifiedCreateInfo = *pCreateInfo;
        uint32_t queueFamilyCount = 0;
        instSnap.dispatch.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueProperties(queueFamilyCount);
        instSnap.dispatch.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueProperties.data());
        std::vector<const char*> enabledExtensionNames;
        if (modifiedCreateInfo.enabledExtensionCount)
        {
            enabledExtensionNames = std::vector<const char*>(modifiedCreateInfo.ppEnabledExtensionNames,
                                                             modifiedCreateInfo.ppEnabledExtensionNames + modifiedCreateInfo.enabledExtensionCount);
        }

        if (supportsMutableFormat && mutableRequested)
        {
            Logger::debug("activating mutable_format");
            addUniqueCString(enabledExtensionNames, "VK_KHR_swapchain_mutable_format");
        }
        else if (hasMutableEnvOverride)
        {
            Logger::info(std::string("Mutable swapchain device extension set via VKHOOK_ENABLE_MUTABLE_SWAPCHAIN=") + (mutableRequested ? "1" : "0"));
        }
        if (deviceProps.apiVersion < VK_API_VERSION_1_2 || instSnap.apiVersion < VK_API_VERSION_1_2)
        {
            addUniqueCString(enabledExtensionNames, "VK_KHR_image_format_list");
        }

        bool enableNvCheckpoints = false;
        bool enableNvDiagnosticsConfig = false;
        bool enableDeviceFault = false;
        bool enableDeviceFaultFeature = false;
        VkPhysicalDeviceFaultFeaturesEXT faultFeatureEnable = {};
        VkDeviceDiagnosticsConfigCreateInfoNV diagnosticsConfigInfo = {};
        if (gpuCrashDiagnosticsRequested)
        {
            if (supportsNvCheckpointExt)
            {
                addUniqueCString(enabledExtensionNames, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
                enableNvCheckpoints = true;
            }
            else
            {
                Logger::warn("VKHOOK_GPU_CRASH_DIAGNOSTICS=1 but VK_NV_device_diagnostic_checkpoints is not supported by this device");
            }

            if (supportsNvDiagnosticsConfigExt)
            {
                addUniqueCString(enabledExtensionNames, VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
                enableNvDiagnosticsConfig = true;
            }
            else
            {
                Logger::warn("VKHOOK_GPU_CRASH_DIAGNOSTICS=1 but VK_NV_device_diagnostics_config is not supported by this device");
            }

            if (supportsDeviceFaultExt)
            {
                addUniqueCString(enabledExtensionNames, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
                enableDeviceFault = true;
            }
            else
            {
                Logger::warn("VKHOOK_GPU_CRASH_DIAGNOSTICS=1 but VK_EXT_device_fault is not supported by this device");
            }
        }
        modifiedCreateInfo.ppEnabledExtensionNames = enabledExtensionNames.data();
        modifiedCreateInfo.enabledExtensionCount   = enabledExtensionNames.size();

        // Active needed Features
        VkPhysicalDeviceFeatures deviceFeatures = {};
        if (modifiedCreateInfo.pEnabledFeatures)
        {
            deviceFeatures = *(modifiedCreateInfo.pEnabledFeatures);
        }
        deviceFeatures.shaderImageGatherExtended = VK_TRUE;
        deviceFeatures.shaderStorageImageReadWithoutFormat = VK_TRUE;
        deviceFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        modifiedCreateInfo.pEnabledFeatures      = &deviceFeatures;

        void* extendedDevicePNext = const_cast<void*>(modifiedCreateInfo.pNext);

        if (enableDeviceFault && !pNextChainContainsSType(modifiedCreateInfo.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT))
        {
            VkPhysicalDeviceFaultFeaturesEXT faultFeatureQuery = {};
            faultFeatureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 featureQuery = {};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            featureQuery.pNext = &faultFeatureQuery;
            if (instSnap.dispatch.GetPhysicalDeviceFeatures2)
            {
                instSnap.dispatch.GetPhysicalDeviceFeatures2(physicalDevice, &featureQuery);
                if (faultFeatureQuery.deviceFault == VK_TRUE)
                {
                    faultFeatureEnable = {};
                    faultFeatureEnable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
                    faultFeatureEnable.deviceFault = VK_TRUE;
                    faultFeatureEnable.pNext = extendedDevicePNext;
                    extendedDevicePNext = &faultFeatureEnable;
                    enableDeviceFaultFeature = true;
                }
                else
                {
                    Logger::warn("VK_EXT_device_fault is present but deviceFault feature is not supported");
                }
            }
            else
            {
                Logger::warn("Cannot query VkPhysicalDeviceFaultFeaturesEXT (vkGetPhysicalDeviceFeatures2 unavailable)");
            }
        }
        else if (enableDeviceFault)
        {
            enableDeviceFaultFeature = true;
        }

        if (enableNvDiagnosticsConfig && !pNextChainContainsSType(modifiedCreateInfo.pNext, VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV))
        {
            diagnosticsConfigInfo = {};
            diagnosticsConfigInfo.sType = VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV;
            diagnosticsConfigInfo.flags =
                VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV
                | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV;
            diagnosticsConfigInfo.pNext = extendedDevicePNext;
            extendedDevicePNext = &diagnosticsConfigInfo;
        }

        modifiedCreateInfo.pNext = extendedDevicePNext;

        bool dlss5Supported = false;
        std::string dlss5UnavailableReason = "DLSS5 backend not built";
        uint32_t opticalFlowFamily = VK_QUEUE_FAMILY_IGNORED;
#ifdef VKHOOK_DLSS5
        Dlss5DeviceFeatures dlss5Features;
        const auto opticalQueue = selectDlss5OpticalQueue(queueProperties, *pCreateInfo);
        opticalFlowFamily = opticalQueue.family;
        dlss5Supported = dlss5Features.prepare(physicalDevice, instSnap.apiVersion,
            instSnap.dispatch.GetPhysicalDeviceFeatures2, deviceProps,
            extensionProperties, modifiedCreateInfo, enabledExtensionNames,
            opticalFlowFamily != VK_QUEUE_FAMILY_IGNORED);
        dlss5UnavailableReason = dlss5Features.error();
        std::vector<VkDeviceQueueCreateInfo> queueInfos;
        const float opticalFlowPriority = 1.0f;
        if (dlss5Supported && dlss5Features.opticalFlowEnabled())
        {
            if (!opticalQueue.shared)
            {
                queueInfos.assign(pCreateInfo->pQueueCreateInfos, pCreateInfo->pQueueCreateInfos + pCreateInfo->queueCreateInfoCount);
                VkDeviceQueueCreateInfo flowQueue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                flowQueue.queueFamilyIndex = opticalFlowFamily;
                flowQueue.queueCount = 1;
                flowQueue.pQueuePriorities = &opticalFlowPriority;
                queueInfos.push_back(flowQueue);
                modifiedCreateInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
                modifiedCreateInfo.pQueueCreateInfos = queueInfos.data();
            }
            Logger::info("DLSS5 optical flow: " + std::string(opticalQueue.shared ? "sharing application" : "reserving private")
                + " queue family " + std::to_string(opticalFlowFamily) + ", queue 0");
        }
        else
        {
            opticalFlowFamily = VK_QUEUE_FAMILY_IGNORED;
            if (dlss5Supported)
                Logger::warn(opticalQueue.family == VK_QUEUE_FAMILY_IGNORED
                    ? "DLSS5 temporal unavailable: no usable separate optical-flow queue (protected/mixed queues cannot be shared)"
                    : "DLSS5 temporal unavailable: VK_NV_optical_flow extension or opticalFlow feature not supported");
        }
#endif

        VkResult ret = createFunc(physicalDevice, &modifiedCreateInfo, pAllocator, pDevice);

        if (ret != VK_SUCCESS)
            return ret;

        auto logicalDevice = std::make_shared<LogicalDevice>();
        logicalDevice->vki                   = instSnap.dispatch;
        logicalDevice->device                = *pDevice;
        logicalDevice->physicalDevice        = physicalDevice;
        logicalDevice->instance              = instSnap.instance;
        logicalDevice->queue                 = VK_NULL_HANDLE;
        logicalDevice->queueFamilyIndex      = 0;
        logicalDevice->commandPool           = VK_NULL_HANDLE;
        logicalDevice->supportsMutableFormat = supportsMutableFormat && mutableRequested;
        logicalDevice->isNvidiaGpu           = (deviceProps.vendorID == 0x10DE);
        logicalDevice->gpuCrashDiagnosticsEnabled = gpuCrashDiagnosticsRequested;
        logicalDevice->supportsNvDiagnosticCheckpoints = enableNvCheckpoints;
        logicalDevice->supportsNvDiagnosticsConfig = enableNvDiagnosticsConfig;
        logicalDevice->supportsDeviceFaultExt = enableDeviceFault && enableDeviceFaultFeature;
        logicalDevice->downstreamGIPA = gipa;
        logicalDevice->downstreamGDPA = gdpa;
        logicalDevice->dlss5Supported = dlss5Supported;
        logicalDevice->dlss5UnavailableReason = dlss5UnavailableReason;

        fillDispatchTableDevice(*pDevice, gdpa, &logicalDevice->vkd);
        if (opticalFlowFamily != VK_QUEUE_FAMILY_IGNORED)
        {
            logicalDevice->dlss5OpticalFlowQueueFamily = opticalFlowFamily;
            logicalDevice->vkd.GetDeviceQueue(*pDevice, opticalFlowFamily, 0, &logicalDevice->dlss5OpticalFlowQueue);
            initializeDispatchTable(logicalDevice->dlss5OpticalFlowQueue, *pDevice);
        }

        // Query supported depth resolve modes once (VK_KHR_depth_resolve_mode,
        // core since 1.2). Drives the MSAA depth resolve path selection and the
        // Advanced UI mode selector. Every implementation must support at least
        // SAMPLE_ZERO, so this never stays 0 on a conformant driver.
        {
            VkPhysicalDeviceDepthStencilResolveProperties resolveProps = {};
            resolveProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES;
            // the core PCIE struct is missing from our vulkan headers; VK_EXT_pci_bus_info
            // has the same layout and sType
            VkPhysicalDevicePCIBusInfoPropertiesEXT pcieProps = {};
            pcieProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT;
            VkPhysicalDeviceDriverProperties driverProps = {};
            driverProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            VkPhysicalDeviceProperties2 props2 = {};
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props2.pNext = &resolveProps;
            resolveProps.pNext = &pcieProps;
            pcieProps.pNext = &driverProps;
            if (instSnap.dispatch.GetPhysicalDeviceProperties2)
            {
                instSnap.dispatch.GetPhysicalDeviceProperties2(physicalDevice, &props2);
                logicalDevice->supportedDepthResolveModes = resolveProps.supportedDepthResolveModes;
                if (logicalDevice->supportedDepthResolveModes == 0)
                    logicalDevice->supportedDepthResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;

                // identity + driver string, for the Diagnostics GPU card
                logicalDevice->gpuName      = props2.properties.deviceName;
                logicalDevice->gpuApiVersion = props2.properties.apiVersion;
                logicalDevice->gpuVendorId   = props2.properties.vendorID;
                if (driverProps.driverName[0] || driverProps.driverInfo[0])
                {
                    std::string info = driverProps.driverName;
                    if (driverProps.driverInfo[0])
                    {
                        if (!info.empty()) info += " ";
                        info += driverProps.driverInfo;
                    }
                    logicalDevice->gpuDriverInfo = info;
                }
                if (pcieProps.pciDomain != 0 || pcieProps.pciBus != 0 || pcieProps.pciDevice != 0 || pcieProps.pciFunction != 0)
                {
                    char slot[32] = {};
                    snprintf(slot, sizeof(slot), "%04x:%02x:%02x.%x",
                             pcieProps.pciDomain, pcieProps.pciBus, pcieProps.pciDevice, pcieProps.pciFunction);
                    logicalDevice->gpuPciSlot = slot;
                }
            }
            else
            {
                Logger::warn("vkGetPhysicalDeviceProperties2 unavailable; assuming SAMPLE_ZERO depth resolve only");
                logicalDevice->supportedDepthResolveModes = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
            }
            Logger::debug("supported depth resolve modes: " + std::to_string(logicalDevice->supportedDepthResolveModes));
        }

        if (logicalDevice->gpuCrashDiagnosticsEnabled)
        {
            if (logicalDevice->supportsNvDiagnosticCheckpoints
                && !(logicalDevice->vkd.GetQueueCheckpointDataNV || logicalDevice->vkd.GetQueueCheckpointData2NV))
            {
                Logger::warn("VK_NV_device_diagnostic_checkpoints enabled but checkpoint query entry points are unavailable");
                logicalDevice->supportsNvDiagnosticCheckpoints = false;
            }

            if (logicalDevice->supportsDeviceFaultExt && !logicalDevice->vkd.GetDeviceFaultInfoEXT)
            {
                Logger::warn("VK_EXT_device_fault enabled but vkGetDeviceFaultInfoEXT entry point is unavailable");
                logicalDevice->supportsDeviceFaultExt = false;
            }

            Logger::info(
                std::string("GPU crash diagnostics enabled (NV checkpoints=")
                + (logicalDevice->supportsNvDiagnosticCheckpoints ? "on" : "off")
                + ", NV diagnostics config=" + (logicalDevice->supportsNvDiagnosticsConfig ? "on" : "off")
                + ", device fault=" + (logicalDevice->supportsDeviceFaultExt ? "on" : "off") + ")");
        }

        for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++)
        {
            auto& queueInfo = pCreateInfo->pQueueCreateInfos[i];
            if (queueInfo.queueFamilyIndex < queueProperties.size()
                && (queueProperties[queueInfo.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            {
                if (!(queueProperties[queueInfo.queueFamilyIndex].queueFlags & VK_QUEUE_COMPUTE_BIT))
                {
                    logicalDevice->dlss5Supported = false;
                    logicalDevice->dlss5UnavailableReason = "DLSS5 needs a graphics queue with compute support";
                }
                logicalDevice->vkd.GetDeviceQueue(logicalDevice->device, queueInfo.queueFamilyIndex, 0, &logicalDevice->queue);

                VkCommandPoolCreateInfo commandPoolCreateInfo;
                commandPoolCreateInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                commandPoolCreateInfo.pNext            = nullptr;
                commandPoolCreateInfo.flags            = 0;
                commandPoolCreateInfo.queueFamilyIndex = queueInfo.queueFamilyIndex;

                Logger::debug("Found graphics capable queue");
                VkResult poolRes = logicalDevice->vkd.CreateCommandPool(logicalDevice->device, &commandPoolCreateInfo, nullptr, &logicalDevice->commandPool);
                if (poolRes != VK_SUCCESS)
                {
                    Logger::err("CreateCommandPool failed: " + std::to_string(poolRes));
                }
                logicalDevice->queueFamilyIndex = queueInfo.queueFamilyIndex;

                initializeDispatchTable(logicalDevice->queue, logicalDevice->device);

                break;
            }
        }

        if (!logicalDevice->queue)
        {
            Logger::err("Did not find a graphics queue! VkHook requires a graphics-capable queue.");
            // Still register the device so destruction works, but effects won't function
        }

        {
            scoped_lock l(globalLock);
            deviceMap[GetKey(*pDevice)] = logicalDevice;
        }

        return VK_SUCCESS;
    }

    void VKAPI_CALL VkHook_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
    {
        if (!device)
            return;

        // Teardown of the layer's own objects runs under the lock; the final
        // forward to the next layer runs after it is released.
        std::unique_lock<std::mutex> l(globalLock);

        Logger::trace("vkDestroyDevice");

        // Last chance to release anything still queued: once the device below is
        // destroyed, calling Vulkan against it is undefined behaviour, so the
        // queue is drained while the handle is still valid. Swapchain destruction
        // will normally have flushed already; this covers the paths that destroy
        // the device without a swapchain teardown, such as a lost device.
        DeferredDestroyQueue::instance().flush();

        auto devIt = deviceMap.find(GetKey(device));
        if (devIt == deviceMap.end() || !devIt->second)
        {
            failUnknownObject("vkDestroyDevice", VK_ERROR_DEVICE_LOST);
            return;
        }
        std::shared_ptr<LogicalDevice> keepAlive = devIt->second;
        LogicalDevice* logicalDevice = keepAlive.get();

        // Destroy all swapchains belonging to this device first
        for (auto swapIt = swapchainMap.begin(); swapIt != swapchainMap.end(); )
        {
            if (swapIt->second && swapIt->second->logicalDevice == logicalDevice)
            {
                swapIt->second->destroy();
                swapIt = swapchainMap.erase(swapIt);
            }
            else
            {
                ++swapIt;
            }
        }

        // Destroy ImGui overlay before device (it uses device resources)
        logicalDevice->imguiOverlay.reset();

        // Destroy persistent depth storage
        destroyPersistentDepthStorage(logicalDevice);

        // Destroy depth copy ring buffer pool and its fences. The teardown takes
        // the ring's own lock so it cannot race a QueueSubmit mid-copy; lock
        // order here is globalLock -> depthCopy, as DepthCopyState documents.
        logicalDevice->depthCopy.withRing([&](DepthCopyState::RingAccess& ring) {
            VkCommandPool pool = VK_NULL_HANDLE;
            std::vector<VkFence> fences;
            ring.detach(pool, fences);

            if (pool == VK_NULL_HANDLE && fences.empty())
                return;

            Logger::debug("DestroyCommandPool (depth copy ring buffer)");
            // Destroy fences first (they're independent of the pool)
            for (VkFence f : fences)
            {
                if (f != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroyFence(device, f, nullptr);
            }
            if (pool != VK_NULL_HANDLE)
                logicalDevice->vkd.DestroyCommandPool(device, pool, pAllocator);
        });

        // Clean up Wayland input resources (no-op if not initialized)
        cleanupWaylandKeyboard();
        cleanupWaylandMouse();

        if (logicalDevice->commandPool != VK_NULL_HANDLE)
        {
            Logger::debug("DestroyCommandPool");
            logicalDevice->vkd.DestroyCommandPool(device, logicalDevice->commandPool, pAllocator);
        }

        // second drain: imguiOverlay.reset() queues the overlay's handles just before
        // the device dies, and there's no later flush to pick them up.
        DeferredDestroyQueue::instance().flush();

        if (logicalDevice->pipelineCache != VK_NULL_HANDLE)
        {
            logicalDevice->vkd.DestroyPipelineCache(device, logicalDevice->pipelineCache, pAllocator);
            logicalDevice->pipelineCache = VK_NULL_HANDLE;
        }

        depthRetryStates.erase(logicalDevice);
        forgetDeviceLoss(logicalDevice);
        releaseDeviceControlLocked(logicalDevice);
        deviceMap.erase(GetKey(device));
        l.unlock();

        keepAlive->vkd.DestroyDevice(device, pAllocator);
    }

    const InterceptEntry kDeviceIntercepts[] = {
        VKHOOK_INTERCEPT(Unconditional, CreateDevice),
        VKHOOK_INTERCEPT(Unconditional, DestroyDevice),
    };
} // namespace

    InterceptTable deviceInterceptTable()
    {
        return makeInterceptTable(kDeviceIntercepts);
    }
} // namespace VkHook
