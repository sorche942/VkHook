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
        auto add = [&](const char* key, const char* label, float value, float minimum, float maximum,
                       const char* tooltip) {
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
            param->tooltip = tooltip;
            effect.parameters.push_back(std::move(param));
        };
        add("intensity", "Intensity", 1.0f, 0.0f, 1.0f,
            "Blend the SDR neural result with the original image. Zero preserves the original bytes; temporal history remains independent.");
        add("localTone", "Local tone", 1.0f, 0.0f, 1.0f,
            "OpenDLSS neural local-tone conditioning at the original resolution.");
        add("localStructure", "Local structure", 1.0f, 0.0f, 1.0f,
            "OpenDLSS neural local-structure conditioning at the original resolution.");
        add("skinStructure", "Skin structure (-1: follow local)", -1.0f, -1.0f, 1.0f,
            "OpenDLSS skin-structure conditioning. Negative values follow Local structure.");
        auto temporal = std::make_unique<BoolParam>();
        temporal->effectName = name;
        temporal->name = "temporal";
        temporal->label = "Temporal reprojection";
        temporal->uiType = "checkbox";
        temporal->defaultValue = true;
        temporal->value = config ? config->getOption<bool>(name + ".temporal", true) : true;
        temporal->tooltip = "Reuse neural history with NVIDIA hardware optical flow, not engine motion vectors. Requires optical-flow hardware support; rejects unreliable motion, disocclusions and scene cuts.";
        effect.parameters.push_back(std::move(temporal));
        add("historyStrength", "History strength", 1.0f, 0.0f, 1.0f,
            "Scale the neural temporal blend by hardware-flow confidence. Zero disables history reuse; rejected history is never blended.");
        auto debug = std::make_unique<IntParam>();
        debug->effectName = name;
        debug->name = "debugView";
        debug->label = "Debug view";
        debug->uiType = "combo";
        debug->items = {"Off", "Optical flow", "History confidence", "Reprojected history", "Temporal blend weight"};
        debug->maxValue = 4;
        debug->value = config ? std::clamp(config->getOption<int>(name + ".debugView", 0), 0, 4) : 0;
        debug->tooltip = "Live GPU diagnostics, overriding Intensity without changing neural history. Flow: direction hue, motion brightness (8 px = half brightness). Confidence: green accepted, black rejected. Blend: white = full history weight. Top strip: magenta = temporal off/unavailable, amber = reset, red = frame rejected/no reuse, green = accepted.";
        effect.parameters.push_back(std::move(debug));
        return effect;
    }
}
