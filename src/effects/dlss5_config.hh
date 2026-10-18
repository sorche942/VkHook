#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include "config.hh"
#include "effect_config.hh"

namespace VKIntox
{
    inline bool isDlss5Name(std::string name)
    {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        if (name == "dlss5") return true;
        return name.rfind("dlss5.", 0) == 0 && name.size() > 6
            && std::all_of(name.begin() + 6, name.end(), [](unsigned char c) { return std::isdigit(c); });
    }

    inline bool isDlss5Effect(const std::string& name, Config* config)
    {
        return isDlss5Name(name) || (config && isDlss5Name(config->getOption<std::string>(name, "")));
    }

    inline EffectConfig dlss5EffectConfig(const std::string& name, Config* config)
    {
        EffectConfig effect;
        effect.name = name;
        effect.effectType = "DLSS5";
        effect.type = EffectType::DLSS5;
        auto add = [&](const char* key, const char* label, float value, float minimum, float maximum) {
            auto param = std::make_unique<FloatParam>();
            param->effectName = name;
            param->name = key;
            param->label = label;
            param->uiType = "slider";
            param->defaultValue = value;
            param->minValue = minimum;
            param->maxValue = maximum;
            const float saved = config ? config->getOption<float>(name + "." + key, value) : value;
            param->value = std::isfinite(saved) ? std::clamp(saved, minimum, maximum) : value;
            param->tooltip = "OpenDLSS neural rendering. Same resolution, SDR, no temporal history (no engine motion vectors).";
            effect.parameters.push_back(std::move(param));
        };
        add("intensity", "Intensity", 1.0f, 0.0f, 1.0f);
        add("localTone", "Local tone", 1.0f, 0.0f, 1.0f);
        add("localStructure", "Local structure", 1.0f, 0.0f, 1.0f);
        add("skinStructure", "Skin structure (-1: follow local)", -1.0f, -1.0f, 1.0f);
        return effect;
    }
}
