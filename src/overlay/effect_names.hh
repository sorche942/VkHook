#pragma once

// Names the overlay shows and compares for chain entries. A chain entry is one
// technique of a shader file and is keyed by EffectId::format(); the user sees
// the technique, plus the file and copy number only where they disambiguate.
// "type" is the file identity: the lower-case shader file name.

#include "effects/effect_id.hh"
#include "overlay/core_snapshot.hh"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace VkHook
{
    inline bool isFxPath(std::string_view path)
    {
        return path.size() > 3 && lowerAscii(path.substr(path.size() - 3)) == ".fx";
    }

    // "name" -> "name.fx" unless it already is a file name or the built-in
    inline std::string fileKeyOfName(std::string_view name)
    {
        if (isBuiltinEffectPath(name) || isFxPath(name))
            return lowerAscii(name);
        return lowerAscii(name) + ".fx";
    }

    // The shader file of a chain entry (lower case).
    inline std::string effectFileKey(const EffectView& effect)
    {
        if (const auto id = EffectId::parse(effect.name))
            return lowerAscii(id->canonicalPath);
        if (isFxPath(effect.filePath))
            return lowerAscii(std::filesystem::path(effect.filePath).filename().string());
        return fileKeyOfName(effect.type);
    }

    // The shader file name an add-effects "type" (a definition name, a file
    // name or an id text) names: the id's own file, the file name of its path,
    // or the type itself with ".fx".
    inline std::string typeFileName(const std::string& type, const std::map<std::string, std::string>& effectPaths)
    {
        if (const auto id = EffectId::parse(type))
            return id->canonicalPath;
        const auto path = effectPaths.find(type);
        if (path != effectPaths.end() && isFxPath(path->second))
            return std::filesystem::path(path->second).filename().string();
        return type + ".fx";
    }

    inline std::string formatEffectLabel(const EffectId& id, bool showFile)
    {
        if (id.isBuiltin())
            return id.canonicalPath + (id.ordinal > 1 ? " #" + std::to_string(id.ordinal) : std::string());
        std::string label = id.technique.empty() ? std::filesystem::path(id.canonicalPath).stem().string() : id.technique;
        if (showFile || id.ordinal > 1)
        {
            label += " (" + id.canonicalPath;
            if (id.ordinal > 1)
                label += " #" + std::to_string(id.ordinal);
            label += ")";
        }
        return label;
    }

    // The chain-list / tab label of an entry; shows the file when the chain has
    // several entries of it.
    inline std::string effectLabel(const EffectCatalog& catalog, const EffectView& effect)
    {
        const auto id = EffectId::parse(effect.name);
        if (!id)
            return effect.name;
        const std::string key = effectFileKey(effect);
        size_t entries = 0;
        for (const EffectView& other : catalog.chain)
            if (effectFileKey(other) == key)
                ++entries;
        return formatEffectLabel(*id, entries > 1);
    }

    // The label of an id text not (yet) in the chain, e.g. a queued entry.
    inline std::string effectLabel(const std::string& name)
    {
        const auto id = EffectId::parse(name);
        return id ? formatEffectLabel(*id, true) : name;
    }
} // namespace VkHook
