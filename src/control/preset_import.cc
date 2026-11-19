#include "control/preset_import.hh"

#include "control/frontend.hh"

#include "config.hh"
#include "config_serializer.hh"
#include "settings_manager.hh"
#include "effects/effect_registry.hh"
#include "logger.hh"

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace VkHook
{
    void applyShaderProfile(Config* config, const std::string& path)
    {
        if (!config)
            return;
        if (!path.empty())
        {
            const auto profile = ConfigSerializer::loadShaderProfileData(path);
            const auto sidecar = ConfigSerializer::loadShaderProfileData(
                ConfigSerializer::getShaderProfileSidecarPath(path));

            auto join = [](const std::vector<std::string>& values) {
                std::string result;
                for (const auto& value : values)
                {
                    if (!result.empty()) result += ':';
                    result += value;
                }
                return result;
            };

            // an owned profile's sidecar is the complete stack: register any
            // definitions it misses before they are collected, so the game
            // config's stale effect list never merges in and a shader the
            // config forgot resolves by filename via the include paths
            if (sidecar.owned)
                for (const auto& instance : sidecar.instances)
                    if (!config->hasOption(instance.name))
                        config->setOption(instance.name, instance.type);

            auto definitions = config->getEffectDefinitions();
            if (baseConfig)
            {
                const auto baseDefinitions = baseConfig->getEffectDefinitions();
                for (const auto& [name, effectPath] : baseDefinitions)
                    definitions.emplace(name, effectPath);
            }
            std::map<std::string, std::vector<std::string>> namesByFile;
            for (const auto& [name, effectPath] : definitions)
            {
                std::string filename = std::filesystem::path(effectPath).filename().string();
                std::transform(filename.begin(), filename.end(), filename.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                namesByFile[filename].push_back(name);
            }
            auto applyParams = [&](const std::vector<ConfigParam>& params) {
            for (const auto& param : params)
            {
                std::vector<std::string> effectNames;
                std::string section = param.effectName;
                std::transform(section.begin(), section.end(), section.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                auto it = namesByFile.find(section);
                if (it == namesByFile.end())
                    it = namesByFile.find(std::filesystem::path(section).stem().string());
                if (it != namesByFile.end())
                    effectNames = it->second;
                else if (!param.effectName.empty())
                {
                    const std::filesystem::path sectionPath(param.effectName);
                    effectNames.push_back(sectionPath.extension() == ".fx"
                                              ? sectionPath.stem().string()
                                              : param.effectName);
                }
                if (!param.paramName.empty() && param.paramName.front() == '@')
                {
                    const std::string macroName = param.paramName.substr(1);
                    if (effectNames.empty())
                    {
                        for (const auto& [name, effectPath] : definitions)
                            config->setOption(name + "@" + macroName, param.value);
                    }
                    else
                    {
                        for (const auto& effectName : effectNames)
                            config->setOption(effectName + "@" + macroName, param.value);
                    }
                    continue;
                }
                for (const auto& effectName : effectNames)
                    config->setOption(effectName + "." + param.paramName, param.value);
            }
            };
            applyParams(profile.params);
            if (sidecar.owned || (!profile.hasEffectList && profile.hasTechniques))
                applyParams(sidecar.params);

            if (sidecar.owned)
            {
                const size_t maxEffects = static_cast<size_t>(settingsManager.getMaxEffects());
                std::vector<std::string> effects, disabled;
                for (const auto& instance : sidecar.instances)
                {
                    if (effects.size() >= maxEffects)
                        break;
                    // skip the removed built-ins: they have no .fx, so a legacy
                    // reference drops here and auto-save rewrites it away. an
                    // installed shader of the same name still resolves.
                    if (EffectRegistry::resolveEffectPath(instance.name, config).empty())
                        continue;
                    effects.push_back(instance.name);
                    if (!instance.enabled)
                        disabled.push_back(instance.name);
                }
                config->setOption("effects", join(effects));
                config->setOption("disabledEffects", join(disabled));
            }
            else if (profile.hasEffectList)
            {
                std::vector<std::string> effects, disabled;
                std::set<std::string> disabledSet(profile.disabledEffects.begin(), profile.disabledEffects.end());
                for (const auto& name : profile.effects)
                {
                    if (EffectRegistry::resolveEffectPath(name, config).empty())
                        continue;
                    effects.push_back(name);
                    if (disabledSet.count(name))
                        disabled.push_back(name);
                }
                config->setOption("effects", join(effects));
                config->setOption("disabledEffects", join(disabled));
            }
            else if (!profile.techniques.empty() || !profile.techniqueSorting.empty())
            {
                // TechniqueSorting is an ordering hint for all techniques in a
                // preset. Techniques is the actual enabled list. Expanding the
                // sorting list (or matching only by filename) imports every
                // technique from a shader pack and can create hundreds of
                // disabled instances that make the overlay unusable.
                std::vector<std::string> effects, disabled;
                const auto& sortedTechniques = profile.techniqueSorting.empty()
                    ? profile.techniques
                    : profile.techniqueSorting;
                std::set<std::string> enabledTechniques;
                for (const auto& technique : profile.techniques)
                {
                    enabledTechniques.insert(technique);
                }
                const bool hasExplicitEnabledTechniques = profile.hasTechniques;
                std::set<std::string> addedEffects;
                std::set<std::string> matchedConfiguredEffects;
                const size_t maxEffects = static_cast<size_t>(settingsManager.getMaxEffects());
                for (const auto& technique : sortedTechniques)
                {
                    if (effects.size() >= maxEffects)
                        break;
                    // TechniqueSorting lists every technique a preset knows;
                    // Techniques is the enabled set. expanding sorting imports
                    // hundreds of disabled instances and buries the UI.
                    if (hasExplicitEnabledTechniques && !enabledTechniques.count(technique))
                        continue;
                    const size_t separator = technique.rfind('@');
                    if (separator == std::string::npos || separator + 1 >= technique.size())
                        continue;

                    const std::string filename = technique.substr(separator + 1);
                    const std::string techniqueName = technique.substr(0, separator);
                    std::string normalizedFilename = filename;
                    std::transform(normalizedFilename.begin(), normalizedFilename.end(), normalizedFilename.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    auto configuredNames = namesByFile.find(normalizedFilename);
                    if (configuredNames == namesByFile.end())
                        configuredNames = namesByFile.find(std::filesystem::path(normalizedFilename).stem().string());
                    std::string effectName;

                    // Resolve imported preset techniques to an existing
                    // VkHook instance by exact technique name. Fall back to
                    // the configured shader filename only when the preset
                    // uses a name that does not correspond to a configured
                    // instance; never expand one file to every instance.
                    if (config->hasOption(techniqueName))
                        effectName = techniqueName;
                    else if (configuredNames != namesByFile.end())
                    {
                        const auto candidate = std::find_if(configuredNames->second.begin(), configuredNames->second.end(),
                            [&](const std::string& name) {
                                return !matchedConfiguredEffects.count(name) && name == techniqueName;
                            });
                        if (candidate != configuredNames->second.end())
                            effectName = *candidate;
                        else if (configuredNames->second.size() == 1 &&
                                 !matchedConfiguredEffects.count(configuredNames->second.front()))
                            effectName = configuredNames->second.front();
                    }
                    // not a configured instance: create one for the installed
                    // shader, so importing isn't limited to effects the game
                    // config already listed. never fall back to the bare
                    // technique name, which resolves to nothing and ghosts.
                    if (effectName.empty())
                    {
                        const std::string stem = std::filesystem::path(filename).stem().string();
                        const std::string resolved = EffectRegistry::resolveEffectPath(stem, config);
                        if (!resolved.empty())
                        {
                            effectName = stem;
                            config->setOption(stem, resolved);
                        }
                    }
                    if (effectName.empty())
                    {
                        Logger::debug("preset technique '" + techniqueName + "' from " + filename
                                      + " matches no installed effect; skipping");
                        continue;
                    }

                    if (!addedEffects.insert(effectName).second)
                        continue;
                    matchedConfiguredEffects.insert(effectName);
                    effects.push_back(effectName);
                }

                config->setOption("effects", join(effects));
                config->setOption("disabledEffects", join(disabled));
            }
        }
        activeShaderProfilePath = path;
    }
} // namespace VkHook
