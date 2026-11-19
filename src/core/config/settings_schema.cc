#include "settings_schema.hh"

#include "ini.hh"
#include "logger.hh"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

namespace VkHook
{
    namespace
    {
        using S = AppSettings;

        SettingDesc boolRow(const char* key, bool S::* field, bool def, const char* heading = "",
                            const char* comment = "", bool onOff = false)
        {
            SettingDesc row{key, heading, comment, SettingKind::Bool};
            row.boolField = field;
            row.defaultValue = def;
            row.onOff = onOff;
            return row;
        }

        SettingDesc intRow(const char* key, int S::* field, int def, int min, int max, const char* heading = "",
                           const char* comment = "")
        {
            SettingDesc row{key, heading, comment, SettingKind::Int};
            row.intField = field;
            row.defaultValue = def;
            row.min = min;
            row.max = max;
            return row;
        }

        SettingDesc floatRow(const char* key, float S::* field, float def, double min, double max, bool zeroIsAuto,
                             const char* heading = "", const char* comment = "")
        {
            SettingDesc row{key, heading, comment, SettingKind::Float};
            row.floatField = field;
            row.defaultValue = def;
            row.min = min;
            row.max = max;
            row.zeroIsAuto = zeroIsAuto;
            return row;
        }

        SettingDesc stringRow(const char* key, std::string S::* field, const char* def, const char* heading = "",
                              const char* comment = "")
        {
            SettingDesc row{key, heading, comment, SettingKind::String};
            row.stringField = field;
            row.defaultValue = std::string(def);
            return row;
        }

        // File order is the order a fresh VkHook.conf is written in. Depth
        // capture defaults on so depth effects work on a game's first launch
        // instead of needing a restart.
        const std::array<SettingDesc, 21>& table()
        {
            static const std::array<SettingDesc, 21> rows = {
                boolRow("overlayBlockInput", &S::overlayBlockInput, true, "Overlay settings"),
                boolRow("usePortalIntegration", &S::usePortalIntegration, true),
                floatRow("uiScale", &S::uiScale, 0.0f, 0.75, 3.0, true, "",
                         "# uiScale: 0 = automatic; manual range 0.75 to 3.0\n"),
                intRow("maxEffects", &S::maxEffects, 10, 1, 200),
                boolRow("autoApply", &S::autoApply, true),
                intRow("autoApplyDelay", &S::autoApplyDelay, 200, 0, 1000),
                stringRow("toggleKey", &S::toggleKey, "End", "Key bindings"),
                stringRow("reloadKey", &S::reloadKey, "F10"),
                stringRow("overlayKey", &S::overlayKey, "Home"),
                boolRow("enableOnLaunch", &S::enableOnLaunch, true, "Startup behavior"),
                boolRow("depthCapture", &S::depthCapture, true, "", "", true),
                boolRow("showDebugWindow", &S::showDebugWindow, false, "Debug"),
                boolRow("separateEffectParams", &S::separateEffectParams, true, "Overlay layout"),
                intRow("depthResolveMode", &S::depthResolveMode, 0, 0, 2, "Advanced (depth buffer)",
                       "# depthResolveMode: 0=auto (prefer average), 1=sample-zero, 2=average\n"),
                stringRow("depthManualPin", &S::depthManualPin, ""),
                boolRow("depthTransientWorkaround", &S::depthTransientWorkaround, true),
                intRow("depthCaptureMethod", &S::depthCaptureMethod, 2, 0, 2, "",
                       "# depthCaptureMethod: 0=off, 1=renderpass-end (recommended), 2=queue-submit (robust fallback)\n"),
                intRow("depthSourceChannel", &S::depthSourceChannel, 0, 0, 6, "Alternative depth buffer handling",
                       "# depthSourceChannel mode selection:\n"
                       "#   0 = Luminance/Red (standard Vulkan depth)\n"
                       "#   1 = Alpha (alpha-encoded depth)\n"
                       "#   2 = Packed RGB (depth in RGB channels)\n"
                       "#   3 = Logarithmic (log-encoded depth)\n"
                       "#   4 = View-space Z (raw view-space Z)\n"
                       "#   5 = NDC (Normalized Device Coordinates)\n"
                       "#   6 = Reversed-Z (inverted for precision)\n"),
                boolRow("depthInvert", &S::depthInvert, false, "",
                        "# depthInvert: invert depth values (flips near/far planes)\n"),
                boolRow("depthFlip", &S::depthFlip, false, "",
                        "# depthFlip/depthFlop: mirror the depth image vertically/horizontally\n"),
                boolRow("depthFlop", &S::depthFlop, false),
            };
            return rows;
        }

        template<typename T>
        bool parseNumber(std::string_view text, T& out)
        {
            const char* end = text.data() + text.size();
            const auto [ptr, ec] = std::from_chars(text.data(), end, out);
            return ec == std::errc() && ptr == end;
        }
    } // namespace

    AppSettings::AppSettings()
    {
        for (const SettingDesc& row : table())
            Schema::set(*this, row, row.defaultValue);
    }

    namespace Schema
    {
        std::span<const SettingDesc> settings()
        {
            return table();
        }

        const SettingDesc* find(std::string_view key)
        {
            for (const SettingDesc& row : table())
                if (key == row.key)
                    return &row;
            return nullptr;
        }

        SettingValue get(const AppSettings& settings, const SettingDesc& desc)
        {
            switch (desc.kind)
            {
                case SettingKind::Bool: return settings.*desc.boolField;
                case SettingKind::Int: return settings.*desc.intField;
                case SettingKind::Float: return settings.*desc.floatField;
                case SettingKind::String: return settings.*desc.stringField;
            }
            return false;
        }

        bool set(AppSettings& settings, const SettingDesc& desc, const SettingValue& value)
        {
            switch (desc.kind)
            {
                case SettingKind::Bool:
                {
                    const bool* v = std::get_if<bool>(&value);
                    if (v)
                        settings.*desc.boolField = *v;
                    return v != nullptr;
                }
                case SettingKind::Int:
                {
                    const int* v = std::get_if<int>(&value);
                    if (v)
                        settings.*desc.intField = std::clamp(*v, static_cast<int>(desc.min), static_cast<int>(desc.max));
                    return v != nullptr;
                }
                case SettingKind::Float:
                {
                    const float* v = std::get_if<float>(&value);
                    if (!v)
                        return false;
                    float stored = *v;
                    if (!std::isfinite(stored))
                        stored = desc.zeroIsAuto ? 0.0f : static_cast<float>(desc.min);
                    else if (desc.zeroIsAuto && stored <= 0.0f)
                        stored = 0.0f;
                    else
                        stored = std::clamp(stored, static_cast<float>(desc.min), static_cast<float>(desc.max));
                    settings.*desc.floatField = stored;
                    return true;
                }
                case SettingKind::String:
                {
                    const std::string* v = std::get_if<std::string>(&value);
                    if (v)
                        settings.*desc.stringField = *v;
                    return v != nullptr;
                }
            }
            return false;
        }

        bool parse(const SettingDesc& desc, std::string_view text, SettingValue& out)
        {
            switch (desc.kind)
            {
                case SettingKind::Bool:
                {
                    const auto parsed = Ini::parseBool(text);
                    if (!parsed)
                        return false;
                    out = *parsed;
                    return true;
                }
                case SettingKind::Int:
                {
                    int parsed = 0;
                    if (!parseNumber(text, parsed))
                        return false;
                    out = parsed;
                    return true;
                }
                case SettingKind::Float:
                {
                    float parsed = 0.0f;
                    if (!parseNumber(text, parsed) || !std::isfinite(parsed))
                        return false;
                    out = parsed;
                    return true;
                }
                case SettingKind::String:
                    out = std::string(text);
                    return true;
            }
            return false;
        }

        std::string format(const SettingDesc& desc, const AppSettings& settings)
        {
            switch (desc.kind)
            {
                case SettingKind::Bool:
                {
                    const bool v = settings.*desc.boolField;
                    return desc.onOff ? (v ? "on" : "off") : (v ? "true" : "false");
                }
                case SettingKind::Int: return std::to_string(settings.*desc.intField);
                case SettingKind::Float:
                {
                    std::ostringstream out;
                    out.imbue(std::locale::classic());
                    out << settings.*desc.floatField;
                    return out.str();
                }
                case SettingKind::String: return Ini::formatValue(settings.*desc.stringField);
            }
            return {};
        }

        void load(AppSettings& settings, const std::vector<std::pair<std::string, std::string>>& entries)
        {
            for (const auto& [key, text] : entries)
            {
                const SettingDesc* desc = find(key);
                if (!desc)
                    continue;
                SettingValue value;
                if (!parse(*desc, text, value))
                {
                    Logger::err("invalid " + key + " value: " + text);
                    continue;
                }
                set(settings, *desc, value);
                if (get(settings, *desc) != value)
                    Logger::warn("clamped " + key + " from " + text + " to " + format(*desc, settings));
            }
        }

        std::vector<Line> lines(const AppSettings& settings)
        {
            std::vector<Line> result;
            for (const SettingDesc& row : table())
                result.push_back({row.heading, row.comment, row.key, format(row, settings)});
            return result;
        }

        std::string fresh(const std::vector<Line>& owned)
        {
            std::string text = "# VkHook configuration\n";
            for (const Line& line : owned)
            {
                if (*line.heading)
                    text += std::string("\n# ") + line.heading + "\n";
                text += line.comment;
                text += line.key + " = " + line.value + "\n";
            }
            return text;
        }

        std::string merge(const std::string& existing, const std::vector<Line>& owned)
        {
            std::map<std::string, const Line*> byKey;
            for (const Line& line : owned)
                byKey[line.key] = &line;

            std::set<std::string> written;
            std::string out;
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
                const auto it = parsed.kind == Ini::LineKind::Pair ? byKey.find(parsed.key) : byKey.end();
                if (it == byKey.end())
                {
                    out += line;
                }
                else
                {
                    out += it->second->key + " = " + it->second->value;
                    if (!line.empty() && line.back() == '\r')
                        out += '\r';
                    written.insert(it->first);
                }
                if (hasNewline)
                    out += '\n';
            }

            std::string added;
            for (const Line& line : owned)
            {
                if (written.count(line.key))
                    continue;
                added += line.comment;
                added += line.key + " = " + line.value + "\n";
            }
            if (!added.empty())
            {
                if (!out.empty() && out.back() != '\n')
                    out += '\n';
                out += "\n# Added by VkHook\n" + added;
            }
            return out;
        }
    } // namespace Schema
} // namespace VkHook
