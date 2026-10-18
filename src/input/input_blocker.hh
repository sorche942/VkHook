#pragma once

namespace VKIntox
{
    // An overlay hitbox, in surface pixels — the same space as wl_pointer
    // coordinates and x11 window-relative coordinates.
    struct InputRect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;

        bool operator==(const InputRect&) const = default;
    };

    // Call once at startup with the config value.
    void initInputBlocker(bool enabled);

    // Overlay hitboxes for noninteractive notifications. An interactive menu
    // owns pointer input throughout the surface, including outside these rects.
    void setInputRects(const InputRect* rects, int count);

    // Global gate: is the overlay on screen at all. Cheap to check.
    void setInputBlocked(bool blocked);

    // The interactive shell is up (toasts alone do not count). Its lifetime
    // owns pointer and keyboard input; Wine relinquishes capture on the initial
    // keyboard grab, before the overlay attempts its pointer grab.
    void setInputInteractive(bool interactive);

    // The overlay is consuming keys right now (a text field or popup).
    void setOverlayWantsKeyboard(bool wantsKeyboard);

    // Newest pointer position in surface pixels. Updates notification hit tests
    // and reapplies the current input ownership.
    void updatePointerPosition(float x, float y);

    // True while a menu owns input, or inside a notification hitbox.
    bool isInputBlockedAt(float x, float y);

    // True when the overlay is on screen (regardless of hitbox).
    bool isInputBlocked();

    // True while the overlay consumes keys (a text field is active or a popup
    // is open). Wayland withholds key events on this, hitbox or not.
    bool isOverlayConsumingKeys();

    // Diagnostics: the configured gate plus the live per-backend state.
    bool inputBlockingEnabled();
    bool inputGrabActive();        // X11: a server-side pointer or keyboard grab is up
    bool inputKeyboardWithheld();  // Wayland keys withheld from the game
    bool inputPointerOverOverlay();
} // namespace VKIntox
