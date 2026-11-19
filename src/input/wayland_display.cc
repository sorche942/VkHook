#include "wayland_display.hh"

namespace VkHook
{
    SurfaceKind getSurfaceKind()
    {
        const auto session = InputSession::current();
        return session ? session->kind() : SurfaceKind::Unknown;
    }

    bool isWayland()
    {
        return getSurfaceKind() == SurfaceKind::Wayland;
    }

    bool isX11()
    {
        return getSurfaceKind() == SurfaceKind::X11;
    }
} // namespace VkHook
