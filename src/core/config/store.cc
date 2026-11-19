#include "store.hh"

#include "ini.hh"
#include "logger.hh"
#include "migrate.hh"
#include "paths.hh"
#include "persist.hh"
#include "store.hh"
#include "util.hh"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace VkHook
{
    namespace
    {
        ConfigStore g_store;
    }

    ConfigStore& ConfigStore::instance()
    {
        return g_store;
    }

    namespace
    {
        // The schema version this process verified or reached at initialize().
        // Zero until then, so a save that runs before migrations (tests, tools)
        // never claims a layout it has not checked.
        std::atomic<int> g_reachedVersion{0};

        Schema::Line versionLine(int version)
        {
            return {"Schema", "# schemaVersion: the file layout generation; VkHook upgrades older layouts once\n",
                    Schema::kVersionKey, std::to_string(version)};
        }

        std::string slurp(const std::string& path)
        {
            std::ifstream file(path, std::ios::binary);
            return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }

        // Rewrites the user's VkHook.conf with `owned` merged in. The text to
        // merge into comes from the user's file or, when they have none, from
        // the system-wide one, so a first save never shadows it with a file
        // that lacks its keys.
        bool rewriteUserFile(std::vector<Schema::Line> owned, bool onlyWhenFileExists = false)
        {
            const std::string baseDir = Paths::baseConfigDir();
            if (baseDir.empty())
            {
                Logger::err("Could not determine config directory");
                return false;
            }
            std::error_code ec;
            std::filesystem::create_directories(baseDir, ec);

            const std::string configPath = Paths::userSettingsFile();
            const bool userFileExists = std::filesystem::exists(configPath, ec);
            const std::string loadedFrom = userFileExists ? configPath : Paths::findSettingsFile();
            // a system-wide VkHook.conf stays the user's source of defaults until
            // they save something of their own; a version stamp alone must not
            // shadow it with a copy
            if (onlyWhenFileExists && !userFileExists && !loadedFrom.empty())
                return true;
            const std::string existing = loadedFrom.empty() ? std::string() : slurp(loadedFrom);
            const std::string text = loadedFrom.empty() ? Schema::fresh(owned) : Schema::merge(existing, owned);

            if (userFileExists && text == existing)
                return true;  // nothing changed: leave the file and its mtime alone

            if (!writeAtomically(configPath, text))
            {
                Logger::err("Could not write VkHook.conf: " + configPath);
                return false;
            }
            return true;
        }
    } // namespace

    AppSettings ConfigStore::loadFile()
    {
        AppSettings settings;
        const std::string configPath = Paths::findSettingsFile();
        if (configPath.empty())
            return settings;

        Logger::info("ConfigStore loading from: " + configPath);
        std::vector<std::pair<std::string, std::string>> entries;
        if (Ini::readFile(configPath, entries))
            Schema::load(settings, entries);
        return settings;
    }

    bool ConfigStore::writeFile(const AppSettings& settings)
    {
        std::vector<Schema::Line> owned = Schema::lines(settings);
        if (const int version = g_reachedVersion.load(); version > 0)
            owned.push_back(versionLine(version));
        return rewriteUserFile(std::move(owned));
    }

    bool ConfigStore::writeSchemaVersion(int version)
    {
        return rewriteUserFile({versionLine(version)}, true);
    }

    void ConfigStore::initialize()
    {
        std::call_once(initOnce, [this] {
            // migrations touch files, so they run on the writer thread like every other mutation
            PersistenceQueue::instance()
                .other("migrate", [] {
                    const int reached = Migrate::run(Paths::baseConfigDir());
                    g_reachedVersion.store(reached);
                    return reached == Schema::kCurrentVersion;
                })
                .get();
            AppSettings loaded = loadFile();
            {
                std::lock_guard<std::mutex> lock(mutex);
                settings = std::move(loaded);
                publishLocked();
            }
            initialized.store(true, std::memory_order_release);
            Logger::info("ConfigStore initialized");
        });
    }

    void ConfigStore::reload()
    {
        AppSettings loaded = loadFile();
        std::lock_guard<std::mutex> lock(mutex);
        settings = std::move(loaded);
        publishLocked();
    }

    std::future<bool> ConfigStore::save()
    {
        // The copy is taken under the lock; the write runs on the writer
        // thread, so no fsync ever happens while `mutex` is held.
        AppSettings copy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            copy = settings;
        }

        return PersistenceQueue::instance().write(Paths::userSettingsFile(), [copy = std::move(copy)] {
            const bool success = ConfigStore::writeFile(copy);
            if (success)
                Logger::debug("Settings saved to config");
            else
                Logger::err("Failed to save settings");
            return success;
        });
    }
} // namespace VkHook
