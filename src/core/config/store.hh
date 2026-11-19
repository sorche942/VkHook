#ifndef VKHOOK_CONFIG_STORE_HH_INCLUDED
#define VKHOOK_CONFIG_STORE_HH_INCLUDED

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "settings_schema.hh"

namespace VkHook
{
    // The one owner of VkHook.conf's settings.
    //
    // The schema (settings_schema.hh) is the single description of every key:
    // loading, saving, clamping and the overlay's settings controls all walk
    // that table. This class adds publication and persistence on top:
    //  - Hot threads (command recording, submit, present) call snapshot() once
    //    and read the immutable struct; no lock is taken.
    //  - set() clamps through the schema, so no caller clamps on its own, and
    //    publishes a fresh snapshot.
    //  - save() queues the file write on the PersistenceQueue; no file I/O ever
    //    happens under the mutex.
    class ConfigStore
    {
    public:
        // The process's store: there is one VkHook.conf. Lock-free to read.
        static ConfigStore& instance();

        // Runs pending migrations (migrate.hh) once, then loads VkHook.conf.
        void initialize();

        bool isInitialized() const { return initialized.load(std::memory_order_acquire); }

        using Snapshot = std::shared_ptr<const AppSettings>;
        Snapshot snapshot() const { return current.load(std::memory_order_acquire); }

        // Stores one setting by schema key and publishes. False for an unknown
        // key or a value of the wrong type.
        bool set(std::string_view key, const SettingValue& value)
        {
            const SettingDesc* desc = Schema::find(key);
            if (!desc)
                return false;
            std::lock_guard<std::mutex> lock(mutex);
            if (!Schema::set(settings, *desc, value))
                return false;
            publishLocked();
            return true;
        }

        // Re-reads VkHook.conf and publishes the result (the ReloadConfig command).
        void reload();

        // Queues VkHook.conf to be rewritten with the current settings. Repeated
        // saves coalesce to the latest values. The future reports whether the
        // write that finally covered these settings succeeded.
        std::future<bool> save();

        // The accessors the hot paths use, one per setting they read.
        std::string getToggleKey() const { return snapshot()->toggleKey; }
        std::string getReloadKey() const { return snapshot()->reloadKey; }
        std::string getOverlayKey() const { return snapshot()->overlayKey; }
        int getMaxEffects() const { return snapshot()->maxEffects; }
        bool getOverlayBlockInput() const { return snapshot()->overlayBlockInput; }
        bool getEnableOnLaunch() const { return snapshot()->enableOnLaunch; }
        bool getDepthCapture() const { return snapshot()->depthCapture; }
        int getDepthResolveMode() const { return snapshot()->depthResolveMode; }
        int getDepthCaptureMethod() const { return snapshot()->depthCaptureMethod; }
        int getDepthSourceChannel() const { return snapshot()->depthSourceChannel; }
        bool getDepthInvert() const { return snapshot()->depthInvert; }
        bool getDepthFlip() const { return snapshot()->depthFlip; }
        bool getDepthFlop() const { return snapshot()->depthFlop; }

        // The settings stored in the first VkHook.conf found (defaults when
        // there is none), parsed and clamped by the schema.
        static AppSettings loadFile();

        // Read-modify-write of the user's VkHook.conf: rewrites the keys the
        // schema owns, appends the ones the file lacks and leaves everything
        // else as the user wrote it. Callers off the main thread go through
        // save().
        static bool writeFile(const AppSettings& settings);

        // Stores `version` as schemaVersion in the user's VkHook.conf, seeding
        // the file from a system-wide one when the user has none yet.
        static bool writeSchemaVersion(int version);

    private:
        void publishLocked() { current.store(std::make_shared<const AppSettings>(settings), std::memory_order_release); }

        std::once_flag initOnce;
        mutable std::mutex mutex;  // guards `settings` and publication order
        AppSettings settings;
        std::atomic<std::shared_ptr<const AppSettings>> current{std::make_shared<const AppSettings>()};
        std::atomic<bool> initialized{false};
    };
} // namespace VkHook

#endif // VKHOOK_CONFIG_STORE_HH_INCLUDED
