#include "x11_input.hh"

#include <X11/Xlib.h>

namespace VkHook
{
    void X11Input::onRawButton(uint32_t button, bool pressed)
    {
        if (!pressed)
            return;
        if (button == 4)
            m_rawScroll += 1.0f;   // wheel up
        else if (button == 5)
            m_rawScroll -= 1.0f;   // wheel down
    }

    MouseState X11Input::mouseState()
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        MouseState state;
        Display* display = connection();
        if (!display)
            return state;

        pumpLocked();
        if (capturedMouseState(state))
        {
            m_rawScroll = 0.0f;
            return state;
        }
        if (!gameFocusedLocked(m_window))
        {
            m_rawScroll = 0.0f;
            return state;
        }

        const Window root = DefaultRootWindow(display);
        Window rootRet = 0, childRet = 0;
        int rootX = 0, rootY = 0, winX = 0, winY = 0;
        unsigned int mask = 0;
        if (!XQueryPointer(display, root, &rootRet, &childRet, &rootX, &rootY, &winX, &winY, &mask))
            return state;

        // the overlay draws in framebuffer space, so report window-relative
        // coordinates rather than root ones.
        int x = rootX;
        int y = rootY;
        if (m_window)
        {
            Window child = 0;
            int wx = 0, wy = 0;
            if (XTranslateCoordinates(display, root, (Window)m_window, rootX, rootY, &wx, &wy, &child))
            {
                x = wx;
                y = wy;
            }
        }

        state.x = x;
        state.y = y;
        state.leftButton = (mask & Button1Mask) != 0;
        state.middleButton = (mask & Button2Mask) != 0;
        state.rightButton = (mask & Button3Mask) != 0;
        state.scrollDelta = m_rawScroll;
        m_rawScroll = 0.0f;
        return state;
    }
} // namespace VkHook
