#include "overlay_ui.hh"
#include "reshade_parser.hh"
#include "config_serializer.hh"
#include "effect_names.hh"
#include "effects/technique_scan.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <set>
#include <string>
#include <vector>

#include "vendor/imgui/imgui.h"
#include "overlay/ui_theme.hh"

namespace VkHook
{
    // ---- Effect search -------------------------------------------------------
    //
    // The list routinely holds 300+ ReShade shaders, so the view is built around
    // a ranked, instantly-filtered search instead of a long scroll. Scoring is
    // cheap and deterministic: exact prefix beats word-prefix beats substring,
    // with a subsequence fallback so "sm" still finds "SMAA" and "smaa".

    static std::string toLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    static bool isSubsequence(const std::string& needle, const std::string& haystack)
    {
        size_t j = 0;
        for (size_t i = 0; i < haystack.size() && j < needle.size(); i++)
            if (haystack[i] == needle[j])
                j++;
        return j == needle.size();
    }

    // Lower score = better. Returns -1 when there is no match.
    static int matchScore(const std::string& name, const std::string& query)
    {
        if (query.empty())
            return 0;
        const std::string n = toLower(name);
        const std::string q = toLower(query);
        if (n.rfind(q, 0) == 0)
            return 0;   // prefix
        // word-prefix: the query starts a word inside the name
        for (size_t i = 1; i < n.size(); i++)
            if ((n[i - 1] == ' ' || n[i - 1] == '_' || n[i - 1] == '-') && n.compare(i, q.size(), q) == 0)
                return 1;
        if (n.find(q) != std::string::npos)
            return 2;   // substring
        if (isSubsequence(q, n))
            return 3;   // fuzzy
        return -1;
    }

    // The first directory under the shader root names the effect package. Nested
    // installs (Shaders/Package/Sub/Effect.fx) still report the top-level package,
    // and a shader sitting directly in Shaders/ reports none.
    static std::string effectPackageForPath(const std::string& path)
    {
        std::vector<std::string> parts;
        std::string current;
        for (char c : path)
        {
            if (c == '/' || c == '\\')
            {
                if (!current.empty()) parts.push_back(current);
                current.clear();
            }
            else
            {
                current.push_back(c);
            }
        }
        if (!current.empty()) parts.push_back(current);

        size_t shaders = parts.size();
        for (size_t i = 0; i < parts.size(); i++)
            if (parts[i] == "Shaders" || parts[i] == "Textures")
                shaders = i;
        return (shaders + 2 < parts.size()) ? parts[shaders + 1] : std::string();
    }

    // One read per file version: a click reuses the scan until the file's mtime moves.
    const std::vector<std::string>& OverlayUi::techniquesOf(const std::string& path)
    {
        std::error_code error;
        const auto mtime = std::filesystem::last_write_time(path, error);
        auto [entry, inserted] = techniqueScans.try_emplace(path);
        if (inserted || error || entry->second.mtime != mtime)
        {
            entry->second.mtime = error ? std::filesystem::file_time_type{} : mtime;
            entry->second.names = scanTechniqueNames(path);
        }
        return entry->second.names;
    }

    void OverlayUi::renderAddEffectsView()
    {
        std::vector<std::string> selectedEffects;
        selectedEffects.reserve(snapshot->effects.chain.size());
        for (const EffectView& effect : snapshot->effects.chain)
            selectedEffects.push_back(effect.name);

        // ESC clears the search (and, when already empty, leaves the view).
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && addEffectsSearch[0] != '\0')
            addEffectsSearch[0] = '\0';

        // Seamless typing: capture printable characters when no widget is focused.
        if (!ImGui::IsAnyItemActive())
        {
            ImGuiIO& io = ImGui::GetIO();
            if (io.InputQueueCharacters.Size > 0)
            {
                for (int i = 0; i < io.InputQueueCharacters.Size; i++)
                {
                    ImWchar c = io.InputQueueCharacters[i];
                    if (c >= 32 && c < 127)
                    {
                        size_t len = strlen(addEffectsSearch);
                        if (len < sizeof(addEffectsSearch) - 1)
                        {
                            addEffectsSearch[len] = static_cast<char>(c);
                            addEffectsSearch[len + 1] = '\0';
                        }
                    }
                }
                io.InputQueueCharacters.clear();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && addEffectsSearch[0] != '\0')
            {
                size_t len = strlen(addEffectsSearch);
                if (len > 0)
                    addEffectsSearch[len - 1] = '\0';
            }
        }

        const auto& effectPaths = snapshot->effects.effectPaths;
        const size_t maxEffectsLimit = static_cast<size_t>(settings().maxEffects);
        const size_t currentCount = selectedEffects.size();
        const size_t pendingCount = pendingAddEffects.size();

        // An add-page "type" is a shader file (by definition name or file name);
        // its chain entries are the entries of that file, or just the one entry
        // when the type itself names a technique.
        auto chainEntriesOf = [&](const std::string& type) {
            std::vector<std::string> names;
            const auto typeId = EffectId::parse(type);
            const bool oneTechnique = typeId && !typeId->technique.empty();
            const std::string key = lowerAscii(typeFileName(type, effectPaths));
            for (const EffectView& effect : snapshot->effects.chain)
            {
                if (oneTechnique)
                {
                    const auto id = EffectId::parse(effect.name);
                    if (id && id->sameTechnique(*typeId))
                        names.push_back(effect.name);
                }
                else if (effectFileKey(effect) == key)
                    names.push_back(effect.name);
            }
            return names;
        };

        // removals free slots, so they count against the limit too
        size_t removedPresent = 0;
        for (const auto& removed : addEffectsRemoved)
            removedPresent += chainEntriesOf(removed).size();
        const bool atLimit = currentCount - std::min(removedPresent, currentCount) + pendingCount >= maxEffectsLimit;

        auto isNameUsed = [&](const std::string& name) {
            const std::string lowered = lowerAscii(name);
            for (const auto& selected : selectedEffects)
                if (lowerAscii(selected) == lowered)
                    return true;
            for (const auto& p : pendingAddEffects)
                if (lowerAscii(p.first) == lowered)
                    return true;
            return false;
        };
        auto isPending = [&](const std::string& effectType) {
            for (const auto& p : pendingAddEffects)
                if (p.second == effectType)
                    return true;
            return false;
        };
        // the smallest ordinal of this technique that neither the chain nor the queue uses
        auto nextEffectId = [&](const EffectId& base) {
            for (uint32_t ordinal = 1; ordinal <= 99; ordinal++)
            {
                const EffectId candidate = base.withOrdinal(ordinal);
                if (!isNameUsed(candidate.format()))
                    return candidate;
            }
            return base.withOrdinal(99);
        };
        // The entries one copy of a type adds: every technique of its file, or
        // the one technique the type names. An unreadable file queues its
        // first technique unresolved; the registry names it.
        auto techniquesForType = [&](const std::string& type) {
            std::vector<EffectId> bases;
            if (const auto typeId = EffectId::parse(type); typeId && (typeId->isBuiltin() || !typeId->technique.empty()))
            {
                bases.push_back(typeId->withOrdinal(1));
                return bases;
            }
            EffectId file;
            file.canonicalPath = typeFileName(type, effectPaths);
            const auto path = effectPaths.find(type);
            if (!file.isBuiltin() && path != effectPaths.end() && isFxPath(path->second))
                for (const std::string& technique : techniquesOf(path->second))
                {
                    EffectId id = file;
                    id.technique = technique;
                    bases.push_back(std::move(id));
                }
            if (bases.empty())
                bases.push_back(file);
            return bases;
        };

        // Session-scoped recents, so frequently used shaders are one click away.
        auto pushRecent = [&](const std::string& type) {
            auto it = std::find(recentEffects.begin(), recentEffects.end(), type);
            if (it != recentEffects.end())
                recentEffects.erase(it);
            recentEffects.insert(recentEffects.begin(), type);
            if (recentEffects.size() > 8)
                recentEffects.resize(8);
        };
        auto queueCopies = [&](const std::string& type, int copies) {
            for (int i = 0; i < copies; i++)
                for (const EffectId& base : techniquesForType(type))
                {
                    if (selectedEffects.size() + pendingAddEffects.size() >= maxEffectsLimit)
                        return;
                    pendingAddEffects.push_back({nextEffectId(base).format(), type});
                }
        };
        auto queueEffect = [&](const std::string& effectType) {
            if (atLimit || isPending(effectType))
                return;
            queueCopies(effectType, 1);
            addEffectsRemoved.erase(effectType);
            pushRecent(effectType);
        };

        auto alreadyInConfig = [&](const std::string& type) { return !chainEntriesOf(type).empty(); };
        auto removeQueued = [&](const std::string& type) {
            pendingAddEffects.erase(std::remove_if(pendingAddEffects.begin(), pendingAddEffects.end(),
                                                   [&](const auto& p) { return p.second == type; }),
                                    pendingAddEffects.end());
        };
        // the extras map starts empty, so an already-listed effect shows 0 copies
        auto duplicateCountFor = [&](const std::string& type) {
            auto it = addEffectsDuplicateCount.find(type);
            return (it == addEffectsDuplicateCount.end()) ? 0 : std::max(0, it->second);
        };
        // the switch is the whole group: an already-listed effect keeps its
        // original, a new one gets one copy, and the count adds extra copies
        auto groupActive = [&](const std::string& type) {
            return isPending(type) || (alreadyInConfig(type) && !addEffectsRemoved.count(type));
        };
        auto copiesFor = [&](const std::string& type) {
            return duplicateCountFor(type) + (alreadyInConfig(type) ? 0 : 1);
        };
        auto toggleQueued = [&](const std::string& type, bool on) {
            removeQueued(type);
            if (!on)
            {
                if (alreadyInConfig(type))
                    addEffectsRemoved.insert(type);
                return;
            }
            addEffectsRemoved.erase(type);
            queueCopies(type, copiesFor(type));
            pushRecent(type);
        };

        // ---- Build the working set ------------------------------------------
        // Rebuilding and re-sorting 300+ shaders every frame is wasted when the
        // inputs rarely move, so cache the result until state, search or package
        // tab actually changes.
        if (snapshot->effects.availableVersion != addEffectsCacheVersion
            || std::string(addEffectsSearch) != addEffectsCacheSearch
            || addEffectsFilterPackage != addEffectsCacheFilter)
        {
            addEffectsEntries.clear();

            std::vector<std::string> sortedCurrent = snapshot->effects.currentConfigEffects;
            std::vector<std::string> sortedDefault = snapshot->effects.defaultConfigEffects;
            std::sort(sortedCurrent.begin(), sortedCurrent.end());
            std::sort(sortedDefault.begin(), sortedDefault.end());

            auto addEntry = [&](const std::string& type, const std::string& path, int group) {
                const int score = matchScore(type, addEffectsSearch);
                if (score >= 0)
                    addEffectsEntries.push_back({type, path, effectPackageForPath(path), group, score});
            };
            for (const auto& et : sortedCurrent)
            {
                auto it = effectPaths.find(et);
                addEntry(et, (it != effectPaths.end()) ? it->second : "", 0);
            }
            for (const auto& et : sortedDefault)
            {
                if (std::find(sortedCurrent.begin(), sortedCurrent.end(), et) != sortedCurrent.end())
                    continue;
                auto it = effectPaths.find(et);
                addEntry(et, (it != effectPaths.end()) ? it->second : "", 1);
            }

            // the tabs are the installed packages, not the working set: a package
            // whose effects are all already added has to stay reachable so its
            // duplicates can still be queued
            {
                std::set<std::string> packages;
                for (const auto& entry : effectPaths)
                {
                    const std::string package = effectPackageForPath(entry.second);
                    if (!package.empty())
                        packages.insert(package);
                }
                addEffectsPackages.assign(packages.begin(), packages.end());
                // VkHook's own shaders lead the tab row; the rest stay alphabetical
                std::sort(addEffectsPackages.begin(), addEffectsPackages.end(),
                          [](const std::string& a, const std::string& b) {
                              if ((a == "VkHook") != (b == "VkHook"))
                                  return a == "VkHook";
                              return a < b;
                          });
            }

            // a tab can vanish when the install changes; fall back to All
            if (!addEffectsFilterPackage.empty() &&
                std::find(addEffectsPackages.begin(), addEffectsPackages.end(), addEffectsFilterPackage) == addEffectsPackages.end())
                addEffectsFilterPackage.clear();

            if (!addEffectsFilterPackage.empty())
                addEffectsEntries.erase(std::remove_if(addEffectsEntries.begin(), addEffectsEntries.end(),
                                                       [&](const AddEffectsEntry& e) { return e.package != addEffectsFilterPackage; }),
                                        addEffectsEntries.end());

            // group results by their effect package, keeping search relevance first
            std::stable_sort(addEffectsEntries.begin(), addEffectsEntries.end(), [](const AddEffectsEntry& a, const AddEffectsEntry& b) {
                if (a.score != b.score)
                    return a.score < b.score;
                if (a.package != b.package)
                    return a.package < b.package;
                if (a.group != b.group)
                    return a.group < b.group;
                return a.type < b.type;
            });

            // the All tab, with no active search, shows a dimmed header per package
            addEffectsRows.clear();
            const bool groupHeads = addEffectsFilterPackage.empty() && addEffectsSearch[0] == '\0';
            std::string currentPackage;
            for (int i = 0; i < static_cast<int>(addEffectsEntries.size()); i++)
            {
                if (groupHeads && addEffectsEntries[i].package != currentPackage)
                {
                    currentPackage = addEffectsEntries[i].package;
                    addEffectsRows.push_back({currentPackage.empty() ? std::string("Other") : currentPackage, -1});
                }
                addEffectsRows.push_back({std::string(), i});
            }

            addEffectsCacheVersion = snapshot->effects.availableVersion;
            addEffectsCacheSearch = addEffectsSearch;
            addEffectsCacheFilter = addEffectsFilterPackage;
        }
        const std::vector<AddEffectsEntry>& entries = addEffectsEntries;
        const std::vector<AddEffectsRow>& rows = addEffectsRows;

        const bool hasSearch = addEffectsSearch[0] != '\0';
        if (ImGui::Button("Back to effects"))
        {
            pendingAddEffects.clear();
            addEffectsDuplicateCount.clear();
            addEffectsRemoved.clear();
            insertPosition = -1;
            inSelectionMode = false;
            addEffectsSearch[0] = '\0';
            return;
        }
        ImGui::SeparatorText(insertPosition >= 0 ? "Insert effects" : "Add effects");
        ImGui::Text("%zu in preset, %zu queued, %zu to remove (limit %zu)",
                    currentCount, pendingCount, removedPresent, maxEffectsLimit);
        if (insertPosition >= 0)
            ImGui::Text("Inserting at position %d", insertPosition + 1);
        if (atLimit)
            ImGui::TextColored(UI::Warning(), "At the limit: remove an effect to queue another.");
        if (addEffectsFocusSearch)
        {
            ImGui::SetKeyboardFocusHere();
            addEffectsFocusSearch = false;
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##search", "Search effects (type to filter)...",
                                 addEffectsSearch, sizeof(addEffectsSearch),
                                 ImGuiInputTextFlags_AutoSelectAll);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
        if (ImGui::BeginCombo("Package", addEffectsFilterPackage.empty() ? "All" : addEffectsFilterPackage.c_str()))
        {
            if (ImGui::Selectable("All", addEffectsFilterPackage.empty()))
                addEffectsFilterPackage.clear();
            for (const auto& package : addEffectsPackages)
                if (ImGui::Selectable(package.c_str(), addEffectsFilterPackage == package))
                    addEffectsFilterPackage = package;
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("%zu results", entries.size());
        if (!hasSearch && !recentEffects.empty() && ImGui::BeginCombo("Recent", "Choose a recent effect"))
        {
            for (const auto& effect : recentEffects)
            {
                if (ImGui::Selectable(effect.c_str()))
                {
                    const std::string selected = effect;
                    queueEffect(selected);
                    break;
                }
            }
            ImGui::EndCombo();
        }

        const float footerHeight = ImGui::GetFrameHeightWithSpacing();
        const float trayHeight = ImGui::GetFrameHeightWithSpacing() * 3.0f;
        ImGui::BeginChild("##results",
                          ImVec2(0, std::max(ImGui::GetFrameHeightWithSpacing() * 4.0f,
                                            ImGui::GetContentRegionAvail().y - footerHeight - trayHeight -
                                            ImGui::GetStyle().ItemSpacing.y * 2.0f)),
                          ImGuiChildFlags_Borders);
        if (rows.empty())
            ImGui::TextDisabled(hasSearch ? "No effects match this filter." : "No effects available.");
        if (ImGui::BeginTable("##effects", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Effect", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Included", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Extra copies", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::GetFontSize() * 10.0f);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(rows.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                    const auto& row = rows[i];
                    ImGui::TableNextRow(0, ImGui::GetFrameHeightWithSpacing());
                    ImGui::TableNextColumn();
                    if (!row.header.empty())
                    {
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextDisabled("%s", row.header.c_str());
                        continue;
                    }
                    const auto& entry = entries[row.entry];
                    ImGui::PushID(entry.type.c_str());
                    bool included = groupActive(entry.type);
                    ImGui::AlignTextToFramePadding();
                    if (ImGui::Selectable(entry.type.c_str(), included, ImGuiSelectableFlags_None,
                                          ImVec2(0, ImGui::GetFrameHeight())))
                        queueEffect(entry.type);
                    if (ImGui::IsItemHovered() && !entry.path.empty())
                        ImGui::SetTooltip("%s", entry.path.c_str());
                    ImGui::TableNextColumn();
                    ImGui::BeginDisabled(!included && atLimit);
                    if (ImGui::Checkbox("##included", &included))
                        toggleQueued(entry.type, included);
                    ImGui::EndDisabled();
                    ImGui::TableNextColumn();
                    if (included)
                    {
                        const int copies = duplicateCountFor(entry.type);
                        ImGui::BeginDisabled(copies == 0);
                        if (ImGui::Button("-"))
                        {
                            addEffectsDuplicateCount[entry.type] = std::max(0, copies - 1);
                            toggleQueued(entry.type, true);
                        }
                        ImGui::EndDisabled();
                        ImGui::SameLine();
                        ImGui::Text("%d", copies);
                        ImGui::SameLine();
                        ImGui::BeginDisabled(atLimit || copies >= 99);
                        if (ImGui::Button("+"))
                        {
                            addEffectsDuplicateCount[entry.type] = copies + 1;
                            toggleQueued(entry.type, true);
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();

        ImGui::BeginChild("##queued", ImVec2(0, trayHeight), ImGuiChildFlags_Borders);
        if (pendingAddEffects.empty())
            UI::TextSupport("Queued effects appear here.");
        for (size_t i = 0; i < pendingAddEffects.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("Remove"))
            {
                pendingAddEffects.erase(pendingAddEffects.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(effectLabel(pendingAddEffects[i].first).c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
        const bool hasRemovals = !addEffectsRemoved.empty();
        const std::string applyLabel = hasRemovals ? "Apply" :
            "Add " + std::to_string(pendingAddEffects.size()) + " effects";
        if (ImGui::Button("Cancel"))
        {
            pendingAddEffects.clear();
            addEffectsDuplicateCount.clear();
            addEffectsRemoved.clear();
            insertPosition = -1;
            inSelectionMode = false;
            addEffectsSearch[0] = '\0';
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(pendingAddEffects.empty() && addEffectsRemoved.empty());
        if (ImGui::Button(applyLabel.c_str()))
        {
            EditChain edit;
            for (const auto& removed : addEffectsRemoved)
            {
                for (const std::string& name : chainEntriesOf(removed))
                    if (std::find(edit.remove.begin(), edit.remove.end(), name) == edit.remove.end())
                        edit.remove.push_back(name);
            }
            edit.add = pendingAddEffects;
            edit.position = insertPosition;
            if (!edit.remove.empty() || !edit.add.empty())
            {
                emit(std::move(edit));
                chainEdited();
            }
            pendingAddEffects.clear();
            addEffectsDuplicateCount.clear();
            addEffectsRemoved.clear();
            insertPosition = -1;
            inSelectionMode = false;
            addEffectsSearch[0] = '\0';
        }
        ImGui::EndDisabled();
    }

} // namespace VkHook
