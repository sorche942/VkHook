#include "image.hh"
#include "memory.hh"
#include "buffer.hh"
#include "format.hh"

namespace VkHook
{
    namespace
    {
        // One-time command buffer in its own transient pool: concurrent builders
        // never share a pool (pools are externally synchronized).
        struct OneTimeCommands
        {
            VkCommandPool   pool   = VK_NULL_HANDLE;
            VkCommandBuffer buffer = VK_NULL_HANDLE;
        };

        OneTimeCommands allocateOneTimeCommandBuffer(LogicalDevice* logicalDevice, const char* what)
        {
            VkCommandPoolCreateInfo poolInfo = {};
            poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            poolInfo.queueFamilyIndex = logicalDevice->queueFamilyIndex;

            OneTimeCommands commands;
            const VkResult poolResult = logicalDevice->vkd.CreateCommandPool(logicalDevice->device, &poolInfo, nullptr, &commands.pool);
            if (poolResult != VK_SUCCESS || commands.pool == VK_NULL_HANDLE)
            {
                Logger::err(std::string(what) + ": vkCreateCommandPool failed: " + std::to_string(poolResult));
                return {};
            }

            VkCommandBufferAllocateInfo allocInfo = {};
            allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandPool        = commands.pool;
            allocInfo.commandBufferCount = 1;

            const VkResult result = logicalDevice->vkd.AllocateCommandBuffers(logicalDevice->device, &allocInfo, &commands.buffer);
            if (result != VK_SUCCESS || commands.buffer == VK_NULL_HANDLE)
            {
                Logger::err(std::string(what) + ": vkAllocateCommandBuffers failed: " + std::to_string(result));
                logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, commands.pool, nullptr);
                return {};
            }
            // initialize dispatch table for commandBuffer since it is a dispatchable object
            initializeDispatchTable(commands.buffer, logicalDevice->device);

            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            const VkResult beginResult = logicalDevice->vkd.BeginCommandBuffer(commands.buffer, &beginInfo);
            if (beginResult != VK_SUCCESS)
            {
                Logger::err(std::string(what) + ": vkBeginCommandBuffer failed: " + std::to_string(beginResult));
                logicalDevice->vkd.DestroyCommandPool(logicalDevice->device, commands.pool, nullptr);
                return {};
            }
            return commands;
        }

        // Ends, submits and synchronously waits for the one-time buffer, then
        // releases it (its pool) together with the optional staging buffer and
        // memory. Returns whether the work completed. When the GPU has not
        // finished within the bounded wait, the objects it may still read are
        // retired instead of destroyed.
        bool finishOneTimeCommands(LogicalDevice* logicalDevice,
                                   const OneTimeCommands& commands,
                                   const char* what,
                                   const char* waitLabel,
                                   VkBuffer stagingBuffer = VK_NULL_HANDLE,
                                   VkDeviceMemory stagingMemory = VK_NULL_HANDLE)
        {
            const VkDevice device = logicalDevice->device;
            const auto destroyInline = [&] {
                logicalDevice->vkd.DestroyCommandPool(device, commands.pool, nullptr);
                if (stagingBuffer != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroyBuffer(device, stagingBuffer, nullptr);
                if (stagingMemory != VK_NULL_HANDLE)
                    logicalDevice->vkd.FreeMemory(device, stagingMemory, nullptr);
            };

            const VkResult endResult = logicalDevice->vkd.EndCommandBuffer(commands.buffer);
            if (endResult != VK_SUCCESS)
            {
                Logger::err(std::string(what) + ": vkEndCommandBuffer failed: " + std::to_string(endResult));
                destroyInline();
                return false;
            }

            VkSubmitInfo submitInfo       = {};
            submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers    = &commands.buffer;

            const VkResult result = logicalDevice->queues.submitAndWait(logicalDevice->queue, submitInfo, 4000);
            if (result == VK_SUCCESS)
            {
                destroyInline();
                return true;
            }
            if (result == VK_TIMEOUT)
            {
                // Still pending on the GPU: the layer destroys these once the serial completes.
                Logger::err(std::string(what) + ": " + waitLabel + " timed out; retiring its objects");
                logicalDevice->retire(VK_OBJECT_TYPE_COMMAND_POOL, handleToU64(commands.pool));
                if (stagingBuffer != VK_NULL_HANDLE)
                    logicalDevice->retire(VK_OBJECT_TYPE_BUFFER, handleToU64(stagingBuffer));
                if (stagingMemory != VK_NULL_HANDLE)
                    logicalDevice->retire(VK_OBJECT_TYPE_DEVICE_MEMORY, handleToU64(stagingMemory));
                return false;
            }

            // The submit itself failed, so nothing was queued and nothing reads these.
            Logger::err(std::string(what) + ": vkQueueSubmit failed: " + std::to_string(result));
            destroyInline();
            return false;
        }
    } // namespace

    std::vector<VkImage> createImages(LogicalDevice*        logicalDevice,
                                      uint32_t              count,
                                      VkExtent3D            extent,
                                      VkFormat              format,
                                      VkImageUsageFlags     usage,
                                      VkMemoryPropertyFlags properties,
                                      VkDeviceMemory&       imageMemory,
                                      uint32_t              mipLevels)
    {
        std::vector<VkImage> images(count);

        VkFormat srgbFormat  = isSRGB(format) ? format : convertToSRGB(format);
        VkFormat unormFormat = isSRGB(format) ? convertToUNORM(format) : format;

        VkFormat formats[] = {unormFormat, srgbFormat};

        VkImageFormatListCreateInfoKHR imageFormatListCreateInfo;
        imageFormatListCreateInfo.sType           = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR;
        imageFormatListCreateInfo.pNext           = nullptr;
        imageFormatListCreateInfo.viewFormatCount = 2;
        imageFormatListCreateInfo.pViewFormats    = formats;

        VkImageCreateInfo imageCreateInfo;
        imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageCreateInfo.pNext = (unormFormat == srgbFormat) ? nullptr : &imageFormatListCreateInfo;
        imageCreateInfo.flags = (unormFormat == srgbFormat) ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (extent.depth == 1)
        {
            imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
        }
        else
        {
            imageCreateInfo.imageType = VK_IMAGE_TYPE_3D;
        }
        imageCreateInfo.format                = format;
        imageCreateInfo.extent                = extent;
        imageCreateInfo.mipLevels             = mipLevels;
        imageCreateInfo.arrayLayers           = 1;
        imageCreateInfo.samples               = VK_SAMPLE_COUNT_1_BIT;
        imageCreateInfo.tiling                = VK_IMAGE_TILING_OPTIMAL;
        imageCreateInfo.usage                 = usage;
        imageCreateInfo.sharingMode           = VK_SHARING_MODE_EXCLUSIVE;
        imageCreateInfo.queueFamilyIndexCount = 0;       // Don't care
        imageCreateInfo.pQueueFamilyIndices   = nullptr; // Don't care
        imageCreateInfo.initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED;

        VkResult result;
        for (uint32_t i = 0; i < count; i++)
        {
            result = logicalDevice->vkd.CreateImage(logicalDevice->device, &imageCreateInfo, nullptr, &(images[i]));
            if (result != VK_SUCCESS)
            {
                Logger::err("createImages: vkCreateImage failed: " + std::to_string(result));
                for (uint32_t created = 0; created < i; created++)
                    logicalDevice->vkd.DestroyImage(logicalDevice->device, images[created], nullptr);
                return {};
            }
        }
        // Allocate a bunch of memory for all images at one
        VkMemoryRequirements memoryRequirements;
        logicalDevice->vkd.GetImageMemoryRequirements(logicalDevice->device, images[0], &memoryRequirements);

        if (memoryRequirements.size % memoryRequirements.alignment != 0)
        {
            memoryRequirements.size = (memoryRequirements.size / memoryRequirements.alignment + 1) * memoryRequirements.alignment;
        }

        VkMemoryAllocateInfo memoryAllocateInfo;
        memoryAllocateInfo.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memoryAllocateInfo.pNext           = nullptr;
        memoryAllocateInfo.allocationSize  = memoryRequirements.size * count;
        memoryAllocateInfo.memoryTypeIndex = findMemoryTypeIndex(logicalDevice, memoryRequirements.memoryTypeBits, properties);
        if (memoryAllocateInfo.memoryTypeIndex == kInvalidMemoryTypeIndex)
        {
            Logger::err("createImages: no suitable memory type");
            for (auto image : images)
                logicalDevice->vkd.DestroyImage(logicalDevice->device, image, nullptr);
            imageMemory = VK_NULL_HANDLE;
            return {};
        }

        result = logicalDevice->vkd.AllocateMemory(logicalDevice->device, &memoryAllocateInfo, nullptr, &imageMemory);
        if (result != VK_SUCCESS)
        {
            Logger::err("createImages: vkAllocateMemory failed: " + std::to_string(result));
            for (auto image : images)
                logicalDevice->vkd.DestroyImage(logicalDevice->device, image, nullptr);
            imageMemory = VK_NULL_HANDLE;
            return {};
        }

        for (uint32_t i = 0; i < count; i++)
        {
            result = logicalDevice->vkd.BindImageMemory(logicalDevice->device, images[i], imageMemory, memoryRequirements.size * i);
            if (result != VK_SUCCESS)
            {
                Logger::err("createImages: vkBindImageMemory failed: " + std::to_string(result));
                for (auto image : images)
                    logicalDevice->vkd.DestroyImage(logicalDevice->device, image, nullptr);
                logicalDevice->vkd.FreeMemory(logicalDevice->device, imageMemory, nullptr);
                imageMemory = VK_NULL_HANDLE;
                return {};
            }
        }
        return images;
    }

    void
    uploadToImage(LogicalDevice* logicalDevice, VkImage image, VkExtent3D extent, uint32_t size, const unsigned char* writeData, uint32_t mipLevels)
    {

        VkBuffer       stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;

        createBuffer(logicalDevice,
                     size,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuffer,
                     stagingMemory);
        if (stagingBuffer == VK_NULL_HANDLE || stagingMemory == VK_NULL_HANDLE)
        {
            Logger::err("uploadToImage: staging buffer creation failed");
            return;
        }

        void*    data   = nullptr;
        VkResult result = logicalDevice->vkd.MapMemory(logicalDevice->device, stagingMemory, 0, size, 0, &data);
        if (result != VK_SUCCESS)
        {
            Logger::err("uploadToImage: vkMapMemory failed: " + std::to_string(result));
            logicalDevice->vkd.DestroyBuffer(logicalDevice->device, stagingBuffer, nullptr);
            logicalDevice->vkd.FreeMemory(logicalDevice->device, stagingMemory, nullptr);
            return;
        }
        std::memcpy(data, writeData, size);
        logicalDevice->vkd.UnmapMemory(logicalDevice->device, stagingMemory);

        const OneTimeCommands oneTime = allocateOneTimeCommandBuffer(logicalDevice, "uploadToImage");
        const VkCommandBuffer commandBuffer = oneTime.buffer;
        if (commandBuffer == VK_NULL_HANDLE)
        {
            logicalDevice->vkd.DestroyBuffer(logicalDevice->device, stagingBuffer, nullptr);
            logicalDevice->vkd.FreeMemory(logicalDevice->device, stagingMemory, nullptr);
            return;
        }

        VkImageMemoryBarrier memoryBarrier;
        memoryBarrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        memoryBarrier.pNext                           = nullptr;
        memoryBarrier.srcAccessMask                   = 0;
        memoryBarrier.dstAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT;
        memoryBarrier.oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
        memoryBarrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        memoryBarrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.image                           = image;
        memoryBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        memoryBarrier.subresourceRange.baseMipLevel   = 0;
        memoryBarrier.subresourceRange.levelCount     = mipLevels;
        memoryBarrier.subresourceRange.baseArrayLayer = 0;
        memoryBarrier.subresourceRange.layerCount     = 1;

        logicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

        VkBufferImageCopy region;
        region.bufferOffset                    = 0;
        region.bufferRowLength                 = 0;
        region.bufferImageHeight               = 0;
        region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel       = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount     = 1;
        region.imageOffset                     = {0, 0, 0};
        region.imageExtent                     = extent;

        logicalDevice->vkd.CmdCopyBufferToImage(commandBuffer, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        memoryBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        memoryBarrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
        memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        logicalDevice->vkd.CmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

        generateMipMaps(logicalDevice, commandBuffer, image, extent, mipLevels);

        finishOneTimeCommands(logicalDevice, oneTime, "uploadToImage", "image upload sync", stagingBuffer, stagingMemory);
    }

    VkImage createSingleImage(LogicalDevice*        logicalDevice,
                              VkExtent3D            extent,
                              VkFormat              format,
                              VkImageUsageFlags     usage,
                              VkMemoryPropertyFlags properties,
                              VkDeviceMemory&       imageMemory,
                              uint32_t              mipLevels)
    {
        std::vector<VkImage> images = createImages(logicalDevice, 1, extent, format, usage, properties, imageMemory, mipLevels);
        if (images.empty())
        {
            imageMemory = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        return images[0];
    }

    void changeImageLayout(LogicalDevice* logicalDevice, const std::vector<VkImage>& images, uint32_t mipLevels)
    {
        const OneTimeCommands oneTime = allocateOneTimeCommandBuffer(logicalDevice, "changeImageLayout");
        const VkCommandBuffer commandBuffer = oneTime.buffer;
        if (commandBuffer == VK_NULL_HANDLE)
            return;

        VkImageMemoryBarrier memoryBarrier;
        memoryBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        memoryBarrier.pNext               = nullptr;
        memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

        memoryBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        memoryBarrier.subresourceRange.baseMipLevel   = 0;
        memoryBarrier.subresourceRange.levelCount     = mipLevels;
        memoryBarrier.subresourceRange.baseArrayLayer = 0;
        memoryBarrier.subresourceRange.layerCount     = 1;

        VkClearColorValue clearValue = {};

        for (auto& image : images)
        {
            memoryBarrier.image = image;
            memoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            memoryBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            memoryBarrier.srcAccessMask = 0;
            memoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            logicalDevice->vkd.CmdPipelineBarrier(
                commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

            logicalDevice->vkd.CmdClearColorImage(
                commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1, &memoryBarrier.subresourceRange);

            memoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            memoryBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            logicalDevice->vkd.CmdPipelineBarrier(
                commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);
        }

        finishOneTimeCommands(logicalDevice, oneTime, "changeImageLayout", "image layout sync");
    }

    void generateMipMaps(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer, VkImage image, VkExtent3D extent, uint32_t mipLevels)
    {
        if (mipLevels < 2)
        {
            return;
        }
        int32_t width  = extent.width;
        int32_t height = extent.height;
        int32_t depth  = extent.depth;

        VkImageMemoryBarrier memoryBarrier;
        memoryBarrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        memoryBarrier.pNext               = nullptr;
        memoryBarrier.image               = image;
        memoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

        memoryBarrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        memoryBarrier.subresourceRange.baseMipLevel   = 0;
        memoryBarrier.subresourceRange.levelCount     = 1;
        memoryBarrier.subresourceRange.baseArrayLayer = 0;
        memoryBarrier.subresourceRange.layerCount     = 1;

        for (uint32_t i = 1; i < mipLevels; i++)
        {
            VkImageBlit imageBlit;

            imageBlit.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            imageBlit.srcSubresource.mipLevel       = i - 1;
            imageBlit.srcSubresource.baseArrayLayer = 0;
            imageBlit.srcSubresource.layerCount     = 1;
            imageBlit.srcOffsets[0]                 = {0, 0, 0};
            imageBlit.srcOffsets[1]                 = {width, height, depth};

            width  = (width == 1) ? 1 : width / 2;
            height = (height == 1) ? 1 : height / 2;
            depth  = (depth == 1) ? 1 : depth / 2;

            imageBlit.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            imageBlit.dstSubresource.mipLevel       = i;
            imageBlit.dstSubresource.baseArrayLayer = 0;
            imageBlit.dstSubresource.layerCount     = 1;
            imageBlit.dstOffsets[0]                 = {0, 0, 0};
            imageBlit.dstOffsets[1]                 = {width, height, depth};

            memoryBarrier.subresourceRange.baseMipLevel = i - 1;

            memoryBarrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
            memoryBarrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            memoryBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                                          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            memoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

            // Level zero was rendered; subsequent levels were just blitted.
            // TOP_OF_PIPE cannot make either producer's writes visible.
            logicalDevice->vkd.CmdPipelineBarrier(
                commandBuffer,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

            memoryBarrier.subresourceRange.baseMipLevel = i;

            memoryBarrier.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
            memoryBarrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            memoryBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                                          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            memoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

            logicalDevice->vkd.CmdPipelineBarrier(
                commandBuffer,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

            logicalDevice->vkd.CmdBlitImage(commandBuffer,
                                             image,
                                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                             image,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             1,
                                             &imageBlit,
                                             VK_FILTER_LINEAR);

            memoryBarrier.subresourceRange.baseMipLevel = i - 1;

            memoryBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            memoryBarrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
            memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

            logicalDevice->vkd.CmdPipelineBarrier(
                commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);

            memoryBarrier.subresourceRange.baseMipLevel = i;

            memoryBarrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            memoryBarrier.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
            memoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            memoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

            logicalDevice->vkd.CmdPipelineBarrier(
                commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &memoryBarrier);
        }
    }
} // namespace VkHook
