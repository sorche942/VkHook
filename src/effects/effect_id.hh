#ifndef EFFECT_ID_HPP_INCLUDED
#define EFFECT_ID_HPP_INCLUDED

// EffectId: the identity of one chain entry.
//
// The unit of the effect chain is a technique, not a file: a `.fx` file with
// several techniques contributes one entry per technique, and each entry runs
// exactly its technique. An entry is named by
//
//     { canonicalPath, technique, ordinal }
//
//   canonicalPath  the shader's file name as installed ("Clarity.fx"), or the
//                  id of a built-in ("DLSS5"). Compared case-insensitively.
//   technique      the technique's name in that file; empty for a built-in, and
//                  empty for "the file's first technique" while still unresolved
//                  (a reference that predates technique-level entries). The
//                  registry resolves it once the file has been parsed.
//   ordinal        1-based copy number among the entries that share the same
//                  (canonicalPath, technique). Never renumbered: removing copy 1
//                  leaves copy 2 named copy 2, so values saved under it stay
//                  attached.
//
// There is exactly one text form (format/parse), used as the registry key, in
// `effects =` / `disabledEffects =`, as the prefix of `<id>.<param>` and
// `<id>@<macro>` config keys, in the sidecar's `Effects=` list and in the UI:
//
//     Clarity.fx|Clarity        ordinal 1
//     Clarity.fx|Clarity|2      ordinal 2
//     DLSS5                     the built-in, ordinal 1
//     DLSS5||2                  the built-in, ordinal 2
//
// '|' is the only separator. Identity no longer rides on '.' (the old "Clarity.2"
// copy suffix) or '@' (the old "instance@file" pairing, which also clashed with
// the "<effect>@<macro>" and "<game>@<profile>" keys).
//
// A name that does not parse (old instance names like "Clarity", "Clarity.2",
// or a user-chosen alias of a `name = file.fx` definition) is a *legacy name*;
// the registry migrates those (see EffectRegistry::ensureEffect). ReShade's own
// "technique@file.fx" syntax exists only at the .ini boundary (reshadeKey).

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace VkHook
{
    struct EffectId
    {
        std::string canonicalPath;
        std::string technique;
        uint32_t    ordinal = 1;

        static constexpr const char* kDlss5 = "DLSS5";

        // True for a built-in effect (no shader file): canonicalPath is its id.
        bool isBuiltin() const;
        // True when `technique` is set, or a built-in (which has none).
        bool isResolved() const { return isBuiltin() || !technique.empty(); }

        // The single text form. Never empty for a valid id.
        std::string format() const;
        // `technique@file.fx`, ReShade's key in Techniques= / TechniqueSorting=.
        // The ordinal has no ReShade equivalent and is dropped.
        std::string reshadeKey() const;

        // nullopt for text that is not an id: empty, ordinal 0, or a bare name
        // without '|' that is neither a built-in nor a `.fx` file name.
        static std::optional<EffectId> parse(std::string_view text);
        // Parses "technique@file.fx" (ordinal 1). nullopt when either side is empty.
        static std::optional<EffectId> parseReshadeKey(std::string_view text);

        // Same (canonicalPath, technique), ignoring the ordinal.
        bool sameTechnique(const EffectId& other) const;
        // The id of the same technique with another ordinal.
        EffectId withOrdinal(uint32_t value) const;

        bool operator==(const EffectId& other) const;
        bool operator!=(const EffectId& other) const { return !(*this == other); }
        bool operator<(const EffectId& other) const;
    };

    // True when `name` is the id of a built-in effect ("DLSS5", any case).
    bool isBuiltinEffectPath(std::string_view name);
    // ASCII lower-casing, the comparison every shader file name goes through.
    std::string lowerAscii(std::string_view text);
} // namespace VkHook

#endif // EFFECT_ID_HPP_INCLUDED
