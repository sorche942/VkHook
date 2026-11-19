#include "control/preset_import.hh"

#include "control/frontend.hh"
#include "control/preset_model.hh"

#include "config.hh"
#include "config_serializer.hh"
#include "effects/effect_registry.hh"
#include "ini.hh"
#include "logger.hh"
#include "store.hh"

#include <algorithm>
#include <string>
#include <vector>

namespace VkHook
{
    void applyShaderProfile(Config* config, EffectRegistry& registry, const std::string& path)
    {
        if (!config)
            return;
        if (!path.empty())
        {
            std::string iniText, sidecarText;
            ConfigSerializer::readShaderProfile(path, iniText, sidecarText);

            ParseOptions options;
            options.maxEffects = static_cast<size_t>(std::max(0, ConfigStore::instance().getMaxEffects()));
            const PathResolver resolve = [config, &registry](std::string_view key) {
                return registry.resolveEffectPath(std::string(key), config);
            };

            Diagnostics diagnostics;
            const EffectStack stack = parse(iniText, sidecarText, resolve, &diagnostics, options);
            for (const auto& message : diagnostics.messages)
                Logger::debug("preset " + path + ": " + message);

            // an id resolves by its canonical path, so no definition is
            // written; a preset-wide macro applies to every instance
            for (const auto& instance : stack.instances)
            {
                const std::string key = instance.name();
                for (const auto& macro : stack.globalMacros)
                    config->setOption(key + "@" + macro.name, macro.value);
                for (const auto& param : instance.params)
                {
                    if (param.values.size() == 1)
                        config->setOption(key + "." + param.name, param.values.front());
                    else
                        for (size_t i = 0; i < param.values.size(); ++i)
                            config->setOption(key + "." + param.name + "[" + std::to_string(i) + "]", param.values[i]);
                }
                for (const auto& macro : instance.macros)
                    config->setOption(key + "@" + macro.name, macro.value);
            }

            if (stack.authoritative)
            {
                std::vector<std::string> effects, disabled;
                for (const auto& instance : stack.instances)
                {
                    effects.push_back(instance.name());
                    if (!instance.enabled)
                        disabled.push_back(instance.name());
                }
                config->setOption("effects", Ini::joinList(effects, ':'));
                config->setOption("disabledEffects", Ini::joinList(disabled, ':'));
            }
        }
        activeShaderProfilePath = path;
    }
} // namespace VkHook
