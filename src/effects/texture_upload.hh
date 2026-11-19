#ifndef TEXTURE_UPLOAD_HPP_INCLUDED
#define TEXTURE_UPLOAD_HPP_INCLUDED

#include <cstdint>
#include <functional>
#include <vector>

#include "vulkan_include.hh"

#include "logical_device.hh"

namespace VkHook
{
    // All the texture initialisation of one effect, recorded into one command
    // buffer and fed from one staging buffer, so building an effect costs one
    // queue submission and one wait however many textures it loads.
    //
    // Each item leaves its image in GENERAL, the layout every effect texture is
    // kept in: an upload fills mip 0 from the staging buffer and generates the
    // remaining mips; a clear zero-fills every mip (a texture whose file could
    // not be loaded is cleared, never left undefined).
    class TextureUploadBatch
    {
    public:
        // Hands the command buffer (recorded and ended) to the caller's queue
        // path, waits until the GPU has finished it and returns whether it did.
        // The batch never touches a queue itself, so the layer decides which
        // lock and which queue the work goes through.
        using SubmitAndWait = std::function<bool(VkCommandBuffer)>;

        void addUpload(VkImage image, VkExtent3D extent, uint32_t mipLevels, std::vector<uint8_t> pixels);
        void addClear(VkImage image, uint32_t mipLevels);

        bool   empty() const { return m_items.empty(); }
        size_t size() const { return m_items.size(); }

        // Staging layout: item i's pixels start at stagingOffset(i), each at a
        // multiple of kStagingAlignment (a valid bufferOffset for every format).
        static constexpr uint64_t kStagingAlignment = 64;
        uint64_t stagingBytes() const { return m_stagingBytes; }
        uint64_t stagingOffset(size_t item) const { return m_items[item].stagingOffset; }

        // Copies every upload's pixels to their offsets inside `mapped`, which
        // must span stagingBytes().
        void fill(void* mapped) const;

        // Records all items into the begun `commandBuffer`. Uploads copy from
        // `staging`.
        void record(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer, VkBuffer staging) const;

        // Staging buffer, one command buffer, one submitAndWait. True when
        // everything completed on the GPU; an empty batch does nothing and
        // returns true.
        bool submit(LogicalDevice* logicalDevice, const SubmitAndWait& submitAndWait) const;

    private:
        struct Item
        {
            VkImage              image = VK_NULL_HANDLE;
            VkExtent3D           extent = {};
            uint32_t             mipLevels = 1;
            std::vector<uint8_t> pixels; // empty: clear
            uint64_t             stagingOffset = 0;
        };

        std::vector<Item> m_items;
        uint64_t          m_stagingBytes = 0;
    };
} // namespace VkHook

#endif // TEXTURE_UPLOAD_HPP_INCLUDED
