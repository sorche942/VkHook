#include "x11_display.hh"

#include "mouse_input.hh"
#include "logger.hh"
#include "wayland_display.hh"

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/XTest.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace VKIntox
{
    namespace
    {
        Display* g_display = nullptr;
        bool g_tried = false;
        int g_xiOpcode = 0;
        bool g_xiReady = false;

        X11KeyCallback g_keyCallback = nullptr;
        X11ButtonCallback g_buttonCallback = nullptr;
        bool g_pointerGrabActive = false;
        bool g_keyboardGrabActive = false;
        bool g_pointerHandoff = false;
        bool g_keyboardHandoff = false;
        bool g_xtestChecked = false;
        bool g_xtestReady = false;
        Cursor g_captureCursor = None;
        double g_pointerX = 0.0;
        double g_pointerY = 0.0;
        int g_pointerWidth = 1;
        int g_pointerHeight = 1;
        bool g_buttons[3] = {};
        float g_scroll = 0.0f;
        bool g_pumping = false;

        // Keys and buttons this layer released into the game when it grabbed.
        // The synthetic releases clear the server's key/button state, so a
        // re-query at release time cannot tell what is still physically down;
        // these sets are kept current from the events the grab delivers and are
        // what gets handed back. Same idea as the Wayland interpose's held keys.
        std::vector<unsigned int> g_handedKeys;
        std::vector<unsigned int> g_handedButtons;

        void initX11()
        {
            if (g_tried)
                return;
            g_tried = true;

            const char* disVar = getenv("DISPLAY");
            if (!disVar || !*disVar)
                return;

            g_display = XOpenDisplay(disVar);
            if (!g_display)
            {
                Logger::warn("X11 input: could not open DISPLAY, keyboard/mouse/blocking disabled");
                return;
            }

            int event = 0, error = 0;
            if (!XQueryExtension(g_display, "XInputExtension", &g_xiOpcode, &event, &error))
                return;

            // XI 2.0 suppresses root-selected raw events during a core grab.
            // XI 2.1 keeps delivering them, which the independent cursor needs:
            // the core grab itself only redirects core button/key events.
            int major = 2, minor = 1;
            if (XIQueryVersion(g_display, &major, &minor) != Success
                || major < 2 || (major == 2 && minor < 1))
            {
                Logger::warn("X11 input: XI 2.1 is required for independent cursor motion");
                return;
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
            XISelectEvents(g_display, DefaultRootWindow(g_display), &em, 1);
            XFlush(g_display);
            g_xiReady = true;
        }
    }

    _XDisplay* x11Display()
    {
        initX11();
        return g_display;
    }

    bool x11Available()
    {
        initX11();
        return g_display != nullptr;
    }

    unsigned long x11GameWindow()
    {
        return getX11Window();
    }

    void setX11KeyCallback(X11KeyCallback cb)
    {
        g_keyCallback = cb;
    }

    void setX11ButtonCallback(X11ButtonCallback cb)
    {
        g_buttonCallback = cb;
    }

    // XTest is how held input is handed across a grab transition. Without it the
    // grab still works, the game is just left holding whatever was down.
    static bool xtestReady(_XDisplay* display)
    {
        if (g_xtestChecked)
            return g_xtestReady;
        g_xtestChecked = true;

        Display* d = reinterpret_cast<Display*>(display);
        int event = 0, error = 0, major = 0, minor = 0;
        g_xtestReady = d != nullptr && XTestQueryExtension(d, &event, &error, &major, &minor);
        if (!g_xtestReady)
            Logger::warn("X11 input: XTest extension missing; keys held across a grab stay stuck in the game");
        return g_xtestReady;
    }

    // Synthetic releases (toGame = false) for every key the server reports as
    // physically down, so the game's key state matches the keyboard before the
    // grab takes the events away; synthetic presses (toGame = true) for the keys
    // this layer released and has seen no real release for, so a held movement
    // key keeps working. XTest events travel the normal input path, so the game
    // sees them as its own. Must not run while our own keyboard grab is up: they
    // would be redirected into the layer instead of the game.
    static void handOffHeldKeys(bool toGame)
    {
        if (!g_display)
            return;

        if (toGame)
        {
            if (g_handedKeys.empty())
                return;
            for (unsigned int keycode : g_handedKeys)
                XTestFakeKeyEvent(g_display, keycode, True, 0);
            XSync(g_display, False);
            Logger::debug("X11 input: re-pressed " + std::to_string(g_handedKeys.size())
                          + " held key(s) after the keyboard grab");
            g_handedKeys.clear();
            return;
        }

        g_handedKeys.clear();
        if (!xtestReady(g_display))
            return;

        char keys[32] = {0};
        XQueryKeymap(g_display, keys);
        for (int keycode = 8; keycode < 256; ++keycode)
        {
            if (!(keys[keycode / 8] & (1 << (keycode % 8))))
                continue;
            XTestFakeKeyEvent(g_display, (unsigned int)keycode, False, 0);
            g_handedKeys.push_back((unsigned int)keycode);
        }
        XSync(g_display, False);

        if (!g_handedKeys.empty())
            Logger::debug("X11 input: released " + std::to_string(g_handedKeys.size())
                          + " held key(s) before the keyboard grab");
    }

    // Same contract as handOffHeldKeys, for the buttons the pointer grab takes
    // away. Core button state only covers 1-5; anything else is not holdable.
    static void handOffHeldButtons(bool toGame)
    {
        if (g_display == nullptr)
            return;

        if (toGame)
        {
            if (g_handedButtons.empty())
                return;
            for (unsigned int button : g_handedButtons)
                XTestFakeButtonEvent(g_display, button, True, 0);
            XSync(g_display, False);
            Logger::debug("X11 input: re-pressed " + std::to_string(g_handedButtons.size())
                          + " held button(s) after the pointer grab");
            g_handedButtons.clear();
            return;
        }

        g_handedButtons.clear();
        if (!xtestReady(g_display))
            return;

        Window rootRet = 0, childRet = 0;
        int rootX = 0, rootY = 0, winX = 0, winY = 0;
        unsigned int mask = 0;
        if (!XQueryPointer(g_display, DefaultRootWindow(g_display), &rootRet, &childRet,
                           &rootX, &rootY, &winX, &winY, &mask))
            return;

        for (unsigned int button = 1; button <= 5; ++button)
        {
            if (!(mask & (Button1Mask << (button - 1))))
                continue;
            XTestFakeButtonEvent(g_display, button, False, 0);
            g_handedButtons.push_back(button);
        }
        XSync(g_display, False);

        if (!g_handedButtons.empty())
            Logger::debug("X11 input: released " + std::to_string(g_handedButtons.size())
                          + " held button(s) before the pointer grab");
    }

    // Keeps the handed-over sets honest while a grab is up: the grab is where
    // press and release events for those keys and buttons arrive, so this is the
    // only place that can tell whether the player let go in the meantime.
    static void trackGrabbedInput(uint32_t keycode, uint32_t button, bool key, bool pressed)
    {
        std::vector<unsigned int>& set = key ? g_handedKeys : g_handedButtons;
        const unsigned int code = key ? keycode : button;
        auto it = std::find(set.begin(), set.end(), code);

        // Only restore input that was held BEFORE capture. A click/key begun in
        // the menu must never become a synthetic press in the game on closing.
        if (!pressed && it != set.end())
        {
            set.erase(it);
        }
    }


    static bool gameFocused(Window gameWindow)
    {
        Window focus = None;
        int revert = 0;
        XGetInputFocus(g_display, &focus, &revert);
        if (focus == PointerRoot)
        {
            Window root, child;
            int rx, ry, wx, wy;
            unsigned int mask;
            if (!XQueryPointer(g_display, DefaultRootWindow(g_display), &root, &child,
                               &rx, &ry, &wx, &wy, &mask))
                return false;
            focus = child;
        }
        const Window focusWindow = focus;
        while (focus != None && focus != DefaultRootWindow(g_display))
        {
            if (focus == gameWindow)
                return true;
            Window root, parent, *children = nullptr;
            unsigned int count = 0;
            if (!XQueryTree(g_display, focus, &root, &parent, &children, &count))
                break;
            if (children)
                XFree(children);
            focus = parent;
        }
        // A Vulkan surface may be a child of Wine's focused top-level window.
        // Accept that ancestor too, but never a root/PointerRoot desktop focus.
        if (focusWindow != None && focusWindow != DefaultRootWindow(g_display))
        {
            while (gameWindow != None && gameWindow != DefaultRootWindow(g_display))
            {
                if (gameWindow == focusWindow)
                    return true;
                Window root, parent, *children = nullptr;
                unsigned int count = 0;
                if (!XQueryTree(g_display, gameWindow, &root, &parent, &children, &count))
                    break;
                if (children)
                    XFree(children);
                gameWindow = parent;
            }
        }
        return false;
    }

    static void initializeCapturedPointer(Window window)
    {
        XWindowAttributes attr{};
        if (XGetWindowAttributes(g_display, window, &attr))
        {
            g_pointerWidth = std::max(attr.width, 1);
            g_pointerHeight = std::max(attr.height, 1);
        }
        g_pointerX = g_pointerWidth / 2.0;
        g_pointerY = g_pointerHeight / 2.0;
        for (bool& button : g_buttons)
            button = false;
        g_scroll = 0.0f;
        if (g_captureCursor == None)
        {
            const char empty = 0;
            Pixmap bitmap = XCreateBitmapFromData(g_display, window, &empty, 1, 1);
            XColor color{};
            g_captureCursor = XCreatePixmapCursor(g_display, bitmap, bitmap, &color, &color, 0, 0);
            XFreePixmap(g_display, bitmap);
        }
    }

    // The one place grabs change. Both grabs are server-side and on this
    // connection, so the game stops seeing the events entirely; the held-input
    // handoff around the first engage and the last release is what keeps the
    // game's state honest.
    void x11SetGrabStates(bool wantPointer, bool wantKeyboard)
    {
        // Open the connection here rather than requiring a poll first: a grab
        // request must not silently do nothing just because no frame has run.
        if (x11Display() == nullptr)
            return;
        const Window gameWindow = (Window)x11GameWindow();
        const bool focused = gameWindow != None && gameFocused(gameWindow);
        if (!focused)
        {
            wantPointer = false;
            wantKeyboard = false;
            // Never inject held input into a newly focused application.
            g_handedKeys.clear();
            g_handedButtons.clear();
        }
        if (wantPointer == g_pointerGrabActive && wantKeyboard == g_keyboardGrabActive
            && (wantPointer || !g_pointerHandoff) && (wantKeyboard || !g_keyboardHandoff))
            return;

        // Drain the old source before changing ownership, so raw/core copies
        // pending on either side of the grab cannot count a wheel tick twice.
        if (!g_pumping)
            pumpX11Input();
        const Window grabWindow = gameWindow;

        // Wine processes NotifyGrab asynchronously. Acquire the keyboard first
        // and retain that ownership while retrying an AlreadyGrabbed pointer.
        if (wantKeyboard && !g_keyboardGrabActive)
        {
            if (!g_keyboardHandoff)
            {
                handOffHeldKeys(false);
                g_keyboardHandoff = true;
            }
            const int status = XGrabKeyboard(g_display, grabWindow, False,
                                             GrabModeAsync, GrabModeAsync, CurrentTime);
            g_keyboardGrabActive = status == GrabSuccess;
            if (g_keyboardGrabActive && g_pointerGrabActive)
            {
                initializeCapturedPointer(grabWindow);
                XChangeActivePointerGrab(g_display, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                         g_captureCursor, CurrentTime);
            }
            if (status != GrabSuccess && status != AlreadyGrabbed)
                Logger::warn("X11 input: keyboard grab failed (" + std::to_string(status) + ")");
        }

        if (wantPointer != g_pointerGrabActive || (!wantPointer && g_pointerHandoff))
        {
            if (wantPointer)
            {
                // Release only once across retries while Wine processes focus.
                if (!g_pointerHandoff)
                {
                    handOffHeldButtons(false);
                    g_pointerHandoff = true;
                }
                initializeCapturedPointer(grabWindow);
                const int status = XGrabPointer(g_display, grabWindow, False,
                                                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                                GrabModeAsync, GrabModeAsync,
                                                g_keyboardGrabActive ? grabWindow : None,
                                                g_keyboardGrabActive ? g_captureCursor : None, CurrentTime);
                g_pointerGrabActive = status == GrabSuccess;
                if (status != GrabSuccess && status != AlreadyGrabbed)
                    Logger::warn("X11 input: pointer grab failed (" + std::to_string(status) + ")");
            }
            else
            {
                XUngrabPointer(g_display, CurrentTime);
                // now that button events reach the game again, hand back what we
                // took: the releases cleared the server's button state, so only
                // what we tracked can be re-pressed
                handOffHeldButtons(true);
                g_pointerHandoff = false;
                g_pointerGrabActive = false;
                for (bool& button : g_buttons)
                    button = false;
                g_scroll = 0.0f;
            }
        }

        // Release the pointer before allowing Wine to reacquire its capture.
        if (!wantKeyboard && (g_keyboardGrabActive || g_keyboardHandoff))
        {
            XUngrabKeyboard(g_display, CurrentTime);
            handOffHeldKeys(true);
            g_keyboardHandoff = false;
            g_keyboardGrabActive = false;
            if (g_pointerGrabActive)
                XChangeActivePointerGrab(g_display, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                         None, CurrentTime);
        }

        XFlush(g_display);

        Logger::debug(std::string("X11 input: grabs pointer=") + (g_pointerGrabActive ? "on" : "off")
                      + " keyboard=" + (g_keyboardGrabActive ? "on" : "off"));
    }


    bool x11GrabActive()
    {
        return g_pointerGrabActive || g_keyboardGrabActive;
    }

    bool x11PointerGrabActive()
    {
        return g_pointerGrabActive;
    }

    bool x11GameFocused()
    {
        return x11Available() && gameFocused((Window)x11GameWindow());
    }

    bool x11CapturedMouseState(MouseState& state)
    {
        if (!g_pointerGrabActive || !g_keyboardGrabActive)
            return false;
        state.x = static_cast<int>(g_pointerX);
        state.y = static_cast<int>(g_pointerY);
        state.leftButton = g_buttons[0];
        state.middleButton = g_buttons[1];
        state.rightButton = g_buttons[2];
        state.scrollDelta = g_scroll;
        g_scroll = 0.0f;
        return true;
    }

    void pumpX11Input()
    {
        if (!x11Available())
            return;
        g_pumping = true;
        if ((g_pointerGrabActive || g_keyboardGrabActive) && !gameFocused((Window)x11GameWindow()))
            x11SetGrabStates(false, false);

        while (XPending(g_display) > 0)
        {
            XEvent ev;
            XNextEvent(g_display, &ev);

            if (ev.type == GenericEvent && ev.xcookie.extension == g_xiOpcode
                && XGetEventData(g_display, &ev.xcookie))
            {
                switch (ev.xcookie.evtype)
                {
                case XI_RawMotion:
                    if (g_pointerGrabActive && g_keyboardGrabActive)
                    {
                        const auto* motion = static_cast<XIRawEvent*>(ev.xcookie.data);
                        const double* value = motion->raw_values;
                        for (int axis = 0; axis < motion->valuators.mask_len * 8; ++axis)
                        {
                            if (!XIMaskIsSet(motion->valuators.mask, axis))
                                continue;
                            if (axis == 0)
                                g_pointerX = std::clamp(g_pointerX + *value, 0.0, double(g_pointerWidth - 1));
                            else if (axis == 1)
                                g_pointerY = std::clamp(g_pointerY + *value, 0.0, double(g_pointerHeight - 1));
                            ++value;
                        }
                    }
                    break;
                case XI_RawKeyPress:
                case XI_RawKeyRelease:
                    // the keyboard grab makes core events authoritative; raw
                    // here would double every press on servers that still
                    // deliver it
                    if (!g_keyboardGrabActive && g_keyCallback)
                    {
                        const bool pressed = ev.xcookie.evtype == XI_RawKeyPress;
                        g_keyCallback(((XIRawEvent*)ev.xcookie.data)->detail, pressed);
                    }
                    break;
                case XI_RawButtonPress:
                case XI_RawButtonRelease:
                    if (!g_pointerGrabActive && g_buttonCallback)
                    {
                        const bool pressed = ev.xcookie.evtype == XI_RawButtonPress;
                        g_buttonCallback(((XIRawEvent*)ev.xcookie.data)->detail, pressed);
                    }
                    break;
                default:
                    break;
                }
                XFreeEventData(g_display, &ev.xcookie);
            }
            else if (ev.type == KeyPress || ev.type == KeyRelease)
            {
                // consumed while grabbed (raw events can stop during a core
                // grab) or as the no-XInput2 fallback; outside both, raw
                // already delivered this key and core would double it.
                if ((g_keyboardGrabActive || !g_xiReady) && g_keyCallback)
                    g_keyCallback(ev.xkey.keycode, ev.type == KeyPress);
                if (g_keyboardGrabActive)
                    trackGrabbedInput(ev.xkey.keycode, 0, true, ev.type == KeyPress);
            }
            else if (ev.type == ButtonPress || ev.type == ButtonRelease)
            {
                if ((g_pointerGrabActive || !g_xiReady) && g_buttonCallback)
                    g_buttonCallback(ev.xbutton.button, ev.type == ButtonPress);
                if (g_pointerGrabActive)
                {
                    const unsigned int button = ev.xbutton.button;
                    const bool pressed = ev.type == ButtonPress;
                    if (button >= 1 && button <= 3)
                        g_buttons[button - 1] = pressed;
                    else if (pressed && (button == 4 || button == 5))
                        g_scroll += button == 4 ? 1.0f : -1.0f;
                    trackGrabbedInput(0, button, false, pressed);
                }
            }
        }
        g_pumping = false;
    }
} // namespace VKIntox
