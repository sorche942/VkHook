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
} // namespace VkHook
