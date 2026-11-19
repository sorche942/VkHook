#ifndef RESHADE_TEXTURE_POOL_HPP_INCLUDED
#define RESHADE_TEXTURE_POOL_HPP_INCLUDED

#include "vulkan_include.hh"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace VkHook
{
    struct LogicalDevice;

    // Where a texture's GPU objects go when it is released. The default (null
    // fn) is LogicalDevice::retire; a custom hook lets tests observe release
    // without a layer device.
    struct ReshadeRetireHook
    {
        using Fn = void (*)(void* ctx, VkObjectType type, uint64_t handle, uint64_t extra);
        Fn   fn  = nullptr;
        void* ctx = nullptr;
    };


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
        ReshadeTexture(LogicalDevice* logicalDevice, const ReshadeTextureDesc& description, ReshadeRetireHook hook = {});
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
        // Retires every owned handle; the retire queue destroys views before
        // the image and memory last once the GPU serial passes.
        void deferRelease();

        const ReshadeRetireHook retireHook;
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
        explicit ReshadeTexturePool(LogicalDevice* logicalDevice, ReshadeRetireHook hook = {})
            : logicalDevice(logicalDevice), retireHook(hook) {}
        ReshadeTexturePool(const ReshadeTexturePool&) = delete;
        ReshadeTexturePool& operator=(const ReshadeTexturePool&) = delete;

        // Registers one more effect instance built from `effectFile` and returns
        // its ordinal (0 for the first instance of that file in this build).
        uint32_t beginEffect(const std::string& effectFile);

        std::shared_ptr<ReshadeTexture> acquire(const std::string& uniqueName, const ReshadeTextureDesc& desc,
                                                uint32_t instanceOrdinal = 0);

    private:
        LogicalDevice* logicalDevice;
        ReshadeRetireHook retireHook;
        std::unordered_map<std::string, uint32_t> instanceCounts;
        std::unordered_map<std::string, std::shared_ptr<ReshadeTexture>> textures;
    };
}

#endif // RESHADE_TEXTURE_POOL_HPP_INCLUDED
