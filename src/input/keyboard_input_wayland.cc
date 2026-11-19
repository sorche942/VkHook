#include "keyboard_input_wayland.hh"

#include "c_resource.hh"
#include "logger.hh"
#include "wayland_input.hh"
#include "wayland_interpose.hh"

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

namespace VkHook
{
    struct WaylandKeyboardListeners
    {
        static void keymap(void* data, wl_keyboard* /*keyboard*/, uint32_t format, int32_t fd, uint32_t size)
        {
            static_cast<WaylandInput*>(data)->onKeymap(fd, format, size);
        }

        static void enter(void* data, wl_keyboard* /*keyboard*/, uint32_t /*serial*/, wl_surface* surface,
                          wl_array* /*keys*/)
        {
            static_cast<WaylandInput*>(data)->onKeyboardEnter(surface);
        }

        static void leave(void* data, wl_keyboard* /*keyboard*/, uint32_t /*serial*/, wl_surface* /*surface*/)
        {
            static_cast<WaylandInput*>(data)->onKeyboardLeave();
        }

        static void key(void* data, wl_keyboard* /*keyboard*/, uint32_t /*serial*/, uint32_t /*time*/,
                        uint32_t key, uint32_t state)
        {
            static_cast<WaylandInput*>(data)->processKey(key, state);
        }

        static void modifiers(void* data, wl_keyboard* /*keyboard*/, uint32_t /*serial*/, uint32_t depressed,
                              uint32_t latched, uint32_t locked, uint32_t group)
        {
            static_cast<WaylandInput*>(data)->onModifiers(depressed, latched, locked, group);
        }

        static void repeatInfo(void* /*data*/, wl_keyboard* /*keyboard*/, int32_t /*rate*/, int32_t /*delay*/)
        {
        }

        static constexpr wl_keyboard_listener listener = {
            .keymap = keymap,
            .enter = enter,
            .leave = leave,
            .key = key,
            .modifiers = modifiers,
            .repeat_info = repeatInfo,
        };
    };

    // Process a key press event
    void WaylandInput::processKey(uint32_t keycode, uint32_t state)
    {
        std::lock_guard<std::mutex> lock(m_keyboardMutex);
        if (!m_xkbState)
            return;

        // Wayland keycodes are evdev codes, XKB expects evdev + 8
        uint32_t xkbKeycode = keycode + 8;

        if (state == WL_KEYBOARD_KEY_STATE_RELEASED)
        {
            m_pressedKeys.erase(keycode);
            xkb_state_update_key(m_xkbState, xkbKeycode, XKB_KEY_UP);
            return;
        }

        // Key press
        m_pressedKeys.insert(keycode);
        xkb_state_update_key(m_xkbState, xkbKeycode, XKB_KEY_DOWN);

        xkb_keysym_t keysym = xkb_state_key_get_one_sym(m_xkbState, xkbKeycode);

        // Track press event for edge detection (survives same-frame release)
        m_keyPressEvents.insert((uint32_t)keysym);

        // Capture key name for keybind editor (skip modifiers)
        if (keysym != XKB_KEY_Shift_L && keysym != XKB_KEY_Shift_R &&
            keysym != XKB_KEY_Control_L && keysym != XKB_KEY_Control_R &&
            keysym != XKB_KEY_Alt_L && keysym != XKB_KEY_Alt_R &&
            keysym != XKB_KEY_Super_L && keysym != XKB_KEY_Super_R)
        {
            char nameBuf[64];
            if (xkb_keysym_get_name(keysym, nameBuf, sizeof(nameBuf)) > 0)
                m_lastKeyName = nameBuf;
        }

        // Navigation and editing keys
        NavKey nav = NavKey::Count;
        switch (keysym)
        {
            case XKB_KEY_BackSpace: nav = NavKey::Backspace; break;
            case XKB_KEY_Delete: case XKB_KEY_KP_Delete: nav = NavKey::Delete; break;
            case XKB_KEY_Return: case XKB_KEY_KP_Enter: nav = NavKey::Enter; break;
            case XKB_KEY_Left: nav = NavKey::Left; break;
            case XKB_KEY_Right: nav = NavKey::Right; break;
            case XKB_KEY_Up: nav = NavKey::Up; break;
            case XKB_KEY_Down: nav = NavKey::Down; break;
            case XKB_KEY_Home: nav = NavKey::Home; break;
            case XKB_KEY_End: nav = NavKey::End; break;
            case XKB_KEY_Prior: nav = NavKey::PageUp; break;
            case XKB_KEY_Next: nav = NavKey::PageDown; break;
            case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: nav = NavKey::Tab; break;
            case XKB_KEY_Escape: nav = NavKey::Escape; break;
            default: break;
        }
        if (nav != NavKey::Count)
        {
            if (m_navPresses.size() < kMaxNavPresses)
                m_navPresses.push_back(nav);
            return;
        }

        // Ctrl/Alt/Super chords are shortcuts, not text
        if (xkb_state_mod_name_is_active(m_xkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0 ||
            xkb_state_mod_name_is_active(m_xkbState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0 ||
            xkb_state_mod_name_is_active(m_xkbState, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)
            return;

        const uint32_t cp = xkb_state_key_get_utf32(m_xkbState, xkbKeycode);
        if (cp >= 0x20 && cp != 0x7F)
            appendUtf8(m_typedChars, cp);
    }

    void WaylandInput::onKeymap(int32_t rawFd, uint32_t format, uint32_t size)
    {
        // The compositor hands us ownership of this descriptor, and the protocol
        // requires us to close it on every path out of this callback. Owning it
        // from the first statement means that holds by construction.
        UniqueFd fd(rawFd);

        if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
            return;

        char* mapStr = (char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd.get(), 0);
        if (mapStr == MAP_FAILED)
        {
            Logger::err("Wayland: failed to mmap keymap");
            return;
        }

        // Clean up old keymap/state
        std::lock_guard<std::mutex> lock(m_keyboardMutex);
        if (m_xkbState)
        {
            xkb_state_unref(m_xkbState);
            m_xkbState = nullptr;
        }
        if (m_xkbKeymap)
        {
            xkb_keymap_unref(m_xkbKeymap);
            m_xkbKeymap = nullptr;
        }

        m_xkbKeymap = xkb_keymap_new_from_string(m_xkbContext, mapStr,
                                                 XKB_KEYMAP_FORMAT_TEXT_V1,
                                                 XKB_KEYMAP_COMPILE_NO_FLAGS);
        munmap(mapStr, size);

        if (!m_xkbKeymap)
        {
            Logger::err("Wayland: failed to compile keymap");
            return;
        }

        m_xkbState = xkb_state_new(m_xkbKeymap);
        if (!m_xkbState)
            Logger::err("Wayland: failed to create XKB state");

        Logger::debug("Wayland: keymap loaded");
    }

    void WaylandInput::onKeyboardEnter(wl_surface* surface)
    {
        const uintptr_t parent = m_parentAtomic.load(std::memory_order_acquire);
        m_keyboardFocus.store(reinterpret_cast<uintptr_t>(surface) == parent || surface == m_inputSurface,
                              std::memory_order_release);
    }

    void WaylandInput::onKeyboardLeave()
    {
        m_keyboardFocus.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(m_keyboardMutex);
        m_pressedKeys.clear();
        m_keyPressEvents.clear();  // Prevent stale keysyms surviving focus loss
        m_navPresses.clear();
    }

    void WaylandInput::onModifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
    {
        std::lock_guard<std::mutex> lock(m_keyboardMutex);
        if (m_xkbState)
            xkb_state_update_mask(m_xkbState, depressed, latched, locked, 0, 0, group);
    }

    // Called with the seat's keyboard capability available
    void WaylandInput::bindKeyboard()
    {
        if (m_keyboard)
            return;

        m_keyboard = wl_seat_get_keyboard(m_seat);
        m_releases.push([this] {
            unregisterOverlayProxy(reinterpret_cast<wl_proxy*>(m_keyboard));
            wl_keyboard_destroy(m_keyboard);
            m_keyboard = nullptr;
        });
        // Register as overlay proxy BEFORE add_listener so the interposition
        // layer passes this through without wrapping
        registerOverlayProxy(reinterpret_cast<wl_proxy*>(m_keyboard));
        wl_keyboard_add_listener(m_keyboard, &WaylandKeyboardListeners::listener, this);
        Logger::debug("Wayland: keyboard bound from shared seat");
    }

    bool WaylandInput::initKeyboard()
    {
        if (m_keyboard)
            return true;

        // Create XKB context once and reuse across retries.
        if (!m_xkbContext)
        {
            m_xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
            if (!m_xkbContext)
            {
                Logger::err("Wayland: failed to create XKB context");
                return false;
            }
        }

        // Initialize shared seat/queue/registry (triggers seat capability callbacks)
        if (!ensureCommon(true, false))
            return false;

        if (m_keyboard)
            Logger::info("Wayland keyboard input initialized");
        else
            Logger::warn("Wayland: no keyboard found on seat");

        return m_keyboard != nullptr;
    }

    // The keyboard proxy itself is destroyed by the release stack; this drops
    // the xkb state and everything accumulated from it.
    void WaylandInput::releaseKeyboard()
    {
        std::lock_guard<std::mutex> lock(m_keyboardMutex);
        if (m_xkbState)
        {
            xkb_state_unref(m_xkbState);
            m_xkbState = nullptr;
        }
        if (m_xkbKeymap)
        {
            xkb_keymap_unref(m_xkbKeymap);
            m_xkbKeymap = nullptr;
        }
        if (m_xkbContext)
        {
            xkb_context_unref(m_xkbContext);
            m_xkbContext = nullptr;
        }

        m_pressedKeys.clear();
        m_keyPressEvents.clear();
        m_typedChars.clear();
        m_lastKeyName.clear();
        m_navPresses.clear();
    }

    uint32_t convertToKeySymWayland(std::string key)
    {
        // XKB keysym names are the same as X11 keysym names
        xkb_keysym_t sym = xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_NO_FLAGS);
        if (sym == XKB_KEY_NoSymbol)
        {
            // Try case-insensitive
            sym = xkb_keysym_from_name(key.c_str(), XKB_KEYSYM_CASE_INSENSITIVE);
        }
        if (sym == XKB_KEY_NoSymbol)
            Logger::err("Wayland: invalid key name: " + key);

        return (uint32_t)sym;
    }

    bool WaylandInput::isKeyPressed(uint32_t ks)
    {
        std::lock_guard<std::mutex> lifecycle(m_lifecycle);
        if (!initKeyboard())
            return false;

        dispatchPending();

        std::lock_guard<std::mutex> lock(m_keyboardMutex);

        // Check accumulated press events first — catches rapid taps where
        // press+release both arrive in the same dispatch cycle
        if (m_keyPressEvents.count(ks))
        {
            m_keyPressEvents.erase(ks);
            return true;
        }

        // Check if any currently-held key maps to this keysym
        if (!m_xkbKeymap)
            return false;

        for (uint32_t keycode : m_pressedKeys)
        {
            uint32_t xkbKeycode = keycode + 8;
            xkb_keysym_t sym = xkb_state_key_get_one_sym(m_xkbState, xkbKeycode);
            if ((uint32_t)sym == ks)
                return true;
        }
        return false;
    }

    void WaylandInput::discardKeyPresses()
    {
        std::lock_guard<std::mutex> lock(m_keyboardMutex);
        m_keyPressEvents.clear();
    }

    KeyboardState WaylandInput::keyboardState()
    {
        KeyboardState state;

        std::lock_guard<std::mutex> lifecycle(m_lifecycle);
        if (!initKeyboard())
            return state;

        dispatchPending();

        std::lock_guard<std::mutex> lock(m_keyboardMutex);

        state.typedChars = std::move(m_typedChars);
        state.lastKeyName = std::move(m_lastKeyName);
        state.navPresses = std::move(m_navPresses);
        if (m_xkbState)
        {
            auto active = [this](const char* name) {
                return xkb_state_mod_name_is_active(m_xkbState, name, XKB_STATE_MODS_EFFECTIVE) > 0;
            };
            state.shift = active(XKB_MOD_NAME_SHIFT);
            state.ctrl = active(XKB_MOD_NAME_CTRL);
            state.alt = active(XKB_MOD_NAME_ALT);
            state.super = active(XKB_MOD_NAME_LOGO);
        }

        m_typedChars.clear();
        m_lastKeyName.clear();
        m_navPresses.clear();

        return state;
    }
} // namespace VkHook
