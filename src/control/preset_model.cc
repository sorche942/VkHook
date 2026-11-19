#include "control/preset_model.hh"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>

namespace VkHook
{
    namespace
    {
        std::string lower(std::string_view text)
        {
            std::string result(text);
            std::transform(result.begin(), result.end(), result.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return result;
        }

        std::string trimmed(std::string_view text)
        {
            const auto begin = text.find_first_not_of(" \t\r\n");
            if (begin == std::string_view::npos)
                return {};
            const auto end = text.find_last_not_of(" \t\r\n");
            return std::string(text.substr(begin, end - begin + 1));
        }

        bool isFx(std::string_view type)
        {
            return type.size() > 3 && lower(type.substr(type.size() - 3)) == ".fx";
        }

        std::string filenameOf(std::string_view path)
        {
            return std::filesystem::path(path).filename().string();
        }

        std::string stemOf(std::string_view filename)
        {
            return std::filesystem::path(filename).stem().string();
        }

        std::string joined(const std::vector<std::string>& items, std::string_view delimiter)
        {
            std::string result;
            for (size_t i = 0; i < items.size(); ++i)
            {
                if (i)
                    result += delimiter;
                result += items[i];
            }
            return result;
        }

        struct Entry
        {
            std::string section;
            std::string key;
            std::string value;
        };

        // Line-based, tolerant of CRLF, a BOM, `;`/`#` comment lines and
        // spaces around `=`. Keys before the first section have section "".
        std::vector<Entry> readEntries(std::string_view text)
        {
            std::vector<Entry> entries;
            if (text.starts_with("\xEF\xBB\xBF"))
                text.remove_prefix(3);
            std::string section;
            size_t begin = 0;
            while (begin <= text.size())
            {
                size_t end = text.find('\n', begin);
                if (end == std::string_view::npos)
                    end = text.size();
                const std::string line = trimmed(text.substr(begin, end - begin));
                begin = end + 1;
                if (line.empty() || line.front() == ';' || line.front() == '#')
                    continue;
                if (line.front() == '[')
                {
                    const auto close = line.find(']');
                    section = close == std::string::npos ? std::string() : line.substr(1, close - 1);
                    continue;
                }
                const auto eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                entries.push_back({section, trimmed(std::string_view(line).substr(0, eq)),
                                   trimmed(std::string_view(line).substr(eq + 1))});
            }
            return entries;
        }

        // ReShade's list syntax: items split at a single comma, a doubled comma
        // is a literal comma inside an item. Items are trimmed, empties dropped.
        std::vector<std::string> splitEscaped(std::string_view text)
        {
            std::vector<std::string> items;
            std::string item;
            auto flush = [&] {
                item = trimmed(item);
                if (!item.empty())
                    items.push_back(std::move(item));
                item.clear();
            };
            for (size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] != ',')
                    item += text[i];
                else if (i + 1 < text.size() && text[i + 1] == ',')
                {
                    item += ',';
                    ++i;
                }
                else
                    flush();
            }
            flush();
            return items;
        }

        // the old ':'-separated effect list keys
        std::vector<std::string> splitColons(std::string_view text)
        {
            std::vector<std::string> items;
            size_t begin = 0;
            while (begin <= text.size())
            {
                size_t end = text.find(':', begin);
                if (end == std::string_view::npos)
                    end = text.size();
                if (auto item = trimmed(text.substr(begin, end - begin)); !item.empty())
                    items.push_back(std::move(item));
                begin = end + 1;
            }
            return items;
        }

        std::string escapeMacroValue(std::string_view value)
        {
            std::string out;
            for (const char c : value)
            {
                out += c;
                if (c == ',')
                    out += ',';
            }
            return out;
        }

        std::vector<PresetMacro> parseMacros(std::string_view value)
        {
            std::vector<PresetMacro> macros;
            for (const auto& definition : splitEscaped(value))
            {
                const auto eq = definition.find('=');
                if (eq != std::string::npos)
                    macros.push_back({trimmed(std::string_view(definition).substr(0, eq)),
                                      trimmed(std::string_view(definition).substr(eq + 1))});
            }
            return macros;
        }

        std::string joinMacros(const std::vector<PresetMacro>& macros)
        {
            std::string out;
            for (size_t i = 0; i < macros.size(); ++i)
            {
                if (i)
                    out += ',';
                out += macros[i].name + '=' + escapeMacroValue(macros[i].value);
            }
            return out;
        }

        void putMacro(std::vector<PresetMacro>& macros, std::string_view name, std::string value)
        {
            for (auto& macro : macros)
                if (macro.name == name)
                {
                    macro.value = std::move(value);
                    return;
                }
            macros.push_back({std::string(name), std::move(value)});
        }

        PresetParam& paramNamed(std::vector<PresetParam>& params, std::string_view name)
        {
            for (auto& param : params)
                if (param.name == name)
                    return param;
            params.push_back({std::string(name), {}});
            return params.back();
        }

        // "Tint[2]" -> ("Tint", 2); anything else -> nullopt
        bool splitComponentKey(std::string_view key, std::string_view& base, size_t& index)
        {
            if (key.size() < 4 || key.back() != ']')
                return false;
            const auto open = key.rfind('[');
            if (open == std::string_view::npos || open == 0)
                return false;
            const auto digits = key.substr(open + 1, key.size() - open - 2);
            const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
            if (digits.empty() || ec != std::errc() || ptr != digits.data() + digits.size())
                return false;
            base = key.substr(0, open);
            return true;
        }

        // a preset value: "0.5" is a scalar, "1,0,0" a three-component vector
        std::vector<std::string> splitValue(std::string_view value)
        {
            std::vector<std::string> values;
            size_t begin = 0;
            while (true)
            {
                const size_t end = value.find(',', begin);
                values.push_back(trimmed(value.substr(begin, end == std::string_view::npos ? end : end - begin)));
                if (end == std::string_view::npos)
                    break;
                begin = end + 1;
            }
            return values;
        }

        struct TechniqueRef
        {
            std::string name;
            std::string file;
            std::string key() const { return name + "@" + file; }
        };

        std::vector<TechniqueRef> parseTechniques(std::string_view value)
        {
            std::vector<TechniqueRef> refs;
            for (const auto& item : splitEscaped(value))
            {
                const auto at = item.rfind('@');
                if (at == std::string::npos || at == 0 || at + 1 >= item.size())
                    continue;
                refs.push_back({item.substr(0, at), item.substr(at + 1)});
            }
            return refs;
        }

        bool isReservedSection(const std::string& section)
        {
            return section.empty() || section == "GENERAL" || section == "VKHOOK" || section == "VKINTOX";
        }

        // The instances a value section applies to. A section of the shared
        // .ini names a shader file (every instance of it) or a built-in. A
        // sidecar section names one instance by its id, or by the legacy
        // instance name of an older sidecar, or (older still) a file.
        std::vector<EffectInstance*> sectionTargets(std::vector<EffectInstance>& instances, const std::string& section,
                                                    bool fromSidecar,
                                                    const std::map<std::string, size_t>& legacyIndex)
        {
            std::vector<EffectInstance*> targets;
            if (fromSidecar)
                if (const auto legacy = legacyIndex.find(section); legacy != legacyIndex.end())
                    return {&instances[legacy->second]};
            const auto id = EffectId::parse(section);
            if (id && (fromSidecar || id->isBuiltin()))
            {
                for (auto& instance : instances)
                    if (instance.id == *id)
                        targets.push_back(&instance);
                if (!targets.empty())
                    return targets;
            }
            if (isFx(section))
            {
                const std::string lowered = lower(section);
                for (auto& instance : instances)
                    if (!instance.id.isBuiltin() && lower(instance.id.canonicalPath) == lowered)
                        targets.push_back(&instance);
            }
            return targets;
        }

        // The file a resolved path names: the built-in's id, the shader's file
        // name, or empty for anything else.
        std::string canonicalFile(const std::string& path)
        {
            if (path.empty())
                return {};
            if (isBuiltinEffectPath(path))
                return EffectId::kDlss5;
            return isFx(path) ? filenameOf(path) : std::string();
        }

        // "Bloom.2" -> 2; 0 when the name has no copy suffix
        uint32_t legacyCopyNumber(std::string_view name)
        {
            const auto dot = name.rfind('.');
            if (dot == std::string_view::npos || dot + 1 >= name.size())
                return 0;
            uint32_t value = 0;
            const auto digits = name.substr(dot + 1);
            const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
            return ec == std::errc() && ptr == digits.data() + digits.size() ? value : 0;
        }

        bool sameValues(const EffectInstance& a, const EffectInstance& b)
        {
            return a.params == b.params && a.macros == b.macros;
        }
    } // namespace

    void EffectInstance::setParam(std::string_view key, std::string value)
    {
        std::string_view base;
        size_t index = 0;
        if (splitComponentKey(key, base, index))
        {
            auto& values = paramNamed(params, base).values;
            if (values.size() <= index)
                values.resize(index + 1);
            values[index] = std::move(value);
            return;
        }
        paramNamed(params, key).values = {std::move(value)};
    }

    void EffectInstance::setMacro(std::string_view macroName, std::string value)
    {
        putMacro(macros, macroName, std::move(value));
    }

    EffectStack parse(std::string_view iniText, std::string_view sidecarText, const PathResolver& resolve,
                      Diagnostics* diagnostics, const ParseOptions& options)
    {
        auto report = [diagnostics](std::string message) {
            if (diagnostics)
                diagnostics->messages.push_back(std::move(message));
        };

        const auto ini = readEntries(iniText);
        const auto sidecar = readEntries(sidecarText);

        EffectStack stack;
        bool hasTechniques = false;
        bool hasLegacyList = false;
        std::vector<TechniqueRef> techniques, sorting;
        std::vector<std::string> legacyEffects, legacyDisabled;
        for (const auto& entry : ini)
        {
            if (entry.section != "" && entry.section != "GENERAL")
                continue;
            if (entry.key == "Techniques")
            {
                hasTechniques = true;
                techniques = parseTechniques(entry.value);
            }
            else if (entry.key == "TechniqueSorting")
                sorting = parseTechniques(entry.value);
            else if (entry.key == "VkHookEffects")
            {
                hasLegacyList = true;
                legacyEffects = splitColons(entry.value);
            }
            else if (entry.key == "VkHookDisabledEffects")
            {
                hasLegacyList = true;
                legacyDisabled = splitColons(entry.value);
            }
            else if (entry.key == "PreprocessorDefinitions")
                for (auto& macro : parseMacros(entry.value))
                    putMacro(stack.globalMacros, macro.name, std::move(macro.value));
        }

        struct Declared
        {
            std::string text;
            bool enabled = true;
        };
        bool owned = false;
        std::vector<Declared> declared;
        for (const auto& entry : sidecar)
        {
            if (entry.section == "")
            {
                if (entry.key == "PreprocessorDefinitions")
                    for (auto& macro : parseMacros(entry.value))
                        putMacro(stack.globalMacros, macro.name, std::move(macro.value));
                continue;
            }
            // "VKINTOX" is the section name an earlier release wrote
            if (entry.section != "VKHOOK" && entry.section != "VKINTOX")
                continue;
            owned = true;
            if (entry.key != "Effects")
                continue;
            for (auto item : splitEscaped(entry.value))
            {
                Declared decl;
                if (item.front() == '*')
                {
                    decl.enabled = false;
                    item.erase(item.begin());
                }
                decl.text = trimmed(item);
                if (!decl.text.empty())
                    declared.push_back(std::move(decl));
            }
        }

        auto atCap = [&] { return stack.instances.size() >= options.maxEffects; };
        auto resolveFile = [&](std::string_view key) { return canonicalFile(resolve(key)); };

        auto ordinalUsed = [&](const EffectId& id, uint32_t ordinal) {
            return std::any_of(stack.instances.begin(), stack.instances.end(), [&](const EffectInstance& instance) {
                return instance.id.sameTechnique(id) && instance.id.ordinal == ordinal;
            });
        };
        // `wanted` when it is free, else the smallest free ordinal
        auto freeOrdinal = [&](const EffectId& id, uint32_t wanted) {
            if (wanted >= 1 && !ordinalUsed(id, wanted))
                return wanted;
            uint32_t ordinal = 1;
            while (ordinalUsed(id, ordinal))
                ++ordinal;
            return ordinal;
        };
        // the instance index, or npos when the cap dropped it
        auto push = [&](EffectId id, bool enabled, const std::string& label) -> size_t {
            if (atCap())
            {
                report(label + " exceeds the effect limit; dropped");
                return std::string::npos;
            }
            EffectInstance instance;
            instance.id = std::move(id);
            instance.enabled = enabled;
            stack.instances.push_back(std::move(instance));
            return stack.instances.size() - 1;
        };

        // the technique an old instance of this file ran: the first one the
        // ini names for it; empty (the file's first technique) when none
        auto firstTechniqueOf = [&](const std::string& file) {
            const std::string wanted = lower(file);
            for (const auto* refs : {&sorting, &techniques})
                for (const auto& ref : *refs)
                    if (lower(ref.file) == wanted)
                        return ref.name;
            return std::string();
        };

        std::map<std::string, size_t> legacyIndex;  // legacy instance name -> stack index
        std::set<std::string> legacySeen;
        auto migrateLegacy = [&](const std::string& name, const std::string& type, bool enabled) {
            std::string file = resolveFile(name);
            if (file.empty() && !type.empty() && type != name)
                file = resolveFile(type);
            if (file.empty())
            {
                const bool aliased = !type.empty() && type != name;
                report("effect '" + name + "'" + (aliased ? " (" + type + ")" : "") +
                       " matches no installed effect; dropped");
                return;
            }
            if (!legacySeen.insert(name).second)
            {
                report("effect '" + name + "' is listed twice; dropped");
                return;
            }
            EffectId id;
            id.canonicalPath = file;
            if (!id.isBuiltin())
                id.technique = firstTechniqueOf(file);
            id.ordinal = freeOrdinal(id, legacyCopyNumber(name));
            const size_t index = push(std::move(id), enabled, "effect '" + name + "'");
            if (index != std::string::npos)
                legacyIndex[name] = index;
        };

        if (owned)
        {
            stack.authoritative = true;
            for (const auto& decl : declared)
            {
                // "name@file.fx" would parse as a file named that, but '@' is
                // the legacy name/type pairing; an id's separator is '|'
                const bool legacyText = decl.text.find('|') == std::string::npos && decl.text.find('@') != std::string::npos;
                const std::optional<EffectId> id = legacyText ? std::nullopt : EffectId::parse(decl.text);
                if (!id)
                {
                    const auto at = decl.text.rfind('@');
                    if (at == std::string::npos || at == 0 || at + 1 >= decl.text.size())
                        migrateLegacy(decl.text, decl.text, decl.enabled);
                    else
                        migrateLegacy(decl.text.substr(0, at), decl.text.substr(at + 1), decl.enabled);
                    continue;
                }
                EffectId resolved = *id;
                resolved.canonicalPath = resolveFile(id->canonicalPath);
                if (resolved.canonicalPath.empty())
                {
                    report("effect '" + decl.text + "' matches no installed effect; dropped");
                    continue;
                }
                if (resolved.isBuiltin())
                    resolved.technique.clear();
                if (std::any_of(stack.instances.begin(), stack.instances.end(),
                                [&](const EffectInstance& instance) { return instance.id == resolved; }))
                {
                    report("effect '" + decl.text + "' is listed twice; dropped");
                    continue;
                }
                push(std::move(resolved), decl.enabled, "effect '" + decl.text + "'");
            }
        }
        else if (hasLegacyList)
        {
            stack.authoritative = true;
            const std::set<std::string> disabled(legacyDisabled.begin(), legacyDisabled.end());
            for (const auto& name : legacyEffects)
                migrateLegacy(name, {}, disabled.count(name) == 0);
        }
        else if (!techniques.empty() || !sorting.empty())
        {
            stack.authoritative = true;
            // TechniqueSorting orders every technique the preset knows;
            // Techniques is the enabled set. Only enabled techniques become
            // instances, so a pack-wide sorting list does not import hundreds
            // of disabled effects.
            std::vector<TechniqueRef> ordered = sorting;
            std::set<std::string> known;
            for (const auto& ref : sorting)
                known.insert(ref.key());
            for (const auto& ref : techniques)
                if (known.insert(ref.key()).second)
                    ordered.push_back(ref);
            std::set<std::string> enabledKeys;
            for (const auto& ref : techniques)
                enabledKeys.insert(ref.key());

            std::map<std::string, std::string> fileOf;  // lowercase ini file -> installed file name
            for (const auto& ref : ordered)
            {
                if (hasTechniques && !enabledKeys.count(ref.key()))
                    continue;
                const std::string label = "technique '" + ref.key() + "'";
                auto cached = fileOf.find(lower(ref.file));
                if (cached == fileOf.end())
                {
                    std::string file = resolveFile(ref.file);
                    if (file.empty())
                        file = resolveFile(stemOf(ref.file));
                    cached = fileOf.emplace(lower(ref.file), std::move(file)).first;
                }
                if (cached->second.empty())
                {
                    report(label + " matches no installed effect; dropped");
                    continue;
                }
                EffectId id;
                id.canonicalPath = cached->second;
                if (!id.isBuiltin())
                    id.technique = ref.name;
                id.ordinal = freeOrdinal(id, 1);
                push(std::move(id), true, label);
            }
        }

        auto applyValues = [&](const std::vector<Entry>& entries, bool fromSidecar) {
            for (const auto& entry : entries)
            {
                if (isReservedSection(entry.section))
                    continue;
                const auto targets = sectionTargets(stack.instances, entry.section, fromSidecar, legacyIndex);
                if (entry.key == "PreprocessorDefinitions")
                {
                    for (auto* target : targets)
                        for (const auto& macro : parseMacros(entry.value))
                            putMacro(target->macros, macro.name, macro.value);
                    continue;
                }
                for (auto* target : targets)
                    paramNamed(target->params, entry.key).values = splitValue(entry.value);
            }
        };
        applyValues(ini, false);
        applyValues(sidecar, true);
        return stack;
    }

    SerializedPreset serialize(const EffectStack& stack)
    {
        SerializedPreset out;

        // Techniques= is exactly the enabled instances, TechniqueSorting= every
        // instance. An id whose technique is still unresolved has no ReShade
        // name; the sidecar keeps it. Built-ins have no ini technique entry.
        std::vector<std::string> enabledList, sortedList;
        std::set<std::string> enabledSeen, sortedSeen;
        for (const auto& instance : stack.instances)
        {
            if (instance.id.isBuiltin() || instance.id.technique.empty())
                continue;
            const std::string key = instance.id.reshadeKey();
            if (sortedSeen.insert(key).second)
                sortedList.push_back(key);
            if (instance.enabled && enabledSeen.insert(key).second)
                enabledList.push_back(key);
        }

        // ReShade's own key order: definitions, techniques, sorting, sections
        if (!stack.globalMacros.empty())
            out.ini += "PreprocessorDefinitions=" + joinMacros(stack.globalMacros) + "\n";
        out.ini += "Techniques=" + joined(enabledList, ",") + "\n";
        out.ini += "TechniqueSorting=" + joined(sortedList, ",") + "\n";

        // One section per shader file (per instance for built-ins), written
        // from the last enabled instance that uses it.
        auto sectionKey = [](const EffectInstance& instance) {
            return instance.id.isBuiltin() ? instance.id.format() : lower(instance.id.canonicalPath);
        };
        std::vector<std::string> sectionOrder;
        std::map<std::string, const EffectInstance*> sectionOwner;
        for (const auto& instance : stack.instances)
        {
            if (!instance.enabled)
                continue;
            const std::string key = sectionKey(instance);
            if (sectionOwner.emplace(key, &instance).second)
                sectionOrder.push_back(key);
            else
                sectionOwner[key] = &instance;
        }
        auto writeValues = [](std::string& text, const EffectInstance& instance) {
            if (!instance.macros.empty())
                text += "PreprocessorDefinitions=" + joinMacros(instance.macros) + "\n";
            for (const auto& param : instance.params)
                if (!param.values.empty())
                    text += param.name + "=" + joined(param.values, ",") + "\n";
        };
        auto hasValues = [](const EffectInstance& instance) {
            return !instance.macros.empty() || !instance.params.empty();
        };
        for (const auto& key : sectionOrder)
        {
            const EffectInstance& owner = *sectionOwner[key];
            if (!hasValues(owner))
                continue;
            out.ini += "\n[" + (owner.id.isBuiltin() ? owner.id.format() : owner.id.canonicalPath) + "]\n";
            writeValues(out.ini, owner);
        }

        std::vector<std::string> entries;
        for (const auto& instance : stack.instances)
            entries.push_back((instance.enabled ? "" : "*") + instance.id.format());
        out.sidecar = "[VKHOOK]\nEffects=" + joined(entries, ", ") + "\n";

        // What the shared section cannot describe: disabled instances, extra
        // copies and instances whose values differ from the file's section.
        for (const auto& instance : stack.instances)
        {
            if (!hasValues(instance))
                continue;
            bool needed = !instance.enabled;
            if (!needed && !instance.id.isBuiltin())
                needed = instance.id.ordinal > 1 || !sameValues(instance, *sectionOwner[sectionKey(instance)]);
            if (!needed)
                continue;
            out.sidecar += "\n[" + instance.id.format() + "]\n";
            writeValues(out.sidecar, instance);
        }
        return out;
    }
} // namespace VkHook
