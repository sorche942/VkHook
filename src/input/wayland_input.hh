#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "capture_attachment.hh"
#include "input_blocker.hh"
#include "keyboard_input.hh"
#include "mouse_input.hh"
#include "release_stack.hh"

// libwayland and xkbcommon types stay opaque here so that only the
// translation units that talk to them include their headers.
struct wl_display;
struct wl_event_queue;
struct wl_registry;
struct wl_seat;
struct wl_compositor;
struct wl_subcompositor;
struct wl_shm;
struct wl_shm_pool;
struct wl_buffer;
struct wl_region;
struct wl_surface;
struct wl_subsurface;
struct wl_keyboard;
struct wl_pointer;
struct wp_viewporter;
struct wp_viewport;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace VkHook
{
    // The Wayland half of an InputSession: one wl_seat and one private event
    // queue on the game's wl_display, a keyboard and a pointer bound from that
    // seat, and a transparent wl_subsurface of the game's surface whose input
    // region is the overlay hitbox. The compositor routes pointer events over
    // the overlay to that subsurface, so the game's own surface never sees them,
    // with no symbol interposition needed for the pointer.
    //
    // Implemented across wayland_input_common.cc (connection, capture surface),
    // keyboard_input_wayland.cc and mouse_input_wayland.cc.
    //
    // Locking: `m_lifecycle` guards every protocol object and is held across
    // init, dispatch and teardown; listener callbacks run inside those and must
    // not take it. The keyboard and pointer state have their own mutexes, always
    // taken after `m_lifecycle`.
    class WaylandInput
    {
    public:
        WaylandInput(wl_display* display, uintptr_t parentSurface);
        WaylandInput(const WaylandInput&) = delete;
        WaylandInput& operator=(const WaylandInput&) = delete;
        ~WaylandInput();

        // The game now presents to this wl_surface (0: none). The capture
        // subsurface is re-attached to it on the spot if the objects exist, and
        // otherwise when they are created.
        void setParentSurface(uintptr_t parent);
        uint32_t parentSurfaceId() const;

        // A game surface (or the capture surface) holds keyboard or pointer focus.
        bool hasFocus() const;

        // Call once at the start of each frame to allow a fresh dispatch.
        // Without this, dispatching deduplicates within a frame.
        void beginFrame();

        // Final teardown: shutdown(), and no later poll re-creates anything.
        void close();

        KeyboardState keyboardState();
        bool isKeyPressed(uint32_t keysym);
        // Drops accumulated hotkey edges (see isKeyPressed in keyboard_input.hh)
        void discardKeyPresses();

        MouseState mouseState();
        // Mirror a button event from the game's pointer into overlay state. The
        // game's wl_pointer receives releases via implicit grab that our
        // private pointer never sees.
        void mirrorButton(uint32_t button, bool pressed);

        // Position/size the capture surface to cover every hitbox in the list and
        // set its input region to the union, so popups that stick out past the main
        // window are captured too. Pass nullptr/0 to disable it (empty region),
        // letting the pointer pass through to the game. Rects are in framebuffer
        // pixels, like everything the overlay draws.
        void setCaptureRects(const InputRect* rects, int count);

        // The overlay's framebuffer (the swapchain extent) for this frame. With
        // the game's buffer scale or viewport it gives the framebuffer pixels per
        // surface-local unit, which the pointer and capture surface are mapped by.
        void setFramebufferExtent(uint32_t width, uint32_t height);
        // Framebuffer pixels per surface-local unit, per axis (1 until known).
        double framebufferPerLogicalX() const { return m_fbPerLogicalX.load(std::memory_order_acquire); }
        double framebufferPerLogicalY() const { return m_fbPerLogicalY.load(std::memory_order_acquire); }

        // Destroys every protocol object, newest first (see ReleaseStack), and
        // the keyboard state. The next poll creates them again.
        void shutdown();

    private:
        friend struct WaylandSeatListeners;
        friend struct WaylandKeyboardListeners;
        friend struct WaylandPointerListeners;

        // connection and capture surface (wayland_input_common.cc)
        bool ensureCommon(bool wantKeyboard, bool wantPointer);
        void bindDevices();
        void onRegistryGlobal(wl_registry* registry, uint32_t name, const char* interface, uint32_t version);
        void onSeatCapabilities(uint32_t capabilities);
        void dispatchPending();
        void shutdownLocked();
        void reconcileCapture();
        bool createCapture(uintptr_t parent);
        void destroyCapture();
        void destroyInputBuffer();
        bool ensureInputBuffer(int hitboxWidth, int hitboxHeight);
        void publishCaptureRects();
        void flush();

        // keyboard (keyboard_input_wayland.cc)
        bool initKeyboard();
        void bindKeyboard();
        void releaseKeyboard();
        void processKey(uint32_t keycode, uint32_t state);
        void onKeymap(int32_t fd, uint32_t format, uint32_t size);
        void onKeyboardEnter(wl_surface* surface);
        void onKeyboardLeave();
        void onModifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group);

        // pointer (mouse_input_wayland.cc)
        bool initPointer();
        void bindPointer();
        void onPointerEnter(wl_surface* surface, int32_t sx, int32_t sy);
        void onPointerLeave(wl_surface* surface);
        void onPointerMotion(int32_t sx, int32_t sy);
        void onPointerButton(uint32_t button, bool pressed);
        void onPointerAxis(wl_pointer* pointer, double value);
        void onPointerFrame();
        void onPointerAxisDiscrete(uint32_t axis, int32_t discrete);
        void onPointerAxisValue120(uint32_t axis, int32_t value120);
        void setPointerPosition(int32_t sx, int32_t sy);

        std::mutex m_lifecycle;
        wl_display* const m_display;
        std::atomic<uintptr_t> m_parentAtomic;

        ReleaseStack m_releases;
        wl_event_queue* m_queue = nullptr;
        wl_display* m_wrapper = nullptr;
        wl_registry* m_registry = nullptr;
        wl_seat* m_seat = nullptr;
        bool m_commonReady = false;
        bool m_closed = false;
        bool m_wantKeyboard = false;
        bool m_wantPointer = false;
        uint32_t m_seatCapabilities = 0;

        // Frame-level dispatch deduplication: multiple callers (mouseState,
        // keyboardState, isKeyPressed x N) within the same frame do one dispatch.
        std::atomic<uint64_t> m_frameId{0};
        uint64_t m_lastDispatchedFrame = 0;

        // capture surface
        wl_compositor* m_compositor = nullptr;
        wl_subcompositor* m_subcompositor = nullptr;
        wl_shm* m_shm = nullptr;
        // optional: scales a 1x1 buffer to the hitbox, so no per-size pixel buffer is needed
        wp_viewporter* m_viewporter = nullptr;
        wp_viewport* m_inputViewport = nullptr;
        wl_surface* m_inputSurface = nullptr;
        wl_subsurface* m_inputSubsurface = nullptr;
        wl_region* m_inputRegion = nullptr;
        wl_buffer* m_inputBuffer = nullptr;
        wl_shm_pool* m_inputPool = nullptr;
        void* m_inputBufferData = nullptr;
        size_t m_inputBufferSize = 0;
        int m_inputShmFd = -1;
        int m_inputBufferWidth = 0;
        int m_inputBufferHeight = 0;
        // the capture surface's position in surface-local units
        float m_inputSurfaceX = 0.0f;
        float m_inputSurfaceY = 0.0f;
        CaptureAttachment m_attachment;
        // the hitboxes last requested, and the ones the compositor has; they
        // differ only until the next publish, so an unchanged overlay does not
        // churn the compositor every frame. The scale they were published at
        // is part of that: the same rects map elsewhere at another scale.
        std::vector<InputRect> m_desiredRects;
        std::vector<InputRect> m_publishedRects;
        double m_publishedPerLogicalX = 0.0;
        double m_publishedPerLogicalY = 0.0;
        std::atomic<double> m_fbPerLogicalX{1.0};
        std::atomic<double> m_fbPerLogicalY{1.0};

        std::atomic<bool> m_keyboardFocus{false};
        std::atomic<bool> m_pointerFocus{false};

        // keyboard
        wl_keyboard* m_keyboard = nullptr;
        xkb_context* m_xkbContext = nullptr;
        xkb_keymap* m_xkbKeymap = nullptr;
        xkb_state* m_xkbState = nullptr;

        // Guards the key state below: the dispatch-thread callbacks mutate it
        // while the overlay thread reads and clears it.
        std::mutex m_keyboardMutex;
        std::unordered_set<uint32_t> m_pressedKeys;
        // Accumulated key press events (keysyms) survive press+release within a
        // single frame so isKeyPressed can detect rapid taps.
        std::unordered_set<uint32_t> m_keyPressEvents;
        std::string m_typedChars;
        std::string m_lastKeyName;
        std::vector<NavKey> m_navPresses;

        // pointer
        wl_pointer* m_pointer = nullptr;
        // which surface the pointer currently has focus on (motion carries no surface)
        wl_surface* m_pointerFocusSurface = nullptr;

        using Clock = std::chrono::steady_clock;
        std::mutex m_mouseMutex;
        int m_pointerX = 0;
        int m_pointerY = 0;
        bool m_leftButton = false;
        bool m_rightButton = false;
        bool m_middleButton = false;
        float m_scroll = 0.0f;
        // Protocol events within a frame can arrive in any order. Commit exactly
        // one representation at frame end: value120, discrete, or continuous.
        float m_continuousScroll = 0.0f;
        float m_discreteScroll = 0.0f;
        float m_highResolutionScroll = 0.0f;
        bool m_hasDiscreteScroll = false;
        bool m_hasHighResolutionScroll = false;
        // Stuck-button safety net. Button state is authoritative from
        // wl_pointer.button: the compositor delivers the release even when the
        // pointer leaves the surface under an implicit grab, so we do not
        // synthesise a release on leave. A very long press timeout remains in
        // case a compositor grab swallows the release entirely.
        Clock::time_point m_leftPressTime{};
        Clock::time_point m_rightPressTime{};
        Clock::time_point m_middlePressTime{};
    };
} // namespace VkHook
