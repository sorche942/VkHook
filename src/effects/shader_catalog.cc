#include "shader_catalog.hh"

#include <chrono>
#include <filesystem>

#include "effect_id.hh"

namespace VkHook
{
    namespace
    {
        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count();
        }
    } // namespace

    std::shared_ptr<const ShaderCatalog::Index> ShaderCatalog::scan(const std::vector<std::string>& roots)
    {
        namespace fs = std::filesystem;
        auto index = std::make_shared<Index>();

        for (const std::string& root : roots)
        {
            std::error_code ec;
            const fs::path  base(root);
            if (!fs::is_directory(base, ec))
                continue;
            try
            {
                fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec);
                for (const fs::recursive_directory_iterator end; it != end; it.increment(ec))
                {
                    if (ec)
                        break;
                    std::error_code fileEc;
                    if (!it->is_regular_file(fileEc))
                        continue;
                    const fs::path& file = it->path();
                    if (lowerAscii(file.extension().string()) != ".fx")
                        continue;

                    const std::string path = file.string();
                    ++index->files;
                    index->byRelative.emplace(lowerAscii(fs::relative(file, base, fileEc).generic_string()), path);
                    index->byFile.emplace(lowerAscii(file.filename().string()), path);
                    index->byStem.emplace(lowerAscii(file.stem().string()), path);
                }
            }
            catch (const fs::filesystem_error&)
            {
                // an unreadable root must not hide the roots after it
            }
        }
        return index;
    }

    void ShaderCatalog::publish(std::shared_ptr<const Index> index)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        index_       = std::move(index);
        lastScanMs_  = nowMs();
        ++generation_;
    }

    void ShaderCatalog::setRoots(std::vector<std::string> roots)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (roots == roots_ && lastScanMs_ >= 0)
                return;
            roots_ = roots;
        }
        publish(scan(roots));
    }

    bool ShaderCatalog::rescan(bool force, int64_t minIntervalMs)
    {
        std::vector<std::string> roots;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!force && lastScanMs_ >= 0 && nowMs() - lastScanMs_ < minIntervalMs)
                return false;
            roots = roots_;
        }
        publish(scan(roots));
        return true;
    }

    std::string ShaderCatalog::find(std::string_view name) const
    {
        if (name.empty())
            return {};

        std::shared_ptr<const Index> index;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            index = index_;
        }

        const std::filesystem::path query(name);
        const std::string           lowerName = lowerAscii(query.generic_string());
        if (query.has_parent_path())
        {
            if (auto hit = index->byRelative.find(lowerName); hit != index->byRelative.end())
                return hit->second;
            if (query.extension().empty())
                if (auto hit = index->byRelative.find(lowerName + ".fx"); hit != index->byRelative.end())
                    return hit->second;
        }

        const std::string fileName = lowerAscii(query.filename().string());
        if (auto hit = index->byFile.find(fileName); hit != index->byFile.end())
            return hit->second;
        if (query.extension().empty())
            if (auto hit = index->byFile.find(fileName + ".fx"); hit != index->byFile.end())
                return hit->second;
        // a stem query: "Foo", or a name whose "extension" is not .fx ("Foo.2")
        if (auto hit = index->byStem.find(lowerAscii(query.stem().string())); hit != index->byStem.end())
            return hit->second;
        if (auto hit = index->byStem.find(fileName); hit != index->byStem.end())
            return hit->second;
        return {};
    }

    size_t ShaderCatalog::size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return index_->files;
    }

    uint64_t ShaderCatalog::generation() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }
} // namespace VkHook
