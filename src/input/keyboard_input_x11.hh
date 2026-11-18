#pragma once

#include <cstdint>
#include <string>
#include "keyboard_input.hh"

namespace VkHook
{
    uint32_t convertToKeySymX11(std::string key);
    void     beginKeyboardInputFrameX11();
    bool     isKeyPressedX11(uint32_t ks);
    KeyboardState getKeyboardStateX11();
} // namespace VkHook
