#include "layer/device_plan.hh"

#ifdef VKHOOK_DLSS5
#include "render/dlss5_device.hh"
#endif

#include <algorithm>
#include <cstring>

namespace VkHook
{
    namespace
    {
        constexpr const char* mutableFormatExtension   = "VK_KHR_swapchain_mutable_format";
        constexpr const char* imageFormatListExtension = "VK_KHR_image_format_list";

        // Sizes of the device-create pNext structs the layer can copy. A chain
        // node ahead of an edited one must be copied (its pNext changes), so an
        // unknown size here limits what the plan may modify, never what it passes on.
        size_t nodeSize(VkStructureType type)
        {
            switch (type)
            {
#define VKHOOK_NODE(stype, T) case stype: return sizeof(T);
            VKHOOK_NODE(VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VkLayerDeviceCreateInfo)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, VkPhysicalDeviceFeatures2)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, VkPhysicalDeviceVulkan11Features)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, VkPhysicalDeviceVulkan12Features)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, VkPhysicalDeviceVulkan13Features)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, VkPhysicalDeviceFaultFeaturesEXT)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV, VkDeviceDiagnosticsConfigCreateInfoNV)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV, VkPhysicalDeviceOpticalFlowFeaturesNV)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, VkPhysicalDevice16BitStorageFeatures)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, VkPhysicalDevice8BitStorageFeatures)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, VkPhysicalDeviceShaderFloat16Int8Features)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, VkPhysicalDeviceSynchronization2Features)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, VkPhysicalDeviceTimelineSemaphoreFeatures)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VkPhysicalDeviceBufferDeviceAddressFeatures)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, VkPhysicalDeviceDescriptorIndexingFeatures)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, VkPhysicalDeviceDynamicRenderingFeatures)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, VkPhysicalDeviceMaintenance4Features)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, VkPhysicalDeviceMultiviewFeatures)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, VkPhysicalDeviceSamplerYcbcrConversionFeatures)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, VkPhysicalDeviceScalarBlockLayoutFeatures)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, VkPhysicalDeviceHostQueryResetFeatures)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, VkPhysicalDeviceUniformBufferStandardLayoutFeatures)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, VkPhysicalDeviceVariablePointersFeatures)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, VkPhysicalDevicePrivateDataFeatures)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, VkPhysicalDeviceRobustness2FeaturesEXT)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, VkPhysicalDeviceCustomBorderColorFeaturesEXT)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicStateFeaturesEXT)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicState2FeaturesEXT)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicState3FeaturesEXT)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, VkPhysicalDeviceDepthClipEnableFeaturesEXT)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, VkPhysicalDeviceTransformFeedbackFeaturesEXT)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, VkPhysicalDeviceAccelerationStructureFeaturesKHR)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, VkPhysicalDeviceRayTracingPipelineFeaturesKHR)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, VkPhysicalDeviceRayQueryFeaturesKHR)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR, VkPhysicalDeviceFragmentShadingRateFeaturesKHR)
            VKHOOK_NODE(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, VkPhysicalDeviceMeshShaderFeaturesEXT)
#undef VKHOOK_NODE
            default: return 0;
            }
        }

        struct Chain
        {
            std::vector<const VkBaseInStructure*> nodes;
            size_t firstUncopyable = 0; // nodes.size() when every node can be copied

            explicit Chain(const void* head)
            {
                for (auto* n = static_cast<const VkBaseInStructure*>(head); n; n = n->pNext)
                    nodes.push_back(n);
                firstUncopyable = nodes.size();
                for (size_t i = 0; i < nodes.size(); ++i)
                    if (!nodeSize(nodes[i]->sType)) { firstUncopyable = i; break; }
            }
            // index of the first node of `type`, or -1
            int find(VkStructureType type) const
            {
                for (size_t i = 0; i < nodes.size(); ++i)
                    if (nodes[i]->sType == type) return static_cast<int>(i);
                return -1;
            }
            bool modifiable(int index) const { return index >= 0 && static_cast<size_t>(index) < firstUncopyable; }
        };

        bool hasName(const std::vector<const char*>& names, const char* name)
        {
            return std::any_of(names.begin(), names.end(), [&](const char* n) { return std::strcmp(n, name) == 0; });
        }

        // appends a copy of `name` unless the list already holds it
        bool addName(std::deque<std::string>& storage, std::vector<const char*>& ptrs, const char* name)
        {
            if (hasName(ptrs, name))
                return false;
            storage.emplace_back(name);
            ptrs.push_back(storage.back().c_str());
            return true;
        }

        void copyNames(std::deque<std::string>& storage, std::vector<const char*>& ptrs, const char* const* names, uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i)
                addName(storage, ptrs, names[i]);
        }
    }

    bool DeviceCaps::hasExtension(const char* name) const
    {
        return std::any_of(extensions.begin(), extensions.end(),
                           [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
    }

    DevicePlan::~DevicePlan() = default;

    bool DevicePlan::hasEnabledExtension(const char* name) const { return hasName(extensionPtrs, name); }

    std::unique_ptr<InstancePlan> planInstanceCreate(const InstanceCaps& caps, const VkInstanceCreateInfo& app)
    {
        auto plan = std::make_unique<InstancePlan>();
        plan->info = app;

        VkApplicationInfo& appInfo = plan->appInfo;
        if (app.pApplicationInfo)
        {
            appInfo = *app.pApplicationInfo;
            if (appInfo.pApplicationName)
            {
                plan->applicationName = appInfo.pApplicationName;
                appInfo.pApplicationName = plan->applicationName.c_str();
            }
            if (appInfo.pEngineName)
            {
                plan->engineName = appInfo.pEngineName;
                appInfo.pEngineName = plan->engineName.c_str();
            }
        }
        else
        {
            appInfo = {};
            appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        }
        appInfo.apiVersion = std::max(appInfo.apiVersion, static_cast<uint32_t>(VK_API_VERSION_1_1));
        if (caps.wantApi13 && caps.loaderApiVersion >= VK_API_VERSION_1_3)
            appInfo.apiVersion = std::max(appInfo.apiVersion, static_cast<uint32_t>(VK_API_VERSION_1_3));

        // Only what the application asked for: the layer needs no instance extension.
        plan->extensionStorage.reserve(app.enabledExtensionCount);
        for (uint32_t i = 0; i < app.enabledExtensionCount; ++i)
            plan->extensionStorage.emplace_back(app.ppEnabledExtensionNames[i]);
        for (const std::string& s : plan->extensionStorage)
            plan->extensionPtrs.push_back(s.c_str());
        plan->layerStorage.reserve(app.enabledLayerCount);
        for (uint32_t i = 0; i < app.enabledLayerCount; ++i)
            plan->layerStorage.emplace_back(app.ppEnabledLayerNames[i]);
        for (const std::string& s : plan->layerStorage)
            plan->layerPtrs.push_back(s.c_str());

        plan->info.pApplicationInfo        = &appInfo;
        plan->info.enabledExtensionCount   = static_cast<uint32_t>(plan->extensionPtrs.size());
        plan->info.ppEnabledExtensionNames = plan->extensionPtrs.data();
        plan->info.enabledLayerCount       = static_cast<uint32_t>(plan->layerPtrs.size());
        plan->info.ppEnabledLayerNames     = plan->layerPtrs.data();
        return plan;
    }

    std::unique_ptr<DevicePlan> planDeviceCreate(const DeviceCaps& caps, const VkDeviceCreateInfo& app)
    {
        auto plan = std::make_unique<DevicePlan>();
        DevicePlan& p = *plan;
        p.info = app;

        const uint32_t effectiveApi = std::min(caps.properties.apiVersion, caps.instanceApiVersion);

        // ---- extensions: the app's list, then what the layer adds ----
        if (app.enabledExtensionCount)
            copyNames(p.extensionStorage, p.extensionPtrs, app.ppEnabledExtensionNames, app.enabledExtensionCount);
        if (app.enabledLayerCount)
            copyNames(p.layerStorage, p.layerPtrs, app.ppEnabledLayerNames, app.enabledLayerCount);

        const bool appHasSwapchain = hasName(p.extensionPtrs, VK_KHR_SWAPCHAIN_EXTENSION_NAME);

        if (effectiveApi < VK_API_VERSION_1_2 && !hasName(p.extensionPtrs, imageFormatListExtension))
        {
            if (caps.hasExtension(imageFormatListExtension))
                addName(p.extensionStorage, p.extensionPtrs, imageFormatListExtension);
            else
                p.warnings.emplace_back("VK_KHR_image_format_list is not supported by this device");
        }
        p.enableImageFormatList = hasName(p.extensionPtrs, imageFormatListExtension);

        const bool mutableRequested = caps.mutableEnvOverridePresent ? caps.mutableEnvValue : true;
        const bool formatListOk     = effectiveApi >= VK_API_VERSION_1_2 || p.enableImageFormatList;
        if (caps.hasExtension(mutableFormatExtension) && mutableRequested && appHasSwapchain && formatListOk)
        {
            p.infos.emplace_back("activating mutable_format");
            addName(p.extensionStorage, p.extensionPtrs, mutableFormatExtension);
        }
        else if (caps.mutableEnvOverridePresent)
        {
            p.infos.emplace_back(std::string("Mutable swapchain device extension set via VKHOOK_ENABLE_MUTABLE_SWAPCHAIN=")
                                 + (mutableRequested ? "1" : "0"));
        }
        p.supportsMutableFormat = mutableRequested && hasName(p.extensionPtrs, mutableFormatExtension);

        if (caps.gpuCrashDiagnostics)
        {
            if (caps.hasExtension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME))
            {
                addName(p.extensionStorage, p.extensionPtrs, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
                p.enableNvCheckpoints = true;
            }
            else
                p.warnings.emplace_back("VKHOOK_GPU_CRASH_DIAGNOSTICS=1 but VK_NV_device_diagnostic_checkpoints is not supported by this device");

            if (caps.hasExtension(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME))
            {
                addName(p.extensionStorage, p.extensionPtrs, VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
                p.enableNvDiagnosticsConfig = true;
            }
            else
                p.warnings.emplace_back("VKHOOK_GPU_CRASH_DIAGNOSTICS=1 but VK_NV_device_diagnostics_config is not supported by this device");

            if (caps.hasExtension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME))
            {
                addName(p.extensionStorage, p.extensionPtrs, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
                p.enableDeviceFault = true;
            }
            else
                p.warnings.emplace_back("VKHOOK_GPU_CRASH_DIAGNOSTICS=1 but VK_EXT_device_fault is not supported by this device");
        }

        // ---- feature chain ----
        const Chain chain(app.pNext);
        // The application's own choice: the capture's layouts follow it and the
        // layer never turns the feature on itself.
        for (const VkBaseInStructure* node : chain.nodes)
        {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES)
                p.separateDepthStencilLayouts |=
                    reinterpret_cast<const VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures*>(node)->separateDepthStencilLayouts == VK_TRUE;
            else if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
                p.separateDepthStencilLayouts |=
                    reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node)->separateDepthStencilLayouts == VK_TRUE;
        }
        const int features2Index = chain.find(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        const int faultIndex     = p.enableDeviceFault ? chain.find(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT) : -1;
        const int diagIndex      = p.enableNvDiagnosticsConfig ? chain.find(VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV) : -1;

        int lastCopied = -1;
        for (int index : {features2Index, faultIndex, diagIndex})
            if (chain.modifiable(index))
                lastCopied = std::max(lastCopied, index);

        std::vector<VkBaseOutStructure*> copies;
        for (int i = 0; i <= lastCopied; ++i)
        {
            const size_t size = nodeSize(chain.nodes[i]->sType);
            p.chainCopies.push_back(std::make_unique<std::byte[]>(size));
            std::memcpy(p.chainCopies.back().get(), chain.nodes[i], size);
            copies.push_back(reinterpret_cast<VkBaseOutStructure*>(p.chainCopies.back().get()));
        }
        for (int i = 0; i <= lastCopied; ++i)
            copies[i]->pNext = i < lastCopied ? copies[i + 1]
                                              : const_cast<VkBaseOutStructure*>(
                                                    reinterpret_cast<const VkBaseOutStructure*>(
                                                        static_cast<size_t>(i + 1) < chain.nodes.size() ? chain.nodes[i + 1] : nullptr));

        const void* head = lastCopied >= 0 ? static_cast<const void*>(copies[0])
                                           : (chain.nodes.empty() ? nullptr : static_cast<const void*>(chain.nodes[0]));

        auto wantFeatures = [&](VkPhysicalDeviceFeatures& f) {
            if (caps.shaderImageGatherExtended)            f.shaderImageGatherExtended = VK_TRUE;
            if (caps.shaderStorageImageReadWithoutFormat)  f.shaderStorageImageReadWithoutFormat = VK_TRUE;
            if (caps.shaderStorageImageWriteWithoutFormat) f.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        };
        auto reportFeatures = [&](const VkPhysicalDeviceFeatures& f) {
            p.enableShaderImageGatherExtended           = f.shaderImageGatherExtended == VK_TRUE;
            p.enableShaderStorageImageReadWithoutFormat  = f.shaderStorageImageReadWithoutFormat == VK_TRUE;
            p.enableShaderStorageImageWriteWithoutFormat = f.shaderStorageImageWriteWithoutFormat == VK_TRUE;
        };

        if (features2Index >= 0)
        {
            // Features2 and pEnabledFeatures must never coexist
            p.info.pEnabledFeatures = nullptr;
            if (chain.modifiable(features2Index))
            {
                auto* f2 = reinterpret_cast<VkPhysicalDeviceFeatures2*>(copies[features2Index]);
                wantFeatures(f2->features);
                reportFeatures(f2->features);
            }
            else
            {
                reportFeatures(reinterpret_cast<const VkPhysicalDeviceFeatures2*>(chain.nodes[features2Index])->features);
                p.warnings.emplace_back("application Features2 follows an unknown pNext struct; VkHook features not merged");
            }
        }
        else
        {
            if (app.pEnabledFeatures)
                p.enabledFeatures = *app.pEnabledFeatures;
            wantFeatures(p.enabledFeatures);
            reportFeatures(p.enabledFeatures);
            const bool wantsAny = app.pEnabledFeatures || p.enableShaderImageGatherExtended
                || p.enableShaderStorageImageReadWithoutFormat || p.enableShaderStorageImageWriteWithoutFormat;
            p.info.pEnabledFeatures = wantsAny ? &p.enabledFeatures : nullptr;
        }

        if (p.enableDeviceFault)
        {
            if (faultIndex >= 0)
            {
                if (chain.modifiable(faultIndex))
                {
                    auto* fault = reinterpret_cast<VkPhysicalDeviceFaultFeaturesEXT*>(copies[faultIndex]);
                    if (caps.deviceFault)
                        fault->deviceFault = VK_TRUE;
                    p.enableDeviceFaultFeature = fault->deviceFault == VK_TRUE;
                }
                else
                {
                    p.enableDeviceFaultFeature =
                        reinterpret_cast<const VkPhysicalDeviceFaultFeaturesEXT*>(chain.nodes[faultIndex])->deviceFault == VK_TRUE;
                }
            }
            else if (caps.deviceFault)
            {
                p.faultFeatures.sType       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
                p.faultFeatures.deviceFault = VK_TRUE;
                p.faultFeatures.pNext       = const_cast<void*>(head);
                head                        = &p.faultFeatures;
                p.enableDeviceFaultFeature  = true;
            }
            else
                p.warnings.emplace_back("VK_EXT_device_fault is present but deviceFault feature is not supported");
        }

        if (p.enableNvDiagnosticsConfig)
        {
            constexpr VkDeviceDiagnosticsConfigFlagsNV flags =
                VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV
                | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV;
            if (diagIndex >= 0)
            {
                if (chain.modifiable(diagIndex))
                    reinterpret_cast<VkDeviceDiagnosticsConfigCreateInfoNV*>(copies[diagIndex])->flags |= flags;
            }
            else
            {
                p.diagnosticsConfig.sType = VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV;
                p.diagnosticsConfig.flags = flags;
                p.diagnosticsConfig.pNext = head;
                head                      = &p.diagnosticsConfig;
            }
        }
        p.info.pNext = head;

        // ---- queues: always copied, the optical-flow queue may be appended ----
        p.queueInfos.assign(app.pQueueCreateInfos, app.pQueueCreateInfos + app.queueCreateInfoCount);
        for (VkDeviceQueueCreateInfo& queue : p.queueInfos)
        {
            if (!queue.pQueuePriorities)
                continue;
            p.queuePriorities.emplace_back(queue.pQueuePriorities, queue.pQueuePriorities + queue.queueCount);
            queue.pQueuePriorities = p.queuePriorities.back().data();
        }

        p.info.enabledExtensionCount   = static_cast<uint32_t>(p.extensionPtrs.size());
        p.info.ppEnabledExtensionNames = p.extensionPtrs.data();
        p.info.enabledLayerCount       = static_cast<uint32_t>(p.layerPtrs.size());
        p.info.ppEnabledLayerNames     = p.layerPtrs.data();

#ifdef VKHOOK_DLSS5
        p.dlss5UnavailableReason = "DLSS5 not requested";
        if (caps.dlss5)
        {
            auto dlss5 = std::make_shared<Dlss5DeviceFeatures>();
            const auto opticalQueue = selectDlss5OpticalQueue(caps.queueFamilies, app);
            p.opticalFlowFamily = opticalQueue.family;
            p.dlss5Supported = dlss5->prepare(caps.physicalDevice, caps.instanceApiVersion, caps.queryFeatures,
                                              caps.properties, caps.extensions, p.info, p.extensionPtrs,
                                              opticalQueue.family != VK_QUEUE_FAMILY_IGNORED);
            p.dlss5UnavailableReason = dlss5->error();
            p.dlss5State = dlss5;

            // prepare may append names it owns statically; make the list self-contained
            std::deque<std::string> owned;
            for (const char*& name : p.extensionPtrs)
            {
                owned.emplace_back(name);
                name = owned.back().c_str();
            }
            p.extensionStorage.swap(owned);
            p.info.ppEnabledExtensionNames = p.extensionPtrs.data();
            p.info.enabledExtensionCount   = static_cast<uint32_t>(p.extensionPtrs.size());

            if (p.dlss5Supported && dlss5->opticalFlowEnabled())
            {
                p.opticalFlowQueueShared = opticalQueue.shared;
                if (!opticalQueue.shared)
                {
                    VkDeviceQueueCreateInfo flowQueue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                    flowQueue.queueFamilyIndex = p.opticalFlowFamily;
                    flowQueue.queueCount       = 1;
                    p.queuePriorities.emplace_back(1, 1.0f);
                    flowQueue.pQueuePriorities = p.queuePriorities.back().data();
                    p.queueInfos.push_back(flowQueue);
                }
                p.infos.emplace_back("DLSS5 optical flow: " + std::string(opticalQueue.shared ? "sharing application" : "reserving private")
                                     + " queue family " + std::to_string(p.opticalFlowFamily) + ", queue 0");
            }
            else
            {
                p.opticalFlowFamily = VK_QUEUE_FAMILY_IGNORED;
                if (p.dlss5Supported)
                    p.warnings.emplace_back(opticalQueue.family == VK_QUEUE_FAMILY_IGNORED
                        ? "DLSS5 temporal unavailable: no usable separate optical-flow queue (protected/mixed queues cannot be shared)"
                        : "DLSS5 temporal unavailable: VK_NV_optical_flow extension or opticalFlow feature not supported");
            }
        }
#endif

        p.info.queueCreateInfoCount = static_cast<uint32_t>(p.queueInfos.size());
        p.info.pQueueCreateInfos    = p.queueInfos.data();
        return plan;
    }
}
