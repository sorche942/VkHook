#ifndef USER_MACROS_HPP_INCLUDED
#define USER_MACROS_HPP_INCLUDED

#include <string>
#include <string_view>

namespace VkHook
{
    // Whether a macro the preprocessor read is one the user may set: not a
    // built-in the layer or ReShade defines, and not an underscore-prefixed
    // internal. Shared by the macro discovery and the compile result.
    inline bool isUserVisibleMacro(std::string_view name)
    {
        if (name.empty() || name[0] == '_')
            return false;
        for (const std::string_view builtin : {"BUFFER_WIDTH", "BUFFER_HEIGHT", "BUFFER_RCP_WIDTH", "BUFFER_RCP_HEIGHT",
                                               "BUFFER_COLOR_DEPTH", "__RESHADE__", "__RESHADE_PERFORMANCE_MODE__",
                                               "__RENDERER__", "__FILE__", "__LINE__", "__DATE__", "__TIME__",
                                               "__VENDOR__", "__APPLICATION__"})
        {
            if (name == builtin)
                return false;
        }
        return true;
    }
} // namespace VkHook

#endif // USER_MACROS_HPP_INCLUDED
