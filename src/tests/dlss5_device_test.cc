#include "dlss5_device.hh"
#include "vk_context.h"
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace VKIntox;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static bool flowSupported = false;
static void VKAPI_CALL available(VkPhysicalDevice, VkPhysicalDeviceFeatures2* features) {
    // Query input starts with the required bits; leave them supported.
    features->features.shaderInt16 = features->features.shaderInt64 = VK_TRUE;
    for (auto* node = static_cast<VkBaseOutStructure*>(features->pNext); node; node = node->pNext)
        if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV)
            reinterpret_cast<VkPhysicalDeviceOpticalFlowFeaturesNV*>(node)->opticalFlow = flowSupported;
}
int main() {
    // vkd3d-proton reserves the optical family itself. This must not disable
    // history, but mixed graphics/compute and protected queues cannot be borrowed.
    std::vector<VkQueueFamilyProperties> families(2);
    families[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    families[0].queueCount = 1;
    families[1].queueFlags = VK_QUEUE_OPTICAL_FLOW_BIT_NV;
    families[1].queueCount = 1;
    VkDeviceQueueCreateInfo queues[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        queues[i].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queues[i].queueFamilyIndex = i;
        queues[i].queueCount = 1;
    }
    VkDeviceCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    queueInfo.pQueueCreateInfos = queues;
    queueInfo.queueCreateInfoCount = 1;
    auto choice = selectDlss5OpticalQueue(families, queueInfo);
    require(choice.family == 1 && !choice.shared, "unused optical family not reserved privately");
    queueInfo.queueCreateInfoCount = 2;
    choice = selectDlss5OpticalQueue(families, queueInfo);
    require(choice.family == 1 && choice.shared, "Proton-reserved optical queue disabled history");
    families.push_back(families[1]);
    choice = selectDlss5OpticalQueue(families, queueInfo);
    require(choice.family == 2 && !choice.shared, "shared queue preferred over available private family");
    families.pop_back();
    queues[1].flags = VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT;
    require(selectDlss5OpticalQueue(families, queueInfo).family == VK_QUEUE_FAMILY_IGNORED,
        "protected application queue borrowed for ordinary optical commands");
    queues[1].flags = 0;
    families[1].queueFlags |= VK_QUEUE_COMPUTE_BIT;
    require(selectDlss5OpticalQueue(families, queueInfo).family == VK_QUEUE_FAMILY_IGNORED,
        "mixed compute queue can deadlock graphics/optical dependencies");
    families[1].queueFlags = VK_QUEUE_OPTICAL_FLOW_BIT_NV;
    families[1].queueCount = 0;
    require(selectDlss5OpticalQueue(families, queueInfo).family == VK_QUEUE_FAMILY_IGNORED,
        "empty queue family selected");
    vk::DeviceRequirements requirements;
    std::vector<VkExtensionProperties> extensions;
    for (const char* name: requirements.extensions) { VkExtensionProperties ext{}; std::strcpy(ext.extensionName,name); extensions.push_back(ext); }
    VkPhysicalDeviceProperties properties{}; properties.vendorID=0x10de; properties.apiVersion=VK_API_VERSION_1_3;
    VkLayerDeviceCreateInfo loader2{}; loader2.sType=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO; loader2.function=VK_LOADER_DATA_CALLBACK;
    VkLayerDeviceCreateInfo loader1{}; loader1.sType=VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO; loader1.function=VK_LAYER_LINK_INFO; loader1.pNext=&loader2;
    VkPhysicalDeviceVulkan12Features original12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    original12.timelineSemaphore=VK_TRUE; original12.pNext=&loader1;
    VkPhysicalDevice16BitStorageFeatures original16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    original16.storagePushConstant16=VK_TRUE; original16.pNext=&original12;
    VkPhysicalDeviceFeatures2 original2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    original2.features.robustBufferAccess=VK_TRUE; original2.pNext=&original16;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; info.pNext=&original2;
    std::vector<const char*> names;
    Dlss5DeviceFeatures enabled;
    require(enabled.prepare(VK_NULL_HANDLE,VK_API_VERSION_1_3,available,properties,extensions,info,names,false),enabled.error().c_str());
    require(info.pEnabledFeatures==nullptr,"features2 and pEnabledFeatures cannot coexist");
    auto* f2=static_cast<const VkPhysicalDeviceFeatures2*>(info.pNext);
    require(f2->features.robustBufferAccess && f2->features.shaderInt64,"base features lost");
    std::set<VkStructureType> seen; int loaders=0;
    for (auto* n=static_cast<const VkBaseInStructure*>(info.pNext);n;n=n->pNext) {
        if(n->sType==VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) { ++loaders; continue; }
        require(seen.insert(n->sType).second,"duplicate feature sType");
        if(n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            auto* f=reinterpret_cast<const VkPhysicalDeviceVulkan11Features*>(n);
            require(f->storageBuffer16BitAccess && f->storagePushConstant16,"promoted app feature lost");
        }
        if(n->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* f=reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(n);
            require(f->shaderFloat16 && f->bufferDeviceAddress && f->timelineSemaphore,"Vulkan12 feature merge failed");
        }
    }
    require(loaders==2,"loader callbacks not preserved");
    require(!original12.shaderFloat16 && !original12.bufferDeviceAddress && original12.pNext==&loader1,"app input mutated");
    require(original2.pNext==&original16 && original16.pNext==&original12 && !original16.storageBuffer16BitAccess,"app pNext mutated");
    // A requested but unsupported motion engine must leave base DLSS5 usable.
    VkExtensionProperties flowExtension{};
    std::strcpy(flowExtension.extensionName, VK_NV_OPTICAL_FLOW_EXTENSION_NAME);
    extensions.push_back(flowExtension);
    VkDeviceCreateInfo noFlowInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    noFlowInfo.pNext = &original2;
    Dlss5DeviceFeatures noFlow;
    std::vector<const char*> noFlowNames;
    require(noFlow.prepare(VK_NULL_HANDLE, VK_API_VERSION_1_3, available, properties, extensions,
        noFlowInfo, noFlowNames, true), "optional flow failure disabled base DLSS5");
    require(!noFlow.opticalFlowEnabled(), "unsupported flow was enabled");
    for (const char* name : noFlowNames)
        require(std::strcmp(name, VK_NV_OPTICAL_FLOW_EXTENSION_NAME) != 0, "unsupported flow extension injected");

    // Merge an application-owned optical feature node, never duplicate or mutate it.
    flowSupported = true;
    VkPhysicalDeviceOpticalFlowFeaturesNV appFlow{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV};
    appFlow.pNext = &original2;
    VkDeviceCreateInfo flowInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    flowInfo.pNext = &appFlow;
    std::vector<const char*> flowNames{VK_NV_OPTICAL_FLOW_EXTENSION_NAME};
    Dlss5DeviceFeatures withFlow;
    require(withFlow.prepare(VK_NULL_HANDLE, VK_API_VERSION_1_3, available, properties, extensions,
        flowInfo, flowNames, true), "supported flow negotiation failed");
    require(withFlow.opticalFlowEnabled(), "supported flow not enabled");
    unsigned flowNodes = 0, flowExtensions = 0;
    for (auto* n = static_cast<const VkBaseInStructure*>(flowInfo.pNext); n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV) {
            ++flowNodes;
            require(reinterpret_cast<const VkPhysicalDeviceOpticalFlowFeaturesNV*>(n)->opticalFlow,
                "merged optical feature is false");
        }
    for (const char* name : flowNames)
        flowExtensions += std::strcmp(name, VK_NV_OPTICAL_FLOW_EXTENSION_NAME) == 0;
    require(flowNodes == 1 && flowExtensions == 1, "duplicate optical feature/extension");
    require(!appFlow.opticalFlow && appFlow.pNext == &original2, "application optical node mutated");
    properties.vendorID=0x1002;
    VkDeviceCreateInfo rejected{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; rejected.pNext=&original2;
    std::vector<const char*> rejectedNames;
    Dlss5DeviceFeatures unsupported;
    require(!unsupported.prepare(VK_NULL_HANDLE,VK_API_VERSION_1_3,available,properties,extensions,rejected,rejectedNames,false),"non-NVIDIA accepted");
    require(rejected.pNext==&original2 && rejectedNames.empty(),"failure modified app info");
    std::cout<<"PASS: feature promotion, existing bits, duplicate loader nodes, const app chain, unsupported GPU"<<std::endl;
}
