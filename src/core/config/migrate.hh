#ifndef VKHOOK_CONFIG_MIGRATE_HH_INCLUDED
#define VKHOOK_CONFIG_MIGRATE_HH_INCLUDED

#include <span>
#include <string>

namespace VkHook::Migrate
{
    // One upgrade of the config directory to schema version `to`. A step must
    // be safe to retry: a crash between the step and the version stamp runs it
    // again on the next start.
    struct Step
    {
        int to;
        const char* name;
        bool (*run)(const std::string& baseDir);
    };

    // The steps in ascending `to` order. Readers never carry code for a layout
    // older than the last step here; that code lives in the step instead.
    std::span<const Step> steps();

    // schemaVersion of VkHook.conf in `baseDir`; 0 when the file or key is absent.
    int storedVersion(const std::string& baseDir);

    // Runs every step newer than the stored version in order, stamping the
    // version after each success, and stops at the first failure. Returns the
    // version reached. Call on the PersistenceQueue's thread: it mutates files.
    int run(const std::string& baseDir);

    // Same, over an explicit step list (the unit test's seam).
    int run(const std::string& baseDir, std::span<const Step> steps);
} // namespace VkHook::Migrate

#endif // VKHOOK_CONFIG_MIGRATE_HH_INCLUDED
