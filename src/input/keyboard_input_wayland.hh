#pragma once

#include <cstdint>
#include <string>
#include "keyboard_input.hh"

namespace VkHook
{
    uint32_t convertToKeySymWayland(std::string key);
    bool     isKeyPressedWayland(uint32_t ks);
    KeyboardState getKeyboardStateWayland();

    // Drops accumulated hotkey edges (see isKeyPressed in keyboard_input.hh)
    void discardWaylandKeyPresses();

    // Initialize Wayland keyboard from a captured wl_display
    bool initWaylandKeyboard();

    // Cleanup
    void cleanupWaylandKeyboard();
} // namespace VkHook
