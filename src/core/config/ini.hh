#ifndef VKHOOK_CONFIG_INI_HH_INCLUDED
#define VKHOOK_CONFIG_INI_HH_INCLUDED

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The one tokenizer for VkHook's `key = value` files (VkHook.conf, per-game
// .conf, shader_manager.conf). Every reader and writer goes through it so the
// same file always means the same thing.
//
// Syntax:
//   key = value           surrounding blanks are trimmed, inner ones kept
//   key = "a # b"         quotes protect '#', '=', blanks and edge whitespace
//   # comment             a '#' outside quotes starts a comment
//   inside quotes, \" is a quote and \\ is a backslash; any other backslash
//   is literal. Outside quotes nothing is escaped.
// The first '=' separates key from value; later ones belong to the value.
namespace VkHook::Ini
{
    enum class LineKind
    {
        Blank,    // empty or whitespace only
        Comment,  // first non-blank character is '#'
        Pair,     // key = value (the value may be empty)
        Other,    // anything else (no '=' outside quotes, section headers, ...)
    };

    struct Line
    {
        LineKind    kind = LineKind::Blank;
        std::string key;
        std::string value;
    };

    Line tokenize(std::string_view line);

    // The text to write after "key = " so tokenize() reads `value` back exactly.
    std::string formatValue(std::string_view value);

    // true/false/1/0/on/off/yes/no, any case. nullopt for anything else.
    std::optional<bool> parseBool(std::string_view text);

    // Items of a delimiter-separated list, each trimmed, empties dropped.
    std::vector<std::string> splitList(std::string_view text, char delimiter);

    std::string trim(std::string_view text);

    // Every Pair line of a file in order. False when the file cannot be read.
    bool readFile(const std::string& path, std::vector<std::pair<std::string, std::string>>& out);
} // namespace VkHook::Ini

#endif // VKHOOK_CONFIG_INI_HH_INCLUDED
