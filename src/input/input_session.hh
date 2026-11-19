#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "input_blocker.hh"
#include "keyboard_input.hh"
#include "mouse_input.hh"

namespace VkHook
{
    // Which graphics WSI the game handed us. The layer processes Wayland and
    // X11; anything else passes straight through.
    enum class SurfaceKind
    {
        Unknown = 0,
        Wayland,
        X11,
    };

    class X11Input;
    class WaylandInput;

    // All input state for one game surface: the X11 connection and grabs, or the
    // Wayland seat, queue, keyboard, pointer and capture subsurface, plus the
    // blocking state the overlay drives. Nothing here is process-wide, so two
    // surfaces (two windows, a recreated window) never share a pointer position,
    // a held-key set or a grab.
    //
    // Lifetime: vkCreate{Xlib,Xcb,Wayland}SurfaceKHR creates the session and
    // vkDestroySurfaceKHR destroys it. The registry holds only weak references,
    // so the session lives exactly as long as its creator's shared_ptr (the
    // surface state). destroy() ends it early and is idempotent.
    //
    // Threads: every public method may be called from any thread. The Wayland
    // and X11 backends serialise their own protocol state internally.
    class InputSession
    {
    public:
        // `nativeDisplay` is the wl_display* for Wayland. For X11 it is the Xlib
        // Display* or xcb connection the game used; the session reads input on
        // its own connection to $DISPLAY and keeps this only for identification.
        // `nativeSurface` is the wl_surface* or the X11 window XID. Returns null
        // for SurfaceKind::Unknown, a null surface, or a null Wayland display.
        static std::shared_ptr<InputSession> create(SurfaceKind kind, void* nativeDisplay, uintptr_t nativeSurface);

        // The session the free-function facade (input_blocker.hh,
        // keyboard_input.hh, mouse_input.hh, wayland_display.hh) acts on. Policy
        // with several live sessions: the most recently created one that has
        // input focus, else the most recently created one. Null when none is
        // live. Focus is Wayland keyboard/pointer focus on the session's
        // surfaces, or X11 focus on its window as last observed.
        static std::shared_ptr<InputSession> current();

        InputSession(const InputSession&) = delete;
        InputSession& operator=(const InputSession&) = delete;
        ~InputSession();

        // Releases every grab and protocol object, in dependency order. Idempotent;
        // afterwards every query returns its idle value. The destructor does the same.
        void destroy();

        // The game replaced its surface on the same display (a new wl_surface, or a
        // new X11 window). Wayland: the capture subsurface is destroyed and
        // re-created on the new parent. X11: grabs on the old window are released
        // and the new window takes over.
        void onSurfaceRecreated(uintptr_t newNativeSurface);

        SurfaceKind kind() const;
        void* nativeDisplay() const;
        uintptr_t nativeSurface() const;
        bool alive() const;
        // Input focus as last observed; drives current().
        bool hasFocus() const;
        // wl_proxy id of the Wayland surface (the key of the syncobj gate), 0 otherwise.
        uint32_t waylandSurfaceId() const;

        // --- blocking (see input_blocker.hh for the contract of each) ---
        void initBlocker(bool enabled);
        void setRects(const InputRect* rects, int count);
        void setBlocked(bool blocked);
        void setInteractive(bool interactive);
        void setWantsKeyboard(bool wantsKeyboard);
        void updatePointerPosition(float x, float y);
        bool blockedAt(float x, float y) const;
        bool blocked() const;
        bool consumingKeys() const;
        bool blockingEnabled() const;
        bool grabActive() const;
        bool keyboardWithheld() const;
        bool pointerOverOverlay() const;
        bool x11PointerGrabActive() const;

        // --- polling ---
        void beginFrame();
        bool isKeyPressed(uint32_t keysym);
        KeyboardState keyboardState();
        MouseState mouseState();

        // Hitboxes of the Wayland capture subsurface, in framebuffer pixels; no-op on X11.
        void setWaylandCaptureRects(const InputRect* rects, int count);
        // The overlay framebuffer's size, which Wayland maps surface-local
        // coordinates to; no-op on X11, where both are pixels.
        void setFramebufferExtent(uint32_t width, uint32_t height);

        // --- game pointer, fed by the Wayland interposer ---
        // The game's own pointer position (surface-local, stored in framebuffer
        // pixels) and whether it sits where the overlay owns input. Buttons are
        // mirrored because the game's implicit grab delivers releases our
        // private pointer never sees.
        void noteGamePointer(double x, double y);
        bool gamePointerBlocked() const;
        void mirrorGameButton(uint32_t button, bool pressed);

        // Warns once per process when something polls input with no session.
        static void warnNoSession(const char* message);

    private:
        InputSession(SurfaceKind kind, void* nativeDisplay, uintptr_t nativeSurface);

        void applyBlocking();
        void applyX11Blocking();
        void applyWaylandBlocking();
        bool pointInRects(float x, float y) const;

        const SurfaceKind m_kind;
        void* const m_nativeDisplay;
        std::atomic<uintptr_t> m_nativeSurface;
        std::atomic<bool> m_destroyed{false};

        std::unique_ptr<X11Input> m_x11;
        std::unique_ptr<WaylandInput> m_wayland;

        std::atomic<bool> m_blockingEnabled{false};
        std::atomic<bool> m_blocked{false};
        // the interactive shell is up (toasts alone do not count): the blocker may
        // take the keyboard away from the game at all
        std::atomic<bool> m_interactive{false};
        // a text field or popup is consuming keys right now
        std::atomic<bool> m_wantsKeyboard{false};

        mutable std::mutex m_rectMutex;
        std::vector<InputRect> m_rects;

        // updated every frame by the overlay, so hitboxes apply without the game
        // having to tell us anything.
        std::atomic<float> m_pointerX{-100000.0f};
        std::atomic<float> m_pointerY{-100000.0f};

        // last pointer position seen on a game pointer; events without
        // coordinates (button, axis, key) are tested against it
        std::atomic<double> m_gamePointerX{-1.0};
        std::atomic<double> m_gamePointerY{-1.0};

        // mirrors the wayland keyboard state so focus only flips on a crossing
        std::atomic<bool> m_keyboardWithheld{false};
    };
} // namespace VkHook
