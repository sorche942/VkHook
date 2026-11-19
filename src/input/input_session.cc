#include "input_session.hh"

#include "logger.hh"
#include "wayland_input.hh"
#include "wayland_interpose.hh"
#include "x11_input.hh"

#include <algorithm>

namespace VkHook
{
    namespace
    {
        // The set of live sessions, which the free-function facade resolves
        // through. Process-wide by necessity: the facade is called from places
        // that hold no session. It holds weak references only (a session lives
        // as long as its surface's owner keeps it).
        struct Registry
        {
            std::mutex mutex;
            // creation order, oldest first. The raw pointer identifies the entry
            // without locking the weak reference: locking under `mutex` could drop
            // the last reference and run a destructor that takes `mutex`.
            struct Entry
            {
                InputSession* raw;
                std::weak_ptr<InputSession> weak;
            };
            std::vector<Entry> sessions;
            std::atomic<bool> warnedNoSession{false};
        };

        // Never destroyed: sessions own libwayland and Xlib objects, and tearing
        // them down from a static destructor at process exit runs after those
        // libraries may be gone.
        Registry& registry()
        {
            static Registry* instance = new Registry;
            return *instance;
        }
    } // namespace

    std::shared_ptr<InputSession> InputSession::create(SurfaceKind kind, void* nativeDisplay, uintptr_t nativeSurface)
    {
        if (kind == SurfaceKind::Unknown || nativeSurface == 0)
            return nullptr;
        if (kind == SurfaceKind::Wayland && !nativeDisplay)
            return nullptr;

        std::shared_ptr<InputSession> session(new InputSession(kind, nativeDisplay, nativeSurface));
        {
            Registry& reg = registry();
            std::lock_guard<std::mutex> lock(reg.mutex);
            std::erase_if(reg.sessions, [](const Registry::Entry& entry) { return entry.weak.expired(); });
            reg.sessions.push_back({session.get(), session});
        }

        Logger::info(kind == SurfaceKind::Wayland ? "input session created for a Wayland surface"
                                                  : "input session created for an X11 window");
        return session;
    }

    std::shared_ptr<InputSession> InputSession::current()
    {
        Registry& reg = registry();
        std::vector<std::weak_ptr<InputSession>> candidates;
        {
            std::lock_guard<std::mutex> lock(reg.mutex);
            candidates.reserve(reg.sessions.size());
            for (const Registry::Entry& entry : reg.sessions)
                candidates.push_back(entry.weak);
        }

        // locked outside the registry mutex: dropping a last reference here runs
        // a destructor that takes it
        std::shared_ptr<InputSession> newest;
        for (auto it = candidates.rbegin(); it != candidates.rend(); ++it)
        {
            std::shared_ptr<InputSession> session = it->lock();
            if (!session || !session->alive())
                continue;
            if (session->hasFocus())
                return session;
            if (!newest)
                newest = std::move(session);
        }
        return newest;
    }

    InputSession::InputSession(SurfaceKind kind, void* nativeDisplay, uintptr_t nativeSurface)
        : m_kind(kind)
        , m_nativeDisplay(nativeDisplay)
        , m_nativeSurface(nativeSurface)
    {
        if (kind == SurfaceKind::X11)
        {
            m_x11 = std::make_unique<X11Input>();
            m_x11->setWindow(static_cast<unsigned long>(nativeSurface));
        }
        else
        {
            m_wayland = std::make_unique<WaylandInput>(static_cast<wl_display*>(nativeDisplay), nativeSurface);
        }
    }

    InputSession::~InputSession()
    {
        destroy();
    }

    // The backends stay allocated: other threads may be inside a poll, and the
    // backends' own locks make close() safe against them. They are freed with
    // the session.
    void InputSession::destroy()
    {
        if (m_destroyed.exchange(true))
            return;

        // drop the keyboard hold before the Wayland keyboard goes away
        if (m_keyboardWithheld.exchange(false))
            withholdGameKeys(false);
        if (m_x11)
            m_x11->close();
        if (m_wayland)
            m_wayland->close();

        Registry& reg = registry();
        {
            std::lock_guard<std::mutex> lock(reg.mutex);
            std::erase_if(reg.sessions, [this](const Registry::Entry& entry) {
                return entry.raw == this || entry.weak.expired();
            });
        }
    }

    void InputSession::onSurfaceRecreated(uintptr_t newNativeSurface)
    {
        if (m_destroyed.load(std::memory_order_acquire) || newNativeSurface == 0)
            return;
        if (m_nativeSurface.exchange(newNativeSurface) == newNativeSurface)
            return;

        Logger::info("input session: the game recreated its surface; re-attaching");
        if (m_x11)
            m_x11->setWindow(static_cast<unsigned long>(newNativeSurface));
        else
            m_wayland->setParentSurface(newNativeSurface);
        applyBlocking();
    }

    SurfaceKind InputSession::kind() const
    {
        return m_kind;
    }

    void* InputSession::nativeDisplay() const
    {
        return m_nativeDisplay;
    }

    uintptr_t InputSession::nativeSurface() const
    {
        return m_nativeSurface.load(std::memory_order_acquire);
    }

    bool InputSession::alive() const
    {
        return !m_destroyed.load(std::memory_order_acquire);
    }

    bool InputSession::hasFocus() const
    {
        if (m_x11)
            return m_x11->hasFocus();
        return m_wayland && m_wayland->hasFocus();
    }

    uint32_t InputSession::waylandSurfaceId() const
    {
        return m_wayland ? m_wayland->parentSurfaceId() : 0;
    }

    // ── blocking ─────────────────────────────────────────────────────────────

    bool InputSession::pointInRects(float x, float y) const
    {
        std::lock_guard<std::mutex> lock(m_rectMutex);
        for (const InputRect& r : m_rects)
            if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height)
                return true;
        return false;
    }

    bool InputSession::pointerOverOverlay() const
    {
        return m_blocked.load(std::memory_order_acquire)
            && pointInRects(m_pointerX.load(std::memory_order_acquire), m_pointerY.load(std::memory_order_acquire));
    }

    bool InputSession::consumingKeys() const
    {
        return m_blocked.load(std::memory_order_acquire)
            && m_interactive.load(std::memory_order_acquire)
            && m_wantsKeyboard.load(std::memory_order_acquire);
    }

    // An interactive menu owns the entire input session, not just its hitboxes.
    // Taking the keyboard first asks Wine to relinquish its pointer grab; the
    // pointer acquisition can then retry until that asynchronous handoff lands.
    void InputSession::applyX11Blocking()
    {
        const bool enabled = m_blockingEnabled.load(std::memory_order_acquire);
        const bool menu = m_blocked.load(std::memory_order_acquire)
                       && m_interactive.load(std::memory_order_acquire);
        const bool pointerGrab = enabled && (menu || pointerOverOverlay());
        const bool keyboardGrab = enabled && menu;

        m_x11->setGrabStates(pointerGrab, keyboardGrab);
    }

    // Wayland withholds the game's own event stream without a server-side grab.
    // Menu ownership lasts until close; notification-only keys follow hitboxes.
    void InputSession::applyWaylandBlocking()
    {
        const bool enabled = m_blockingEnabled.load(std::memory_order_acquire);
        const bool withhold = enabled && m_blocked.load(std::memory_order_acquire)
            && (m_interactive.load(std::memory_order_acquire) || pointerOverOverlay() || consumingKeys());

        // releases/re-presses the exact held keys; mouse is left alone.
        if (m_keyboardWithheld.exchange(withhold) != withhold)
            withholdGameKeys(withhold);
    }

    void InputSession::applyBlocking()
    {
        if (m_destroyed.load(std::memory_order_acquire))
            return;
        if (m_x11)
            applyX11Blocking();
        else
            applyWaylandBlocking();
    }

    void InputSession::initBlocker(bool enabled)
    {
        m_blockingEnabled.store(enabled, std::memory_order_release);
        Logger::debug(std::string("Input blocking ")
                      + (enabled ? (m_x11 ? "enabled (X11: menu ownership)" : "enabled (Wayland: menu ownership)")
                                 : "disabled"));
    }

    void InputSession::setRects(const InputRect* rects, int count)
    {
        std::lock_guard<std::mutex> lock(m_rectMutex);
        m_rects.assign(rects, rects + (count > 0 ? count : 0));
    }

    void InputSession::setBlocked(bool blocked)
    {
        m_blocked.store(blocked, std::memory_order_release);
        applyBlocking();
    }

    void InputSession::setInteractive(bool interactive)
    {
        m_interactive.store(interactive, std::memory_order_release);
        applyBlocking();
    }

    void InputSession::setWantsKeyboard(bool wantsKeyboard)
    {
        m_wantsKeyboard.store(wantsKeyboard, std::memory_order_release);
        applyBlocking();
    }

    void InputSession::updatePointerPosition(float x, float y)
    {
        m_pointerX.store(x, std::memory_order_release);
        m_pointerY.store(y, std::memory_order_release);
        applyBlocking();
    }

    bool InputSession::blockedAt(float x, float y) const
    {
        return m_blockingEnabled.load(std::memory_order_acquire)
            && m_blocked.load(std::memory_order_acquire)
            && (m_interactive.load(std::memory_order_acquire) || pointInRects(x, y));
    }

    bool InputSession::blocked() const
    {
        return m_blocked.load(std::memory_order_acquire);
    }

    bool InputSession::blockingEnabled() const
    {
        return m_blockingEnabled.load(std::memory_order_acquire);
    }

    bool InputSession::grabActive() const
    {
        return m_x11 && m_x11->grabActive();
    }

    bool InputSession::keyboardWithheld() const
    {
        return m_keyboardWithheld.load(std::memory_order_acquire);
    }

    bool InputSession::x11PointerGrabActive() const
    {
        return m_x11 && m_x11->pointerGrabActive();
    }

    // ── polling ──────────────────────────────────────────────────────────────

    void InputSession::beginFrame()
    {
        if (m_destroyed.load(std::memory_order_acquire))
            return;
        if (m_x11)
            m_x11->beginKeyboardFrame();
        else
            m_wayland->beginFrame();
    }

    bool InputSession::isKeyPressed(uint32_t keysym)
    {
        if (m_destroyed.load(std::memory_order_acquire))
            return false;

        if (consumingKeys())
        {
            // the keystrokes belong to the text field; forget them so they do
            // not fire the hotkey the moment the field closes
            if (m_wayland)
                m_wayland->discardKeyPresses();
            return false;
        }
        return m_x11 ? m_x11->isKeyPressed(keysym) : m_wayland->isKeyPressed(keysym);
    }

    KeyboardState InputSession::keyboardState()
    {
        if (m_destroyed.load(std::memory_order_acquire))
            return KeyboardState();
        return m_x11 ? m_x11->keyboardState() : m_wayland->keyboardState();
    }

    MouseState InputSession::mouseState()
    {
        if (m_destroyed.load(std::memory_order_acquire))
            return MouseState();
        return m_x11 ? m_x11->mouseState() : m_wayland->mouseState();
    }

    void InputSession::setWaylandCaptureRects(const InputRect* rects, int count)
    {
        if (m_wayland && !m_destroyed.load(std::memory_order_acquire))
            m_wayland->setCaptureRects(rects, count);
    }

    void InputSession::setFramebufferExtent(uint32_t width, uint32_t height)
    {
        if (m_wayland && !m_destroyed.load(std::memory_order_acquire))
            m_wayland->setFramebufferExtent(width, height);
    }

    // ── game pointer, fed by the Wayland interposer ──────────────────────────

    void InputSession::noteGamePointer(double x, double y)
    {
        // hitboxes are in framebuffer pixels
        const double perX = m_wayland ? m_wayland->framebufferPerLogicalX() : 1.0;
        const double perY = m_wayland ? m_wayland->framebufferPerLogicalY() : 1.0;
        m_gamePointerX.store(x * perX, std::memory_order_release);
        m_gamePointerY.store(y * perY, std::memory_order_release);
    }

    bool InputSession::gamePointerBlocked() const
    {
        return blockedAt(static_cast<float>(m_gamePointerX.load(std::memory_order_acquire)),
                         static_cast<float>(m_gamePointerY.load(std::memory_order_acquire)));
    }

    void InputSession::mirrorGameButton(uint32_t button, bool pressed)
    {
        if (m_wayland && !m_destroyed.load(std::memory_order_acquire))
            m_wayland->mirrorButton(button, pressed);
    }

    // ── process-level helpers ────────────────────────────────────────────────

    void InputSession::warnNoSession(const char* message)
    {
        if (!registry().warnedNoSession.exchange(true))
            Logger::warn(message);
    }
} // namespace VkHook
