#pragma once

#include <string>
#include <vector>

namespace VkHook
{
    // The names of the techniques declared in a ReShade .fx file, in file order,
    // without duplicates. A pure text scan of the file itself: comments, string
    // literals and preprocessor lines are ignored, `#include`s and `#if` gating
    // are not followed (the registry reports a technique the compile does not
    // produce). Empty for an unreadable file.
    std::vector<std::string> scanTechniqueNames(const std::string& fxPath);

    // The same scan over text already in memory.
    std::vector<std::string> scanTechniqueNamesInText(const std::string& text);
} // namespace VkHook
