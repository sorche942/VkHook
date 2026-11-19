#include "ini.hh"

#include <cctype>
#include <fstream>

namespace VkHook::Ini
{
    namespace
    {
        bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r'; }
    }

    std::string trim(std::string_view text)
    {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && isBlank(text[begin]))
            ++begin;
        while (end > begin && isBlank(text[end - 1]))
            --end;
        return std::string(text.substr(begin, end - begin));
    }

    Line tokenize(std::string_view line)
    {
        Line result;
        size_t first = 0;
        while (first < line.size() && isBlank(line[first]))
            ++first;
        if (first == line.size())
            return result;
        if (line[first] == '#')
        {
            result.kind = LineKind::Comment;
            return result;
        }

        // Each side keeps the length of its last quoted or non-blank character,
        // so trimming leaves blanks inside quotes alone.
        std::string key;
        std::string value;
        size_t keyKeep = 0;
        size_t valueKeep = 0;
        bool foundEquals = false;
        bool inQuotes = false;
        bool valueStarted = false;

        for (size_t i = first; i < line.size(); ++i)
        {
            const char c = line[i];
            std::string& target = foundEquals ? value : key;
            size_t& keep = foundEquals ? valueKeep : keyKeep;
            if (inQuotes)
            {
                if (c == '"')
                    inQuotes = false;
                else if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\'))
                    target += line[++i];
                else
                    target += c;
                keep = target.size();
                continue;
            }
            if (c == '#')
                break;
            if (c == '"')
            {
                inQuotes = true;
                if (foundEquals)
                    valueStarted = true;
                keep = target.size();
                continue;
            }
            if (c == '=' && !foundEquals)
            {
                foundEquals = true;
                continue;
            }
            if (isBlank(c))
            {
                // leading blanks of the value are dropped, inner ones kept
                if (foundEquals && !valueStarted)
                    continue;
                if (!foundEquals && key.empty())
                    continue;
                target += c;
                continue;
            }
            if (foundEquals)
                valueStarted = true;
            target += c;
            keep = target.size();
        }

        key.resize(keyKeep);
        value.resize(valueKeep);
        if (!foundEquals || key.empty())
        {
            result.kind = LineKind::Other;
            return result;
        }
        result.kind = LineKind::Pair;
        result.key = std::move(key);
        result.value = std::move(value);
        return result;
    }

    std::string formatValue(std::string_view value)
    {
        bool needsQuotes = false;
        if (!value.empty() && (isBlank(value.front()) || isBlank(value.back())))
            needsQuotes = true;
        for (const char c : value)
        {
            if (c == '#' || c == '"' || c == '=' || c == '\n' || c == '\r' || c == '\\')
                needsQuotes = true;
        }
        if (!needsQuotes)
            return std::string(value);

        std::string out = "\"";
        for (const char c : value)
        {
            if (c == '"' || c == '\\')
                out += '\\';
            out += c;
        }
        out += '"';
        return out;
    }

    std::optional<bool> parseBool(std::string_view text)
    {
        std::string lower;
        for (const char c : trim(text))
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "true" || lower == "1" || lower == "on" || lower == "yes")
            return true;
        if (lower == "false" || lower == "0" || lower == "off" || lower == "no")
            return false;
        return std::nullopt;
    }

    std::vector<std::string> splitList(std::string_view text, char delimiter)
    {
        std::vector<std::string> items;
        size_t begin = 0;
        while (begin <= text.size())
        {
            size_t end = text.find(delimiter, begin);
            if (end == std::string_view::npos)
                end = text.size();
            std::string item = trim(text.substr(begin, end - begin));
            if (!item.empty())
                items.push_back(std::move(item));
            begin = end + 1;
        }
        return items;
    }

    bool readFile(const std::string& path, std::vector<std::pair<std::string, std::string>>& out)
    {
        std::ifstream file(path);
        if (!file.is_open())
            return false;
        std::string text;
        while (std::getline(file, text))
        {
            Line line = tokenize(text);
            if (line.kind == LineKind::Pair)
                out.emplace_back(std::move(line.key), std::move(line.value));
        }
        return true;
    }
} // namespace VkHook::Ini
