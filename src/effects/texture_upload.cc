#include "texture_upload.hh"

#include <cstring>
#include <string>

#include "buffer.hh"
#include "image.hh"
#include "logger.hh"

namespace VkHook
{
    namespace
    {
        // The one place this file hands GPU objects back. Once the GPU has
        // finished with them they are released here and now; otherwise (the wait
        // failed or timed out, so the command buffer may still be executing)
        // they go to the device's RetireQueue, tagged with the upload's serial.
        // The command buffer lives in a pool of its own (pools are externally
        // synchronized and concurrent builders must not share one), so the pool
        // is what gets released.
        void retireUploadObjects(LogicalDevice* logicalDevice,
                                 VkCommandPool pool,
                                 VkBuffer staging,
                                 VkDeviceMemory stagingMemory,
                                 bool gpuFinished)
        {
            const VkDevice device = logicalDevice->device;
            if (gpuFinished)
            {
                if (pool != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroyCommandPool(device, pool, nullptr);
                if (staging != VK_NULL_HANDLE)
                    logicalDevice->vkd.DestroyBuffer(device, staging, nullptr);
                if (stagingMemory != VK_NULL_HANDLE)
                    logicalDevice->vkd.FreeMemory(device, stagingMemory, nullptr);
                return;
            }
            if (pool != VK_NULL_HANDLE)
                logicalDevice->retire(VK_OBJECT_TYPE_COMMAND_POOL, pool);
            if (staging != VK_NULL_HANDLE)
                logicalDevice->retire(VK_OBJECT_TYPE_BUFFER, staging);
            if (stagingMemory != VK_NULL_HANDLE)
                logicalDevice->retire(VK_OBJECT_TYPE_DEVICE_MEMORY, stagingMemory);
        }

        uint64_t alignUp(uint64_t value, uint64_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }
    } // namespace

    void TextureUploadBatch::addUpload(VkImage image, VkExtent3D extent, uint32_t mipLevels, std::vector<uint8_t> pixels)
    {
        Item item;
        item.image = image;
        item.extent = extent;
        item.mipLevels = mipLevels;
        item.stagingOffset = alignUp(m_stagingBytes, kStagingAlignment);
        m_stagingBytes = item.stagingOffset + pixels.size();
        item.pixels = std::move(pixels);
        m_items.push_back(std::move(item));
    }

    void TextureUploadBatch::addClear(VkImage image, uint32_t mipLevels)
    {
        Item item;
        item.image = image;
        item.mipLevels = mipLevels;
        item.stagingOffset = alignUp(m_stagingBytes, kStagingAlignment);
        m_items.push_back(std::move(item));
    }

    void TextureUploadBatch::fill(void* mapped) const
    {
        for (const Item& item : m_items)
        {
            if (!item.pixels.empty())
                std::memcpy(static_cast<uint8_t*>(mapped) + item.stagingOffset, item.pixels.data(), item.pixels.size());
        }
    }

    void TextureUploadBatch::record(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer, VkBuffer staging) const
    {
        auto& vkd = logicalDevice->vkd;

        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        for (const Item& item : m_items)
        {
            barrier.image = item.image;
            barrier.subresourceRange.levelCount = item.mipLevels;

            // Every texel is overwritten, so the old contents are discarded.
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkd.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                   nullptr, 0, nullptr, 1, &barrier);

            if (item.pixels.empty())
            {
                const VkClearColorValue clear = {};
                vkd.CmdClearColorImage(commandBuffer, item.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                                       &barrier.subresourceRange);
            }
            else
            {
                VkBufferImageCopy region = {};
                region.bufferOffset = item.stagingOffset;
                region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                region.imageSubresource.mipLevel = 0;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = item.extent;
                vkd.CmdCopyBufferToImage(commandBuffer, staging, item.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            }

            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = item.pixels.empty() ? (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
                                                        : VK_ACCESS_SHADER_READ_BIT;
            vkd.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                   nullptr, 0, nullptr, 1, &barrier);

            if (!item.pixels.empty())
                generateMipMaps(logicalDevice, commandBuffer, item.image, item.extent, item.mipLevels);
        }
    }

    bool TextureUploadBatch::submit(LogicalDevice* logicalDevice, const SubmitAndWait& submitAndWait) const
    {
        if (m_items.empty())
            return true;

        VkBuffer       staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        // A batch of clears alone has no pixels; the buffer is still created so
        // the recording path is one path, but never smaller than a valid size.
        createBuffer(logicalDevice, m_stagingBytes ? m_stagingBytes : kStagingAlignment, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, stagingMemory);
        if (staging == VK_NULL_HANDLE || stagingMemory == VK_NULL_HANDLE)
        {
            Logger::err("texture upload: staging buffer creation failed");
            return false;
        }

        void*    mapped = nullptr;
        VkResult result = logicalDevice->vkd.MapMemory(logicalDevice->device, stagingMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
        if (result != VK_SUCCESS)
        {
            Logger::err("texture upload: vkMapMemory failed: " + std::to_string(result));
            retireUploadObjects(logicalDevice, VK_NULL_HANDLE, staging, stagingMemory, true);
            return false;
        }
        fill(mapped);
        logicalDevice->vkd.UnmapMemory(logicalDevice->device, stagingMemory);

        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = logicalDevice->queueFamilyIndex;
        VkCommandPool pool = VK_NULL_HANDLE;
        result = logicalDevice->vkd.CreateCommandPool(logicalDevice->device, &poolInfo, nullptr, &pool);
        if (result != VK_SUCCESS || pool == VK_NULL_HANDLE)
        {
            Logger::err("texture upload: vkCreateCommandPool failed: " + std::to_string(result));
            retireUploadObjects(logicalDevice, VK_NULL_HANDLE, staging, stagingMemory, true);
            return false;
        }

        VkCommandBufferAllocateInfo allocate = {};
        allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandPool = pool;
        allocate.commandBufferCount = 1;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        result = allocateLayerCommandBuffers(logicalDevice, &allocate, &commandBuffer);
        if (result != VK_SUCCESS || commandBuffer == VK_NULL_HANDLE)
        {
            Logger::err("texture upload: vkAllocateCommandBuffers failed: " + std::to_string(result));
            retireUploadObjects(logicalDevice, pool, staging, stagingMemory, true);
            return false;
        }
        // The command buffer is a dispatchable object the loader did not create.
        initializeDispatchTable(commandBuffer, logicalDevice->device);

        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = logicalDevice->vkd.BeginCommandBuffer(commandBuffer, &begin);
        if (result != VK_SUCCESS)
        {
            Logger::err("texture upload: vkBeginCommandBuffer failed: " + std::to_string(result));
            retireUploadObjects(logicalDevice, pool, staging, stagingMemory, true);
            return false;
        }

        record(logicalDevice, commandBuffer, staging);

        result = logicalDevice->vkd.EndCommandBuffer(commandBuffer);
        if (result != VK_SUCCESS)
        {
            Logger::err("texture upload: vkEndCommandBuffer failed: " + std::to_string(result));
            retireUploadObjects(logicalDevice, pool, staging, stagingMemory, true);
            return false;
        }

        const bool finished = submitAndWait(commandBuffer);
        retireUploadObjects(logicalDevice, pool, staging, stagingMemory, finished);
        return finished;
    }
} // namespace VkHook
