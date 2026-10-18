#include "dlss5_device.hh"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include "vk_context.h"

namespace VKIntox
{
    namespace
    {
#include "dlss5_device_chain.inc"

        void mergeBits(void* destination, const void* source, size_t begin, size_t end)
        {
            for (size_t offset = begin; offset < end; offset += sizeof(VkBool32))
            {
                VkBool32 a, b;
                std::memcpy(&a, static_cast<const char*>(destination) + offset, sizeof(a));
                std::memcpy(&b, static_cast<const char*>(source) + offset, sizeof(b));
                a |= b;
                std::memcpy(static_cast<char*>(destination) + offset, &a, sizeof(a));
            }
        }

        bool supportsBits(const void* available, const void* required, size_t begin, size_t end)
        {
            for (size_t offset = begin; offset < end; offset += sizeof(VkBool32))
            {
                VkBool32 a, b;
                std::memcpy(&a, static_cast<const char*>(available) + offset, sizeof(a));
                std::memcpy(&b, static_cast<const char*>(required) + offset, sizeof(b));
                if (b && !a) return false;
            }
            return true;
        }
    }

    struct Dlss5DeviceFeatures::State
    {
        vk::DeviceRequirements required;
        std::vector<std::unique_ptr<std::byte[]>> copies;
        std::string error;
    };

    Dlss5DeviceFeatures::Dlss5DeviceFeatures() : state(std::make_unique<State>()) {}
    Dlss5DeviceFeatures::~Dlss5DeviceFeatures() = default;
    const std::string& Dlss5DeviceFeatures::error() const { return state->error; }

    bool Dlss5DeviceFeatures::prepare(VkPhysicalDevice physical, uint32_t instanceVersion,
                                     PFN_vkGetPhysicalDeviceFeatures2 queryFeatures,
                                     const VkPhysicalDeviceProperties& properties,
                                     const std::vector<VkExtensionProperties>& available,
                                     VkDeviceCreateInfo& info, std::vector<const char*>& extensions)
    {
        auto refuse = [&](std::string reason) { state->error = std::move(reason); return false; };
        if (properties.vendorID != 0x10de)
            return refuse("DLSS5 PTX requires an NVIDIA GPU");
        if (!queryFeatures || properties.apiVersion < VK_API_VERSION_1_3 || instanceVersion < VK_API_VERSION_1_3)
            return refuse("DLSS5 requires a Vulkan 1.3 instance and device");
        auto& req = state->required;
        for (const char* name : req.extensions)
            if (std::none_of(available.begin(), available.end(), [&](const auto& ext) {
                    return std::strcmp(ext.extensionName, name) == 0;
                }))
                return refuse(std::string("DLSS5 requires ") + name);

        vk::DeviceRequirements supported;
        queryFeatures(physical, &supported.features);
        if (!supportsBits(&supported.features.features, &req.features.features, 0, sizeof(VkPhysicalDeviceFeatures)))
            return refuse("DLSS5 requires shaderInt16 and shaderInt64");
        auto* need = reinterpret_cast<VkBaseOutStructure*>(req.features.pNext);
        auto* have = reinterpret_cast<VkBaseOutStructure*>(supported.features.pNext);
        for (; need && have; need = need->pNext, have = have->pNext)
        {
            const auto [begin, end] = deviceFeatureRange(need->sType);
            if (!end || !supportsBits(have, need, begin, end))
                return refuse("DLSS5 device feature unavailable (sType=" + std::to_string(need->sType) + ")");
        }

        if (info.pEnabledFeatures)
            mergeBits(&req.features.features, info.pEnabledFeatures, 0, sizeof(VkPhysicalDeviceFeatures));
        VkBaseOutStructure* last = reinterpret_cast<VkBaseOutStructure*>(&req.features);
        while (last->pNext) last = last->pNext;
        for (auto* node = static_cast<const VkBaseInStructure*>(info.pNext); node; node = node->pNext)
        {
            if (node->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            {
                mergeBits(&req.features.features, &reinterpret_cast<const VkPhysicalDeviceFeatures2*>(node)->features,
                          0, sizeof(VkPhysicalDeviceFeatures));
                continue;
            }
            if (promoteDeviceFeatures(node, req)) continue;
            auto* target = reinterpret_cast<VkBaseOutStructure*>(req.features.pNext);
            while (target && target->sType != node->sType) target = target->pNext;
            const auto [begin, end] = deviceFeatureRange(node->sType);
            if (target && end)
            {
                mergeBits(target, node, begin, end);
                continue;
            }
            const size_t size = deviceChainSize(node->sType);
            if (!size)
                return refuse("Cannot copy application device pNext sType=" + std::to_string(node->sType));
            auto copy = std::make_unique<std::byte[]>(size);
            std::memcpy(copy.get(), node, size);
            auto* added = reinterpret_cast<VkBaseOutStructure*>(copy.get());
            added->pNext = nullptr;
            last->pNext = added;
            last = added;
            state->copies.push_back(std::move(copy));
        }
        for (const char* name : req.extensions)
            if (std::none_of(extensions.begin(), extensions.end(), [&](const char* ext) { return std::strcmp(ext, name) == 0; }))
                extensions.push_back(name);
        info.pNext = &req.features;
        info.pEnabledFeatures = nullptr;
        info.ppEnabledExtensionNames = extensions.data();
        info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        return true;
    }
}
