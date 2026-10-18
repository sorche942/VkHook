#pragma once
#include <memory>
#include <string>
#include <vector>
#include "vulkan_include.hh"

namespace VKIntox
{
    struct Dlss5OpticalQueue
    {
        uint32_t family = VK_QUEUE_FAMILY_IGNORED;
        bool shared = false;
    };
    // Prefer a private family. Proton may already reserve the dedicated optical
    // family; its ordinary queue zero can be shared with host synchronization.
    Dlss5OpticalQueue selectDlss5OpticalQueue(const std::vector<VkQueueFamilyProperties>& families,
                                             const VkDeviceCreateInfo& application);

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
                     VkDeviceCreateInfo& info, std::vector<const char*>& extensions,
                     bool requestOpticalFlow);
        const std::string& error() const;
        bool opticalFlowEnabled() const;
    private:
        struct State;
        std::unique_ptr<State> state;
    };
}
