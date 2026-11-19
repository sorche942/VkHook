// EffectId: the one text form, its parser, and the ReShade key.

#include "effect_id.hh"

#include <cstdio>
#include <set>
#include <string>

using namespace VkHook;

namespace
{
    int failures = 0;

    void expect(bool condition, const char* message)
    {
        if (!condition)
        {
            ++failures;
            std::printf("FAILED: %s\n", message);
        }
    }

    EffectId id(const char* path, const char* technique, uint32_t ordinal = 1)
    {
        return EffectId{path, technique, ordinal};
    }
}

int main()
{
    expect(id("Clarity.fx", "Clarity").format() == "Clarity.fx|Clarity", "ordinal 1 is not written");
    expect(id("Clarity.fx", "Clarity", 2).format() == "Clarity.fx|Clarity|2", "ordinal 2 is written");
    expect(id("DLSS5", "").format() == "DLSS5", "built-in id");
    expect(id("DLSS5", "", 3).format() == "DLSS5||3", "built-in copy");
    expect(id("Clarity.fx", "Clarity").reshadeKey() == "Clarity@Clarity.fx", "reshade key is technique@file");
    expect(id("Clarity.fx", "Clarity", 4).reshadeKey() == "Clarity@Clarity.fx", "reshade key drops the ordinal");

    // format/parse round trip over the shapes that matter
    const EffectId samples[] = {
        id("Clarity.fx", "Clarity"),       id("Clarity.fx", "Clarity", 2),   id("qUINT_mxao.fx", "MXAO", 17),
        id("a.b.fx", "Tech_1"),            id("Pack Name.fx", "T", 2),       id("DLSS5", ""),
        id("DLSS5", "", 2),                id("x.fx", "2"),                  id("x.fx", "2", 2),
        id("we|rd.fx", "T"),               id("we|rd.fx", "T", 5),
    };
    for (const EffectId& sample : samples)
    {
        const auto parsed = EffectId::parse(sample.format());
        expect(parsed && *parsed == sample, ("round trip: " + sample.format()).c_str());
    }

    // a technique that is all digits is a technique, not an ordinal
    {
        const auto parsed = EffectId::parse("x.fx|2");
        expect(parsed && parsed->technique == "2" && parsed->ordinal == 1, "digits-only technique");
    }

    // old instance names are not ids; the registry migrates them
    expect(!EffectId::parse("Clarity"), "bare name is legacy");
    expect(!EffectId::parse("Clarity.2"), "numbered name is legacy");
    expect(!EffectId::parse("Clarity@Clarity.fx"), "instance@type is legacy");
    expect(!EffectId::parse(""), "empty");
    expect(!EffectId::parse("x.fx|T|0"), "ordinal 0 is invalid");
    expect(!EffectId::parse("x.fx|"), "empty technique is not what format() writes");

    // a file reference without a technique is an unresolved id: its first technique
    {
        const auto parsed = EffectId::parse("Clarity.fx");
        expect(parsed && !parsed->isResolved() && parsed->technique.empty(), "bare file is unresolved");
    }
    {
        const auto parsed = EffectId::parse("dlss5");
        expect(parsed && parsed->isBuiltin() && parsed->canonicalPath == "DLSS5" && parsed->isResolved(),
               "built-in names are case-insensitive and normalised");
    }

    // identity compares file names case-insensitively, techniques exactly
    expect(id("Clarity.fx", "A") == id("clarity.FX", "A"), "file name case");
    expect(id("Clarity.fx", "A") != id("Clarity.fx", "a"), "technique is case-sensitive");
    expect(id("Clarity.fx", "A") != id("Clarity.fx", "A", 2), "ordinal is part of identity");
    expect(id("Clarity.fx", "A").sameTechnique(id("clarity.fx", "A", 5)), "sameTechnique ignores the ordinal");
    expect(id("a.fx", "A") < id("b.fx", "A") && id("a.fx", "A") < id("a.fx", "B") && id("a.fx", "A") < id("a.fx", "A", 2),
           "ordering");

    // ReShade keys
    {
        const auto key = EffectId::parseReshadeKey("Bloom@qUINT_bloom.fx");
        expect(key && key->technique == "Bloom" && key->canonicalPath == "qUINT_bloom.fx" && key->ordinal == 1,
               "reshade key parse");
        expect(!EffectId::parseReshadeKey("Bloom") && !EffectId::parseReshadeKey("@x.fx") && !EffectId::parseReshadeKey("a@"),
               "reshade key needs both sides");
    }

    // The text form contains no '.'-ordinal and no '@': neither separator carries identity.
    for (const EffectId& sample : samples)
    {
        const std::string text = sample.format();
        expect(text.find('@') == std::string::npos, "no '@' in the text form");
        expect(text.find(':') == std::string::npos && text.find('#') == std::string::npos,
               "no list separator or comment character in the text form");
    }

    // distinct ids have distinct texts
    {
        std::set<std::string> texts;
        for (const EffectId& sample : samples)
            texts.insert(sample.format());
        expect(texts.size() == sizeof(samples) / sizeof(samples[0]), "format is injective on the samples");
    }

    if (failures == 0)
        std::printf("effect id tests passed\n");
    return failures == 0 ? 0 : 1;
}
