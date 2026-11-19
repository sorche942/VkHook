#pragma once

// Pure planning of vkCreateInstance / vkCreateDevice (AGENTS.md invariant 9).
//
// The interceptors gather capabilities with Vulkan queries, hand them to a plan
// function together with the application's create info, and call the next layer
// with the plan's create info. The plan functions never call Vulkan themselves
// (the only function pointer they use is the optional DLSS5 feature query in
// DeviceCaps), never mutate the application's memory, and return an object that
// owns every byte they changed.
//
// Ownership rules for the created info:
//  - extension/layer names, application strings, queue create infos with their
//    priorities, enabled features and every pNext struct the layer modified or
//    added are deep copies owned by the plan;
//  - pNext structs the layer does not modify (loader callbacks after the last
//    modified node, queue-level pNext, VkApplicationInfo::pNext, unmodified
//    instance chains) stay linked by reference to the application's memory, which
//    must outlive the create call as the Vulkan spec already requires;
//  - a chain node ahead of a modified node is copied too (its pNext changes) when
//    its size is known. If an unknown struct precedes a node the layer would
//    have to edit, that node is left untouched and the feature it carries is
//    reported as not enabled.
//
// Plans are neither copyable nor movable: their create infos point into
// themselves. Callers hold them by unique_ptr.

#include "vulkan_include.hh"

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace VkHook
{
    struct InstanceCaps
    {
        uint32_t loaderApiVersion = VK_API_VERSION_1_0;
        // DLSS5 needs a 1.3 instance when the loader offers one
        bool     wantApi13 = false;
    };

    class InstancePlan
    {
    public:
        InstancePlan() = default;
        InstancePlan(const InstancePlan&)            = delete;
        InstancePlan& operator=(const InstancePlan&) = delete;

        const VkInstanceCreateInfo& createInfo() const { return info; }
        uint32_t                    apiVersion() const { return appInfo.apiVersion; }

    private:
        friend std::unique_ptr<InstancePlan> planInstanceCreate(const InstanceCaps&, const VkInstanceCreateInfo&);

        VkInstanceCreateInfo     info{};
        VkApplicationInfo        appInfo{};
        std::string              applicationName;
        std::string              engineName;
        std::vector<std::string> extensionStorage;
        std::vector<const char*> extensionPtrs;
        std::vector<std::string> layerStorage;
        std::vector<const char*> layerPtrs;
    };

    std::unique_ptr<InstancePlan> planInstanceCreate(const InstanceCaps& caps, const VkInstanceCreateInfo& app);

    struct DeviceCaps
    {
        std::vector<VkExtensionProperties> extensions; // supported by the physical device
        VkPhysicalDeviceProperties         properties{};
        bool shaderImageGatherExtended              = false;
        bool shaderStorageImageReadWithoutFormat    = false;
        bool shaderStorageImageWriteWithoutFormat   = false;
        bool deviceFault                            = false;
        std::vector<VkQueueFamilyProperties> queueFamilies;
        uint32_t instanceApiVersion                 = VK_API_VERSION_1_0;
        // VKHOOK_ENABLE_MUTABLE_SWAPCHAIN
        bool mutableEnvOverridePresent              = false;
        bool mutableEnvValue                        = true;
        // VKHOOK_GPU_CRASH_DIAGNOSTICS
        bool gpuCrashDiagnostics                    = false;
        // DLSS5 negotiation (ignored unless the backend is built)
        bool                             dlss5         = false;
        VkPhysicalDevice                 physicalDevice = VK_NULL_HANDLE;
        PFN_vkGetPhysicalDeviceFeatures2 queryFeatures  = nullptr;

        bool hasExtension(const char* name) const;
    };

    class DevicePlan
    {
    public:
        DevicePlan() = default;
        ~DevicePlan();
        DevicePlan(const DevicePlan&)            = delete;
        DevicePlan& operator=(const DevicePlan&) = delete;

        const VkDeviceCreateInfo& createInfo() const { return info; }

        // what the layer ended up enabling
        bool supportsMutableFormat                  = false; // extension active and not vetoed by the env override
        bool enableImageFormatList                  = false; // VK_KHR_image_format_list in the final list
        bool enableNvCheckpoints                    = false;
        bool enableNvDiagnosticsConfig              = false;
        bool enableDeviceFault                      = false; // VK_EXT_device_fault extension
        bool enableDeviceFaultFeature               = false; // deviceFault feature bit set in the final chain
        bool enableShaderImageGatherExtended        = false;
        bool enableShaderStorageImageReadWithoutFormat  = false;
        bool enableShaderStorageImageWriteWithoutFormat = false;
        bool separateDepthStencilLayouts            = false; // the app enabled it (separate depth/stencil layouts usable)
        bool dlss5Supported                         = false;
        std::string dlss5UnavailableReason          = "DLSS5 backend not built";
        uint32_t opticalFlowFamily                  = VK_QUEUE_FAMILY_IGNORED;
        bool     opticalFlowQueueShared             = false; // true: borrows an application queue, nothing reserved

        // messages for the caller to log; the plan itself never logs
        std::vector<std::string> infos;
        std::vector<std::string> warnings;

        bool hasEnabledExtension(const char* name) const;

    private:
        friend std::unique_ptr<DevicePlan> planDeviceCreate(const DeviceCaps&, const VkDeviceCreateInfo&);

        VkDeviceCreateInfo info{};

        std::deque<std::string>              extensionStorage;
        std::vector<const char*>             extensionPtrs;
        std::deque<std::string>              layerStorage;
        std::vector<const char*>             layerPtrs;
        std::vector<VkDeviceQueueCreateInfo> queueInfos;
        std::deque<std::vector<float>>       queuePriorities;
        VkPhysicalDeviceFeatures             enabledFeatures{};
        std::vector<std::unique_ptr<std::byte[]>> chainCopies;
        VkPhysicalDeviceFaultFeaturesEXT     faultFeatures{};
        VkDeviceDiagnosticsConfigCreateInfoNV diagnosticsConfig{};
        std::shared_ptr<void>                dlss5State; // keeps the DLSS5 pNext chain alive
    };

    std::unique_ptr<DevicePlan> planDeviceCreate(const DeviceCaps& caps, const VkDeviceCreateInfo& app);
}
