#include "keyboard_input.hh"

#include "input_session.hh"
#include "logger.hh"

#include <xkbcommon/xkbcommon.h>

// Free-function facade over the current InputSession.

namespace VkHook
{
    // X11 and XKB share keysym names and values, so the lookup needs no session:
    // the hotkeys are resolved when the device is created, often before the
    // application has created its first surface.
    uint32_t convertToKeySym(std::string key)
    {
        xkb_keysym_t sym = xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_NO_FLAGS);
        if (sym == XKB_KEY_NoSymbol)
            sym = xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
        if (sym == XKB_KEY_NoSymbol)
            Logger::err("invalid key name: " + key);
        return static_cast<uint32_t>(sym);
    }

    void beginKeyboardInputFrame()
    {
        if (auto session = InputSession::current())
            session->beginFrame();
    }

    bool isKeyPressed(uint32_t ks)
    {
        if (auto session = InputSession::current())
            return session->isKeyPressed(ks);
        InputSession::warnNoSession("unsupported Vulkan surface: input polling disabled; returning no input");
        return false;
    }

    KeyboardState getKeyboardState()
    {
        if (auto session = InputSession::current())
            return session->keyboardState();
        InputSession::warnNoSession("unsupported Vulkan surface: input polling disabled; returning no input");
        return KeyboardState();
    }
} // namespace VkHook
