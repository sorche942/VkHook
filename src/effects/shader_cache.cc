#include "shader_cache.hh"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "logger.hh"
#include "paths.hh"
#include "persist.hh"
#include "reshade_compile.hh"
#include "reshade_module_io.hh"

namespace VkHook
{
    namespace
    {
        constexpr uint32_t kMagic = 0x43534b56u; // "VKSC"
        constexpr uint32_t kFormatVersion = 1;
        constexpr size_t   kMaxTagBytes = 256;
        constexpr uint64_t kMaxEntryBytes = 256ull << 20;
        // Keeps the directory from growing for ever as sources are edited; the
        // oldest files go first.
        constexpr size_t kMaxEntries = 1024;
        constexpr const char* kExtension = ".vksc";
        std::atomic<uint32_t> g_tempCounter{0};

        uint64_t mixU64(uint64_t seed, uint64_t value)
        {
            return hashBytes(&value, sizeof(value), seed);
        }

        uint64_t mixString(uint64_t seed, const std::string& value)
        {
            // The length first, so ("ab","c") and ("a","bc") differ.
            return hashBytes(value.data(), value.size(), mixU64(seed, value.size()));
        }

        std::string hex64(uint64_t value)
        {
            char text[17];
            std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
            return text;
        }

        void putU32(std::vector<uint8_t>& out, uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
                out.push_back(static_cast<uint8_t>(value >> shift));
        }

        void putU64(std::vector<uint8_t>& out, uint64_t value)
        {
            for (int shift = 0; shift < 64; shift += 8)
                out.push_back(static_cast<uint8_t>(value >> shift));
        }

        uint32_t getU32(const uint8_t* p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }

        uint64_t getU64(const uint8_t* p)
        {
            return static_cast<uint64_t>(getU32(p)) | (static_cast<uint64_t>(getU32(p + 4)) << 32);
        }

        std::vector<uint8_t> encodeEntry(const ShaderCacheKey& key, const std::string& tag, const reshadefx::module& module)
        {
            const std::vector<uint8_t> payload = serializeReshadeModule(module);
            std::vector<uint8_t>       out;
            out.reserve(payload.size() + 64 + tag.size());
            putU32(out, kMagic);
            putU32(out, kFormatVersion);
            putU32(out, static_cast<uint32_t>(tag.size()));
            out.insert(out.end(), tag.begin(), tag.end());
            putU64(out, key.sourceHash);
            putU64(out, key.sourceSize);
            putU64(out, key.macroHash);
            putU32(out, key.codegenFlags);
            putU64(out, payload.size());
            putU64(out, hashBytes(payload.data(), payload.size()));
            out.insert(out.end(), payload.begin(), payload.end());
            return out;
        }

        // False on anything that is not a complete, intact entry for exactly this
        // key and tag.
        bool decodeEntry(const std::vector<uint8_t>& file, const ShaderCacheKey& key, const std::string& tag,
                         reshadefx::module& module)
        {
            constexpr size_t kFixedHead = 4 + 4 + 4;
            constexpr size_t kFixedTail = 8 + 8 + 8 + 4 + 8 + 8;
            if (file.size() < kFixedHead)
                return false;
            const uint8_t* p = file.data();
            if (getU32(p) != kMagic || getU32(p + 4) != kFormatVersion)
                return false;
            const uint32_t tagLength = getU32(p + 8);
            if (tagLength > kMaxTagBytes || file.size() - kFixedHead < tagLength + kFixedTail)
                return false;
            if (tagLength != tag.size() || std::memcmp(p + kFixedHead, tag.data(), tagLength) != 0)
                return false;
            p += kFixedHead + tagLength;
            if (getU64(p) != key.sourceHash || getU64(p + 8) != key.sourceSize || getU64(p + 16) != key.macroHash ||
                getU32(p + 24) != key.codegenFlags)
                return false;
            const uint64_t payloadSize = getU64(p + 28);
            const uint64_t checksum = getU64(p + 36);
            p += 44;
            const size_t left = file.size() - static_cast<size_t>(p - file.data());
            if (payloadSize != left)
                return false;
            if (hashBytes(p, left) != checksum)
                return false;
            return deserializeReshadeModule(p, left, module);
        }

        // Temp file in the same directory, then rename: a reader never sees half a
        // file, and a crash mid-write leaves only a stray temp file.
        bool writeFileAtomically(const std::string& path, const std::vector<uint8_t>& bytes)
        {
            const std::filesystem::path  target(path);
            std::error_code              error;
            std::filesystem::create_directories(target.parent_path(), error);
            if (error)
                return false;

            const std::string temp = path + ".tmp." + std::to_string(static_cast<long>(getpid())) + "." +
                                     std::to_string(g_tempCounter.fetch_add(1));
            FILE* file = std::fopen(temp.c_str(), "wb");
            if (!file)
                return false;
            const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
            const bool closed = std::fclose(file) == 0;
            if (!written || !closed || std::rename(temp.c_str(), path.c_str()) != 0)
            {
                std::remove(temp.c_str());
                return false;
            }
            return true;
        }

        void pruneDirectory(const std::filesystem::path& directory)
        {
            struct Entry
            {
                std::filesystem::path path;
                std::filesystem::file_time_type time;
            };
            std::vector<Entry> entries;
            std::error_code    error;
            for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
            {
                if (it->path().extension() != kExtension)
                    continue;
                std::error_code timeError;
                const auto      time = it->last_write_time(timeError);
                if (!timeError)
                    entries.push_back({it->path(), time});
            }
            if (entries.size() <= kMaxEntries)
                return;
            std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
            for (size_t i = 0; i < entries.size() - kMaxEntries; ++i)
            {
                std::error_code removeError;
                std::filesystem::remove(entries[i].path, removeError);
            }
        }

        CompileCache<reshadefx::module>::Key memoryKey(const ShaderCacheKey& key)
        {
            uint64_t digest = mixU64(kFnvOffsetBasis, key.sourceHash);
            digest = mixU64(digest, key.sourceSize);
            digest = mixU64(digest, key.macroHash);
            return {digest, key.codegenFlags};
        }
    } // namespace

    ShaderCacheKey makeShaderCacheKey(const std::string& preprocessedSource, uint64_t macroHash, uint32_t codegenFlags)
    {
        ShaderCacheKey key;
        key.sourceHash = hashBytes(preprocessedSource.data(), preprocessedSource.size());
        key.sourceSize = preprocessedSource.size();
        key.macroHash = macroHash;
        key.codegenFlags = codegenFlags;
        return key;
    }

    uint64_t hashPreprocessSetup(const ReshadePreprocessSetup& setup)
    {
        uint64_t digest = kFnvOffsetBasis;
        digest = mixU64(digest, setup.bufferWidth);
        digest = mixU64(digest, setup.bufferHeight);
        digest = mixU64(digest, setup.colorBitDepth);
        digest = mixU64(digest, setup.userDefs.size());
        for (const auto& def : setup.userDefs)
        {
            digest = mixString(digest, def.name);
            digest = mixString(digest, def.value);
        }
        return digest;
    }

    std::string defaultShaderCacheDirectory()
    {
        const std::string base = Paths::cacheHome();
        return base.empty() ? std::string() : base + "/VkHook/shaders";
    }

    ShaderCache::ShaderCache(size_t memoryCapacity, std::string directory, std::string compilerTag, WriteScheduler scheduler)
        : m_memory(memoryCapacity)
        , m_directory(std::move(directory))
        , m_tag(std::move(compilerTag))
        , m_schedule(std::move(scheduler))
    {
    }

    std::string ShaderCache::entryPath(const ShaderCacheKey& key) const
    {
        if (m_directory.empty())
            return {};
        const uint64_t flagsAndSize = mixU64(key.codegenFlags, key.sourceSize);
        return m_directory + "/" + hex64(key.sourceHash) + "-" + hex64(key.macroHash) + "-" + hex64(flagsAndSize) + kExtension;
    }

    ShaderCache::Handle ShaderCache::loadFromDisk(const ShaderCacheKey& key) const
    {
        const std::string path = entryPath(key);
        if (path.empty())
            return nullptr;

        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file)
            return nullptr;
        std::vector<uint8_t> bytes;
        bool                 readable = std::fseek(file, 0, SEEK_END) == 0;
        const long           length = readable ? std::ftell(file) : -1;
        readable = readable && length >= 0 && static_cast<uint64_t>(length) <= kMaxEntryBytes &&
                   std::fseek(file, 0, SEEK_SET) == 0;
        if (readable)
        {
            bytes.resize(static_cast<size_t>(length));
            readable = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
        }
        std::fclose(file);
        if (!readable)
            return nullptr;

        auto module = std::make_shared<reshadefx::module>();
        if (!decodeEntry(bytes, key, m_tag, *module))
        {
            Logger::debug("shader cache: ignoring unusable entry " + path);
            return nullptr;
        }
        return module;
    }

    ShaderCache::Handle ShaderCache::lookup(const ShaderCacheKey& key)
    {
        const auto memKey = memoryKey(key);
        if (Handle hit = m_memory.lookup(memKey))
            return hit;
        Handle fromDisk = loadFromDisk(key);
        if (fromDisk)
            m_memory.store(memKey, fromDisk);
        return fromDisk;
    }

    void ShaderCache::store(const ShaderCacheKey& key, Handle module)
    {
        const auto memKey = memoryKey(key);
        // Present means it was stored before or came from disk; either way the
        // file already exists.
        if (!module || m_memory.lookup(memKey))
            return;
        m_memory.store(memKey, module);

        const std::string path = entryPath(key);
        if (path.empty() || !m_schedule)
            return;
        m_schedule(path, [path, key, tag = m_tag, module = std::move(module)] {
            if (!writeFileAtomically(path, encodeEntry(key, tag, *module)))
            {
                Logger::debug("shader cache: could not write " + path);
                return false;
            }
            pruneDirectory(std::filesystem::path(path).parent_path());
            return true;
        });
    }

    std::unique_ptr<ShaderCache> makeDefaultShaderCache()
    {
        return std::make_unique<ShaderCache>(
            64, defaultShaderCacheDirectory(), kShaderCompilerTag + std::string("/") + std::to_string(sizeof(reshadefx::module)),
            [](const std::string& path, std::function<bool()> job) {
                PersistenceQueue::instance().other(path, std::move(job));
            });
    }
} // namespace VkHook
