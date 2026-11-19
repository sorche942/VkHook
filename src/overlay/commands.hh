#ifndef VKHOOK_OVERLAY_COMMANDS_HH_INCLUDED
#define VKHOOK_OVERLAY_COMMANDS_HH_INCLUDED

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "config_serializer.hh"
#include "effects/params/effect_param.hh"
#include "settings_schema.hh"
#include "vulkan_include.hh"

namespace VkHook
{
    // What the overlay asks of the layer. The views never touch core state
    // (invariant 8): every user action becomes one of these values, pushed on a
    // CommandQueue that core drains once per present, on the present thread.
    // Each command names what the user did, not how core does it.

    // --- effect parameters and chain ---
    // A new value for one parameter. `value` is a full copy of the parameter
    // (same effect, name and type as the layer's); core copies its value in.
    struct SetParam
    {
        std::string effect;
        std::shared_ptr<const EffectParam> value;
    };
    struct ResetParams { std::string effect; };
    struct SetMacro { std::string effect; std::string name; std::string value; };
    struct SetEffectEnabled { std::string effect; bool enabled = true; };
    // the global effects on/off
    struct ToggleEffects {};
    // Remove `remove`, then insert `add` (instance, type) at `position`
    // (-1 appends). Runs as one edit so the position lines up with what is left.
    struct EditChain
    {
        std::vector<std::string> remove;
        std::vector<std::pair<std::string, std::string>> add;
        int position = -1;
    };
    struct ReorderEffects { std::vector<std::string> order; };
    struct ClearEffects {};
    // rebuild the effect chain from the layer's current parameters and macros
    struct RebuildChain {};
    // write the active shader preset to disk
    struct SaveProfile {};

    // --- shader presets and configs ---
    struct SwitchProfile { std::string name; };
    // creates `name` as a copy of the active preset and switches to it
    struct CreateProfile { std::string name; };
    struct RenameProfile { std::string from; std::string to; };
    struct DeleteProfile { std::string name; };
    // copies an external ReShade preset into the preset directory and activates it
    struct ImportPreset { std::string path; };
    struct LoadConfig { std::string path; };
    struct SaveConfig { std::string name; };
    struct SetDefaultConfig { std::string name; };
    struct DeleteConfig { std::string name; };
    // re-read every config file from disk
    struct ReloadConfig {};

    // --- depth ---
    struct PinDepth { VkImageView view = VK_NULL_HANDLE; };
    struct ClearDepthPin {};
    // forget the active buffer, the best candidate and any pin
    struct RefreshDepth {};
    // a depth encoding or transform setting changed: re-record the resolve path
    struct DepthTransformChanged {};

    // --- settings ---
    // Set one schema key (settings_schema.hh); core clamps and publishes it.
    struct SetSetting { std::string key; SettingValue value; };
    // The settings the user was editing are final: persist them and re-read
    // everything derived from them (key bindings, input blocking).
    struct SettingsChanged {};
    // The shader directories changed: persist them and rescan available effects.
    struct ShaderPathsChanged { ShaderManagerConfig config; };

    using Command = std::variant<SetParam, ResetParams, SetMacro, SetEffectEnabled, ToggleEffects, EditChain,
                                 ReorderEffects, ClearEffects, RebuildChain, SaveProfile, SwitchProfile,
                                 CreateProfile, RenameProfile, DeleteProfile, ImportPreset, LoadConfig, SaveConfig,
                                 SetDefaultConfig, DeleteConfig, ReloadConfig, PinDepth, ClearDepthPin,
                                 RefreshDepth, DepthTransformChanged, SetSetting, SettingsChanged,
                                 ShaderPathsChanged>;

    // Many producers (the overlay's thread, input handlers), one consumer. The
    // lock covers only a vector swap, so neither side waits on anything slow.
    class CommandQueue
    {
    public:
        void push(Command command)
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.push_back(std::move(command));
        }

        // Moves everything queued so far into `out` (appending), in push order.
        void drain(std::vector<Command>& out)
        {
            std::vector<Command> taken;
            {
                std::lock_guard<std::mutex> lock(mutex);
                taken.swap(pending);
            }
            for (Command& command : taken)
                out.push_back(std::move(command));
        }

        bool empty() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return pending.empty();
        }

    private:
        mutable std::mutex mutex;
        std::vector<Command> pending;
    };

    // Copies the value fields of `source` into `target`, which must be the same
    // type (same ParamType and component count). False when they differ; the
    // descriptive fields (label, range, items) are never touched.
    bool assignParamValue(EffectParam& target, const EffectParam& source);
} // namespace VkHook

#endif // VKHOOK_OVERLAY_COMMANDS_HH_INCLUDED
