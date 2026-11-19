#include "x11_display.hh"

#include "x11_input.hh"

#include "logger.hh"

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <vector>

namespace VkHook
{
    namespace
    {
        constexpr std::chrono::milliseconds kFocusTtl{250};
    }

    X11Input::X11Input() = default;

    X11Input::~X11Input()
    {
        close();
    }

    void X11Input::close()
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_display)
        {
            // Both grabs end with the connection, but the held-input handoff must
            // run first or the game is left with keys the player let go of.
            setGrabStatesLocked(false, false);
            if (m_captureCursor != None)
                XFreeCursor(m_display, m_captureCursor);
            XCloseDisplay(m_display);
            m_display = nullptr;
        }
        // a connection is never opened again
        m_tried = true;
    }

    _XDisplay* X11Input::connection()
    {
        if (m_tried)
            return m_display;
        m_tried = true;

        const char* disVar = getenv("DISPLAY");
        if (!disVar || !*disVar)
            return nullptr;

        m_display = XOpenDisplay(disVar);
        if (!m_display)
        {
            Logger::warn("X11 input: could not open DISPLAY, keyboard/mouse/blocking disabled");
            return nullptr;
        }

        int event = 0, error = 0;
        if (!XQueryExtension(m_display, "XInputExtension", &m_xiOpcode, &event, &error))
            return m_display;

        // XI 2.0 suppresses root-selected raw events during a core grab.
        // XI 2.1 keeps delivering them, which the independent cursor needs:
        // the core grab itself only redirects core button/key events.
        int major = 2, minor = 1;
        if (XIQueryVersion(m_display, &major, &minor) != Success
            || major < 2 || (major == 2 && minor < 1))
        {
            Logger::warn("X11 input: XI 2.1 is required for independent cursor motion");
            return m_display;
        }

        // Core events remain authoritative for grabbed keys/buttons, while
        // raw motion continues independently of game-controlled warps.
        unsigned char mask[XIMaskLen(XI_RawMotion)] = {0};
        XISetMask(mask, XI_RawKeyPress);
        XISetMask(mask, XI_RawKeyRelease);
        XISetMask(mask, XI_RawButtonPress);
        XISetMask(mask, XI_RawButtonRelease);
        XISetMask(mask, XI_RawMotion);
        XIEventMask em = {XIAllMasterDevices, sizeof(mask), mask};
        XISelectEvents(m_display, DefaultRootWindow(m_display), &em, 1);
        XFlush(m_display);
        m_xiReady = true;
        return m_display;
    }

    void X11Input::setWindow(unsigned long window)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (window == m_window)
            return;

        // grabs belong to the window they were taken on
        if (m_display)
            setGrabStatesLocked(false, false);
        m_window = window;
        m_focus.valid = false;
        m_focusObserved.store(false, std::memory_order_release);
    }

    unsigned long X11Input::window() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_window;
    }

    // XTest is how held input is handed across a grab transition. Without it the
    // grab still works, the game is just left holding whatever was down.
    bool X11Input::xtestReady()
    {
        if (m_xtestChecked)
            return m_xtestReady;
        m_xtestChecked = true;

        int event = 0, error = 0, major = 0, minor = 0;
        m_xtestReady = m_display != nullptr && XTestQueryExtension(m_display, &event, &error, &major, &minor);
        if (!m_xtestReady)
            Logger::warn("X11 input: XTest extension missing; keys held across a grab stay stuck in the game");
        return m_xtestReady;
    }

    // Synthetic releases (toGame = false) for every key the server reports as
    // physically down, so the game's key state matches the keyboard before the
    // grab takes the events away; synthetic presses (toGame = true) for the keys
    // this layer released and has seen no real release for, so a held movement
    // key keeps working. XTest events travel the normal input path, so the game
    // sees them as its own. Must not run while our own keyboard grab is up: they
    // would be redirected into the layer instead of the game.
    void X11Input::handOffHeldKeys(bool toGame)
    {
        if (!m_display)
            return;

        if (toGame)
        {
            if (m_handedKeys.empty())
                return;
            for (unsigned int keycode : m_handedKeys)
                XTestFakeKeyEvent(m_display, keycode, True, 0);
            XSync(m_display, False);
            Logger::debug("X11 input: re-pressed " + std::to_string(m_handedKeys.size())
                          + " held key(s) after the keyboard grab");
            m_handedKeys.clear();
            return;
        }

        m_handedKeys.clear();
        if (!xtestReady())
            return;

        char keys[32] = {0};
        XQueryKeymap(m_display, keys);
        for (int keycode = 8; keycode < 256; ++keycode)
        {
            if (!(keys[keycode / 8] & (1 << (keycode % 8))))
                continue;
            XTestFakeKeyEvent(m_display, (unsigned int)keycode, False, 0);
            m_handedKeys.push_back((unsigned int)keycode);
        }
        XSync(m_display, False);

        if (!m_handedKeys.empty())
            Logger::debug("X11 input: released " + std::to_string(m_handedKeys.size())
                          + " held key(s) before the keyboard grab");
    }

    // Same contract as handOffHeldKeys, for the buttons the pointer grab takes
    // away. Core button state only covers 1-5; anything else is not holdable.
    void X11Input::handOffHeldButtons(bool toGame)
    {
        if (m_display == nullptr)
            return;

        if (toGame)
        {
            if (m_handedButtons.empty())
                return;
            for (unsigned int button : m_handedButtons)
                XTestFakeButtonEvent(m_display, button, True, 0);
            XSync(m_display, False);
            Logger::debug("X11 input: re-pressed " + std::to_string(m_handedButtons.size())
                          + " held button(s) after the pointer grab");
            m_handedButtons.clear();
            return;
        }

        m_handedButtons.clear();
        if (!xtestReady())
            return;

        Window rootRet = 0, childRet = 0;
        int rootX = 0, rootY = 0, winX = 0, winY = 0;
        unsigned int mask = 0;
        if (!XQueryPointer(m_display, DefaultRootWindow(m_display), &rootRet, &childRet,
                           &rootX, &rootY, &winX, &winY, &mask))
            return;

        for (unsigned int button = 1; button <= 5; ++button)
        {
            if (!(mask & (Button1Mask << (button - 1))))
                continue;
            XTestFakeButtonEvent(m_display, button, False, 0);
            m_handedButtons.push_back(button);
        }
        XSync(m_display, False);

        if (!m_handedButtons.empty())
            Logger::debug("X11 input: released " + std::to_string(m_handedButtons.size())
                          + " held button(s) before the pointer grab");
    }

    // Keeps the handed-over sets honest while a grab is up: the grab is where
    // press and release events for those keys and buttons arrive, so this is the
    // only place that can tell whether the player let go in the meantime.
    void X11Input::trackGrabbedInput(uint32_t keycode, uint32_t button, bool key, bool pressed)
    {
        std::vector<unsigned int>& set = key ? m_handedKeys : m_handedButtons;
        const unsigned int code = key ? keycode : button;
        auto it = std::find(set.begin(), set.end(), code);

        // Only restore input that was held BEFORE capture. A click/key begun in
        // the menu must never become a synthetic press in the game on closing.
        if (!pressed && it != set.end())
        {
            set.erase(it);
        }
    }

    // The slow path: queries the server. Also (re)selects focus events on the
    // game window and its ancestors, so the next change invalidates the cache.
    bool X11Input::queryGameFocused(unsigned long gameWindow)
    {
        Window focus = None;
        int revert = 0;
        XGetInputFocus(m_display, &focus, &revert);
        if (focus == PointerRoot)
        {
            Window root, child;
            int rx, ry, wx, wy;
            unsigned int mask;
            if (!XQueryPointer(m_display, DefaultRootWindow(m_display), &root, &child,
                               &rx, &ry, &wx, &wy, &mask))
                return false;
            focus = child;
        }

        auto watch = [this](Window window) {
            if (std::find(m_focusWatched.begin(), m_focusWatched.end(), window) != m_focusWatched.end())
                return;
            XSelectInput(m_display, window, FocusChangeMask);
            m_focusWatched.push_back(window);
        };

        // ancestors of the game window, nearest first
        std::vector<Window> gameChain;
        for (Window w = gameWindow; w != None && w != DefaultRootWindow(m_display);)
        {
            gameChain.push_back(w);
            watch(w);
            Window root, parent, *children = nullptr;
            unsigned int count = 0;
            if (!XQueryTree(m_display, w, &root, &parent, &children, &count))
                break;
            if (children)
                XFree(children);
            w = parent;
        }

        // the focused window is the game window or one of its descendants
        // (a Vulkan surface child of Wine's focused top-level counts too)
        const Window focusWindow = focus;
        for (Window f = focus; f != None && f != DefaultRootWindow(m_display);)
        {
            if (f == gameWindow)
                return true;
            Window root, parent, *children = nullptr;
            unsigned int count = 0;
            if (!XQueryTree(m_display, f, &root, &parent, &children, &count))
                break;
            if (children)
                XFree(children);
            f = parent;
        }
        // ... or one of its ancestors, but never a root/PointerRoot desktop focus
        if (focusWindow != None && focusWindow != DefaultRootWindow(m_display))
        {
            if (std::find(gameChain.begin(), gameChain.end(), focusWindow) != gameChain.end())
                return true;
        }
        return false;
    }

    bool X11Input::gameFocusedLocked(unsigned long gameWindow)
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_focus.valid && m_focus.window == gameWindow && now - m_focus.at < kFocusTtl)
            return m_focus.focused;

        m_focus.focused = gameWindow != None && queryGameFocused(gameWindow);
        m_focus.window = gameWindow;
        m_focus.at = now;
        m_focus.valid = true;
        m_focusObserved.store(m_focus.focused, std::memory_order_release);
        return m_focus.focused;
    }

    void X11Input::initializeCapturedPointer(unsigned long window)
    {
        XWindowAttributes attr{};
        if (XGetWindowAttributes(m_display, window, &attr))
        {
            m_pointerWidth = std::max(attr.width, 1);
            m_pointerHeight = std::max(attr.height, 1);
        }
        m_pointerX = m_pointerWidth / 2.0;
        m_pointerY = m_pointerHeight / 2.0;
        for (bool& button : m_buttons)
            button = false;
        m_scroll = 0.0f;
        if (m_captureCursor == None)
        {
            const char empty = 0;
            Pixmap bitmap = XCreateBitmapFromData(m_display, window, &empty, 1, 1);
            XColor color{};
            m_captureCursor = XCreatePixmapCursor(m_display, bitmap, bitmap, &color, &color, 0, 0);
            XFreePixmap(m_display, bitmap);
        }
    }

    // The one place grabs change. Both grabs are server-side and on this
    // connection, so the game stops seeing the events entirely; the held-input
    // handoff around the first engage and the last release is what keeps the
    // game's state honest.
    void X11Input::setGrabStatesLocked(bool wantPointer, bool wantKeyboard)
    {
        // Open the connection here rather than requiring a poll first: a grab
        // request must not silently do nothing just because no frame has run.
        if (connection() == nullptr)
            return;

        // Nothing wanted and nothing held: no query, no work. This runs on
        // every input-state change, several times a frame.
        if (!wantPointer && !wantKeyboard && !m_pointerGrabActive && !m_keyboardGrabActive
            && !m_pointerHandoff && !m_keyboardHandoff)
            return;

        const Window gameWindow = (Window)m_window;
        const bool focused = gameWindow != None && gameFocusedLocked(gameWindow);
        if (!focused)
        {
            wantPointer = false;
            wantKeyboard = false;
            // Never inject held input into a newly focused application.
            m_handedKeys.clear();
            m_handedButtons.clear();
        }
        if (wantPointer == m_pointerGrabActive && wantKeyboard == m_keyboardGrabActive
            && (wantPointer || !m_pointerHandoff) && (wantKeyboard || !m_keyboardHandoff))
            return;

        // Drain the old source before changing ownership, so raw/core copies
        // pending on either side of the grab cannot count a wheel tick twice.
        if (!m_pumping)
            pumpLocked();
        const Window grabWindow = gameWindow;

        // Wine processes NotifyGrab asynchronously. Acquire the keyboard first
        // and retain that ownership while retrying an AlreadyGrabbed pointer.
        if (wantKeyboard && !m_keyboardGrabActive)
        {
            if (!m_keyboardHandoff)
            {
                handOffHeldKeys(false);
                m_keyboardHandoff = true;
            }
            const int status = XGrabKeyboard(m_display, grabWindow, False,
                                             GrabModeAsync, GrabModeAsync, CurrentTime);
            m_keyboardGrabActive = status == GrabSuccess;
            if (m_keyboardGrabActive && m_pointerGrabActive)
            {
                initializeCapturedPointer(grabWindow);
                XChangeActivePointerGrab(m_display, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                         m_captureCursor, CurrentTime);
            }
            if (status != GrabSuccess && status != AlreadyGrabbed)
                Logger::warn("X11 input: keyboard grab failed (" + std::to_string(status) + ")");
        }

        if (wantPointer != m_pointerGrabActive || (!wantPointer && m_pointerHandoff))
        {
            if (wantPointer)
            {
                // Release only once across retries while Wine processes focus.
                if (!m_pointerHandoff)
                {
                    handOffHeldButtons(false);
                    m_pointerHandoff = true;
                }
                initializeCapturedPointer(grabWindow);
                const int status = XGrabPointer(m_display, grabWindow, False,
                                                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                                GrabModeAsync, GrabModeAsync,
                                                m_keyboardGrabActive ? grabWindow : None,
                                                m_keyboardGrabActive ? m_captureCursor : None, CurrentTime);
                m_pointerGrabActive = status == GrabSuccess;
                if (status != GrabSuccess && status != AlreadyGrabbed)
                    Logger::warn("X11 input: pointer grab failed (" + std::to_string(status) + ")");
            }
            else
            {
                XUngrabPointer(m_display, CurrentTime);
                // now that button events reach the game again, hand back what we
                // took: the releases cleared the server's button state, so only
                // what we tracked can be re-pressed
                handOffHeldButtons(true);
                m_pointerHandoff = false;
                m_pointerGrabActive = false;
                for (bool& button : m_buttons)
                    button = false;
                m_scroll = 0.0f;
            }
        }

        // Release the pointer before allowing Wine to reacquire its capture.
        if (!wantKeyboard && (m_keyboardGrabActive || m_keyboardHandoff))
        {
            XUngrabKeyboard(m_display, CurrentTime);
            handOffHeldKeys(true);
            m_keyboardHandoff = false;
            m_keyboardGrabActive = false;
            if (m_pointerGrabActive)
                XChangeActivePointerGrab(m_display, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                         None, CurrentTime);
        }

        XFlush(m_display);

        Logger::debug(std::string("X11 input: grabs pointer=") + (m_pointerGrabActive ? "on" : "off")
                      + " keyboard=" + (m_keyboardGrabActive ? "on" : "off"));
    }

    void X11Input::setGrabStates(bool wantPointer, bool wantKeyboard)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        setGrabStatesLocked(wantPointer, wantKeyboard);
    }

    bool X11Input::grabActive() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_pointerGrabActive || m_keyboardGrabActive;
    }

    bool X11Input::pointerGrabActive() const
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return m_pointerGrabActive;
    }

    bool X11Input::gameFocused()
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        return connection() != nullptr && gameFocusedLocked((Window)m_window);
    }

    bool X11Input::hasFocus() const
    {
        return m_focusObserved.load(std::memory_order_acquire);
    }

    // Supplies the virtual pointer and event-driven buttons while captured.
    // Raw device deltas, not game-controlled absolute pointer warps, move it.
    bool X11Input::capturedMouseState(MouseState& state)
    {
        if (!m_pointerGrabActive || !m_keyboardGrabActive)
            return false;
        state.x = static_cast<int>(m_pointerX);
        state.y = static_cast<int>(m_pointerY);
        state.leftButton = m_buttons[0];
        state.middleButton = m_buttons[1];
        state.rightButton = m_buttons[2];
        state.scrollDelta = m_scroll;
        m_scroll = 0.0f;
        return true;
    }

    void X11Input::pump()
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        pumpLocked();
    }

    void X11Input::pumpLocked()
    {
        if (!connection())
            return;
        m_pumping = true;
        if ((m_pointerGrabActive || m_keyboardGrabActive) && !gameFocusedLocked((Window)m_window))
            setGrabStatesLocked(false, false);

        while (XPending(m_display) > 0)
        {
            XEvent ev;
            XNextEvent(m_display, &ev);

            if (ev.type == GenericEvent && ev.xcookie.extension == m_xiOpcode
                && XGetEventData(m_display, &ev.xcookie))
            {
                switch (ev.xcookie.evtype)
                {
                case XI_RawMotion:
                    if (m_pointerGrabActive && m_keyboardGrabActive)
                    {
                        const auto* motion = static_cast<XIRawEvent*>(ev.xcookie.data);
                        const double* value = motion->raw_values;
                        for (int axis = 0; axis < motion->valuators.mask_len * 8; ++axis)
                        {
                            if (!XIMaskIsSet(motion->valuators.mask, axis))
                                continue;
                            if (axis == 0)
                                m_pointerX = std::clamp(m_pointerX + *value, 0.0, double(m_pointerWidth - 1));
                            else if (axis == 1)
                                m_pointerY = std::clamp(m_pointerY + *value, 0.0, double(m_pointerHeight - 1));
                            ++value;
                        }
                    }
                    break;
                case XI_RawKeyPress:
                case XI_RawKeyRelease:
                    // the keyboard grab makes core events authoritative; raw
                    // here would double every press on servers that still
                    // deliver it
                    if (!m_keyboardGrabActive)
                    {
                        const bool pressed = ev.xcookie.evtype == XI_RawKeyPress;
                        processKey(static_cast<unsigned int>(((XIRawEvent*)ev.xcookie.data)->detail), pressed);
                    }
                    break;
                case XI_RawButtonPress:
                case XI_RawButtonRelease:
                    if (!m_pointerGrabActive)
                    {
                        const bool pressed = ev.xcookie.evtype == XI_RawButtonPress;
                        onRawButton(((XIRawEvent*)ev.xcookie.data)->detail, pressed);
                    }
                    break;
                default:
                    break;
                }
                XFreeEventData(m_display, &ev.xcookie);
            }
            else if (ev.type == FocusIn || ev.type == FocusOut)
            {
                // focus moved on the game window or an ancestor: re-ask next time
                m_focus.valid = false;
            }
            else if (ev.type == KeyPress || ev.type == KeyRelease)
            {
                // consumed while grabbed (raw events can stop during a core
                // grab) or as the no-XInput2 fallback; outside both, raw
                // already delivered this key and core would double it.
                if (m_keyboardGrabActive || !m_xiReady)
                    processKey(ev.xkey.keycode, ev.type == KeyPress);
                if (m_keyboardGrabActive)
                    trackGrabbedInput(ev.xkey.keycode, 0, true, ev.type == KeyPress);
            }
            else if (ev.type == ButtonPress || ev.type == ButtonRelease)
            {
                if (m_pointerGrabActive || !m_xiReady)
                    onRawButton(ev.xbutton.button, ev.type == ButtonPress);
                if (m_pointerGrabActive)
                {
                    const unsigned int button = ev.xbutton.button;
                    const bool pressed = ev.type == ButtonPress;
                    if (button >= 1 && button <= 3)
                        m_buttons[button - 1] = pressed;
                    else if (pressed && (button == 4 || button == 5))
                        m_scroll += button == 4 ? 1.0f : -1.0f;
                    trackGrabbedInput(0, button, false, pressed);
                }
            }
        }
        m_pumping = false;
    }
} // namespace VkHook
