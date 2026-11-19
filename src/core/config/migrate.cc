#include "migrate.hh"

#include "config_serializer.hh"
#include "ini.hh"
#include "logger.hh"
#include "paths.hh"
#include "persist.hh"
#include "settings_schema.hh"
#include "store.hh"
#include "util.hh"

#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace VkHook::Migrate
{
    namespace
    {
        namespace fs = std::filesystem;

        // Effect packages used to install under reshade/packages/. Merges that
        // tree into reshade/ keeping any file already at the destination so a
        // newer install is never clobbered, then drops the legacy tree only if
        // every file made it across. An incomplete copy fails the step, which
        // is retried on the next start.
        bool mergeReshadePackages(const fs::path& baseDir)
        {
            const fs::path reshade = baseDir / "reshade";
            const fs::path legacy = reshade / "packages";

            std::error_code ec;
            if (!fs::is_directory(legacy, ec) || ec)
                return true;

            bool complete = true;
            fs::recursive_directory_iterator it(legacy, fs::directory_options::skip_permission_denied, ec);
            const fs::recursive_directory_iterator end;
            for (; !ec && it != end; it.increment(ec))
            {
                std::error_code entryError;
                if (it->is_symlink(entryError) || !it->is_regular_file(entryError) || entryError)
                    continue;

                std::error_code relError;
                const auto relative = fs::relative(it->path(), legacy, relError);
                if (relError || relative.empty())
                {
                    complete = false;
                    continue;
                }

                const auto dest = reshade / relative;
                fs::create_directories(dest.parent_path(), entryError);
                if (entryError)
                {
                    complete = false;
                    continue;
                }
                if (fs::exists(dest, entryError))
                    continue;  // the new layout wins

                fs::copy_file(it->path(), dest, fs::copy_options::none, entryError);
                if (entryError)
                    complete = false;
            }

            if (!complete)
            {
                Logger::warn("VkHook: reshade/packages migration incomplete; leaving it in place");
                return false;
            }

            std::error_code removeError;
            fs::remove_all(legacy, removeError);
            if (removeError)
                Logger::warn("VkHook: could not remove the legacy reshade/packages directory");
            else
                Logger::info("VkHook: migrated reshade/packages into reshade/");
            return true;
        }

        // The experimental <game>@default.conf naming moved to <game>.conf.
        // A <game>.conf that already exists wins and the old file stays.
        bool renameAtDefaultConfigs(const fs::path& baseDir)
        {
            const fs::path configs = baseDir / "configs";
            std::error_code ec;
            if (!fs::is_directory(configs, ec) || ec)
                return true;

            constexpr std::string_view suffix = "@default.conf";
            bool ok = true;
            for (fs::directory_iterator it(configs, ec), end; !ec && it != end; it.increment(ec))
            {
                const std::string name = it->path().filename().string();
                if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
                    continue;
                const fs::path target = configs / (name.substr(0, name.size() - suffix.size()) + ".conf");
                std::error_code entryError;
                if (fs::exists(target, entryError))
                    continue;
                fs::rename(it->path(), target, entryError);
                if (entryError)
                {
                    Logger::err("Could not rename game config " + it->path().string() + " to " + target.string());
                    ok = false;
                }
                else
                    Logger::info("Renamed game config " + it->path().string() + " to " + target.string());
            }
            return ok;
        }

        // Older setup versions wrote every discovered shader or texture path on
        // one comma-separated line. The reader takes one path per line now, so
        // the lines are split once here.
        bool splitShaderManagerLists(const fs::path& baseDir)
        {
            const fs::path path = baseDir / "shader_manager.conf";
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return true;
            const std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

            std::string out;
            bool changed = false;
            size_t pos = 0;
            while (pos < existing.size())
            {
                size_t eol = existing.find('\n', pos);
                const bool hasNewline = eol != std::string::npos;
                if (!hasNewline)
                    eol = existing.size();
                const std::string line = existing.substr(pos, eol - pos);
                pos = hasNewline ? eol + 1 : eol;

                const Ini::Line parsed = Ini::tokenize(line);
                const bool list = parsed.kind == Ini::LineKind::Pair
                    && (parsed.key == "shaderPath" || parsed.key == "texturePath")
                    && parsed.value.find(',') != std::string::npos;
                if (!list)
                {
                    out += line;
                    if (hasNewline)
                        out += '\n';
                    continue;
                }
                changed = true;
                const auto items = Ini::splitList(parsed.value, ',');
                for (size_t i = 0; i < items.size(); ++i)
                {
                    out += parsed.key + " = " + Ini::formatValue(items[i]);
                    if (i + 1 < items.size() || hasNewline)
                        out += '\n';
                }
            }
            if (!changed)
                return true;
            if (!writeAtomically(path.string(), out))
            {
                Logger::err("Could not rewrite shader_manager.conf: " + path.string());
                return false;
            }
            Logger::info("VkHook: split comma-separated paths in shader_manager.conf");
            return true;
        }

        bool toVersion1(const std::string& baseDir)
        {
            const fs::path base(baseDir);
            // no short-circuit: a failing step must not keep the others from running
            const bool packages = mergeReshadePackages(base);
            const bool renames = renameAtDefaultConfigs(base);
            const bool lists = splitShaderManagerLists(base);
            return packages && renames && lists;
        }

        // Removes `key` and the "# key: ..." comment the schema writes above it
        // from the user's VkHook.conf (including the "#   N = ..." option lines
        // the schema writes below a "# key mode selection:" header); every
        // other line stays as the user wrote it.
        bool dropSettingKey(const fs::path& file, std::string_view key)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return true;
            const std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

            const std::string commentPrefix = "# " + std::string(key);
            bool inOptionList = false;
            std::string out;
            bool changed = false;
            size_t pos = 0;
            while (pos < existing.size())
            {
                size_t eol = existing.find('\n', pos);
                const bool hasNewline = eol != std::string::npos;
                if (!hasNewline)
                    eol = existing.size();
                const std::string line = existing.substr(pos, eol - pos);
                pos = hasNewline ? eol + 1 : eol;

                const Ini::Line parsed = Ini::tokenize(line);
                const bool header = parsed.kind == Ini::LineKind::Comment
                    && line.compare(0, commentPrefix.size(), commentPrefix) == 0
                    && (line.size() == commentPrefix.size() || line[commentPrefix.size()] == ':'
                        || line[commentPrefix.size()] == ' ');
                const bool option = inOptionList && line.size() > 5 && line.compare(0, 4, "#   ") == 0
                    && line[4] >= '0' && line[4] <= '9';
                const bool drop = (parsed.kind == Ini::LineKind::Pair && parsed.key == key) || header || option;
                inOptionList = header || option;
                if (drop)
                {
                    changed = true;
                    continue;
                }
                out += line;
                if (hasNewline)
                    out += '\n';
            }
            if (!changed)
                return true;
            if (!writeAtomically(file.string(), out))
            {
                Logger::err("Could not rewrite " + file.string());
                return false;
            }
            return true;
        }

        // depthResolveMode = 2 (average) becomes 0 (average); the line's comment
        // and every other line stay as the user wrote them.
        bool remapDepthResolveMode(const fs::path& file)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return true;
            const std::string existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();

            std::string out;
            bool changed = false;
            size_t pos = 0;
            while (pos < existing.size())
            {
                size_t eol = existing.find('\n', pos);
                const bool hasNewline = eol != std::string::npos;
                if (!hasNewline)
                    eol = existing.size();
                std::string line = existing.substr(pos, eol - pos);
                pos = hasNewline ? eol + 1 : eol;

                const Ini::Line parsed = Ini::tokenize(line);
                if (parsed.kind == Ini::LineKind::Pair && parsed.key == "depthResolveMode" && parsed.value == "2")
                {
                    changed = true;
                    const bool crlf = !line.empty() && line.back() == '\r';
                    line = "depthResolveMode = 0";
                    if (crlf)
                        line += '\r';
                }
                out += line;
                if (hasNewline)
                    out += '\n';
            }
            if (!changed)
                return true;
            if (!writeAtomically(file.string(), out))
            {
                Logger::err("Could not rewrite " + file.string());
                return false;
            }
            return true;
        }

        // depthRebuildMode chose between idling the queue and fence-gated depth
        // rebuilds. Rebuilds never idle the queue any more, so the key is dead.
        bool toVersion2(const std::string& baseDir)
        {
            return dropSettingKey(fs::path(baseDir) / "VkHook.conf", "depthRebuildMode");
        }

        // The depth capture no longer reads depthSourceChannel or
        // depthCaptureMethod, and depthResolveMode is now 0 = average,
        // 1 = sample zero (the old 2 = average is the same as 0).
        bool toVersion3(const std::string& baseDir)
        {
            const fs::path file = fs::path(baseDir) / "VkHook.conf";
            const bool channel = dropSettingKey(file, "depthSourceChannel");
            const bool method = dropSettingKey(file, "depthCaptureMethod");
            const bool resolve = remapDepthResolveMode(file);
            return channel && method && resolve;
        }

        constexpr std::array<Step, 3> kSteps = {{
            {1, "legacy config layouts", toVersion1},
            {2, "drop depthRebuildMode", toVersion2},
            {3, "drop depth source settings, collapse depthResolveMode", toVersion3},
        }};
    } // namespace

    std::span<const Step> steps()
    {
        return kSteps;
    }

    int storedVersion(const std::string& baseDir)
    {
        std::vector<std::pair<std::string, std::string>> entries;
        if (baseDir.empty() || !Ini::readFile(baseDir + "/VkHook.conf", entries))
            return 0;
        for (const auto& [key, value] : entries)
        {
            int version = 0;
            const char* end = value.data() + value.size();
            if (key == Schema::kVersionKey && std::from_chars(value.data(), end, version).ptr == end && version > 0)
                return version;
        }
        return 0;
    }

    int run(const std::string& baseDir)
    {
        return run(baseDir, steps());
    }

    int run(const std::string& baseDir, std::span<const Step> steps)
    {
        if (baseDir.empty())
            return 0;
        int version = storedVersion(baseDir);
        for (const Step& step : steps)
        {
            if (step.to <= version)
                continue;
            Logger::info(std::string("config migration to v") + std::to_string(step.to) + ": " + step.name);
            if (!step.run(baseDir))
            {
                Logger::err(std::string("config migration failed: ") + step.name);
                return version;
            }
            if (!ConfigStore::writeSchemaVersion(step.to))
                return version;
            version = step.to;
        }
        return version;
    }
} // namespace VkHook::Migrate
