#include "wayland_input.hh"

#include "logger.hh"
#include "wayland_interpose.hh"

#include <wayland-client.h>

#include <chrono>
#include <cstring>
#include <string>

namespace VkHook
{
    namespace
    {
        // Linux evdev button codes: BTN_LEFT=0x110, BTN_RIGHT=0x111, BTN_MIDDLE=0x112
        constexpr uint32_t kButtonLeft = 0x110;
        constexpr uint32_t kButtonRight = 0x111;
        constexpr uint32_t kButtonMiddle = 0x112;
        constexpr int kAutoReleaseHardTimeoutMs = 30000;
    } // namespace

    // Every event libwayland 1.26 defines has an entry: a pointer bound at the
    // seat's version can be sent any of them.
    struct WaylandPointerListeners
    {
        static void enter(void* data, wl_pointer*, uint32_t, wl_surface* surface, wl_fixed_t sx, wl_fixed_t sy)
        {
            static_cast<WaylandInput*>(data)->onPointerEnter(surface, sx, sy);
        }

        static void leave(void* data, wl_pointer*, uint32_t, wl_surface* surface)
        {
            static_cast<WaylandInput*>(data)->onPointerLeave(surface);
        }

        static void motion(void* data, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy)
        {
            static_cast<WaylandInput*>(data)->onPointerMotion(sx, sy);
        }

        static void button(void* data, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state)
        {
            static_cast<WaylandInput*>(data)->onPointerButton(button, state == WL_POINTER_BUTTON_STATE_PRESSED);
        }

        static void axis(void* data, wl_pointer* pointer, uint32_t, uint32_t axis, wl_fixed_t value)
        {
            if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
                static_cast<WaylandInput*>(data)->onPointerAxis(pointer, wl_fixed_to_double(value));
        }

        static void frame(void* data, wl_pointer*)
        {
            static_cast<WaylandInput*>(data)->onPointerFrame();
        }

        static void axisSource(void*, wl_pointer*, uint32_t)
        {
        }

        static void axisStop(void*, wl_pointer*, uint32_t, uint32_t)
        {
        }

        static void axisDiscrete(void* data, wl_pointer*, uint32_t axis, int32_t discrete)
        {
            static_cast<WaylandInput*>(data)->onPointerAxisDiscrete(axis, discrete);
        }

        static void axisValue120(void* data, wl_pointer*, uint32_t axis, int32_t value120)
        {
            static_cast<WaylandInput*>(data)->onPointerAxisValue120(axis, value120);
        }

        // wl_pointer v9: which way the compositor flipped the axis ("natural
        // scrolling"). The overlay scrolls with the delta it is given.
        static void axisRelativeDirection(void*, wl_pointer*, uint32_t, uint32_t)
        {
        }

        // wl_pointer v11: the compositor moved the pointer itself
        static void warp(void* data, wl_pointer*, wl_fixed_t sx, wl_fixed_t sy)
        {
            static_cast<WaylandInput*>(data)->onPointerMotion(sx, sy);
        }

        static constexpr wl_pointer_listener listener = {
            .enter = enter,
            .leave = leave,
            .motion = motion,
            .button = button,
            .axis = axis,
            .frame = frame,
            .axis_source = axisSource,
            .axis_stop = axisStop,
            .axis_discrete = axisDiscrete,
            .axis_value120 = axisValue120,
            .axis_relative_direction = axisRelativeDirection,
            .warp = warp,
        };
    };

    // Events are surface-local, on our capture surface or the game's; map them
    // into the game's framebuffer pixels, which is what ImGui draws in. Caller
    // holds m_mouseMutex.
    void WaylandInput::setPointerPosition(int32_t sx, int32_t sy)
    {
        const bool onInputSurface = m_inputSurface != nullptr && m_pointerFocusSurface == m_inputSurface;
        const double originX = onInputSurface ? m_inputSurfaceX : 0.0;
        const double originY = onInputSurface ? m_inputSurfaceY : 0.0;
        m_pointerX = static_cast<int>((wl_fixed_to_double(sx) + originX) * framebufferPerLogicalX());
        m_pointerY = static_cast<int>((wl_fixed_to_double(sy) + originY) * framebufferPerLogicalY());
    }

    void WaylandInput::onPointerEnter(wl_surface* surface, int32_t sx, int32_t sy)
    {
        const uintptr_t parent = m_parentAtomic.load(std::memory_order_acquire);
        m_pointerFocus.store(reinterpret_cast<uintptr_t>(surface) == parent || surface == m_inputSurface,
                             std::memory_order_release);

        std::lock_guard<std::mutex> lock(m_mouseMutex);
        m_pointerFocusSurface = surface;
        setPointerPosition(sx, sy);

        // Do NOT clear button state here. Surface reconfigurations (swapchain
        // resize) cause rapid leave/enter cycles while the user is dragging.
        // Clearing buttons on enter breaks ImGui drag operations. Button state
        // is tracked purely from wl_pointer.button events. If a compositor grab
        // (Alt+drag) consumes a release, the next user click naturally clears it.

        Logger::trace("Wayland: pointer enter at " + std::to_string(m_pointerX) + "," + std::to_string(m_pointerY));
    }

    void WaylandInput::onPointerLeave(wl_surface* /*surface*/)
    {
        // keep the last focus surface: motion carries no surface, and clearing
        // it here mistranslates the next one.
        m_pointerFocus.store(false, std::memory_order_release);
        Logger::trace("Wayland: pointer leave");
    }

    void WaylandInput::onPointerMotion(int32_t sx, int32_t sy)
    {
        std::lock_guard<std::mutex> lock(m_mouseMutex);
        setPointerPosition(sx, sy);
    }

    void WaylandInput::onPointerButton(uint32_t button, bool pressed)
    {
        Logger::trace("Wayland: pointer button " + std::to_string(button) + " " + (pressed ? "pressed" : "released"));
        mirrorButton(button, pressed);
    }

    void WaylandInput::mirrorButton(uint32_t button, bool pressed)
    {
        std::lock_guard<std::mutex> lock(m_mouseMutex);
        const auto now = Clock::now();
        switch (button)
        {
            case kButtonLeft:
                m_leftButton = pressed;
                if (pressed) m_leftPressTime = now;
                break;
            case kButtonRight:
                m_rightButton = pressed;
                if (pressed) m_rightPressTime = now;
                break;
            case kButtonMiddle:
                m_middleButton = pressed;
                if (pressed) m_middlePressTime = now;
                break;
            default:
                break;
        }
    }

    void WaylandInput::onPointerAxis(wl_pointer* pointer, double value)
    {
        const float scroll = -static_cast<float>(value) / 10.0f;
        std::lock_guard<std::mutex> lock(m_mouseMutex);
        if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(pointer)) < WL_POINTER_FRAME_SINCE_VERSION)
            m_scroll += scroll;
        else
            m_continuousScroll += scroll;
    }

    void WaylandInput::onPointerFrame()
    {
        std::lock_guard<std::mutex> lock(m_mouseMutex);
        m_scroll += m_hasHighResolutionScroll ? m_highResolutionScroll
                  : m_hasDiscreteScroll ? m_discreteScroll : m_continuousScroll;
        m_continuousScroll = m_discreteScroll = m_highResolutionScroll = 0.0f;
        m_hasDiscreteScroll = m_hasHighResolutionScroll = false;
    }

    // Discrete scroll events (wheel clicks) — preferred over continuous axis
    void WaylandInput::onPointerAxisDiscrete(uint32_t axis, int32_t discrete)
    {
        if (axis != 0)
            return;

        std::lock_guard<std::mutex> lock(m_mouseMutex);
        m_hasDiscreteScroll = true;
        m_discreteScroll -= static_cast<float>(discrete);
    }

    // High-resolution scroll (wl_pointer v8+). 120 units = one wheel click.
    // Preferred over both axis and axis_discrete when available.
    void WaylandInput::onPointerAxisValue120(uint32_t axis, int32_t value120)
    {
        if (axis != 0)
            return;

        std::lock_guard<std::mutex> lock(m_mouseMutex);
        m_hasHighResolutionScroll = true;
        m_highResolutionScroll -= static_cast<float>(value120) / 120.0f;
    }

    // Called with the seat's pointer capability available
    void WaylandInput::bindPointer()
    {
        if (m_pointer)
            return;

        m_pointer = wl_seat_get_pointer(m_seat);
        m_releases.push([this] {
            unregisterOverlayProxy(reinterpret_cast<wl_proxy*>(m_pointer));
            wl_pointer_destroy(m_pointer);
            m_pointer = nullptr;
        });
        // Register as overlay proxy BEFORE add_listener so the interposition
        // layer passes this through without wrapping
        registerOverlayProxy(reinterpret_cast<wl_proxy*>(m_pointer));
        wl_pointer_add_listener(m_pointer, &WaylandPointerListeners::listener, this);
        Logger::debug("Wayland: pointer bound from shared seat");
    }

    bool WaylandInput::initPointer()
    {
        if (m_pointer)
            return true;

        // Initialize shared seat/queue/registry (triggers seat capability callbacks)
        if (!ensureCommon(false, true))
            return false;

        if (m_pointer)
            Logger::info("Wayland mouse input initialized");
        else
            Logger::warn("Wayland: no pointer found on seat");

        return m_pointer != nullptr;
    }

    MouseState WaylandInput::mouseState()
    {
        MouseState state;

        std::lock_guard<std::mutex> lifecycle(m_lifecycle);
        if (!initPointer())
            return state;

        dispatchPending();

        std::lock_guard<std::mutex> lock(m_mouseMutex);

        // Stuck-button safety net only: a normal press holds as long as the
        // compositor says so. The old leave-based release fired whenever a
        // compositor dropped pointer focus mid-press (a transient leave on a
        // subsurface is common), which cleared `held` and snapped the button
        // morph back before the user released.
        const auto now = Clock::now();
        auto expired = [&](bool down, Clock::time_point since) {
            return down && std::chrono::duration_cast<std::chrono::milliseconds>(now - since).count()
                               > kAutoReleaseHardTimeoutMs;
        };
        if (expired(m_leftButton, m_leftPressTime))
            m_leftButton = false;
        if (expired(m_rightButton, m_rightPressTime))
            m_rightButton = false;
        if (expired(m_middleButton, m_middlePressTime))
            m_middleButton = false;

        state.x = m_pointerX;
        state.y = m_pointerY;
        state.leftButton = m_leftButton;
        state.rightButton = m_rightButton;
        state.middleButton = m_middleButton;
        state.scrollDelta = m_scroll;
        m_scroll = 0.0f;

        return state;
    }
} // namespace VkHook
