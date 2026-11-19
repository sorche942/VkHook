#include "keyboard_input_x11.hh"

#include "logger.hh"
#include "x11_input.hh"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>

#include <algorithm>
#include <string>

namespace VkHook
{
    static bool isModifierKeysym(KeySym keysym)
    {
        return keysym == XK_Shift_L || keysym == XK_Shift_R || keysym == XK_Control_L || keysym == XK_Control_R ||
               keysym == XK_Alt_L || keysym == XK_Alt_R || keysym == XK_Super_L || keysym == XK_Super_R ||
               keysym == XK_Meta_L || keysym == XK_Meta_R || keysym == XK_ISO_Level3_Shift;
    }

    void X11Input::trackModifier(unsigned long keysym, bool pressed)
    {
        const int delta = pressed ? 1 : -1;
        int* counter = nullptr;
        if (keysym == XK_Shift_L || keysym == XK_Shift_R) counter = &m_shiftDown;
        else if (keysym == XK_Control_L || keysym == XK_Control_R) counter = &m_ctrlDown;
        else if (keysym == XK_Alt_L || keysym == XK_Alt_R || keysym == XK_Meta_L || keysym == XK_Meta_R) counter = &m_altDown;
        else if (keysym == XK_Super_L || keysym == XK_Super_R) counter = &m_superDown;
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

    void X11Input::processKey(unsigned int rawKeycode, bool pressed)
    {
        Display* display = connection();
        if (!display)
            return;

        const KeyCode keycode = static_cast<KeyCode>(rawKeycode);
        const KeySym base = XkbKeycodeToKeysym(display, keycode, 0, 0);
        trackModifier(base, pressed);
        if (!pressed)
            return;

        // capture the key name for the keybind editor, skipping modifiers.
        if (!isModifierKeysym(base))
        {
            if (const char* keyName = XKeysymToString(base))
                m_lastKeyName = keyName;
        }

        NavKey nav;
        if (navKeyFor(base, nav))
        {
            if (m_navPresses.size() < kMaxNavPresses)
                m_navPresses.push_back(nav);
            return;
        }

        // Ctrl/Alt/Super chords are shortcuts, not text
        if (m_ctrlDown || m_altDown || m_superDown)
            return;

        const KeySym actual = XkbKeycodeToKeysym(display, keycode, 0, m_shiftDown ? 1 : 0);
        if (const uint32_t cp = codepointFor(actual))
            appendUtf8(m_typedChars, cp);
    }

    void X11Input::ensureKeyboardPrimed()
    {
        if (m_keyboardPrimed)
            return;
        m_keyboardPrimed = true;
        // cache the keymap once so the first shifted character is correct.
        if (Display* display = connection())
            XQueryKeymap(display, m_cachedKeymap);
    }

    void X11Input::beginKeyboardFrame()
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_keymapFrameId++;
    }

    uint32_t convertToKeySymX11(std::string key)
    {
        const uint32_t result = (uint32_t)XStringToKeysym(key.c_str());
        if (!result)
            Logger::err("invalid key");
        return result;
    }

    bool X11Input::isKeyPressed(uint32_t ks)
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        Display* display = connection();
        if (!display)
            return false;

        ensureKeyboardPrimed();

        // hotkeys belong to the game window: polling the keyboard from here
        // would otherwise fire them while another application has the focus.
        // gameFocused() answers from a cache, so this costs no round trip.
        if (!gameFocusedLocked(m_window))
            return false;

        if (m_lastQueriedKeymapFrame != m_keymapFrameId)
        {
            XQueryKeymap(display, m_cachedKeymap);
            m_lastQueriedKeymapFrame = m_keymapFrameId;
        }

        KeyCode keycode = 0;
        auto cached = m_keysymToKeycode.find(ks);
        if (cached != m_keysymToKeycode.end())
            keycode = cached->second;
        else
        {
            keycode = XKeysymToKeycode(display, (KeySym)ks);
            m_keysymToKeycode.emplace(ks, keycode);
        }

        if (keycode == 0)
            return false;

        return !!(m_cachedKeymap[keycode >> 3] & (1 << (keycode & 7)));
    }

    KeyboardState X11Input::keyboardState()
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        KeyboardState state;
        if (!connection())
            return state;

        ensureKeyboardPrimed();
        pumpLocked();

        state.typedChars = std::move(m_typedChars);
        state.lastKeyName = std::move(m_lastKeyName);
        state.navPresses = std::move(m_navPresses);
        state.shift = m_shiftDown > 0;
        state.ctrl = m_ctrlDown > 0;
        state.alt = m_altDown > 0;
        state.super = m_superDown > 0;

        m_typedChars.clear();
        m_lastKeyName.clear();
        m_navPresses.clear();

        return state;
    }
} // namespace VkHook
