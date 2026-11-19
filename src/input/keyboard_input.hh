#pragma once

#include <cstdint>
#include <string>
#include <cstddef>
#include <vector>

namespace VkHook
{
    // Keys the overlay needs for navigation and editing. Each press (including
    // auto-repeat) is reported once, in order; ImGui gets a press and a release.
    enum class NavKey : uint8_t
    {
        Backspace,
        Delete,
        Enter,
        Left,
        Right,
        Up,
        Down,
        Home,
        End,
        PageUp,
        PageDown,
        Tab,
        Escape,
        Count,
    };

    struct KeyboardState
    {
        std::string typedChars;     // Text typed since last call, UTF-8
        std::string lastKeyName;    // X11 name of last key pressed (for keybind capture)
        std::vector<NavKey> navPresses;  // navigation/editing presses since last call
        // modifiers held right now
        bool shift = false;
        bool ctrl = false;
        bool alt = false;
        bool super = false;
    };

    // Accumulators between getKeyboardState calls are bounded: while the overlay
    // is hidden nobody drains them, and a game's key presses must not pile up.
    constexpr size_t kMaxTypedBytes = 256;
    constexpr size_t kMaxNavPresses = 64;

    uint32_t convertToKeySym(std::string key);
    void beginKeyboardInputFrame();

    // True while the hotkey is held. Always false while the overlay is
    // consuming keys (a text field or popup is active), so typing "Home" into a
    // field cannot also fire the overlay hotkey.
    bool     isKeyPressed(uint32_t ks);
    KeyboardState getKeyboardState();

    // Appends a code point as UTF-8, honouring kMaxTypedBytes.
    inline void appendUtf8(std::string& out, uint32_t cp)
    {
        if (out.size() + 4 > kMaxTypedBytes || cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return;
        if (cp < 0x80)
            out += static_cast<char>(cp);
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
} // namespace VkHook
