#ifndef SHADER_CATALOG_HPP_INCLUDED
#define SHADER_CATALOG_HPP_INCLUDED

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace VkHook
{
    // The index of installed `.fx` files: file name / stem / relative path ->
    // path. It replaces the recursive directory walk every effect lookup used to
    // do (a miss walked every shader pack on disk, once per frame for a name that
    // does not exist).
    //
    // Lifetime: owned by one EffectRegistry, so one per device. A scan reads the
    // directory trees, so it runs with no lock held and is published as an
    // immutable index; find() only copies a shared_ptr under the mutex.
    //
    // Precedence: roots are searched in the order given, and within a root the
    // first file found in directory order wins, for each of the three keys. A
    // name that is a relative path ("Pack/Shaders/Foo.fx") is matched against
    // each root before the file-name and stem keys.
    class ShaderCatalog
    {
    public:
        // Replaces the search roots. Rescans when the list differs from the
        // current one; otherwise does nothing. Does file I/O: call with no lock held.
        void setRoots(std::vector<std::string> roots);

        // Rescans the current roots, but at most once per `minInterval`
        // milliseconds unless `force`. Returns true when a scan ran. Used when a
        // lookup missed: a shader copied into an existing root shows up without
        // the miss path walking the disk on every frame.
        bool rescan(bool force = false, int64_t minIntervalMs = 2000);

        // The path of the shader named by `name` (file name with or without
        // ".fx", stem, or a path relative to a root), or "". Case-insensitive.
        std::string find(std::string_view name) const;

        size_t   size() const;
        uint64_t generation() const;

    private:
        struct Index
        {
            std::unordered_map<std::string, std::string> byRelative;  // lowercased "dir/File.fx" under its root
            std::unordered_map<std::string, std::string> byFile;      // lowercased "file.fx"
            std::unordered_map<std::string, std::string> byStem;      // lowercased "file"
            size_t                                       files = 0;
        };

        static std::shared_ptr<const Index> scan(const std::vector<std::string>& roots);
        void                                publish(std::shared_ptr<const Index> index);

        mutable std::mutex                  mutex_;
        std::vector<std::string>            roots_;
        std::shared_ptr<const Index>        index_ = std::make_shared<const Index>();
        int64_t                             lastScanMs_ = -1;
        uint64_t                            generation_ = 0;
    };
} // namespace VkHook

#endif // SHADER_CATALOG_HPP_INCLUDED
