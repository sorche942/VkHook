#pragma once

#include <cstdint>
#include <string>

namespace VkHook
{
    // Stateless: XKB keysym lookup by name needs no seat.
    uint32_t convertToKeySymWayland(std::string key);

    // Destroys the Wayland objects of the current session (keyboard, pointer,
    // seat, queue, capture surface), in dependency order. The next poll creates
    // them again. A session's destructor does this too; once every caller
    // creates and destroys sessions with its surface these two are redundant.
    void cleanupWaylandKeyboard();
} // namespace VkHook
