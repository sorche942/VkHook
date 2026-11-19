#include "layer/device_plan.hh"

#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

#ifdef VKHOOK_DLSS5
#include "render/dlss5_device.hh"
#include "vk_context.h"
#endif

using namespace VkHook;

static void require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

static VkExtensionProperties extension(const char* name)
{
    VkExtensionProperties e{};
    std::strncpy(e.extensionName, name, VK_MAX_EXTENSION_NAME_SIZE - 1);
    return e;
}

static DeviceCaps fullCaps()
{
    DeviceCaps caps;
    for (const char* name : {VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_swapchain_mutable_format", "VK_KHR_image_format_list",
                             VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME, VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME,
                             VK_EXT_DEVICE_FAULT_EXTENSION_NAME})
        caps.extensions.push_back(extension(name));
    caps.properties.apiVersion                  = VK_API_VERSION_1_1;
    caps.instanceApiVersion                     = VK_API_VERSION_1_1;
    caps.shaderImageGatherExtended              = true;
    caps.shaderStorageImageReadWithoutFormat    = true;
    caps.shaderStorageImageWriteWithoutFormat   = true;
    caps.deviceFault                            = true;
    VkQueueFamilyProperties family{};
    family.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    family.queueCount = 1;
    caps.queueFamilies.push_back(family);
    return caps;
}

static VkDeviceQueueCreateInfo queueInfo(uint32_t family, const float* priority)
{
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = family;
    q.queueCount       = 1;
    q.pQueuePriorities = priority;
    return q;
}

template <typename T>
static const T* findNode(const void* chain, VkStructureType type, int* count = nullptr)
{
    const T* found = nullptr;
    int      n     = 0;
    for (auto* node = static_cast<const VkBaseInStructure*>(chain); node; node = node->pNext)
        if (node->sType == type)
        {
            found = reinterpret_cast<const T*>(node);
            ++n;
        }
    if (count)
        *count = n;
    return found;
}

static bool hasExt(const VkDeviceCreateInfo& info, const char* name)
{
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
        if (std::strcmp(info.ppEnabledExtensionNames[i], name) == 0)
            return true;
    return false;
}

static void requireNoDuplicates(const VkDeviceCreateInfo& info)
{
    std::set<std::string> seen;
    for (uint32_t i = 0; i < info.enabledExtensionCount; ++i)
        require(seen.insert(info.ppEnabledExtensionNames[i]).second, "duplicate extension");
}

static void features2Merged()
{
    DeviceCaps caps = fullCaps();
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo q = queueInfo(0, &priority);
    const char* appExt[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features.robustBufferAccess = VK_TRUE;
    f2.pNext = &f12;
    VkLayerDeviceCreateInfo loader{};
    loader.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
    loader.function = VK_LAYER_LINK_INFO;
    loader.pNext = &f2;
    VkDeviceCreateInfo app{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    app.pNext = &loader;
    app.queueCreateInfoCount = 1;
    app.pQueueCreateInfos = &q;
    app.enabledExtensionCount = 1;
    app.ppEnabledExtensionNames = appExt;

    auto plan = planDeviceCreate(caps, app);
    const VkDeviceCreateInfo& out = plan->createInfo();
    require(out.pEnabledFeatures == nullptr, "pEnabledFeatures set alongside Features2");
    int count = 0;
    auto* merged = findNode<VkPhysicalDeviceFeatures2>(out.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &count);
    require(merged && count == 1, "Features2 missing or duplicated");
    require(merged != &f2, "app Features2 reused instead of copied");
    require(merged->features.robustBufferAccess, "app feature lost");
    require(merged->features.shaderImageGatherExtended && merged->features.shaderStorageImageReadWithoutFormat
                && merged->features.shaderStorageImageWriteWithoutFormat, "wanted features not merged");
    require(!f2.features.shaderImageGatherExtended && !f2.features.shaderStorageImageReadWithoutFormat
                && !f2.features.shaderStorageImageWriteWithoutFormat, "app Features2 mutated");
    require(f2.pNext == &f12 && loader.pNext == &f2 && app.pNext == &loader, "app chain relinked");
    auto* tail12 = findNode<VkPhysicalDeviceVulkan12Features>(out.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    require(tail12 == &f12, "unmodified node after the edit should stay linked by reference");
    require(plan->enableShaderImageGatherExtended && plan->enableShaderStorageImageReadWithoutFormat
                && plan->enableShaderStorageImageWriteWithoutFormat, "plan does not report features");
    require(findNode<VkLayerDeviceCreateInfo>(out.pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) != nullptr,
            "loader struct dropped");
}

static void enabledFeaturesCopied()
{
    DeviceCaps caps = fullCaps();
    VkPhysicalDeviceFeatures features{};
    features.geometryShader = VK_TRUE;
    VkDeviceCreateInfo app{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    app.pEnabledFeatures = &features;
    auto plan = planDeviceCreate(caps, app);
    const VkDeviceCreateInfo& out = plan->createInfo();
    require(out.pEnabledFeatures && out.pEnabledFeatures != &features, "features not copied");
    require(out.pEnabledFeatures->geometryShader && out.pEnabledFeatures->shaderImageGatherExtended
                && out.pEnabledFeatures->shaderStorageImageReadWithoutFormat
                && out.pEnabledFeatures->shaderStorageImageWriteWithoutFormat, "features not merged");
    require(!features.shaderImageGatherExtended, "app features mutated");
    require(out.pNext == nullptr, "no chain expected");
}

static void unsupportedFeaturesNotEnabled()
{
    DeviceCaps caps = fullCaps();
    caps.shaderImageGatherExtended = false;
    caps.shaderStorageImageWriteWithoutFormat = false;
    VkDeviceCreateInfo app{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    auto plan = planDeviceCreate(caps, app);
    require(plan->createInfo().pEnabledFeatures, "supported feature dropped");
    require(!plan->createInfo().pEnabledFeatures->shaderImageGatherExtended
                && !plan->createInfo().pEnabledFeatures->shaderStorageImageWriteWithoutFormat
                && plan->createInfo().pEnabledFeatures->shaderStorageImageReadWithoutFormat, "unsupported feature forced");
    require(!plan->enableShaderImageGatherExtended && plan->enableShaderStorageImageReadWithoutFormat, "plan flags wrong");

    caps = fullCaps();
    caps.shaderImageGatherExtended = caps.shaderStorageImageReadWithoutFormat = caps.shaderStorageImageWriteWithoutFormat = false;
    plan = planDeviceCreate(caps, app);
    require(plan->createInfo().pEnabledFeatures == nullptr, "pEnabledFeatures created with nothing to enable");

    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    app.pNext = &f2;
    plan = planDeviceCreate(caps, app);
    auto* merged = findNode<VkPhysicalDeviceFeatures2>(plan->createInfo().pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    require(merged && !merged->features.shaderImageGatherExtended, "unsupported feature forced into Features2");
}

static void unknownNodeBeforeFeatures2()
{
    DeviceCaps caps = fullCaps();
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkBaseOutStructure unknown{static_cast<VkStructureType>(0x7ffffff0), reinterpret_cast<VkBaseOutStructure*>(&f2)};
    VkDeviceCreateInfo app{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    app.pNext = &unknown;
    auto plan = planDeviceCreate(caps, app);
    require(plan->createInfo().pNext == &unknown && plan->createInfo().pEnabledFeatures == nullptr,
            "chain with unknown leading node must pass through unmodified");
    require(!f2.features.shaderImageGatherExtended && !plan->enableShaderImageGatherExtended, "uncopyable Features2 edited");
    require(!plan->warnings.empty(), "no warning for skipped merge");
}

static void mutableFormat()
{
    DeviceCaps caps = fullCaps();
    const char* swapchain[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo app{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};

    auto plan = planDeviceCreate(caps, app);
    require(!plan->supportsMutableFormat && !hasExt(plan->createInfo(), "VK_KHR_swapchain_mutable_format"),
            "mutable format without VK_KHR_swapchain");

    app.enabledExtensionCount = 1;
    app.ppEnabledExtensionNames = swapchain;
    plan = planDeviceCreate(caps, app);
    require(plan->supportsMutableFormat && hasExt(plan->createInfo(), "VK_KHR_swapchain_mutable_format"), "mutable format not enabled");
    require(plan->enableImageFormatList && hasExt(plan->createInfo(), "VK_KHR_image_format_list"),
            "image_format_list missing below 1.2");

    DeviceCaps modern = caps;
    modern.properties.apiVersion = modern.instanceApiVersion = VK_API_VERSION_1_3;
    plan = planDeviceCreate(modern, app);
    require(plan->supportsMutableFormat && !hasExt(plan->createInfo(), "VK_KHR_image_format_list"),
            "image_format_list added on 1.2+");

    DeviceCaps noMutable = caps;
    noMutable.extensions.erase(noMutable.extensions.begin() + 1);
    plan = planDeviceCreate(noMutable, app);
    require(!plan->supportsMutableFormat && !hasExt(plan->createInfo(), "VK_KHR_swapchain_mutable_format"), "unsupported mutable enabled");

    DeviceCaps noList = caps;
    noList.extensions.erase(noList.extensions.begin() + 2);
    plan = planDeviceCreate(noList, app);
    require(!plan->supportsMutableFormat && !hasExt(plan->createInfo(), "VK_KHR_image_format_list"),
            "mutable format enabled without image_format_list");

    DeviceCaps off = caps;
    off.mutableEnvOverridePresent = true;
    off.mutableEnvValue = false;
    plan = planDeviceCreate(off, app);
    require(!plan->supportsMutableFormat && !hasExt(plan->createInfo(), "VK_KHR_swapchain_mutable_format"), "env override off ignored");
    require(!plan->infos.empty(), "override not reported");
    off.mutableEnvValue = true;
    plan = planDeviceCreate(off, app);
    require(plan->supportsMutableFormat, "env override on ignored");
}

static void separateDepthStencil()
{
    DeviceCaps caps = fullCaps();
    VkDeviceCreateInfo app{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    require(!planDeviceCreate(caps, app)->separateDepthStencilLayouts, "default must be off");

    VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures sep{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES};
    app.pNext = &sep;
    require(!planDeviceCreate(caps, app)->separateDepthStencilLayouts, "struct present but off");
    sep.separateDepthStencilLayouts = VK_TRUE;
    require(planDeviceCreate(caps, app)->separateDepthStencilLayouts, "1.2 extension struct not seen");

    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &v12;
    app.pNext = &f2;
    require(!planDeviceCreate(caps, app)->separateDepthStencilLayouts, "v12 off");
    v12.separateDepthStencilLayouts = VK_TRUE;
    require(planDeviceCreate(caps, app)->separateDepthStencilLayouts, "v12 struct not seen");
}

static void faultFeatures()
{
    DeviceCaps caps = fullCaps();
    caps.gpuCrashDiagnostics = true;
    VkDeviceCreateInfo app{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};

    auto plan = planDeviceCreate(caps, app);
    require(plan->enableNvCheckpoints && plan->enableNvDiagnosticsConfig && plan->enableDeviceFault && plan->enableDeviceFaultFeature,
            "diagnostics not planned");
    int n = 0;
    auto* fault = findNode<VkPhysicalDeviceFaultFeaturesEXT>(plan->createInfo().pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, &n);
    require(fault && n == 1 && fault->deviceFault, "fault features not added");
    auto* diag = findNode<VkDeviceDiagnosticsConfigCreateInfoNV>(plan->createInfo().pNext,
                                                                  VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV, &n);
    require(diag && n == 1 && diag->flags, "diagnostics config not added");

    // app-chained structs: edited in copies, never duplicated
    VkPhysicalDeviceFaultFeaturesEXT appFault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    VkDeviceDiagnosticsConfigCreateInfoNV appDiag{VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV};
    appDiag.pNext = &appFault;
    app.pNext = &appDiag;
    plan = planDeviceCreate(caps, app);
    fault = findNode<VkPhysicalDeviceFaultFeaturesEXT>(plan->createInfo().pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, &n);
    require(fault && n == 1 && fault != &appFault && fault->deviceFault && plan->enableDeviceFaultFeature, "app fault struct not merged");
    diag = findNode<VkDeviceDiagnosticsConfigCreateInfoNV>(plan->createInfo().pNext,
                                                            VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV, &n);
    require(diag && n == 1 && diag != &appDiag && diag->flags, "app diagnostics struct not merged");
    require(!appFault.deviceFault && !appDiag.flags && appDiag.pNext == &appFault, "app structs mutated");

    // unsupported feature bit: extension on, feature off, nothing added
    DeviceCaps noFeature = caps;
    noFeature.deviceFault = false;
    app.pNext = nullptr;
    plan = planDeviceCreate(noFeature, app);
    require(plan->enableDeviceFault && !plan->enableDeviceFaultFeature
                && !findNode<VkPhysicalDeviceFaultFeaturesEXT>(plan->createInfo().pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT),
            "unsupported deviceFault feature enabled");

    DeviceCaps noExt = caps;
    noExt.extensions.clear();
    noExt.properties.apiVersion = noExt.instanceApiVersion = VK_API_VERSION_1_3;
    plan = planDeviceCreate(noExt, app);
    require(!plan->enableNvCheckpoints && !plan->enableNvDiagnosticsConfig && !plan->enableDeviceFault && plan->createInfo().pNext == nullptr,
            "unsupported diagnostics enabled");
    require(plan->warnings.size() == 3, "missing warnings");

    DeviceCaps off = caps;
    off.gpuCrashDiagnostics = false;
    plan = planDeviceCreate(off, app);
    require(!plan->enableDeviceFault && plan->createInfo().pNext == nullptr, "diagnostics enabled without request");
}

static void deepCopyIndependence()
{
    DeviceCaps caps = fullCaps();
    caps.gpuCrashDiagnostics = true;
    auto* name = new std::string(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    auto* dup  = new std::string(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    const char* names[] = {name->c_str(), dup->c_str(), VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME};
    auto* priority = new float(0.5f);
    auto* queue    = new VkDeviceQueueCreateInfo(queueInfo(0, priority));
    auto* f2       = new VkPhysicalDeviceFeatures2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2->features.robustBufferAccess = VK_TRUE;
    VkDeviceCreateInfo app{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    app.pNext = f2;
    app.queueCreateInfoCount = 1;
    app.pQueueCreateInfos = queue;
    app.enabledExtensionCount = 3;
    app.ppEnabledExtensionNames = names;

    auto plan = planDeviceCreate(caps, app);
    requireNoDuplicates(plan->createInfo());
    require(plan->createInfo().enabledExtensionCount == 5 || plan->createInfo().enabledExtensionCount == 6, "unexpected extension count");

    std::memset(&name[0][0], 'x', name->size());
    *priority = -1.0f;
    queue->queueFamilyIndex = 99;
    f2->features.robustBufferAccess = VK_FALSE;
    delete name; delete dup; delete priority; delete queue; delete f2;

    const VkDeviceCreateInfo& out = plan->createInfo();
    require(hasExt(out, VK_KHR_SWAPCHAIN_EXTENSION_NAME), "extension string not copied");
    require(out.queueCreateInfoCount == 1 && out.pQueueCreateInfos[0].queueFamilyIndex == 0
                && out.pQueueCreateInfos[0].pQueuePriorities[0] == 0.5f, "queue info not copied");
    auto* merged = findNode<VkPhysicalDeviceFeatures2>(out.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    require(merged && merged->features.robustBufferAccess, "Features2 not copied");
}

static void instancePlan()
{
    InstanceCaps caps;
    caps.loaderApiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo app{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};

    auto plan = planInstanceCreate(caps, app);
    require(plan->apiVersion() == VK_API_VERSION_1_1, "missing app info not raised to 1.1");
    require(plan->createInfo().pApplicationInfo && plan->createInfo().pApplicationInfo->sType == VK_STRUCTURE_TYPE_APPLICATION_INFO,
            "app info not synthesized");

    auto* appName = new std::string("game");
    auto* engine  = new std::string("engine");
    VkApplicationInfo info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    info.pApplicationName = appName->c_str();
    info.pEngineName = engine->c_str();
    info.applicationVersion = 7;
    info.apiVersion = VK_API_VERSION_1_0;
    const char* ext[] = {"VK_KHR_surface"};
    app.pApplicationInfo = &info;
    app.enabledExtensionCount = 1;
    app.ppEnabledExtensionNames = ext;
    plan = planInstanceCreate(caps, app);
    require(plan->apiVersion() == VK_API_VERSION_1_1, "1.0 app not raised to 1.1");
    require(info.apiVersion == VK_API_VERSION_1_0, "app info mutated");

    info.apiVersion = VK_API_VERSION_1_2;
    plan = planInstanceCreate(caps, app);
    require(plan->apiVersion() == VK_API_VERSION_1_2, "1.2 lowered or raised without DLSS5");

    caps.wantApi13 = true;
    plan = planInstanceCreate(caps, app);
    require(plan->apiVersion() == VK_API_VERSION_1_3, "DLSS5 did not raise to 1.3");
    caps.loaderApiVersion = VK_API_VERSION_1_2;
    plan = planInstanceCreate(caps, app);
    require(plan->apiVersion() == VK_API_VERSION_1_2, "raised past the loader version");
    info.apiVersion = VK_API_VERSION_1_4;
    caps.loaderApiVersion = VK_API_VERSION_1_3;
    plan = planInstanceCreate(caps, app);
    require(plan->apiVersion() == VK_API_VERSION_1_4, "app 1.4 lowered");

    std::memset(&appName[0][0], 'x', appName->size());
    delete appName; delete engine;
    const VkInstanceCreateInfo& out = plan->createInfo();
    require(std::strcmp(out.pApplicationInfo->pApplicationName, "game") == 0 && std::strcmp(out.pApplicationInfo->pEngineName, "engine") == 0
                && out.pApplicationInfo->applicationVersion == 7, "application info not deep-copied");
    require(out.enabledExtensionCount == 1 && std::strcmp(out.ppEnabledExtensionNames[0], "VK_KHR_surface") == 0 && out.ppEnabledExtensionNames != ext,
            "instance extensions changed");
}

#ifdef VKHOOK_DLSS5
static bool flowSupported = false;
static void VKAPI_CALL queryAll(VkPhysicalDevice, VkPhysicalDeviceFeatures2* features)
{
    features->features.shaderInt16 = features->features.shaderInt64 = VK_TRUE;
    for (auto* node = static_cast<VkBaseOutStructure*>(features->pNext); node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV)
            reinterpret_cast<VkPhysicalDeviceOpticalFlowFeaturesNV*>(node)->opticalFlow = flowSupported;
}

static void dlss5()
{
    DeviceCaps caps = fullCaps();
    vk::DeviceRequirements requirements;
    for (const char* name : requirements.extensions)
        caps.extensions.push_back(extension(name));
    caps.extensions.push_back(extension(VK_NV_OPTICAL_FLOW_EXTENSION_NAME));
    caps.properties.vendorID   = 0x10de;
    caps.properties.apiVersion = caps.instanceApiVersion = VK_API_VERSION_1_3;
    caps.dlss5 = true;
    caps.queryFeatures = queryAll;
    VkQueueFamilyProperties flowFamily{};
    flowFamily.queueFlags = VK_QUEUE_OPTICAL_FLOW_BIT_NV;
    flowFamily.queueCount = 1;
    caps.queueFamilies.push_back(flowFamily);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = queueInfo(0, &priority);
    VkLayerDeviceCreateInfo loader{};
    loader.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
    loader.function = VK_LAYER_LINK_INFO;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    f12.pNext = &loader;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features.robustBufferAccess = VK_TRUE;
    f2.pNext = &f12;
    const char* swapchain[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo app{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    app.pNext = &f2;
    app.queueCreateInfoCount = 1;
    app.pQueueCreateInfos = &queue;
    app.enabledExtensionCount = 1;
    app.ppEnabledExtensionNames = swapchain;

    flowSupported = false;
    auto plan = planDeviceCreate(caps, app);
    require(plan->dlss5Supported, plan->dlss5UnavailableReason.c_str());
    const VkDeviceCreateInfo& out = plan->createInfo();
    require(out.pEnabledFeatures == nullptr, "features2 and pEnabledFeatures cannot coexist");
    require(hasExt(out, VK_KHR_SWAPCHAIN_EXTENSION_NAME), "app extension lost");
    for (const char* name : requirements.extensions)
        require(hasExt(out, name), "DLSS5 extension missing");
    requireNoDuplicates(out);
    auto* f2out = static_cast<const VkPhysicalDeviceFeatures2*>(out.pNext);
    require(f2out->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, "DLSS5 chain head is not Features2");
    require(f2out->features.robustBufferAccess && f2out->features.shaderInt64 && f2out->features.shaderImageGatherExtended
                && f2out->features.shaderStorageImageWriteWithoutFormat, "base features lost");
    int loaders = 0;
    std::set<VkStructureType> seen;
    for (auto* n = static_cast<const VkBaseInStructure*>(out.pNext); n; n = n->pNext)
    {
        if (n->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) { ++loaders; continue; }
        require(seen.insert(n->sType).second, "duplicate feature sType");
        if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
        {
            auto* f = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(n);
            require(f->shaderFloat16 && f->bufferDeviceAddress && f->timelineSemaphore, "Vulkan12 feature merge failed");
        }
    }
    require(loaders == 1, "loader node not preserved");
    require(!f12.shaderFloat16 && !f12.bufferDeviceAddress && f12.pNext == &loader && f2.pNext == &f12, "app input mutated");
    require(!plan->enableDeviceFault && plan->opticalFlowFamily == VK_QUEUE_FAMILY_IGNORED && out.queueCreateInfoCount == 1,
            "optical flow queue added without optical flow");

    // optical flow: private family queue appended to a copied queue list
    flowSupported = true;
    plan = planDeviceCreate(caps, app);
    require(plan->dlss5Supported && plan->opticalFlowFamily == 1 && !plan->opticalFlowQueueShared, "optical flow family wrong");
    require(plan->createInfo().queueCreateInfoCount == 2 && plan->createInfo().pQueueCreateInfos[1].queueFamilyIndex == 1
                && plan->createInfo().pQueueCreateInfos[1].pQueuePriorities[0] == 1.0f, "optical flow queue not appended");
    require(plan->createInfo().pQueueCreateInfos != &queue && plan->createInfo().pQueueCreateInfos[0].pQueuePriorities != &priority,
            "app queue infos referenced");
    require(hasExt(plan->createInfo(), VK_NV_OPTICAL_FLOW_EXTENSION_NAME), "optical flow extension missing");
    requireNoDuplicates(plan->createInfo());

    // Proton-style: the app already reserved the optical family
    VkDeviceQueueCreateInfo queues[2] = {queueInfo(0, &priority), queueInfo(1, &priority)};
    app.queueCreateInfoCount = 2;
    app.pQueueCreateInfos = queues;
    plan = planDeviceCreate(caps, app);
    require(plan->opticalFlowFamily == 1 && plan->opticalFlowQueueShared && plan->createInfo().queueCreateInfoCount == 2,
            "shared optical queue handling wrong");

    // unsupported GPU: DLSS5 refused and the plan equals the plain one
    caps.properties.vendorID = 0x1002;
    plan = planDeviceCreate(caps, app);
    require(!plan->dlss5Supported && !plan->dlss5UnavailableReason.empty(), "non-NVIDIA accepted");
    auto* plain = findNode<VkPhysicalDeviceFeatures2>(plan->createInfo().pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    require(plain && plain->features.shaderImageGatherExtended && !plain->features.shaderInt64, "failed DLSS5 altered the plan");
}
#endif

int main()
{
    try
    {
        features2Merged();
        enabledFeaturesCopied();
        unsupportedFeaturesNotEnabled();
        unknownNodeBeforeFeatures2();
        mutableFormat();
        faultFeatures();
        separateDepthStencil();
        deepCopyIndependence();
        instancePlan();
#ifdef VKHOOK_DLSS5
        dlss5();
#endif
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL: " << e.what() << std::endl;
        return 1;
    }
    std::cout << "PASS: device plan" << std::endl;
}
