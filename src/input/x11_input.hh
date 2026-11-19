#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "keyboard_input.hh"
#include "mouse_input.hh"

struct _XDisplay;

namespace VkHook
{
    // The X11 half of an InputSession. One connection means one event queue: XInput2
    // raw events, active-grab events and query-based polling all drain from the
    // same place. X11 headers (whose `None` and friends collide with symbols the
    // codebase defines) are included only by the translation units that implement
    // this class (x11_display.cc, keyboard_input_x11.cc, mouse_input_x11.cc), so
    // the class uses plain integer types for XIDs.
    //
    // Every public method takes the connection lock, which is recursive because
    // a grab change drains events and a drained event can change a grab.
    class X11Input
    {
    public:
        X11Input();
        X11Input(const X11Input&) = delete;
        X11Input& operator=(const X11Input&) = delete;
        // Releases grabs, frees the capture cursor and closes the connection.
        ~X11Input();

        // Final teardown: releases grabs and closes the connection now; every
        // later call is a no-op. The destructor does the same.
        void close();

        // The game window grabs and focus tests apply to. Changing it releases
        // the grabs held on the old one.
        void setWindow(unsigned long window);
        unsigned long window() const;

        // Holds or releases a server-side grab on the game window so the game
        // stops seeing input while the overlay is used. Call pump() on every
        // frame to keep the redirected events flowing.
        //
        // The pointer grab blocks clicks and motion, the keyboard grab blocks keys.
        // The keyboard is acquired first for an interactive menu: Wine reacts to
        // NotifyGrab by releasing the game's capture. Failed grabs remain inactive
        // and may be retried on the next frame.
        void setGrabStates(bool pointer, bool keyboard);
        bool grabActive() const;
        bool pointerGrabActive() const;
        bool gameFocused();
        // Last focus answer, without asking the server.
        bool hasFocus() const;

        // Drains pending events into the key/button/pointer state. Once per frame.
        void pump();

        void beginKeyboardFrame();
        bool isKeyPressed(uint32_t keysym);
        KeyboardState keyboardState();
        MouseState mouseState();

    private:
        // connection (x11_display.cc)
        _XDisplay* connection();
        bool xtestReady();
        void handOffHeldKeys(bool toGame);
        void handOffHeldButtons(bool toGame);
        void trackGrabbedInput(uint32_t keycode, uint32_t button, bool key, bool pressed);
        bool queryGameFocused(unsigned long gameWindow);
        bool gameFocusedLocked(unsigned long gameWindow);
        void initializeCapturedPointer(unsigned long window);
        void setGrabStatesLocked(bool pointer, bool keyboard);
        void pumpLocked();
        bool capturedMouseState(MouseState& state);

        // keyboard (keyboard_input_x11.cc)
        void ensureKeyboardPrimed();
        void processKey(unsigned int keycode, bool pressed);
        void trackModifier(unsigned long keysym, bool pressed);

        // mouse (mouse_input_x11.cc)
        void onRawButton(uint32_t button, bool pressed);

        mutable std::recursive_mutex m_mutex;

        _XDisplay* m_display = nullptr;
        bool m_tried = false;
        int m_xiOpcode = 0;
        bool m_xiReady = false;
        unsigned long m_window = 0;

        bool m_pointerGrabActive = false;
        bool m_keyboardGrabActive = false;
        bool m_pointerHandoff = false;
        bool m_keyboardHandoff = false;
        bool m_xtestChecked = false;
        bool m_xtestReady = false;
        unsigned long m_captureCursor = 0;
        double m_pointerX = 0.0;
        double m_pointerY = 0.0;
        int m_pointerWidth = 1;
        int m_pointerHeight = 1;
        bool m_buttons[3] = {};
        float m_scroll = 0.0f;
        bool m_pumping = false;

        // Keys and buttons this layer released into the game when it grabbed.
        // The synthetic releases clear the server's key/button state, so a
        // re-query at release time cannot tell what is still physically down;
        // these sets are kept current from the events the grab delivers and are
        // what gets handed back. Same idea as the Wayland interpose's held keys.
        std::vector<unsigned int> m_handedKeys;
        std::vector<unsigned int> m_handedButtons;

        // Whether the game window has the keyboard focus. Answering takes
        // XGetInputFocus plus an XQueryTree walk, each a blocking round trip, and
        // it is asked several times per frame. The answer is cached and dropped
        // when a FocusIn/FocusOut arrives on the game window or one of its
        // ancestors (selected below); the TTL only covers focus moves those
        // events cannot see, such as a window manager reparenting.
        struct FocusCache
        {
            bool valid = false;
            bool focused = false;
            unsigned long window = 0;
            std::chrono::steady_clock::time_point at{};
        };
        FocusCache m_focus;
        std::vector<unsigned long> m_focusWatched;  // windows with FocusChangeMask selected
        std::atomic<bool> m_focusObserved{false};

        // keyboard state
        std::string m_typedChars;
        std::string m_lastKeyName;
        std::vector<NavKey> m_navPresses;
        // modifier key counts (left + right), kept from the key events themselves so
        // no server query is needed to know what is held
        int m_shiftDown = 0;
        int m_ctrlDown = 0;
        int m_altDown = 0;
        int m_superDown = 0;
        uint64_t m_keymapFrameId = 0;
        uint64_t m_lastQueriedKeymapFrame = UINT64_MAX;
        char m_cachedKeymap[32] = {};
        bool m_keyboardPrimed = false;
        std::unordered_map<uint32_t, unsigned char> m_keysymToKeycode;

        // wheel ticks seen as raw button presses while no grab is up
        float m_rawScroll = 0.0f;
    };
} // namespace VkHook
