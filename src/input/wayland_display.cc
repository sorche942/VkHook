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

    void setWaylandDisplay(wl_display* display)
    {
        InputSession::stageLegacyWaylandDisplay(display);
    }

    void setWaylandSurface(wl_surface* surface)
    {
        InputSession::adoptLegacySurface(SurfaceKind::Wayland, reinterpret_cast<uintptr_t>(surface));
    }

    void setX11Window(unsigned long window)
    {
        InputSession::adoptLegacySurface(SurfaceKind::X11, static_cast<uintptr_t>(window));
    }
} // namespace VkHook
