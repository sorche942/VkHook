#include "layer/pipeline_cache.hh"

#include "layer/guard.hh"
#include "logger.hh"
#include "paths.hh"
#include "persist.hh"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace VkHook
{
    namespace
    {
        std::string hex(const uint8_t* bytes, size_t count)
        {
            std::string out;
            char        buf[3];
            for (size_t i = 0; i < count; ++i)
            {
                std::snprintf(buf, sizeof(buf), "%02x", bytes[i]);
                out += buf;
            }
            return out;
        }

        // Writes via a temp file so a crash mid-write never leaves a truncated blob.
        bool writeFileAtomic(const std::string& path, const std::vector<char>& data)
        {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
            if (ec)
                return false;

            const std::string tmp = path + ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out)
                    return false;
                out.write(data.data(), static_cast<std::streamsize>(data.size()));
                if (!out)
                    return false;
            }
            std::filesystem::rename(tmp, path, ec);
            return !ec;
        }
    } // namespace

    std::string pipelineCachePath(const VkPhysicalDeviceProperties& properties)
    {
        const std::string home = Paths::cacheHome();
        if (home.empty())
            return "";
        return home + "/VkHook/pipeline-" + std::to_string(properties.vendorID) + "-" + std::to_string(properties.deviceID)
               + "-" + hex(properties.pipelineCacheUUID, VK_UUID_SIZE) + ".bin";
    }

    bool pipelineCacheBlobMatches(const void* data, size_t size, const VkPhysicalDeviceProperties& properties)
    {
        struct HeaderV1
        {
            uint32_t headerSize;
            uint32_t headerVersion;
            uint32_t vendorID;
            uint32_t deviceID;
            uint8_t  uuid[VK_UUID_SIZE];
        };
        if (!data || size < sizeof(HeaderV1))
            return false;
        HeaderV1 header;
        std::memcpy(&header, data, sizeof(header));
        return header.headerSize >= sizeof(HeaderV1)
               && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
               && header.vendorID == properties.vendorID
               && header.deviceID == properties.deviceID
               && std::memcmp(header.uuid, properties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
    }

    void loadPipelineCache(LogicalDevice& device, const VkPhysicalDeviceProperties& properties)
    {
        std::vector<char> blob;
        const std::string path = pipelineCachePath(properties);
        if (!path.empty())
        {
            std::ifstream in(path, std::ios::binary);
            if (in)
                blob.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (!blob.empty() && !pipelineCacheBlobMatches(blob.data(), blob.size(), properties))
            {
                Logger::info("pipeline cache file does not match this device, starting empty: " + path);
                blob.clear();
            }
        }

        VkPipelineCacheCreateInfo info = {};
        info.sType           = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        info.initialDataSize = blob.size();
        info.pInitialData    = blob.empty() ? nullptr : blob.data();

        VkPipelineCache cache = VK_NULL_HANDLE;
        VkResult        res   = device.vkd.CreatePipelineCache(device.device, &info, nullptr, &cache);
        if (res != VK_SUCCESS && !blob.empty())
        {
            // the driver refused the blob: fall back to an empty cache
            info.initialDataSize = 0;
            info.pInitialData    = nullptr;
            res                  = device.vkd.CreatePipelineCache(device.device, &info, nullptr, &cache);
        }
        if (res != VK_SUCCESS)
        {
            Logger::warn("vkCreatePipelineCache failed (" + std::to_string(res) + "); effects will create their own");
            return;
        }
        device.pipelineCache = cache;
        if (!blob.empty())
            Logger::info("pipeline cache loaded: " + std::to_string(blob.size()) + " bytes");
    }

    void savePipelineCache(LogicalDevice& device)
    {
        VkPipelineCache cache;
        {
            std::lock_guard<std::mutex> l(device.stateLock);
            cache = device.pipelineCache;
        }
        if (cache == VK_NULL_HANDLE)
            return;

        const std::string path = pipelineCachePath(device.gpuProperties);
        if (path.empty())
            return;

        guardLayerWork(&device, "savePipelineCache", [&] {
            size_t size = 0;
            if (device.vkd.GetPipelineCacheData(device.device, cache, &size, nullptr) != VK_SUCCESS || size == 0)
                return;
            std::vector<char> blob(size);
            const VkResult    res = device.vkd.GetPipelineCacheData(device.device, cache, &size, blob.data());
            if (res != VK_SUCCESS && res != VK_INCOMPLETE)
                return;
            blob.resize(size);

            PersistenceQueue::instance().write(path, [path, blob = std::move(blob)] {
                const bool ok = writeFileAtomic(path, blob);
                if (!ok)
                    Logger::warn("could not write pipeline cache: " + path);
                return ok;
            });
        });
    }
} // namespace VkHook
