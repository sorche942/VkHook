#include "reshade_texture_pool.hh"

#include "format.hh"
#include "image.hh"
#include "logger.hh"
#include "util.hh"
#include "vk_handle.hh"

#include <stdexcept>

namespace VkHook
{
    namespace
    {
        VkImageView createView(LogicalDevice* logicalDevice, VkImage image, VkFormat format,
                               VkImageViewType viewType, uint32_t mipLevels)
        {
            // The vector-based view helpers allocate twice for a single view.
            // Keep this fixed-size creation on the stack, with the same defaults.
            VkImageViewCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            info.image = image;
            info.viewType = viewType;
            info.format = format;
            info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            info.subresourceRange.levelCount = mipLevels;
            info.subresourceRange.layerCount = 1;
            // Mutable images also support storage through their UNORM view.
            // sRGB formats cannot inherit that storage-image usage.
            VkImageViewUsageCreateInfo usage = {};
            if (isSRGB(format))
            {
                usage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
                usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
                if (viewType != VK_IMAGE_VIEW_TYPE_3D)
                    usage.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                info.pNext = &usage;
            }
            VkImageView view = VK_NULL_HANDLE;
            const VkResult result = logicalDevice->vkd.CreateImageView(logicalDevice->device, &info, nullptr, &view);
            if (result != VK_SUCCESS || view == VK_NULL_HANDLE)
                throw std::runtime_error("ReShade shared texture view creation failed: " + std::to_string(result));
            return view;
        }

        std::string describe(const ReshadeTextureDesc& desc)
        {
            return std::to_string(desc.extent.width) + "x" + std::to_string(desc.extent.height) + "x"
                + std::to_string(desc.extent.depth) + " format=" + std::to_string(desc.format)
                + " mips=" + std::to_string(desc.mipLevels) + " viewType=" + std::to_string(desc.viewType);
        }
    }

    bool ReshadeTextureDesc::operator==(const ReshadeTextureDesc& other) const noexcept
    {
        return extent.width == other.extent.width && extent.height == other.extent.height
            && extent.depth == other.extent.depth && format == other.format
            && mipLevels == other.mipLevels && viewType == other.viewType;
    }

    ReshadeTexture::ReshadeTexture(LogicalDevice* logicalDevice, const ReshadeTextureDesc& description)
        : desc(description), device(logicalDevice->device), destroyImage(logicalDevice->vkd.DestroyImage),
          destroyImageView(logicalDevice->vkd.DestroyImageView), freeMemory(logicalDevice->vkd.FreeMemory)
    {
        try
        {
            VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (desc.viewType != VK_IMAGE_VIEW_TYPE_3D)
                usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            image = createSingleImage(logicalDevice, desc.extent, desc.format, usage,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory, desc.mipLevels);
            if (image == VK_NULL_HANDLE || memory == VK_NULL_HANDLE)
                throw std::runtime_error("ReShade shared texture image allocation failed");

            const VkFormat unormFormat = convertToUNORM(desc.format);
            const VkFormat srgbFormat = convertToSRGB(desc.format);
            unormView = createView(logicalDevice, image, unormFormat, desc.viewType, desc.mipLevels);
            srgbView = srgbFormat == unormFormat ? unormView
                : createView(logicalDevice, image, srgbFormat, desc.viewType, desc.mipLevels);
            unormRenderView = desc.mipLevels == 1 ? unormView
                : createView(logicalDevice, image, unormFormat, desc.viewType, 1);
            srgbRenderView = srgbFormat == unormFormat ? unormRenderView
                : desc.mipLevels == 1 ? srgbView
                : createView(logicalDevice, image, srgbFormat, desc.viewType, 1);

            changeImageLayout(logicalDevice, {image}, desc.mipLevels);
        }
        catch (...)
        {
            // A throwing constructor has no destructor, and initialization may
            // already have submitted GPU work. Use the same deferred ownership.
            deferRelease();
            throw;
        }
    }

    ReshadeTexture::~ReshadeTexture()
    {
        deferRelease();
    }

    void ReshadeTexture::deferRelease()
    {
        auto& queue = DeferredDestroyQueue::instance();
        if (image != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Resource, [device = device, release = destroyImage, image = image] {
                release(device, image, nullptr);
            });

        // Reverse registration within Resource releases all distinct views
        // before their image, without a heap-allocated deduplication container.
        const VkImageView views[] = {unormView, srgbView, unormRenderView, srgbRenderView};
        for (size_t i = 0; i < 4; ++i)
        {
            if (views[i] == VK_NULL_HANDLE)
                continue;
            bool duplicate = false;
            for (size_t j = 0; j < i; ++j)
                duplicate |= views[j] == views[i];
            if (!duplicate)
                queue.push(DestroyPhase::Resource, [device = device, release = destroyImageView, view = views[i]] {
                    release(device, view, nullptr);
                });
        }
        if (memory != VK_NULL_HANDLE)
            queue.push(DestroyPhase::Memory, [device = device, release = freeMemory, memory = memory] {
                release(device, memory, nullptr);
            });
    }

    std::shared_ptr<ReshadeTexture> ReshadeTexturePool::acquire(const std::string& uniqueName, const ReshadeTextureDesc& desc)
    {
        const auto found = textures.find(uniqueName);
        if (found != textures.end())
        {
            if (!(found->second->desc == desc))
                throw std::runtime_error("Incompatible shared ReShade texture '" + uniqueName + "': existing "
                                         + describe(found->second->desc) + ", requested " + describe(desc));
            Logger::debug("[reshade-texture-pool] reuse name=" + uniqueName
                          + " image=" + convertToString(found->second->image));
            return found->second;
        }

        auto texture = std::make_shared<ReshadeTexture>(logicalDevice, desc);
        textures.emplace(uniqueName, texture);
        Logger::debug("[reshade-texture-pool] create name=" + uniqueName + " image=" + convertToString(texture->image));
        return texture;
    }
}
