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
#include <cctype>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace VkHook
{
    namespace
    {
        std::string lowerCase(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }
    } // namespace

    void applyShaderProfile(Config* config, const std::string& path)
    {
        if (!config)
            return;
        if (!path.empty())
        {
            std::string iniText, sidecarText;
            ConfigSerializer::readShaderProfile(path, iniText, sidecarText);

            // the instances the game config and VkHook.conf already define, by
            // shader filename, so a preset's techniques land on those names
            std::map<std::string, std::vector<std::string>> namesByFile;
            {
                auto definitions = config->getEffectDefinitions();
                if (baseConfig)
                    for (const auto& [name, effectPath] : baseConfig->getEffectDefinitions())
                        definitions.emplace(name, effectPath);
                for (const auto& [name, effectPath] : definitions)
                    namesByFile[lowerCase(std::filesystem::path(effectPath).filename().string())].push_back(name);
                for (auto& [file, names] : namesByFile)
                    std::sort(names.begin(), names.end());
            }

            ParseOptions options;
            options.maxEffects = static_cast<size_t>(std::max(0, ConfigStore::instance().getMaxEffects()));
            options.configuredNames = [&namesByFile](std::string_view file) {
                const auto it = namesByFile.find(std::string(file));
                return it == namesByFile.end() ? std::vector<std::string>{} : it->second;
            };
            const PathResolver resolve = [config](std::string_view key) {
                return EffectRegistry::resolveEffectPath(std::string(key), config);
            };

            Diagnostics diagnostics;
            const EffectStack stack = parse(iniText, sidecarText, resolve, &diagnostics, options);
            for (const auto& message : diagnostics.messages)
                Logger::debug("preset " + path + ": " + message);

            // every instance the preset keeps must be a definition of the
            // config, so it resolves again on the next load
            for (const auto& instance : stack.instances)
                if (!config->hasOption(instance.name))
                    config->setOption(instance.name, instance.type);

            // a preset-wide macro applies to every effect the config knows
            if (!stack.globalMacros.empty())
            {
                std::vector<std::string> names;
                for (const auto& [name, effectPath] : config->getEffectDefinitions())
                    names.push_back(name);
                if (baseConfig)
                    for (const auto& [name, effectPath] : baseConfig->getEffectDefinitions())
                        names.push_back(name);
                for (const auto& name : names)
                    for (const auto& macro : stack.globalMacros)
                        config->setOption(name + "@" + macro.name, macro.value);
            }

            for (const auto& instance : stack.instances)
            {
                for (const auto& param : instance.params)
                {
                    if (param.values.size() == 1)
                        config->setOption(instance.name + "." + param.name, param.values.front());
                    else
                        for (size_t i = 0; i < param.values.size(); ++i)
                            config->setOption(instance.name + "." + param.name + "[" + std::to_string(i) + "]",
                                              param.values[i]);
                }
                for (const auto& macro : instance.macros)
                    config->setOption(instance.name + "@" + macro.name, macro.value);
            }

            if (stack.authoritative)
            {
                std::vector<std::string> effects, disabled;
                for (const auto& instance : stack.instances)
                {
                    effects.push_back(instance.name);
                    if (!instance.enabled)
                        disabled.push_back(instance.name);
                }
                config->setOption("effects", Ini::joinList(effects, ':'));
                config->setOption("disabledEffects", Ini::joinList(disabled, ':'));
            }
        }
        activeShaderProfilePath = path;
    }
} // namespace VkHook
