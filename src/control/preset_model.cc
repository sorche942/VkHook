#include "control/preset_model.hh"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <map>
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

        // sections of the shared .ini name a shader file; sidecar sections name
        // an instance (or, in files written by older versions, a file).
        std::vector<EffectInstance*> sectionTargets(std::vector<EffectInstance>& instances,
                                                    const std::string& section)
        {
            std::vector<EffectInstance*> targets;
            const std::string lowered = lower(section);
            for (auto& instance : instances)
                if (isFx(instance.type) && lower(instance.type) == lowered)
                    targets.push_back(&instance);
            if (!targets.empty())
                return targets;
            for (auto& instance : instances)
                if (instance.name == section)
                    targets.push_back(&instance);
            if (!targets.empty() || !isFx(section))
                return targets;
            const std::string stem = stemOf(section);
            for (auto& instance : instances)
                if (instance.name == stem)
                    targets.push_back(&instance);
            return targets;
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
            std::string name;
            std::string type;
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
                const auto at = item.rfind('@');
                if (at == std::string::npos || at + 1 >= item.size())
                    decl.name = decl.type = item;
                else
                {
                    decl.name = item.substr(0, at);
                    decl.type = item.substr(at + 1);
                }
                if (!decl.name.empty())
                    declared.push_back(std::move(decl));
            }
        }

        std::set<std::string> usedNames;
        auto atCap = [&] { return stack.instances.size() >= options.maxEffects; };

        // The techniques a shader file contributes to an instance. The ini's
        // sorting list is the full set; Techniques marks the enabled ones.
        auto assignTechniques = [&](EffectInstance& instance, const std::set<std::string>* enabledKeys) {
            if (!isFx(instance.type))
                return;
            const std::string file = lower(instance.type);
            std::set<std::string> seen;
            auto add = [&](const std::vector<TechniqueRef>& refs) {
                for (const auto& ref : refs)
                    if (lower(ref.file) == file && seen.insert(ref.name).second)
                        instance.techniques.push_back(
                            {ref.name, !enabledKeys || enabledKeys->count(ref.key()) != 0});
            };
            add(sorting);
            add(techniques);
            if (instance.techniques.empty())
                instance.techniques.push_back({stemOf(instance.type), true});
            const bool anyEnabled = std::any_of(instance.techniques.begin(), instance.techniques.end(),
                                                [](const PresetTechnique& t) { return t.enabled; });
            // an owned preset's sidecar decides whether the instance runs; the
            // per-technique flags only narrow an enabled instance
            if (!instance.enabled || !anyEnabled)
                for (auto& technique : instance.techniques)
                    technique.enabled = true;
        };

        std::set<std::string> enabledKeys;
        for (const auto& ref : techniques)
            enabledKeys.insert(ref.key());
        const std::set<std::string>* enabledFilter = hasTechniques ? &enabledKeys : nullptr;

        if (owned)
        {
            stack.authoritative = true;
            for (const auto& decl : declared)
            {
                if (atCap())
                {
                    report("effect '" + decl.name + "' exceeds the effect limit; dropped");
                    continue;
                }
                std::string path = resolve(decl.name);
                if (path.empty() && decl.type != decl.name)
                    path = resolve(decl.type);
                if (path.empty())
                {
                    report("effect '" + decl.name + "' (" + decl.type + ") matches no installed effect; dropped");
                    continue;
                }
                if (!usedNames.insert(decl.name).second)
                {
                    report("effect '" + decl.name + "' is listed twice; dropped");
                    continue;
                }
                EffectInstance instance;
                instance.name = decl.name;
                instance.type = decl.type;
                instance.enabled = decl.enabled;
                assignTechniques(instance, enabledFilter);
                stack.instances.push_back(std::move(instance));
            }
        }
        else if (hasLegacyList)
        {
            stack.authoritative = true;
            const std::set<std::string> disabled(legacyDisabled.begin(), legacyDisabled.end());
            for (const auto& name : legacyEffects)
            {
                if (atCap())
                {
                    report("effect '" + name + "' exceeds the effect limit; dropped");
                    continue;
                }
                const std::string path = resolve(name);
                if (path.empty())
                {
                    report("effect '" + name + "' matches no installed effect; dropped");
                    continue;
                }
                if (!usedNames.insert(name).second)
                    continue;
                EffectInstance instance;
                instance.name = name;
                instance.type = isFx(path) ? filenameOf(path) : name;
                instance.enabled = disabled.count(name) == 0;
                assignTechniques(instance, nullptr);
                stack.instances.push_back(std::move(instance));
            }
        }
        else if (!techniques.empty() || !sorting.empty())
        {
            stack.authoritative = true;
            // TechniqueSorting orders every technique the preset knows;
            // Techniques is the enabled set. Only files with an enabled
            // technique become instances, so a pack-wide sorting list does not
            // import hundreds of disabled effects.
            std::vector<TechniqueRef> ordered = sorting.empty() ? techniques : sorting;
            if (!sorting.empty())
            {
                std::set<std::string> known;
                for (const auto& ref : sorting)
                    known.insert(ref.key());
                for (const auto& ref : techniques)
                    if (known.insert(ref.key()).second)
                        ordered.push_back(ref);
            }
            std::set<std::string> enabledFiles;
            for (const auto& ref : ordered)
                if (!enabledFilter || enabledFilter->count(ref.key()))
                    enabledFiles.insert(lower(ref.file));

            std::map<std::string, size_t> instanceOfFile;
            std::set<std::string> failedFiles;
            for (const auto& ref : ordered)
            {
                const std::string file = lower(ref.file);
                if (!enabledFiles.count(file) || failedFiles.count(file))
                    continue;
                auto existing = instanceOfFile.find(file);
                if (existing == instanceOfFile.end())
                {
                    if (atCap())
                    {
                        failedFiles.insert(file);
                        report("technique '" + ref.name + "@" + ref.file + "' exceeds the effect limit; dropped");
                        continue;
                    }
                    std::string name, path;
                    // an instance the caller already has for this technique
                    // name, but only when it really is this shader file
                    if (!usedNames.count(ref.name))
                        if (const std::string candidate = resolve(ref.name);
                            !candidate.empty() && lower(filenameOf(candidate)) == file)
                        {
                            name = ref.name;
                            path = candidate;
                        }
                    if (name.empty() && options.configuredNames)
                    {
                        const auto configured = options.configuredNames(file);
                        std::string pick;
                        if (std::find(configured.begin(), configured.end(), ref.name) != configured.end())
                            pick = ref.name;
                        else if (configured.size() == 1)
                            pick = configured.front();
                        if (!pick.empty() && !usedNames.count(pick))
                            if (std::string candidate = resolve(pick); !candidate.empty())
                            {
                                name = pick;
                                path = std::move(candidate);
                            }
                    }
                    // not an instance the caller has: name a new one after the
                    // installed shader. never fall back to the bare technique
                    // name, which names nothing and would ghost in the UI.
                    if (name.empty())
                    {
                        std::string candidate = resolve(ref.file);
                        if (candidate.empty())
                            candidate = resolve(stemOf(ref.file));
                        if (!candidate.empty() && !usedNames.count(stemOf(ref.file)))
                        {
                            name = stemOf(ref.file);
                            path = std::move(candidate);
                        }
                    }
                    if (name.empty())
                    {
                        failedFiles.insert(file);
                        report("technique '" + ref.name + "@" + ref.file + "' matches no installed effect; dropped");
                        continue;
                    }
                    usedNames.insert(name);
                    EffectInstance instance;
                    instance.name = name;
                    instance.type = isFx(path) ? filenameOf(path) : ref.file;
                    stack.instances.push_back(std::move(instance));
                    existing = instanceOfFile.emplace(file, stack.instances.size() - 1).first;
                }
                auto& instance = stack.instances[existing->second];
                const bool seen = std::any_of(instance.techniques.begin(), instance.techniques.end(),
                                              [&](const PresetTechnique& t) { return t.name == ref.name; });
                if (!seen)
                    instance.techniques.push_back({ref.name, !enabledFilter || enabledFilter->count(ref.key()) != 0});
            }
        }

        auto applyValues = [&](const std::vector<Entry>& entries) {
            for (const auto& entry : entries)
            {
                if (isReservedSection(entry.section))
                    continue;
                const auto targets = sectionTargets(stack.instances, entry.section);
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
        applyValues(ini);
        applyValues(sidecar);
        return stack;
    }

    SerializedPreset serialize(const EffectStack& stack)
    {
        SerializedPreset out;

        std::vector<std::string> enabledList, sortedList;
        std::set<std::string> enabledSeen, sortedSeen;
        std::map<std::string, int> sharedCount;
        for (const auto& instance : stack.instances)
        {
            if (!isFx(instance.type))
                continue;
            ++sharedCount[lower(instance.type)];
            std::vector<PresetTechnique> techniques = instance.techniques;
            if (techniques.empty())
                techniques.push_back({stemOf(instance.type), true});
            for (const auto& technique : techniques)
            {
                const std::string entry = technique.name + "@" + instance.type;
                if (sortedSeen.insert(entry).second)
                    sortedList.push_back(entry);
                if (instance.enabled && technique.enabled && enabledSeen.insert(entry).second)
                    enabledList.push_back(entry);
            }
        }

        // ReShade's own key order: definitions, techniques, sorting, sections
        if (!stack.globalMacros.empty())
            out.ini += "PreprocessorDefinitions=" + joinMacros(stack.globalMacros) + "\n";
        out.ini += "Techniques=" + joined(enabledList, ",") + "\n";
        out.ini += "TechniqueSorting=" + joined(sortedList, ",") + "\n";

        // One section per shader file (per instance for built-ins), written
        // from the last enabled instance that uses it. Disabled instances and
        // instances sharing a file keep their own values in the sidecar.
        std::vector<std::string> sectionOrder;
        std::map<std::string, const EffectInstance*> sectionOwner;
        for (const auto& instance : stack.instances)
        {
            if (!instance.enabled)
                continue;
            const std::string section = isFx(instance.type) ? instance.type : instance.name;
            const std::string key = isFx(instance.type) ? lower(section) : section;
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
            out.ini += "\n[" + (isFx(owner.type) ? owner.type : owner.name) + "]\n";
            writeValues(out.ini, owner);
        }

        std::vector<std::string> entries;
        for (const auto& instance : stack.instances)
        {
            std::string entry = instance.type.empty() || instance.type == instance.name
                                    ? instance.name
                                    : instance.name + "@" + instance.type;
            if (!instance.enabled)
                entry.insert(entry.begin(), '*');
            entries.push_back(std::move(entry));
        }
        out.sidecar = "[VKHOOK]\nEffects=" + joined(entries, ", ") + "\n";
        for (const auto& instance : stack.instances)
        {
            const bool shared = isFx(instance.type) && sharedCount[lower(instance.type)] > 1;
            if ((instance.enabled && !shared) || !hasValues(instance))
                continue;
            out.sidecar += "\n[" + instance.name + "]\n";
            writeValues(out.sidecar, instance);
        }
        return out;
    }
} // namespace VkHook
