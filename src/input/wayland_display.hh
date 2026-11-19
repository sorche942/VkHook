#pragma once

#include "input_session.hh"

struct wl_display;
struct wl_surface;

namespace VkHook
{
    // Facade over the current InputSession (input_session.hh), which owns the
    // surface capture. SurfaceKind is declared there.

    // Kind of the current session; Unknown when no surface was captured.
    SurfaceKind getSurfaceKind();

    // True once the game created a Wayland Vulkan surface.
    bool isWayland();

    // True once the game created an Xlib/Xcb Vulkan surface.
    bool isX11();

    // Called from vkCreateWaylandSurfaceKHR with the game's wl_display, then
    // setWaylandSurface with its wl_surface; together they create the session
    // (or re-target it, when the game recreated its surface on the same display).
    void setWaylandDisplay(wl_display* display);
    void setWaylandSurface(wl_surface* surface);

    // Called from vkCreateXlib/XcbSurfaceKHR to capture the game's XID. Xlib's
    // `None` and the rest of X11/X.h stay out of this header on purpose: they
    // collide with symbols the codebase already defines, so the X11 headers
    // live only in the isolated input translation units.
    void setX11Window(unsigned long window);
} // namespace VkHook
