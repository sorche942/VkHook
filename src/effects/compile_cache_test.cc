// Tests for the compile cache: the bounded in-memory level that lets a chain
// reload reuse an already-compiled ReShade module instead of re-parsing every
// effect, and the versioned on-disk level behind it. The key semantics, the
// bound, and "a damaged cache file is a miss, never a crash" are the whole
// contract, so they are pinned here; the threaded case fails if the internal
// lock ever goes away.

#include "compile_cache.hh"
#include "persist.hh"
#include "reshade_module_io.hh"
#include "shader_cache.hh"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

using VkHook::CompileCache;
using VkHook::hashBytes;
using VkHook::PersistenceQueue;
using VkHook::ShaderCache;
using VkHook::ShaderCacheKey;

namespace
{
    int g_failures = 0;
    int g_checks   = 0;

    void expect(bool condition, const std::string& what)
    {
        ++g_checks;
        if (!condition)
        {
            g_failures++;
            std::printf("  FAILED: %s\n", what.c_str());
        }
    }

    using StringCache = CompileCache<std::string>;

    void put(StringCache& cache, const StringCache::Key& key, const char* value)
    {
        cache.store(key, std::make_shared<const std::string>(value));
    }

    // The stored value, or "<miss>".
    std::string get(const StringCache& cache, const StringCache::Key& key)
    {
        const auto hit = cache.lookup(key);
        return hit ? *hit : std::string("<miss>");
    }

    void test_empty_cache_misses()
    {
        StringCache cache;
        expect(cache.lookup({123, 0}) == nullptr, "an empty cache misses");
        expect(cache.size() == 0, "an empty cache is empty");
    }

    void test_store_then_lookup_round_trips()
    {
        StringCache cache;
        put(cache, {hashBytes("effect", 6), 7}, "compiled-module");

        expect(get(cache, {hashBytes("effect", 6), 7}) == "compiled-module", "a stored key hits and returns the value");
        expect(cache.size() == 1, "one entry after one store");
    }

    void test_hit_shares_instead_of_copying()
    {
        StringCache cache;
        const StringCache::Key key{9, 0};
        auto                   value = std::make_shared<const std::string>("payload");
        cache.store(key, value);
        expect(cache.lookup(key).get() == value.get(), "a hit hands out the stored object, not a copy");
        expect(cache.lookup(key).get() == cache.lookup(key).get(), "two hits share one object");
    }

    void test_evicted_entry_stays_alive_for_holders()
    {
        StringCache cache(1);
        put(cache, {1, 0}, "first");
        auto held = cache.lookup({1, 0});
        put(cache, {2, 0}, "second");
        expect(cache.lookup({1, 0}) == nullptr, "the evicted entry is gone from the cache");
        expect(held && *held == "first", "an effect still using an evicted module keeps it alive");
    }

    void test_flags_partition_the_key()
    {
        StringCache cache;
        const uint64_t h = hashBytes("same-source", 11);
        put(cache, {h, 0}, "flags-0");
        put(cache, {h, 1}, "flags-1");

        expect(get(cache, {h, 0}) == "flags-0", "flag word 0 is its own entry");
        expect(get(cache, {h, 1}) == "flags-1", "flag word 1 is its own entry");
        expect(get(cache, {h, 2}) == "<miss>", "an unseen flag word misses");
    }

    void test_source_partitions_the_key()
    {
        StringCache cache;
        put(cache, {hashBytes("a", 1), 0}, "module-a");
        put(cache, {hashBytes("b", 1), 0}, "module-b");

        expect(get(cache, {hashBytes("a", 1), 0}) == "module-a", "source a is distinct");
        expect(get(cache, {hashBytes("b", 1), 0}) == "module-b", "source b is distinct");
    }

    void test_restore_does_not_duplicate()
    {
        StringCache cache;
        const StringCache::Key key{42, 0};
        put(cache, key, "first");
        put(cache, key, "second");

        expect(cache.size() == 1, "re-storing the same key does not grow the cache");
        expect(get(cache, key) == "first", "first value wins; a reload is not a mutation");
    }

    void test_fifo_bound_evicts_oldest()
    {
        StringCache cache(3);
        put(cache, {1, 0}, "one");
        put(cache, {2, 0}, "two");
        put(cache, {3, 0}, "three");
        expect(cache.size() == 3, "cache fills to capacity");

        put(cache, {4, 0}, "four");
        expect(cache.size() == 3, "capacity is not exceeded");

        expect(get(cache, {1, 0}) == "<miss>", "the oldest entry was evicted");
        expect(get(cache, {2, 0}) == "two", "the next entry survives");
        expect(get(cache, {3, 0}) == "three", "the third entry survives");
        expect(get(cache, {4, 0}) == "four", "the new entry is present");
    }

    void test_clear_empties()
    {
        StringCache cache;
        put(cache, {1, 0}, "x");
        cache.clear();
        expect(get(cache, {1, 0}) == "<miss>", "clear removes every entry");
        expect(cache.size() == 0, "clear resets the size");
    }

    void test_hash_is_stable_and_sensitive()
    {
        const char* a = "depth_resolve";
        const char* b = "depth_resolvf";  // one byte different
        expect(hashBytes(a, 13) == hashBytes(a, 13), "hashing is deterministic");
        expect(hashBytes(a, 13) != hashBytes(b, 13), "a one-byte change changes the hash");
        expect(hashBytes(a, 5) != hashBytes(a, 13), "a length change changes the hash");
        expect(hashBytes("", 0) == hashBytes("", 0), "the empty input hashes consistently");
        expect(hashBytes(a, 13, 1) != hashBytes(a, 13, 2), "the seed changes the digest");
    }

    void test_concurrent_store_and_lookup()
    {
        // capacity is large enough that nothing is evicted, so every stored key
        // must still be retrievable after all threads finish
        CompileCache<int> cache(1u << 20);

        const int kThreads = 8;
        const int kPerThread = 4000;
        std::atomic<bool> bad{false};

        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t)
        {
            workers.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i)
                {
                    const uint64_t h = static_cast<uint64_t>(t) * kPerThread + i;
                    cache.store({h, 0}, std::make_shared<const int>(static_cast<int>(h)));
                }
            });
        }
        for (auto& w : workers)
            w.join();

        for (int t = 0; t < kThreads; ++t)
        {
            for (int i = 0; i < kPerThread; ++i)
            {
                const uint64_t h = static_cast<uint64_t>(t) * kPerThread + i;
                const auto hit = cache.lookup({h, 0});
                if (!hit || *hit != static_cast<int>(h))
                    bad.store(true);
            }
        }

        expect(!bad.load(), "every concurrently stored entry is retrievable");
        expect(cache.size() == static_cast<size_t>(kThreads) * kPerThread,
               "no concurrent store was lost or duplicated");
    }

    // ---- disk level ----

    reshadefx::module makeModule(uint32_t seed)
    {
        reshadefx::module m;
        m.hlsl = "// hlsl " + std::to_string(seed);
        m.spirv = {0x07230203u, 0x00010500u, seed, 0xdeadbeefu, 0u, 0xffffffffu};

        reshadefx::entry_point entry;
        entry.name = "PS_Main";
        entry.stage = reshadefx::entry_point_stage::compute;
        entry.local_size_x = 8;
        entry.local_size_y = 4;
        m.entry_points.push_back(entry);

        reshadefx::texture_info tex{};
        tex.id = 3;
        tex.semantic = "COLOR";
        tex.unique_name = "texColor";
        tex.width = 1920;
        tex.height = 1080;
        tex.levels = 5;
        tex.format = reshadefx::texture_format::rgba16f;
        reshadefx::annotation note{};
        note.type.base = reshadefx::type::t_string;
        note.name = "source";
        note.value.string_data = "noise.png";
        note.value.array_data.resize(2);
        note.value.array_data[1].as_float[3] = 2.5f;
        tex.annotations.push_back(note);
        m.textures.push_back(tex);

        reshadefx::sampler_info smp;
        smp.unique_name = "sampColor";
        smp.texture_name = "texColor";
        smp.filter = reshadefx::texture_filter::min_mag_mip_point;
        smp.address_u = reshadefx::texture_address_mode::wrap;
        smp.lod_bias = -0.75f;
        smp.srgb = 1;
        smp.storage = true;
        m.samplers.push_back(smp);

        reshadefx::uniform_info uni{};
        uni.name = "Strength";
        uni.type.base = reshadefx::type::t_float;
        uni.type.rows = 1;
        uni.type.cols = 1;
        uni.type.array_length = -1;
        uni.size = 4;
        uni.offset = 16;
        uni.has_initializer_value = true;
        uni.initializer_value.as_float[0] = 0.5f;
        m.uniforms.push_back(uni);

        reshadefx::technique_info tech;
        tech.name = "Main";
        reshadefx::pass_info pass;
        pass.vs_entry_point = "VS";
        pass.ps_entry_point = "PS";
        pass.render_target_names[0] = "texColor";
        pass.render_target_names[7] = "other";
        pass.blend_enable = 1;
        pass.src_blend = reshadefx::pass_blend_func::src_alpha;
        pass.stencil_reference_value = 7;
        pass.topology = reshadefx::primitive_topology::line_strip;
        pass.viewport_width = 640;
        tech.passes.push_back(pass);
        tech.passes.push_back(pass);
        m.techniques.push_back(tech);

        m.total_uniform_size = 32;
        m.num_sampler_bindings = 1;
        m.num_texture_bindings = 1;
        return m;
    }

    std::string tempDirectory()
    {
        const auto base = std::filesystem::temp_directory_path() /
                          ("vkhook-shader-cache-test-" + std::to_string(static_cast<long>(getpid())));
        std::filesystem::remove_all(base);
        std::filesystem::create_directories(base);
        return base.string();
    }

    std::vector<uint8_t> readBytes(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    void writeBytes(const std::string& path, const std::vector<uint8_t>& bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    void test_module_round_trips()
    {
        const reshadefx::module original = makeModule(1);
        const auto              bytes = VkHook::serializeReshadeModule(original);
        reshadefx::module       decoded;
        expect(VkHook::deserializeReshadeModule(bytes.data(), bytes.size(), decoded), "a serialized module decodes");
        expect(VkHook::serializeReshadeModule(decoded) == bytes, "decoding then encoding reproduces the bytes");
        expect(decoded.spirv == original.spirv && decoded.hlsl == original.hlsl, "spirv and hlsl survive");
        expect(decoded.textures.size() == 1 && decoded.textures[0].annotations.size() == 1 &&
                   decoded.textures[0].annotations[0].value.array_data.size() == 2 &&
                   decoded.textures[0].annotations[0].value.array_data[1].as_float[3] == 2.5f,
               "nested annotation constants survive");
        expect(decoded.techniques.size() == 1 && decoded.techniques[0].passes.size() == 2 &&
                   decoded.techniques[0].passes[1].render_target_names[7] == "other",
               "passes survive");
        expect(decoded.samplers.size() == 1 && decoded.samplers[0].storage && decoded.samplers[0].lod_bias == -0.75f,
               "sampler state survives");
        expect(decoded.uniforms.size() == 1 && decoded.uniforms[0].type.array_length == -1, "negative array length survives");
    }

    void test_module_decode_rejects_damage()
    {
        const auto        bytes = VkHook::serializeReshadeModule(makeModule(2));
        reshadefx::module scratch;

        bool anyTruncatedAccepted = false;
        for (size_t length = 0; length < bytes.size(); ++length)
            anyTruncatedAccepted |= VkHook::deserializeReshadeModule(bytes.data(), length, scratch);
        expect(!anyTruncatedAccepted, "no truncation of a module decodes");

        auto trailing = bytes;
        trailing.push_back(0);
        expect(!VkHook::deserializeReshadeModule(trailing.data(), trailing.size(), scratch), "trailing bytes are rejected");

        // A length field claiming far more than the buffer holds must not allocate.
        std::vector<uint8_t> huge(bytes.size(), 0xff);
        expect(!VkHook::deserializeReshadeModule(huge.data(), huge.size(), scratch), "all-ones garbage is rejected");

        auto badMagic = bytes;
        // hlsl string is first; spirv count follows it. Flip the SPIR-V magic word.
        const size_t spirvWordOffset = 4 + makeModule(2).hlsl.size() + 4;
        badMagic[spirvWordOffset] ^= 0xff;
        expect(!VkHook::deserializeReshadeModule(badMagic.data(), badMagic.size(), scratch), "a non-SPIR-V blob is rejected");
    }

    struct Fixture
    {
        std::string dir = tempDirectory();
        PersistenceQueue queue;
        std::shared_ptr<std::atomic<int>> writes = std::make_shared<std::atomic<int>>(0);

        std::unique_ptr<ShaderCache> make(const char* tag = "tag-1", size_t capacity = 8)
        {
            return std::make_unique<ShaderCache>(
                capacity, dir, tag,
                [this](const std::string& path, std::function<bool()> job) {
                    writes->fetch_add(1);
                    queue.other(path, std::move(job));
                });
        }

        ~Fixture()
        {
            queue.shutdown();
            std::filesystem::remove_all(dir);
        }
    };

    ShaderCacheKey keyFor(const char* source, uint64_t macros = 0, uint32_t flags = 5)
    {
        return VkHook::makeShaderCacheKey(source, macros, flags);
    }

    void test_disk_round_trip_across_instances()
    {
        Fixture f;
        const ShaderCacheKey key = keyFor("float4 main() { return 1; }");
        {
            auto cache = f.make();
            expect(cache->lookup(key) == nullptr, "a cold cache misses");
            cache->store(key, std::make_shared<const reshadefx::module>(makeModule(3)));
            f.queue.waitForIdle();
        }
        expect(std::filesystem::exists(f.make()->entryPath(key)), "the entry was written under the cache directory");

        auto fresh = f.make();
        const auto hit = fresh->lookup(key);
        expect(hit != nullptr, "a new process (fresh memory level) hits on disk");
        expect(hit && VkHook::serializeReshadeModule(*hit) == VkHook::serializeReshadeModule(makeModule(3)),
               "the disk hit equals the stored module");
        expect(fresh->lookup(key).get() == hit.get(), "the disk hit is promoted to memory and then shared");

        const int before = f.writes->load();
        fresh->store(key, hit);
        expect(f.writes->load() == before, "a module that is already cached is not written again");
    }

    void test_key_parts_partition_the_disk_entries()
    {
        Fixture f;
        auto cache = f.make();
        cache->store(keyFor("src", 1, 5), std::make_shared<const reshadefx::module>(makeModule(10)));
        f.queue.waitForIdle();

        auto fresh = f.make();
        expect(fresh->lookup(keyFor("src", 1, 5)) != nullptr, "the exact key hits");
        expect(fresh->lookup(keyFor("src", 2, 5)) == nullptr, "different macros miss");
        expect(fresh->lookup(keyFor("src", 1, 4)) == nullptr, "different codegen flags miss");
        expect(fresh->lookup(keyFor("srd", 1, 5)) == nullptr, "different source misses");
    }

    void test_compiler_tag_retires_entries()
    {
        Fixture f;
        const ShaderCacheKey key = keyFor("src");
        f.make("compiler-1")->store(key, std::make_shared<const reshadefx::module>(makeModule(4)));
        f.queue.waitForIdle();
        expect(f.make("compiler-1")->lookup(key) != nullptr, "the same compiler tag hits");
        expect(f.make("compiler-2")->lookup(key) == nullptr, "another compiler tag misses");
    }

    void test_corrupt_entries_are_misses()
    {
        Fixture f;
        const ShaderCacheKey key = keyFor("float4 main() { return 2; }");
        f.make()->store(key, std::make_shared<const reshadefx::module>(makeModule(5)));
        f.queue.waitForIdle();
        const std::string path = f.make()->entryPath(key);
        const auto        good = readBytes(path);
        expect(!good.empty(), "the entry file exists to damage");

        bool truncatedHit = false;
        for (size_t length = 0; length < good.size(); length += (length < 80 ? 1 : 7))
        {
            writeBytes(path, std::vector<uint8_t>(good.begin(), good.begin() + length));
            truncatedHit |= f.make()->lookup(key) != nullptr;
        }
        expect(!truncatedHit, "no truncated entry is accepted");

        bool flippedHit = false;
        for (size_t at = 0; at < good.size(); at += 3)
        {
            auto damaged = good;
            damaged[at] ^= 0x5a;
            writeBytes(path, damaged);
            flippedHit |= f.make()->lookup(key) != nullptr;
        }
        expect(!flippedHit, "no entry with a flipped byte is accepted");

        auto appended = good;
        appended.push_back(0);
        writeBytes(path, appended);
        expect(f.make()->lookup(key) == nullptr, "an entry with trailing bytes is a miss");

        writeBytes(path, std::vector<uint8_t>(good.size(), 0xff));
        expect(f.make()->lookup(key) == nullptr, "garbage of the right size is a miss");

        writeBytes(path, {});
        expect(f.make()->lookup(key) == nullptr, "an empty file is a miss");

        std::filesystem::remove(path);
        std::filesystem::create_directory(path);
        expect(f.make()->lookup(key) == nullptr, "a directory where the file should be is a miss");
        std::filesystem::remove(path);

        // The next compile overwrites the damage and the cache works again.
        writeBytes(path, std::vector<uint8_t>(good.begin(), good.begin() + good.size() / 2));
        auto repairing = f.make();
        expect(repairing->lookup(key) == nullptr, "a half-written entry misses");
        repairing->store(key, std::make_shared<const reshadefx::module>(makeModule(5)));
        f.queue.waitForIdle();
        expect(f.make()->lookup(key) != nullptr, "storing after a corrupt miss repairs the entry");
    }

    void test_entry_with_foreign_version_is_a_miss()
    {
        Fixture f;
        const ShaderCacheKey key = keyFor("versioned");
        f.make()->store(key, std::make_shared<const reshadefx::module>(makeModule(6)));
        f.queue.waitForIdle();
        const std::string path = f.make()->entryPath(key);
        auto              bytes = readBytes(path);
        bytes[4] += 1; // format version word
        writeBytes(path, bytes);
        expect(f.make()->lookup(key) == nullptr, "an entry from another format version is a miss");
    }

    void test_unwritable_directory_does_not_break_the_cache()
    {
        Fixture f;
        // A regular file where the directory should be: create_directories fails.
        std::filesystem::remove_all(f.dir);
        writeBytes(f.dir, {1, 2, 3});
        auto cache = f.make();
        const ShaderCacheKey key = keyFor("nowhere");
        cache->store(key, std::make_shared<const reshadefx::module>(makeModule(7)));
        f.queue.waitForIdle();
        expect(cache->lookup(key) != nullptr, "the memory level still serves after a failed disk write");
        std::filesystem::remove(f.dir);
    }

    void test_no_directory_disables_disk()
    {
        ShaderCache cache(4, "", "tag", [](const std::string&, std::function<bool()>) { expect(false, "no write is scheduled without a directory"); });
        const ShaderCacheKey key = keyFor("memory-only");
        cache.store(key, std::make_shared<const reshadefx::module>(makeModule(8)));
        expect(cache.lookup(key) != nullptr, "memory-only cache serves");
        expect(cache.entryPath(key).empty(), "memory-only cache has no entry path");
    }

    void test_cache_directory_follows_xdg()
    {
        setenv("XDG_CACHE_HOME", "/tmp/vkhook-xdg-cache", 1);
        expect(VkHook::defaultShaderCacheDirectory() == "/tmp/vkhook-xdg-cache/VkHook/shaders", "XDG_CACHE_HOME wins");
        unsetenv("XDG_CACHE_HOME");
        setenv("HOME", "/tmp/vkhook-home", 1);
        expect(VkHook::defaultShaderCacheDirectory() == "/tmp/vkhook-home/.cache/VkHook/shaders", "~/.cache is the fallback");
    }
} // namespace

int main()
{
    test_empty_cache_misses();
    test_hit_shares_instead_of_copying();
    test_evicted_entry_stays_alive_for_holders();
    test_store_then_lookup_round_trips();
    test_flags_partition_the_key();
    test_source_partitions_the_key();
    test_restore_does_not_duplicate();
    test_fifo_bound_evicts_oldest();
    test_clear_empties();
    test_hash_is_stable_and_sensitive();
    test_concurrent_store_and_lookup();
    test_module_round_trips();
    test_module_decode_rejects_damage();
    test_disk_round_trip_across_instances();
    test_key_parts_partition_the_disk_entries();
    test_compiler_tag_retires_entries();
    test_corrupt_entries_are_misses();
    test_entry_with_foreign_version_is_a_miss();
    test_unwritable_directory_does_not_break_the_cache();
    test_no_directory_disables_disk();
    test_cache_directory_follows_xdg();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
