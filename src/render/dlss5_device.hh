#pragma once
#include <memory>
#include <string>
#include <vector>
#include "vulkan_include.hh"

namespace VKIntox
{
    // Owns a copied pNext chain through the downstream vkCreateDevice call.
    // Failure leaves the application's create info and extensions unchanged.
    class Dlss5DeviceFeatures
    {
    public:
        Dlss5DeviceFeatures();
        ~Dlss5DeviceFeatures();
        bool prepare(VkPhysicalDevice physical, uint32_t instanceVersion,
                     PFN_vkGetPhysicalDeviceFeatures2 queryFeatures,
                     const VkPhysicalDeviceProperties& properties,
                     const std::vector<VkExtensionProperties>& available,
                     VkDeviceCreateInfo& info, std::vector<const char*>& extensions);
        const std::string& error() const;
    private:
        struct State;
        std::unique_ptr<State> state;
    };
}
