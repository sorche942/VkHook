#pragma once

#include <cstdint>

struct wl_proxy;

namespace VkHook
{
    // Register overlay-owned proxies so the interposition layer skips them.
    // Call BEFORE wl_pointer_add_listener / wl_keyboard_add_listener.
    void registerOverlayProxy(wl_proxy* proxy);
    void unregisterOverlayProxy(wl_proxy* proxy);

    // The surface-local (logical) size the game gave `surface` for a buffer of
    // bufferWidth x bufferHeight pixels: its wp_viewport destination if it set
    // one, else the buffer divided by its wl_surface buffer scale. Pointer
    // events and subsurface geometry are in these units; the swapchain is in
    // buffer pixels, and with a fractional output scale the two differ.
    void gameSurfaceLogicalSize(wl_proxy* surface, uint32_t bufferWidth, uint32_t bufferHeight,
                                double& width, double& height);

    // Ask for every key the wrapped game keyboards hold to be released (true)
    // or re-pressed if still physically held (false). Called when the overlay
    // takes or gives back the keyboard, so a held movement key stops without
    // dropping keyboard focus: a synthetic leave/enter stalls input until the
    // next press.
    //
    // This only records the request. The game's key handlers belong to the
    // thread that dispatches the game's event queue, so the next interposed
    // event on that thread performs the release/re-press before it is handled;
    // it is never done from the caller's thread or under a lock. Mouse buttons
    // are deliberately untouched: a synthetic release would break click/drag
    // ordering. Only works when the listener/dispatcher interposition is
    // active.
    void withholdGameKeys(bool withhold);
}
