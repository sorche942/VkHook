#pragma once

#include <cstdint>

struct _XDisplay;

namespace VkHook
{
    struct MouseState;
    // Shared X11 connection for the input backends and the input blocker. One
    // connection means one event queue: XInput2 raw events, active-grab events,
    // and query-based polling all drain from the same place.
    _XDisplay* x11Display();
    bool x11Available();

    // The game window captured from the Vulkan surface, as an XID, or 0.
    unsigned long x11GameWindow();

    // Raw key transitions, from XInput2 and from an active grab.
    using X11KeyCallback = void (*)(uint32_t keycode, bool pressed);
    using X11ButtonCallback = void (*)(uint32_t button, bool pressed);
    void setX11KeyCallback(X11KeyCallback cb);
    void setX11ButtonCallback(X11ButtonCallback cb);

    // Drains pending events and hands key/button transitions to the callbacks.
    // Call once per frame; a no-op when X11 is absent.
    void pumpX11Input();

    // Holds or releases a server-side grab on the game window so the game stops
    // seeing input while the overlay is used. Call pumpX11Input() on every frame
    // to keep the redirected events flowing.
    //
    // The pointer grab blocks clicks and motion, the keyboard grab blocks keys.
    // The keyboard is acquired first for an interactive menu: Wine reacts to
    // NotifyGrab by releasing the game's capture. Failed grabs remain inactive
    // and may be retried on the next frame.
    void x11SetGrabStates(bool pointer, bool keyboard);
    bool x11GrabActive();
    bool x11PointerGrabActive();
    bool x11GameFocused();
    // Supplies the virtual pointer and event-driven buttons while captured.
    // Raw device deltas, not game-controlled absolute pointer warps, move it.
    bool x11CapturedMouseState(MouseState& state);
} // namespace VkHook
