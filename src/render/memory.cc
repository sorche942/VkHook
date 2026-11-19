#include "memory.hh"

#include <mutex>
#include <unordered_map>

namespace VkHook
{
    namespace
    {
        // A physical device's memory properties never change, and the lookup used
        // to run a driver call for every buffer and image. Keyed by physical
        // device so several devices (and instances) each keep their own entry.
        const VkPhysicalDeviceMemoryProperties& cachedMemoryProperties(LogicalDevice* logicalDevice)
        {
            static std::mutex cacheMutex;
            static std::unordered_map<VkPhysicalDevice, VkPhysicalDeviceMemoryProperties> cache;

            std::lock_guard<std::mutex> lock(cacheMutex);
            auto it = cache.find(logicalDevice->physicalDevice);
            if (it == cache.end())
            {
                VkPhysicalDeviceMemoryProperties properties = {};
                logicalDevice->vki.GetPhysicalDeviceMemoryProperties(logicalDevice->physicalDevice, &properties);
                it = cache.emplace(logicalDevice->physicalDevice, properties).first;
            }
            // unordered_map references stay valid across later insertions.
            return it->second;
        }
    } // namespace

    uint32_t findMemoryTypeIndex(LogicalDevice* logicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties)
    {
        const VkPhysicalDeviceMemoryProperties& memory = cachedMemoryProperties(logicalDevice);

        // Pass 1: exact match (all requested flags present)
        for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
        {
            if ((typeFilter & (1u << i)) && (memory.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        }

        // Pass 2: fallback for translation layers (e.g. Android-on-Linux via
        // Sober) that do not expose DEVICE_LOCAL for some formats. Preferences
        // such as DEVICE_LOCAL may be dropped; host access may not. Callers map
        // the memory and write without flushing, so a type that is not
        // HOST_VISIBLE (and HOST_COHERENT when asked for) is not a fallback.
        const VkMemoryPropertyFlags required =
            properties & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
        {
            if ((typeFilter & (1u << i)) && (memory.memoryTypes[i].propertyFlags & required) == required)
            {
                Logger::warn("No memory type with exact flags found; falling back to type " + std::to_string(i));
                return i;
            }
        }

        Logger::err("No memory type matches filter bits " + std::to_string(typeFilter) + " with host access flags " +
                    std::to_string(required));
        return kInvalidMemoryTypeIndex;
    }
} // namespace VkHook
