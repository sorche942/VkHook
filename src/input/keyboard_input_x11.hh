#pragma once

#include <cstdint>
#include <string>

namespace VkHook
{
    // Stateless: keysym lookup by name needs no connection.
    uint32_t convertToKeySymX11(std::string key);
} // namespace VkHook
