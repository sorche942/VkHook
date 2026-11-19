#include "settings_manager.hh"
#include "logger.hh"
#include "paths.hh"
#include "persist.hh"

namespace VkHook
{
    // Global instance
    SettingsManager settingsManager;

    void SettingsManager::initialize()
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (initialized.load(std::memory_order_relaxed))
            return;

        settings = ConfigSerializer::loadSettings();
        publishLocked();
        initialized.store(true, std::memory_order_release);
        Logger::info("SettingsManager initialized");
    }

    std::future<bool> SettingsManager::save()
    {
        // The copy is taken under the lock; the write itself runs on the
        // writer thread, so no fsync ever happens while `mutex` is held.
        VkBasaltSettings copy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            copy = settings;
        }

        return PersistenceQueue::instance().write(Paths::userSettingsFile(), [copy = std::move(copy)] {
            const bool success = ConfigSerializer::saveSettings(copy);
            if (success)
                Logger::debug("Settings saved to config");
            else
                Logger::err("Failed to save settings");
            return success;
        });
    }

} // namespace VkHook
