#pragma once

#include "input_blocker.hh"

namespace VkHook
{
    // Facade over the current InputSession (see input_session.hh); the Wayland
    // seat, queue and capture surface live in WaylandInput (wayland_input.hh).

    // Position/size the input-capture subsurface to cover every hitbox in the
    // list and set its input region to the union, so popups that stick out past
    // the main window are captured too. Pass nullptr/0 to disable it (empty
    // region), letting the pointer pass through to the game.
    void setWaylandInputSurfaceRects(const InputRect* rects, int count);

    // Call once at the start of each frame to allow a fresh dispatch.
    // Without this, polling deduplicates within a frame.
    void beginWaylandInputFrame();
} // namespace VkHook
