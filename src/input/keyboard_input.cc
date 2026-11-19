#include "keyboard_input.hh"

#include "input_session.hh"
#include "keyboard_input_wayland.hh"
#include "keyboard_input_x11.hh"

// Free-function facade over the current InputSession.

namespace VkHook
{
    uint32_t convertToKeySym(std::string key)
    {
        const auto session = InputSession::current();
        if (!session)
            return 0u;
        return session->kind() == SurfaceKind::Wayland ? convertToKeySymWayland(key) : convertToKeySymX11(key);
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
