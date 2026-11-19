#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include "config.hh"
#include "effect_config.hh"
#include "effect_id.hh"

namespace VkHook
{
    // `name` is an EffectId text ("DLSS5", "DLSS5||2") or a legacy instance
    // name ("DLSS5", "DLSS5.2").
    inline bool isDlss5Name(std::string name)
    {
        if (const auto id = EffectId::parse(name))
            return id->isBuiltin();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        if (name == "dlss5") return true;
        return name.rfind("dlss5.", 0) == 0 && name.size() > 6
            && std::all_of(name.begin() + 6, name.end(), [](unsigned char c) { return std::isdigit(c); });
    }

    inline bool isDlss5Effect(const std::string& name, Config* config)
    {
        return isDlss5Name(name) || (config && isDlss5Name(config->getOption<std::string>(name, "")));
    }

    // A saved value of one DLSS5 option: `<name>.<key>`, else the same key under
    // the legacy name the entry was migrated from.
    template<typename T>
    T dlss5SavedOption(Config* config, const std::string& name, const std::string& legacyName,
                       const char* key, const T& fallback)
    {
        if (!config)
            return fallback;
        const std::string current = name + "." + key;
        if (!legacyName.empty() && !config->hasOption(current))
            return config->getOption<T>(legacyName + "." + key, fallback);
        return config->getOption<T>(current, fallback);
    }

    // The DLSS5 entry of `id`; `legacyName` is the instance name it replaces ("" for none).
    inline EffectEntry dlss5EffectEntry(const EffectId& id, const std::string& legacyName, Config* config)
    {
        const std::string name = id.format();
        EffectEntry entry;
        EffectConfig& effect = entry.info;
        effect.name = name;
        effect.id = id;
        effect.legacyName = legacyName;
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
            const float saved = dlss5SavedOption<float>(config, name, legacyName, key, value);
            param->value = std::isfinite(saved) ? std::clamp(saved, minimum, maximum) : value;
            param->tooltip = tooltip;
            entry.parameters.push_back(std::move(param));
        };
        add("intensity", "Intensity", 1.0f, 0.0f, 1.0f,
            "Blend the SDR neural result with the original image. Zero preserves the original bytes; temporal history remains independent.");
        add("localTone", "Local tone", 1.0f, 0.0f, 1.0f,
            "OpenDLSS neural local-tone conditioning at the original resolution.");
        add("localStructure", "Local structure", 1.0f, 0.0f, 1.0f,
            "OpenDLSS neural local-structure conditioning at the original resolution.");
        add("skinStructure", "Skin structure (-1: follow local)", -1.0f, -1.0f, 1.0f,
            "OpenDLSS skin-structure conditioning when Auto-mask is enabled. Negative values follow Local structure; ignored when Auto-mask is disabled.");
        auto autoMask = std::make_unique<BoolParam>();
        autoMask->effectName = name;
        autoMask->name = "autoMask";
        autoMask->label = "Auto-mask";
        autoMask->uiType = "checkbox";
        autoMask->defaultValue = true;
        autoMask->value = dlss5SavedOption<bool>(config, name, legacyName, "autoMask", true);
        autoMask->tooltip = "OpenDLSS structure-conditioning mode, not an explicit segmentation mask. Disable to use global Local structure and ignore Skin structure.";
        entry.parameters.push_back(std::move(autoMask));
        add("style", "Style (experimental)", 0.0f, 0.0f, 128.0f,
            "Experimental OpenDLSS conditioning, passed to the model as Style / 128. The 0-128 range is exploratory, not a validated visual scale. Zero preserves the existing input.");
        auto seed = std::make_unique<UintParam>();
        seed->effectName = name;
        seed->name = "seed";
        seed->label = "Noise seed";
        seed->uiType = "slider";
        seed->maxValue = 65535;
        seed->value = dlss5SavedOption<uint32_t>(config, name, legacyName, "seed", 0u);
        seed->tooltip = "Fixed Gaussian input seed, not randomized each frame. Right-click to enter any unsigned 32-bit value (0-4294967295). Changing it resets temporal history; it is not a quality level.";
        entry.parameters.push_back(std::move(seed));
        auto temporal = std::make_unique<BoolParam>();
        temporal->effectName = name;
        temporal->name = "temporal";
        temporal->label = "Temporal reprojection";
        temporal->uiType = "checkbox";
        temporal->defaultValue = true;
        temporal->value = dlss5SavedOption<bool>(config, name, legacyName, "temporal", true);
        temporal->tooltip = "Reuse neural history with NVIDIA hardware optical flow, not engine motion vectors. Requires optical-flow hardware support; rejects unreliable motion, disocclusions and scene cuts.";
        entry.parameters.push_back(std::move(temporal));
        add("historyStrength", "History strength", 1.0f, 0.0f, 1.0f,
            "Scale the neural temporal blend by hardware-flow confidence. Zero disables history reuse; rejected history is never blended.");
        auto debug = std::make_unique<IntParam>();
        debug->effectName = name;
        debug->name = "debugView";
        debug->label = "Debug view";
        debug->uiType = "combo";
        debug->items = {"Off", "Optical flow", "History confidence", "Reprojected history", "Temporal blend weight"};
        debug->maxValue = 4;
        debug->value = std::clamp(dlss5SavedOption<int>(config, name, legacyName, "debugView", 0), 0, 4);
        debug->tooltip = "Live GPU diagnostics, overriding Intensity without changing neural history. Flow: direction hue, motion brightness (8 px = half brightness). Confidence: green accepted, black rejected. Blend: white = full history weight. Top strip: magenta = temporal off/unavailable, amber = reset, red = frame rejected/no reuse, green = accepted.";
        entry.parameters.push_back(std::move(debug));
        return entry;
    }
}
