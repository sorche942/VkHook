#ifndef VKHOOK_CONFIG_SETTINGS_SCHEMA_HH_INCLUDED
#define VKHOOK_CONFIG_SETTINGS_SCHEMA_HH_INCLUDED

#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace VkHook
{
    // The values of VkHook.conf. Construction applies the schema defaults, so a
    // member can never be left uninitialised. Every other fact about a setting
    // (key, default, range, comment) lives in the table in settings_schema.cc;
    // this struct is only where the values sit.
    struct AppSettings
    {
        AppSettings();

        int maxEffects;
        bool overlayBlockInput;
        // Route file pickers and other integrations through the XDG desktop
        // portals when the session provides them, instead of the in-overlay
        // fallbacks. Off is for sessions with no portal service.
        bool usePortalIntegration;
        // Zero selects automatic framebuffer-based scaling; otherwise 0.75 to 3.
        float uiScale;
        std::string toggleKey;
        std::string reloadKey;
        std::string overlayKey;
        bool enableOnLaunch;
        bool depthCapture;
        bool autoApply;
        int autoApplyDelay;  // ms between the last edit and the automatic apply
        bool showDebugWindow;
        // Effect parameters in their own tabbed card (default) or inline in the
        // effect list.
        bool separateEffectParams;
        // Depth resolve mode for MSAA sources: 0 auto, 1 sample-zero, 2 average.
        int depthResolveMode;
        // How a depth change rebuilds the resolve path: 0 idles the queue
        // first, 1 rebuilds behind the layer's own fences.
        int depthRebuildMode;
        // Manual depth buffer pin: the tracked image view key, "" for auto.
        std::string depthManualPin;
        // Inject a depth resolve attachment for transient multisampled depth.
        bool depthTransientWorkaround;
        // 0 off, 1 render-pass-end hook, 2 queue-submit interception.
        int depthCaptureMethod;
        // Depth encoding: 0 red, 1 alpha, 2 packed RGB, 3 logarithmic,
        // 4 view-space Z, 5 NDC, 6 reversed-Z.
        int depthSourceChannel;
        bool depthInvert;
        bool depthFlip;
        bool depthFlop;
    };

    using SettingValue = std::variant<bool, int, float, std::string>;

    enum class SettingKind
    {
        Bool,
        Int,
        Float,
        String,
    };

    // One row of the schema. The table drives loading, saving, clamping and the
    // settings UI bindings, so a new setting is one row plus one struct member.
    struct SettingDesc
    {
        const char* key;
        const char* heading;  // banner written before this key in a freshly created file; may be empty
        const char* comment;  // lines written right above the key; may be empty
        SettingKind kind;
        bool AppSettings::* boolField = nullptr;
        int AppSettings::* intField = nullptr;
        float AppSettings::* floatField = nullptr;
        std::string AppSettings::* stringField = nullptr;
        SettingValue defaultValue;
        double min = 0.0;  // inclusive clamp for Int and Float
        double max = 0.0;
        bool zeroIsAuto = false;  // Float: 0 means automatic; anything else is clamped into [min, max]
        bool onOff = false;       // Bool written as on/off instead of true/false
    };

    namespace Schema
    {
        // Bumped when a file layout changes in a way that needs a migration
        // (migrate.hh). Stored in VkHook.conf as `schemaVersion`.
        constexpr int kCurrentVersion = 1;
        constexpr const char* kVersionKey = "schemaVersion";

        std::span<const SettingDesc> settings();
        const SettingDesc* find(std::string_view key);

        SettingValue get(const AppSettings& settings, const SettingDesc& desc);

        // Stores `value` (ignored when its type does not match the row), then
        // clamps it into the row's range. Returns false when nothing was stored.
        bool set(AppSettings& settings, const SettingDesc& desc, const SettingValue& value);

        // Parses a file value for the row; nullopt when it is not a valid value
        // of the row's type. The clamp happens in set().
        bool parse(const SettingDesc& desc, std::string_view text, SettingValue& out);

        // The text written after "key = ".
        std::string format(const SettingDesc& desc, const AppSettings& settings);

        // Applies every recognised key of a parsed file, logging invalid and
        // clamped values; unknown keys are left alone.
        void load(AppSettings& settings, const std::vector<std::pair<std::string, std::string>>& entries);

        // One key = value line with the comment that explains it.
        struct Line
        {
            const char* heading;
            const char* comment;
            std::string key;
            std::string value;
        };
        std::vector<Line> lines(const AppSettings& settings);

        // The text of a file VkHook creates from scratch.
        std::string fresh(const std::vector<Line>& owned);

        // Rewrites the owned keys of `existing` in place and appends the ones it
        // lacks; unknown keys, comments, blank lines, order and CRLF endings are
        // copied through untouched.
        std::string merge(const std::string& existing, const std::vector<Line>& owned);
    } // namespace Schema
} // namespace VkHook

#endif // VKHOOK_CONFIG_SETTINGS_SCHEMA_HH_INCLUDED
