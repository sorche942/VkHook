#pragma once

namespace VkHook
{
    // True while the current session holds a server-side pointer grab on the
    // game window. Facade over InputSession::x11PointerGrabActive().
    bool x11PointerGrabActive();
} // namespace VkHook
