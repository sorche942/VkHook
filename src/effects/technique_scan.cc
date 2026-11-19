#include "effects/technique_scan.hh"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace VkHook
{
    namespace
    {
        bool isIdentStart(char c)
        {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
        }

        bool isIdentChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        }

        // The text with comments, string literals and preprocessor lines
        // blanked out; newlines stay so line starts keep their meaning.
        std::string stripNonCode(const std::string& text)
        {
            std::string out(text.size(), ' ');
            bool lineStart = true;
            for (size_t i = 0; i < text.size();)
            {
                const char c = text[i];
                if (c == '\n')
                {
                    out[i++] = '\n';
                    lineStart = true;
                    continue;
                }
                if (c == '/' && i + 1 < text.size() && text[i + 1] == '/')
                {
                    while (i < text.size() && text[i] != '\n')
                        ++i;
                    continue;
                }
                if (c == '/' && i + 1 < text.size() && text[i + 1] == '*')
                {
                    i += 2;
                    while (i < text.size() && !(text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/'))
                    {
                        if (text[i] == '\n')
                            out[i] = '\n';
                        ++i;
                    }
                    i = std::min(i + 2, text.size());
                    continue;
                }
                if (lineStart && c == '#')
                {
                    // a directive runs to the end of its line, past `\` continuations
                    while (i < text.size() && text[i] != '\n')
                    {
                        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '\n')
                        {
                            out[i + 1] = '\n';
                            i += 2;
                            continue;
                        }
                        ++i;
                    }
                    continue;
                }
                if (c == '"')
                {
                    ++i;
                    while (i < text.size() && text[i] != '"' && text[i] != '\n')
                        i += text[i] == '\\' && i + 1 < text.size() && text[i + 1] != '\n' ? 2 : 1;
                    if (i < text.size() && text[i] == '"')
                        ++i;
                    continue;
                }
                if (c != ' ' && c != '\t' && c != '\r')
                    lineStart = false;
                out[i] = c;
                ++i;
            }
            return out;
        }
    } // namespace

    std::vector<std::string> scanTechniqueNamesInText(const std::string& text)
    {
        const std::string code = stripNonCode(text);
        std::vector<std::string> names;
        auto skipSpace = [&](size_t i) {
            while (i < code.size() && std::isspace(static_cast<unsigned char>(code[i])))
                ++i;
            return i;
        };
        for (size_t i = 0; i < code.size();)
        {
            if (!isIdentStart(code[i]))
            {
                ++i;
                continue;
            }
            size_t end = i;
            while (end < code.size() && isIdentChar(code[end]))
                ++end;
            // `technique` is a reserved word, so it names a declaration wherever it stands
            if (code.compare(i, end - i, "technique") == 0 && end - i == 9)
            {
                const size_t nameBegin = skipSpace(end);
                if (nameBegin > end && nameBegin < code.size() && isIdentStart(code[nameBegin]))
                {
                    size_t nameEnd = nameBegin;
                    while (nameEnd < code.size() && isIdentChar(code[nameEnd]))
                        ++nameEnd;
                    std::string name = code.substr(nameBegin, nameEnd - nameBegin);
                    if (std::find(names.begin(), names.end(), name) == names.end())
                        names.push_back(std::move(name));
                    end = nameEnd;
                }
            }
            i = end;
        }
        return names;
    }

    std::vector<std::string> scanTechniqueNames(const std::string& fxPath)
    {
        std::ifstream file(fxPath, std::ios::binary);
        if (!file)
            return {};
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return scanTechniqueNamesInText(text);
    }
} // namespace VkHook
