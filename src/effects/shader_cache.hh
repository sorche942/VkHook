#ifndef SHADER_CACHE_HPP_INCLUDED
#define SHADER_CACHE_HPP_INCLUDED

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "compile_cache.hh"
#include "reshade/effect_module.hh"

namespace VkHook
{
    struct ReshadePreprocessSetup;

    // Everything the compile of one effect depends on. The preprocessor output
    // has every include file and every macro already folded into the text, so
    // its hash covers the include files' contents and the macros' effect on the
    // source; the macro digest and the codegen flags are kept as separate fields
    // anyway so two inputs that differ only there can never share an entry, and
    // the compiler tag retires every entry when the compiler or the encoding
    // changes.
    //
    // The preprocessor is deliberately NOT skipped on a hit: whether an include
    // resolves to a different file than it did last time (a new file earlier in
    // the include path) can only be known by running it.
    struct ShaderCacheKey
    {
        uint64_t sourceHash = 0;
        uint64_t sourceSize = 0;
        uint64_t macroHash = 0;
        uint32_t codegenFlags = 0;

        bool operator==(const ShaderCacheKey& other) const
        {
            return sourceHash == other.sourceHash && sourceSize == other.sourceSize &&
                   macroHash == other.macroHash && codegenFlags == other.codegenFlags;
        }
    };

    ShaderCacheKey makeShaderCacheKey(const std::string& preprocessedSource, uint64_t macroHash, uint32_t codegenFlags);

    // Digest of the setup that shapes preprocessing: buffer extent, colour depth,
    // depth source mode, user macros (order-sensitive, as the preprocessor is).
    uint64_t hashPreprocessSetup(const ReshadePreprocessSetup& setup);

    // Bump when the vendored compiler, its codegen arguments in
    // compileReshadeSource, or the module encoding changes.
    inline constexpr const char* kShaderCompilerTag = "reshadefx-spirv-v1";

    // `${XDG_CACHE_HOME:-~/.cache}/VkHook/shaders`; empty when neither variable
    // is usable, which disables the disk level.
    std::string defaultShaderCacheDirectory();

    // Two-level cache of compiled modules: bounded memory in front of a
    // versioned on-disk store.
    //
    // Disk entries are single files named by a digest of the key. A file holds
    // a header (magic, format version, the full key, the compiler tag) and a
    // checksummed payload. A file that is missing, truncated, from another
    // version or key, or fails its checksum or decode is a miss, never an error;
    // the next store overwrites it.
    //
    // Writes never happen on the caller's thread: they go through the injected
    // scheduler, which in the layer is the PersistenceQueue (single ordered
    // writer, no file I/O on the present thread). Reads are synchronous.
    class ShaderCache
    {
    public:
        using Handle = std::shared_ptr<const reshadefx::module>;
        // Runs `job` off the caller's thread. `path` identifies the file so the
        // scheduler can coalesce writes to it.
        using WriteScheduler = std::function<void(const std::string& path, std::function<bool()> job)>;

        // An empty `directory` disables the disk level.
        ShaderCache(size_t memoryCapacity, std::string directory, std::string compilerTag, WriteScheduler scheduler);

        Handle lookup(const ShaderCacheKey& key);
        void   store(const ShaderCacheKey& key, Handle module);

        // The path an entry for `key` lives at (empty without a disk level).
        std::string entryPath(const ShaderCacheKey& key) const;

    private:
        Handle loadFromDisk(const ShaderCacheKey& key) const;

        CompileCache<reshadefx::module> m_memory;
        std::string                     m_directory;
        std::string                     m_tag;
        WriteScheduler                  m_schedule;
    };

    // A cache wired to the default directory and the PersistenceQueue. Each
    // EffectRegistry (one per device) owns one: the memory level holds compiled
    // modules, which are immutable CPU data, and the disk level is shared by
    // every cache on the machine through the content-addressed files.
    std::unique_ptr<ShaderCache> makeDefaultShaderCache();
}

#endif // SHADER_CACHE_HPP_INCLUDED
