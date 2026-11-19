#ifndef SETTINGS_MANAGER_HPP_INCLUDED
#define SETTINGS_MANAGER_HPP_INCLUDED

#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <memory>
#include <mutex>
#include <string>

#include "config_serializer.hh"
#include "default_config.hh"

namespace VkHook
{
    // Single source of truth for all VkHook settings.
    // Similar to EffectRegistry for effect parameters.
    //
    // Usage:
    // - Call initialize() once at startup to load from config
    // - Hot threads (command recording, submit, present) call snapshot() once
    //   and read the immutable struct; no lock is taken.
    // - The UI mutates through the setters and calls save(), which queues the
    //   disk write on the PersistenceQueue; no file I/O happens under a lock.
    class SettingsManager
    {
    public:
        // Initialize from VkHook.conf (call once at startup)
        void initialize();

        // Check if already initialized
        bool isInitialized() const { return initialized.load(std::memory_order_acquire); }

        // Immutable view of the current settings. Lock-free and safe from any
        // thread; every setter publishes a fresh copy.
        using Snapshot = std::shared_ptr<const VkBasaltSettings>;
        Snapshot snapshot() const { return current.load(std::memory_order_acquire); }

        // Queue VkHook.conf to be rewritten with the current settings. Repeated
        // saves coalesce to the latest values. The future reports whether the
        // write that finally covered these settings succeeded.
        std::future<bool> save();

        // Getters
        // The string getters return by value: callers on other threads would
        // otherwise hold a reference into a field a setter can reassign.
        int getMaxEffects() const
        {
            const Snapshot s = snapshot();
            return std::clamp(s->maxEffects, 1, 200);
        }
        bool getOverlayBlockInput() const
        {
            const Snapshot s = snapshot();
            return s->overlayBlockInput;
        }
        std::string getToggleKey() const
        {
            const Snapshot s = snapshot();
            return s->toggleKey;
        }
        std::string getReloadKey() const
        {
            const Snapshot s = snapshot();
            return s->reloadKey;
        }
        std::string getOverlayKey() const
        {
            const Snapshot s = snapshot();
            return s->overlayKey;
        }
        bool getUsePortalIntegration() const
        {
            const Snapshot s = snapshot();
            return s->usePortalIntegration;
        }
        float getUiScale() const
        {
            const Snapshot s = snapshot();
            return s->uiScale;
        }
        bool getEnableOnLaunch() const
        {
            const Snapshot s = snapshot();
            return s->enableOnLaunch;
        }
        bool getDepthCapture() const
        {
            const Snapshot s = snapshot();
            return s->depthCapture;
        }
        bool getAutoApply() const
        {
            const Snapshot s = snapshot();
            return s->autoApply;
        }
        int getAutoApplyDelay() const
        {
            const Snapshot s = snapshot();
            return s->autoApplyDelay;
        }
        bool getShowDebugWindow() const
        {
            const Snapshot s = snapshot();
            return s->showDebugWindow;
        }
        bool getSeparateEffectParams() const
        {
            const Snapshot s = snapshot();
            return s->separateEffectParams;
        }
        int getDepthResolveMode() const
        {
            const Snapshot s = snapshot();
            return s->depthResolveMode;
        }
        int getDepthRebuildMode() const
        {
            const Snapshot s = snapshot();
            return std::clamp(s->depthRebuildMode, 0, 1);
        }
        std::string getDepthManualPin() const
        {
            const Snapshot s = snapshot();
            return s->depthManualPin;
        }
        bool getDepthTransientWorkaround() const
        {
            const Snapshot s = snapshot();
            return s->depthTransientWorkaround;
        }
        int getDepthCaptureMethod() const
        {
            const Snapshot s = snapshot();
            return std::clamp(s->depthCaptureMethod, 0, 2);
        }
        int getDepthSourceChannel() const
        {
            const Snapshot s = snapshot();
            return std::clamp(s->depthSourceChannel, 0, 6);
        }
        bool getDepthInvert() const
        {
            const Snapshot s = snapshot();
            return s->depthInvert;
        }
        bool getDepthFlip() const
        {
            const Snapshot s = snapshot();
            return s->depthFlip;
        }
        bool getDepthFlop() const
        {
            const Snapshot s = snapshot();
            return s->depthFlop;
        }

        // Setters (update in-memory state, call save() to persist)
        void setMaxEffects(int value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.maxEffects = std::clamp(value, 1, 200);
            publishLocked();
        }
        void setOverlayBlockInput(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.overlayBlockInput = value;
            publishLocked();
        }
        void setToggleKey(const std::string& value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.toggleKey = value;
            publishLocked();
        }
        void setReloadKey(const std::string& value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.reloadKey = value;
            publishLocked();
        }
        void setOverlayKey(const std::string& value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.overlayKey = value;
            publishLocked();
        }
        void setUsePortalIntegration(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.usePortalIntegration = value;
            publishLocked();
        }
        void setUiScale(float value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.uiScale = std::isfinite(value) && value > 0.0f ? std::clamp(value, 0.75f, 3.0f) : 0.0f;
            publishLocked();
        }
        void setEnableOnLaunch(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.enableOnLaunch = value;
            publishLocked();
        }
        void setDepthCapture(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthCapture = value;
            publishLocked();
        }
        void setAutoApply(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.autoApply = value;
            publishLocked();
        }
        void setAutoApplyDelay(int value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.autoApplyDelay = value;
            publishLocked();
        }
        void setShowDebugWindow(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.showDebugWindow = value;
            publishLocked();
        }
        void setSeparateEffectParams(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.separateEffectParams = value;
            publishLocked();
        }
        void setDepthResolveMode(int value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthResolveMode = value;
            publishLocked();
        }
        void setDepthRebuildMode(int value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthRebuildMode = std::clamp(value, 0, 1);
            publishLocked();
        }
        void setDepthManualPin(const std::string& value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthManualPin = value;
            publishLocked();
        }
        void setDepthTransientWorkaround(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthTransientWorkaround = value;
            publishLocked();
        }
        void setDepthCaptureMethod(int value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthCaptureMethod = std::clamp(value, 0, 2);
            publishLocked();
        }
        void setDepthSourceChannel(int value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthSourceChannel = std::clamp(value, 0, 6);
            publishLocked();
        }
        void setDepthInvert(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthInvert = value;
            publishLocked();
        }
        void setDepthFlip(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthFlip = value;
            publishLocked();
        }
        void setDepthFlop(bool value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            settings.depthFlop = value;
            publishLocked();
        }

        // Get raw settings struct (for bulk operations)
        VkBasaltSettings getSettings() const
        {
            return *snapshot();
        }

    private:
        // Copies `settings` into a new immutable snapshot and publishes it.
        // Callers hold `mutex`, which serialises writers; readers never lock.
        void publishLocked()
        {
            current.store(std::make_shared<const VkBasaltSettings>(settings), std::memory_order_release);
        }

        mutable std::mutex mutex;  // guards `settings` and publication order
        VkBasaltSettings settings;
        std::atomic<std::shared_ptr<const VkBasaltSettings>> current{std::make_shared<const VkBasaltSettings>()};
        std::atomic<bool> initialized{false};
    };

    // Global settings manager instance (like effectRegistry)
    extern SettingsManager settingsManager;

} // namespace VkHook

#endif // SETTINGS_MANAGER_HPP_INCLUDED
