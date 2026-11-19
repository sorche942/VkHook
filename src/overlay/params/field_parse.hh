#ifndef FIELD_PARSE_HPP_INCLUDED
#define FIELD_PARSE_HPP_INCLUDED

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

// Text <-> component conversion for the numeric field editors. No ImGui here so
// it can be unit-tested. Parsing uses std::from_chars: a minus sign is not part
// of an unsigned grammar, so "-1" is rejected for uint parameters instead of
// wrapping around, and anything after the number is an error, not ignored.
namespace VkHook::FieldParse
{
    // Reads exactly `count` values separated by commas and/or blanks. Writes
    // `out` only when every component parses, so a half-typed value never
    // leaves a parameter partly changed.
    template<typename T>
    bool parseComponents(std::string_view text, T* out, uint32_t count)
    {
        T parsed[4] = {};
        if (count == 0 || count > 4)
            return false;

        size_t pos = 0;
        auto skipSeparators = [&] {
            while (pos < text.size() && (text[pos] == ',' || text[pos] == ' ' || text[pos] == '\t'))
                ++pos;
        };

        for (uint32_t i = 0; i < count; ++i)
        {
            skipSeparators();
            if (pos >= text.size())
                return false;
            const char* begin = text.data() + pos;
            const char* end = text.data() + text.size();
            const auto [ptr, ec] = std::from_chars(begin, end, parsed[i]);
            if (ec != std::errc())
                return false;
            if constexpr (std::is_floating_point_v<T>)
            {
                if (!std::isfinite(parsed[i]))
                    return false;
            }
            pos = static_cast<size_t>(ptr - text.data());
            // a component must end at a separator or the end of the text
            if (pos < text.size() && text[pos] != ',' && text[pos] != ' ' && text[pos] != '\t')
                return false;
        }
        skipSeparators();
        if (pos != text.size())
            return false;

        for (uint32_t i = 0; i < count; ++i)
            out[i] = parsed[i];
        return true;
    }

    template<typename T>
    std::string formatComponents(const T* values, uint32_t count)
    {
        std::string result;
        for (uint32_t i = 0; i < count; ++i)
        {
            char buffer[48];
            const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), values[i]);
            if (i)
                result += ", ";
            result.append(buffer, ec == std::errc() ? ptr : buffer);
        }
        return result;
    }
} // namespace VkHook::FieldParse

#endif // FIELD_PARSE_HPP_INCLUDED
