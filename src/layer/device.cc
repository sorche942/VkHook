#include "layer/entry.hh"
#include "layer/registry.hh"
#include "layer/guard.hh"
#include "layer/env.hh"

#include "control/frontend.hh"
#include "depth/tracker.hh"
#include "diag/gpu_crash.hh"

#include "util.hh"
#include "logger.hh"
#include "effects/dlss5_config.hh"
#ifdef VKHOOK_DLSS5
#include "effects/effect_dlss5.hh"
#endif
#include "layer/device_plan.hh"
#include "layer/pipeline_cache.hh"
#include "gpu/retire_device.hh"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <string>
#include <vector>
#include <memory>

namespace VkHook
{
namespace
{
    // Everything the post-create setup needs, gathered by vkCreateDevice.
    struct DeviceBuild
    {
        VkPhysicalDevice           physicalDevice;
        VkDevice                   device;
        const InstanceSnapshot&    instance;
        const DevicePlan&          plan;
        const DeviceCaps&          caps;
        const VkDeviceCreateInfo*  appCreateInfo;
        PFN_vkGetInstanceProcAddr  gipa;
        PFN_vkGetDeviceProcAddr    gdpa;
        bool                       gpuCrashDiagnosticsRequested;
    };

    // The layer's per-device state, built after the real device exists. May throw;
    // vkCreateDevice then tears the device down again.
    std::shared_ptr<LogicalDevice> buildLogicalDevice(const DeviceBuild& build)
    {
        const VkPhysicalDeviceProperties& deviceProps = build.caps.properties;
        const InstanceSnapshot&           instSnap    = build.instance;
        const DevicePlan&                 plan        = build.plan;
        const VkDevice                    device      = build.device;

        auto logicalDevice = std::make_shared<LogicalDevice>();
        logicalDevice->vki                   = instSnap.dispatch;
        logicalDevice->device                = device;
        logicalDevice->physicalDevice        = build.physicalDevice;
        logicalDevice->instance              = instSnap.instance;
        logicalDevice->queue                 = VK_NULL_HANDLE;
        logicalDevice->queueFamilyIndex      = 0;
        logicalDevice->supportsMutableFormat = plan.supportsMutableFormat;
        logicalDevice->isNvidiaGpu           = (deviceProps.vendorID == 0x10DE);
        logicalDevice->gpuProperties         = deviceProps;
        logicalDevice->gpuCrashDiagnosticsEnabled = build.gpuCrashDiagnosticsRequested;
        logicalDevice->supportsNvDiagnosticCheckpoints = plan.enableNvCheckpoints;
        logicalDevice->supportsNvDiagnosticsConfig = plan.enableNvDiagnosticsConfig;
        logicalDevice->supportsDeviceFaultExt = plan.enableDeviceFault && plan.enableDeviceFaultFeature;
        logicalDevice->downstreamGIPA = build.gipa;
        logicalDevice->downstreamGDPA = build.gdpa;
        logicalDevice->dlss5Supported = plan.dlss5Supported;
        logicalDevice->dlss5UnavailableReason = plan.dlss5UnavailableReason;

        // what vkGetDeviceProcAddr may hand out (AGENTS.md: null for commands
        // whose extension the device did not enable)
        const VkDeviceCreateInfo& created = plan.createInfo();
        for (uint32_t i = 0; i < created.enabledExtensionCount; ++i)
            logicalDevice->enabledExtensions.insert(created.ppEnabledExtensionNames[i]);
        logicalDevice->deviceApiVersion = std::min(instSnap.apiVersion, deviceProps.apiVersion);

        fillDispatchTableDevice(device, build.gdpa, &logicalDevice->vkd);
        {
            QueueDispatch queueDispatch;
            fillQueueDispatch(device, build.gdpa, &queueDispatch);
            logicalDevice->queues.init(device, queueDispatch, logicalDevice->timeline);
            bindRetireDestroyer(*logicalDevice);
        }

        if (plan.opticalFlowFamily != VK_QUEUE_FAMILY_IGNORED)
        {
            logicalDevice->dlss5OpticalFlowQueueFamily = plan.opticalFlowFamily;
            logicalDevice->vkd.GetDeviceQueue(device, plan.opticalFlowFamily, 0, &logicalDevice->dlss5OpticalFlowQueue);
            initializeDispatchTable(logicalDevice->dlss5OpticalFlowQueue, device);
            logicalDevice->queues.add(logicalDevice->dlss5OpticalFlowQueue, plan.opticalFlowFamily, 0);
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
                instSnap.dispatch.GetPhysicalDeviceProperties2(build.physicalDevice, &props2);
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

        const auto& queueProperties = build.caps.queueFamilies;
        for (uint32_t i = 0; i < build.appCreateInfo->queueCreateInfoCount; i++)
        {
            auto& queueInfo = build.appCreateInfo->pQueueCreateInfos[i];
            if (queueInfo.queueFamilyIndex < queueProperties.size()
                && (queueProperties[queueInfo.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            {
                if (!(queueProperties[queueInfo.queueFamilyIndex].queueFlags & VK_QUEUE_COMPUTE_BIT))
                {
                    logicalDevice->dlss5Supported = false;
                    logicalDevice->dlss5UnavailableReason = "DLSS5 needs a graphics queue with compute support";
                }
                logicalDevice->vkd.GetDeviceQueue(logicalDevice->device, queueInfo.queueFamilyIndex, 0, &logicalDevice->queue);

                Logger::debug("Found graphics capable queue");
                logicalDevice->queueFamilyIndex = queueInfo.queueFamilyIndex;

                initializeDispatchTable(logicalDevice->queue, logicalDevice->device);
                logicalDevice->queues.add(logicalDevice->queue, queueInfo.queueFamilyIndex, 0);

                break;
            }
        }

        if (!logicalDevice->queue)
        {
            Logger::err("Did not find a graphics queue! VkHook requires a graphics-capable queue.");
            // Still register the device so destruction works, but effects won't function
        }

        loadPipelineCache(*logicalDevice, deviceProps);

        logicalDevice->control = std::make_shared<DeviceControlState>(logicalDevice.get());

        return logicalDevice;
    }

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

        // Vulkan queries live here; planDeviceCreate is pure (AGENTS.md invariant 9)
        DeviceCaps caps;
        {
            uint32_t extensionCount = 0;
            VkResult res = instSnap.dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
            if (res != VK_SUCCESS || extensionCount == 0)
            {
                Logger::warn("EnumerateDeviceExtensionProperties failed or no extensions");
            }
            else
            {
                caps.extensions.resize(extensionCount);
                res = instSnap.dispatch.EnumerateDeviceExtensionProperties(
                    physicalDevice, nullptr, &extensionCount, caps.extensions.data());
                if (res != VK_SUCCESS)
                {
                    Logger::warn("EnumerateDeviceExtensionProperties second call failed");
                    caps.extensions.clear();
                }
                else
                {
                    caps.extensions.resize(extensionCount);
                }
            }
        }

        instSnap.dispatch.GetPhysicalDeviceProperties(physicalDevice, &caps.properties);
        {
            uint32_t queueFamilyCount = 0;
            instSnap.dispatch.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
            caps.queueFamilies.resize(queueFamilyCount);
            instSnap.dispatch.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, caps.queueFamilies.data());
        }

        if (instSnap.dispatch.GetPhysicalDeviceFeatures2)
        {
            VkPhysicalDeviceFaultFeaturesEXT faultQuery = {};
            faultQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
            VkPhysicalDeviceFeatures2 featureQuery = {};
            featureQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            // the fault struct may only be queried when the extension exists
            if (caps.hasExtension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME))
                featureQuery.pNext = &faultQuery;
            instSnap.dispatch.GetPhysicalDeviceFeatures2(physicalDevice, &featureQuery);
            caps.shaderImageGatherExtended            = featureQuery.features.shaderImageGatherExtended == VK_TRUE;
            caps.shaderStorageImageReadWithoutFormat  = featureQuery.features.shaderStorageImageReadWithoutFormat == VK_TRUE;
            caps.shaderStorageImageWriteWithoutFormat = featureQuery.features.shaderStorageImageWriteWithoutFormat == VK_TRUE;
            caps.deviceFault                          = faultQuery.deviceFault == VK_TRUE;
        }
        else
        {
            Logger::warn("vkGetPhysicalDeviceFeatures2 unavailable: VkHook device features and fault reporting stay off");
        }

        caps.instanceApiVersion = instSnap.apiVersion;
        caps.mutableEnvValue = getMutableSwapchainEnvOverride(caps.mutableEnvOverridePresent, true);
        const bool gpuCrashDiagnosticsRequested = isGpuCrashDiagEnabled();
        caps.gpuCrashDiagnostics = gpuCrashDiagnosticsRequested;
#ifdef VKHOOK_DLSS5
        caps.dlss5          = true;
        caps.physicalDevice = physicalDevice;
        caps.queryFeatures  = instSnap.dispatch.GetPhysicalDeviceFeatures2;
#endif

        const std::unique_ptr<DevicePlan> plan = planDeviceCreate(caps, *pCreateInfo);
        for (const std::string& message : plan->infos)
            Logger::info(message);
        for (const std::string& message : plan->warnings)
            Logger::warn(message);

        VkResult ret = createFunc(physicalDevice, &plan->createInfo(), pAllocator, pDevice);

        if (ret != VK_SUCCESS)
            return ret;

        try
        {
            DeviceBuild build{physicalDevice, *pDevice, instSnap, *plan, caps, pCreateInfo, gipa, gdpa, gpuCrashDiagnosticsRequested};
            registerDevice(GetKey(*pDevice), buildLogicalDevice(build));
            return VK_SUCCESS;
        }
        catch (const std::exception& e)
        {
            reportHookException(nullptr, "vkCreateDevice", e.what());
        }
        catch (...)
        {
            reportHookException(nullptr, "vkCreateDevice", nullptr);
        }

        // The layer could not set the device up. Fail the creation cleanly
        // rather than hand the app a device the layer does not track.
        if (auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(gdpa(*pDevice, "vkDestroyDevice")))
            destroyDevice(*pDevice, pAllocator);
        *pDevice = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    void VKAPI_CALL VkHook_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
    {
        if (!device)
            return;

        Logger::trace("vkDestroyDevice");

        auto keepAlive = findDevice(GetKey(device));
        if (!keepAlive)
        {
            failUnknownObject("vkDestroyDevice", VK_ERROR_DEVICE_LOST);
            return;
        }
        LogicalDevice* logicalDevice = keepAlive.get();

        guardLayerWork(logicalDevice, "vkDestroyDevice", [&] {
            // The only real vkDeviceWaitIdle in the layer, and nothing is held: the
            // app must have stopped using the device (AGENTS.md invariant 3). From
            // here every retired object can go.
            logicalDevice->vkd.DeviceWaitIdle(device);

            // swapchains first: their generations and shared images retire into the queue
            std::unordered_map<VkSwapchainKHR, std::shared_ptr<LogicalSwapchain>> swapchains;
            {
                std::lock_guard<std::mutex> l(logicalDevice->stateLock);
                swapchains.swap(logicalDevice->swapchains);
            }
            swapchains.clear();

#ifdef VKHOOK_DLSS5
            // the effects are gone: drop the cached networks of this device before its
            // handle can be reused by a later one
            clearDlss5ModelCache(device);
#endif

            // the overlay's UI uses device resources; every renderer that drew with it
            // went with the swapchains above
            logicalDevice->control.reset();

            destroyPersistentDepthStorage(logicalDevice);

            // The ring's teardown takes its own lock so it cannot race a QueueSubmit
            // mid-copy; the device is idle, so the objects go directly.
            logicalDevice->depthCopy.withRing([&](DepthCopyState::RingAccess& ring) {
                VkCommandPool pool = VK_NULL_HANDLE;
                std::vector<VkFence> fences;
                ring.detach(pool, fences);

                if (pool == VK_NULL_HANDLE && fences.empty())
                    return;

                Logger::debug("DestroyCommandPool (depth copy ring buffer)");
                for (VkFence f : fences)
                {
                    if (f != VK_NULL_HANDLE)
                        logicalDevice->vkd.DestroyFence(device, f, nullptr);
                }
                if (pool != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroyCommandPool(device, pool, pAllocator);
            });

            // everything the layer retired, including the overlay's handles above
            logicalDevice->retireQueue.drainAll();

            savePipelineCache(*logicalDevice);
            if (logicalDevice->pipelineCache != VK_NULL_HANDLE)
            {
                logicalDevice->vkd.DestroyPipelineCache(device, logicalDevice->pipelineCache, pAllocator);
                logicalDevice->pipelineCache = VK_NULL_HANDLE;
            }

            logicalDevice->queues.destroy();
        });

        unregisterDevice(GetKey(device));

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
