#include "dlss5_device.hh"
#include "vk_context.h"
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace VKIntox;
static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static void VKAPI_CALL available(VkPhysicalDevice, VkPhysicalDeviceFeatures2* features) {
    // Query input starts with the required bits; leave them supported.
    features->features.shaderInt16 = features->features.shaderInt64 = VK_TRUE;
}
int main() {
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
    require(enabled.prepare(VK_NULL_HANDLE,VK_API_VERSION_1_3,available,properties,extensions,info,names),enabled.error().c_str());
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
    properties.vendorID=0x1002;
    VkDeviceCreateInfo rejected{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; rejected.pNext=&original2;
    std::vector<const char*> rejectedNames;
    Dlss5DeviceFeatures unsupported;
    require(!unsupported.prepare(VK_NULL_HANDLE,VK_API_VERSION_1_3,available,properties,extensions,rejected,rejectedNames),"non-NVIDIA accepted");
    require(rejected.pNext==&original2 && rejectedNames.empty(),"failure modified app info");
    std::cout<<"PASS: feature promotion, existing bits, duplicate loader nodes, const app chain, unsupported GPU"<<std::endl;
}
