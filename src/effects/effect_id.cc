#include "effect_id.hh"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace VkHook
{
    namespace
    {
        constexpr char kSep = '|';

        bool endsWithFx(std::string_view text)
        {
            return text.size() > 3 && lowerAscii(text.substr(text.size() - 3)) == ".fx";
        }
    } // namespace

    std::string lowerAscii(std::string_view text)
    {
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    bool isBuiltinEffectPath(std::string_view name)
    {
        return lowerAscii(name) == "dlss5";
    }

    bool EffectId::isBuiltin() const
    {
        return isBuiltinEffectPath(canonicalPath);
    }

    std::string EffectId::format() const
    {
        std::string out = canonicalPath;
        if (ordinal > 1)
            return out + kSep + technique + kSep + std::to_string(ordinal);
        if (!technique.empty())
            out += kSep + technique;
        return out;
    }

    std::string EffectId::reshadeKey() const
    {
        return technique + "@" + canonicalPath;
    }

    std::optional<EffectId> EffectId::parse(std::string_view text)
    {
        if (text.empty())
            return std::nullopt;

        EffectId id;
        // "<path>|<technique>|<ordinal>": the ordinal is the last field when it is
        // all digits and two separators precede the end.
        const size_t last = text.rfind(kSep);
        if (last != std::string_view::npos)
        {
            const std::string_view tail = text.substr(last + 1);
            const size_t           prev = last == 0 ? std::string_view::npos : text.rfind(kSep, last - 1);
            uint32_t               value = 0;
            const auto [ptr, ec] = std::from_chars(tail.data(), tail.data() + tail.size(), value);
            if (prev != std::string_view::npos && !tail.empty() && ec == std::errc() && ptr == tail.data() + tail.size())
            {
                if (value == 0)
                    return std::nullopt;
                id.ordinal = value;
                text       = text.substr(0, last);
            }
        }

        const size_t sep = text.rfind(kSep);
        if (sep == std::string_view::npos)
        {
            if (isBuiltinEffectPath(text))
                id.canonicalPath = kDlss5;
            else if (endsWithFx(text) && text.find('@') == std::string_view::npos)
                id.canonicalPath = std::string(text);  // "name@file.fx" is the old pairing, a legacy name
            else
                return std::nullopt;
            return id;
        }

        id.canonicalPath = std::string(text.substr(0, sep));
        id.technique     = std::string(text.substr(sep + 1));
        if (id.canonicalPath.empty())
            return std::nullopt;
        if (isBuiltinEffectPath(id.canonicalPath))
        {
            id.canonicalPath = kDlss5;
            id.technique.clear();
        }
        else if (id.technique.empty() && id.ordinal == 1)
            return std::nullopt;  // "file.fx|" is not what format() writes
        return id;
    }

    std::optional<EffectId> EffectId::parseReshadeKey(std::string_view text)
    {
        const size_t at = text.rfind('@');
        if (at == std::string_view::npos || at == 0 || at + 1 >= text.size())
            return std::nullopt;
        EffectId id;
        id.technique     = std::string(text.substr(0, at));
        id.canonicalPath = std::string(text.substr(at + 1));
        return id;
    }

    bool EffectId::sameTechnique(const EffectId& other) const
    {
        return technique == other.technique && lowerAscii(canonicalPath) == lowerAscii(other.canonicalPath);
    }

    EffectId EffectId::withOrdinal(uint32_t value) const
    {
        EffectId copy = *this;
        copy.ordinal  = value;
        return copy;
    }

    bool EffectId::operator==(const EffectId& other) const
    {
        return ordinal == other.ordinal && sameTechnique(other);
    }

    bool EffectId::operator<(const EffectId& other) const
    {
        const std::string a = lowerAscii(canonicalPath), b = lowerAscii(other.canonicalPath);
        if (a != b)
            return a < b;
        if (technique != other.technique)
            return technique < other.technique;
        return ordinal < other.ordinal;
    }
} // namespace VkHook
