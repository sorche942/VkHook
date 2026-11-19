#ifndef RESHADE_TEXTURE_POOL_HPP_INCLUDED
#define RESHADE_TEXTURE_POOL_HPP_INCLUDED

#include "vulkan_include.hh"

#include <memory>
#include <string>
#include <unordered_map>

namespace VkHook
{
    struct LogicalDevice;

    struct ReshadeTextureDesc
    {
        VkExtent3D extent;
        VkFormat format;
        uint32_t mipLevels;
        VkImageViewType viewType;

        bool operator==(const ReshadeTextureDesc& other) const noexcept;
    };

    struct ReshadeTexture
    {
        ReshadeTexture(LogicalDevice* logicalDevice, const ReshadeTextureDesc& description);
        ~ReshadeTexture();
        ReshadeTexture(const ReshadeTexture&) = delete;
        ReshadeTexture& operator=(const ReshadeTexture&) = delete;

        const ReshadeTextureDesc desc;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView unormView = VK_NULL_HANDLE;
        VkImageView srgbView = VK_NULL_HANDLE;
        VkImageView unormRenderView = VK_NULL_HANDLE;
        VkImageView srgbRenderView = VK_NULL_HANDLE;

    private:
        void deferRelease();

        const VkDevice device;
        const PFN_vkDestroyImage destroyImage;
        const PFN_vkDestroyImageView destroyImageView;
        const PFN_vkFreeMemory freeMemory;
    };

    // One pool per effect-chain build. Its strong references preserve producers
    // during construction; consumers retain their own references afterward.
    //
    // Sharing follows ReShade: effect files that declare a texture of the same
    // name share it. Two instances of the SAME file (X and X.2) are not one
    // effect, so each instance gets its own set of those textures, history
    // included. An instance is identified by its ordinal among the instances of
    // its file in this build: the first instance of every file shares with the
    // first instance of every other, the second with the second, and so on.
    class ReshadeTexturePool
    {
    public:
        explicit ReshadeTexturePool(LogicalDevice* logicalDevice) : logicalDevice(logicalDevice) {}
        ReshadeTexturePool(const ReshadeTexturePool&) = delete;
        ReshadeTexturePool& operator=(const ReshadeTexturePool&) = delete;

        // Registers one more effect instance built from `effectFile` and returns
        // its ordinal (0 for the first instance of that file in this build).
        uint32_t beginEffect(const std::string& effectFile);

        std::shared_ptr<ReshadeTexture> acquire(const std::string& uniqueName, const ReshadeTextureDesc& desc,
                                                uint32_t instanceOrdinal = 0);

    private:
        LogicalDevice* logicalDevice;
        std::unordered_map<std::string, uint32_t> instanceCounts;
        std::unordered_map<std::string, std::shared_ptr<ReshadeTexture>> textures;
    };
}

#endif // RESHADE_TEXTURE_POOL_HPP_INCLUDED
