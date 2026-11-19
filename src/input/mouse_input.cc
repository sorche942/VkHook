#include "mouse_input.hh"

#include "input_session.hh"

// Free-function facade over the current InputSession.

namespace VkHook
{
    MouseState getMouseState()
    {
        if (auto session = InputSession::current())
            return session->mouseState();
        InputSession::warnNoSession("unsupported Vulkan surface: input polling disabled; returning no input");
        return MouseState();
    }
} // namespace VkHook
