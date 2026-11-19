#include "input_blocker.hh"

#include "input_session.hh"
#include "wayland_input_common.hh"
#include "x11_display.hh"

// Free-function facade over the current InputSession (see InputSession::current()
// for the policy with several surfaces). With no session every query returns its
// idle value and every setter is a no-op: the layer has nothing to block.

namespace VkHook
{
    void initInputBlocker(bool enabled)
    {
        if (auto session = InputSession::current())
            session->initBlocker(enabled);
    }

    void setInputRects(const InputRect* rects, int count)
    {
        if (auto session = InputSession::current())
            session->setRects(rects, count);
    }

    void setInputBlocked(bool onScreen)
    {
        if (auto session = InputSession::current())
            session->setBlocked(onScreen);
    }

    void setInputInteractive(bool value)
    {
        if (auto session = InputSession::current())
            session->setInteractive(value);
    }

    void setOverlayWantsKeyboard(bool value)
    {
        if (auto session = InputSession::current())
            session->setWantsKeyboard(value);
    }

    void updatePointerPosition(float x, float y)
    {
        if (auto session = InputSession::current())
            session->updatePointerPosition(x, y);
    }

    bool isInputBlockedAt(float x, float y)
    {
        const auto session = InputSession::current();
        return session && session->blockedAt(x, y);
    }

    bool isInputBlocked()
    {
        const auto session = InputSession::current();
        return session && session->blocked();
    }

    bool isOverlayConsumingKeys()
    {
        const auto session = InputSession::current();
        return session && session->consumingKeys();
    }

    bool inputBlockingEnabled()
    {
        const auto session = InputSession::current();
        return session && session->blockingEnabled();
    }

    bool inputGrabActive()
    {
        const auto session = InputSession::current();
        return session && session->grabActive();
    }

    bool inputKeyboardWithheld()
    {
        const auto session = InputSession::current();
        return session && session->keyboardWithheld();
    }

    bool inputPointerOverOverlay()
    {
        const auto session = InputSession::current();
        return session && session->pointerOverOverlay();
    }

    void setWaylandInputSurfaceRects(const InputRect* rects, int count)
    {
        if (auto session = InputSession::current())
            session->setWaylandCaptureRects(rects, count);
    }

    void beginWaylandInputFrame()
    {
        if (auto session = InputSession::current())
            session->beginFrame();
    }

    bool x11PointerGrabActive()
    {
        const auto session = InputSession::current();
        return session && session->x11PointerGrabActive();
    }
} // namespace VkHook
