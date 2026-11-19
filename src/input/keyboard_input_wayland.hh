#pragma once

#include <cstdint>
#include <string>

namespace VkHook
{
    // Stateless: XKB keysym lookup by name needs no seat.
    uint32_t convertToKeySymWayland(std::string key);
} // namespace VkHook
