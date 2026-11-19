#include "keyboard_input_x11.hh"

#include "logger.hh"
#include "x11_display.hh"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace VkHook
{
    static std::string typedCharsAccumulator;
    static std::string lastKeyNameAccumulator;
    static std::vector<NavKey> navAccumulator;
    // modifier key counts (left + right), kept from the key events themselves so
    // no server query is needed to know what is held
    static int shiftDown = 0;
    static int ctrlDown = 0;
    static int altDown = 0;
    static int superDown = 0;
    static uint64_t keymapFrameId = 0;
    static uint64_t lastQueriedKeymapFrame = std::numeric_limits<uint64_t>::max();
    static char cachedKeymap[32] = {};
    static std::unordered_map<uint32_t, KeyCode> keysymToKeycodeCache;

    static bool isModifierKeysym(KeySym keysym)
    {
        return keysym == XK_Shift_L || keysym == XK_Shift_R || keysym == XK_Control_L || keysym == XK_Control_R ||
               keysym == XK_Alt_L || keysym == XK_Alt_R || keysym == XK_Super_L || keysym == XK_Super_R ||
               keysym == XK_Meta_L || keysym == XK_Meta_R || keysym == XK_ISO_Level3_Shift;
    }

    static void trackModifier(KeySym keysym, bool pressed)
    {
        const int delta = pressed ? 1 : -1;
        int* counter = nullptr;
        if (keysym == XK_Shift_L || keysym == XK_Shift_R) counter = &shiftDown;
        else if (keysym == XK_Control_L || keysym == XK_Control_R) counter = &ctrlDown;
        else if (keysym == XK_Alt_L || keysym == XK_Alt_R || keysym == XK_Meta_L || keysym == XK_Meta_R) counter = &altDown;
        else if (keysym == XK_Super_L || keysym == XK_Super_R) counter = &superDown;
        if (counter)
            *counter = std::max(0, *counter + delta);
    }

    static bool navKeyFor(KeySym keysym, NavKey& out)
    {
        switch (keysym)
        {
            case XK_BackSpace: out = NavKey::Backspace; return true;
            case XK_Delete: case XK_KP_Delete: out = NavKey::Delete; return true;
            case XK_Return: case XK_KP_Enter: out = NavKey::Enter; return true;
            case XK_Left: out = NavKey::Left; return true;
            case XK_Right: out = NavKey::Right; return true;
            case XK_Up: out = NavKey::Up; return true;
            case XK_Down: out = NavKey::Down; return true;
            case XK_Home: out = NavKey::Home; return true;
            case XK_End: out = NavKey::End; return true;
            case XK_Prior: out = NavKey::PageUp; return true;
            case XK_Next: out = NavKey::PageDown; return true;
            case XK_Tab: case XK_ISO_Left_Tab: out = NavKey::Tab; return true;
            case XK_Escape: out = NavKey::Escape; return true;
            default: return false;
        }
    }

    // Latin-1 and Unicode keysyms map straight to code points; anything else
    // (function keys, dead keys) types nothing.
    static uint32_t codepointFor(KeySym keysym)
    {
        if ((keysym >= 0x20 && keysym <= 0x7E) || (keysym >= 0xA0 && keysym <= 0xFF))
            return static_cast<uint32_t>(keysym);
        if (keysym >= 0x01000100 && keysym <= 0x0110FFFF)
            return static_cast<uint32_t>(keysym - 0x01000000);
        if (keysym >= XK_KP_0 && keysym <= XK_KP_9)
            return static_cast<uint32_t>('0' + (keysym - XK_KP_0));
        return 0;
    }

    static void processKey(KeyCode keycode, bool pressed)
    {
        Display* display = x11Display();
        if (!display)
            return;

        const KeySym base = XkbKeycodeToKeysym(display, keycode, 0, 0);
        trackModifier(base, pressed);
        if (!pressed)
            return;

        // capture the key name for the keybind editor, skipping modifiers.
        if (!isModifierKeysym(base))
        {
            if (const char* keyName = XKeysymToString(base))
                lastKeyNameAccumulator = keyName;
        }

        NavKey nav;
        if (navKeyFor(base, nav))
        {
            if (navAccumulator.size() < kMaxNavPresses)
                navAccumulator.push_back(nav);
            return;
        }

        // Ctrl/Alt/Super chords are shortcuts, not text
        if (ctrlDown || altDown || superDown)
            return;

        const KeySym actual = XkbKeycodeToKeysym(display, keycode, 0, shiftDown ? 1 : 0);
        if (const uint32_t cp = codepointFor(actual))
            appendUtf8(typedCharsAccumulator, cp);
    }

    static void onRawKey(uint32_t keycode, bool pressed)
    {
        processKey(static_cast<KeyCode>(keycode), pressed);
    }

    static void ensureRegistered()
    {
        static bool done = false;
        if (done)
            return;
        done = true;
        setX11KeyCallback(onRawKey);
        // cache the keymap once so the first shifted character is correct.
        if (Display* display = x11Display())
            XQueryKeymap(display, cachedKeymap);
    }

    void beginKeyboardInputFrameX11()
    {
        keymapFrameId++;
    }

    uint32_t convertToKeySymX11(std::string key)
    {
        const uint32_t result = (uint32_t)XStringToKeysym(key.c_str());
        if (!result)
            Logger::err("invalid key");
        return result;
    }

    bool isKeyPressedX11(uint32_t ks)
    {
        Display* display = x11Display();
        if (!display)
            return false;

        ensureRegistered();

        // hotkeys belong to the game window: polling the keyboard from here
        // would otherwise fire them while another application has the focus.
        // x11GameFocused() answers from a cache, so this costs no round trip.
        if (!x11GameFocused())
            return false;

        if (lastQueriedKeymapFrame != keymapFrameId)
        {
            XQueryKeymap(display, cachedKeymap);
            lastQueriedKeymapFrame = keymapFrameId;
        }

        KeyCode keycode = 0;
        auto cached = keysymToKeycodeCache.find(ks);
        if (cached != keysymToKeycodeCache.end())
            keycode = cached->second;
        else
        {
            keycode = XKeysymToKeycode(display, (KeySym)ks);
            keysymToKeycodeCache.emplace(ks, keycode);
        }

        if (keycode == 0)
            return false;

        return !!(cachedKeymap[keycode >> 3] & (1 << (keycode & 7)));
    }

    KeyboardState getKeyboardStateX11()
    {
        KeyboardState state;
        if (!x11Available())
            return state;

        ensureRegistered();
        pumpX11Input();

        state.typedChars = std::move(typedCharsAccumulator);
        state.lastKeyName = std::move(lastKeyNameAccumulator);
        state.navPresses = std::move(navAccumulator);
        state.shift = shiftDown > 0;
        state.ctrl = ctrlDown > 0;
        state.alt = altDown > 0;
        state.super = superDown > 0;

        typedCharsAccumulator.clear();
        lastKeyNameAccumulator.clear();
        navAccumulator.clear();

        return state;
    }
} // namespace VkHook
