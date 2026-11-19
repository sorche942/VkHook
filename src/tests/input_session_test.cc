// InputSession without a display server: the registry's current-session policy,
// per-session blocking state, surface re-targeting, destruction, and the
// free-function facade. No DISPLAY is set, so X11 sessions never open a connection, and
// the Wayland session is never polled, so its display pointer is never used.
#include "input_session.hh"
#include "keyboard_input.hh"

#include "wayland_display.hh"
#include "wayland_interpose.hh"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace
{
    int failures = 0;

    void check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
            ++failures;
    }

    // the interposer's entry points, recorded instead of linked
    std::vector<bool> withholdCalls;
} // namespace

namespace VkHook
{
    void registerOverlayProxy(wl_proxy*)
    {
    }

    void unregisterOverlayProxy(wl_proxy*)
    {
    }

    void withholdGameKeys(bool withhold)
    {
        withholdCalls.push_back(withhold);
    }
} // namespace VkHook

using namespace VkHook;

namespace
{
    void creationTests()
    {
        check(InputSession::create(SurfaceKind::Unknown, nullptr, 1) == nullptr, "unknown kind is refused");
        check(InputSession::create(SurfaceKind::X11, nullptr, 0) == nullptr, "a null surface is refused");
        check(InputSession::create(SurfaceKind::Wayland, nullptr, 1) == nullptr, "a Wayland surface needs a display");
        check(InputSession::current() == nullptr, "no session yet");

        std::shared_ptr<InputSession> s = InputSession::create(SurfaceKind::X11, nullptr, 100);
        check(s && s->kind() == SurfaceKind::X11 && s->nativeSurface() == 100 && s->alive(), "X11 session created");
        check(InputSession::current() == s, "a lone session is current");
        s->onSurfaceRecreated(150);
        check(s->nativeSurface() == 150, "the session follows a recreated window");
        s->destroy();
        check(!s->alive() && InputSession::current() == nullptr, "a destroyed session is no longer current");
        s->destroy();
        check(!s->alive(), "destroy is idempotent");
        check(s->mouseState().x == 0 && s->keyboardState().typedChars.empty(), "a destroyed session polls idle");
    }

    void policyTests()
    {
        auto first = InputSession::create(SurfaceKind::X11, nullptr, 1);
        auto second = InputSession::create(SurfaceKind::X11, nullptr, 2);
        check(InputSession::current() == second, "with two sessions and no focus, the newest is current");

        second->destroy();
        check(InputSession::current() == first, "destroying the newest falls back to the older one");

        auto third = InputSession::create(SurfaceKind::X11, nullptr, 3);
        third.reset();
        check(InputSession::current() == first, "a session whose owner dropped it is gone from the registry");

        first.reset();
        check(InputSession::current() == nullptr, "no owner, no session");
    }

    void blockingTests()
    {
        auto a = InputSession::create(SurfaceKind::X11, nullptr, 10);
        auto b = InputSession::create(SurfaceKind::X11, nullptr, 20);

        const InputRect rect{10.0f, 10.0f, 100.0f, 50.0f};
        a->initBlocker(true);
        a->setRects(&rect, 1);
        a->setBlocked(true);
        check(a->blockedAt(20.0f, 20.0f), "a notification hitbox blocks inside it");
        check(!a->blockedAt(500.0f, 500.0f), "and not outside it");
        check(!b->blockedAt(20.0f, 20.0f) && !b->blocked() && !b->blockingEnabled(),
              "another session is unaffected");

        a->setInteractive(true);
        check(a->blockedAt(500.0f, 500.0f), "an interactive menu owns the whole surface");
        a->setInteractive(false);

        a->updatePointerPosition(20.0f, 20.0f);
        check(a->pointerOverOverlay(), "the overlay pointer is over the hitbox");
        a->noteGamePointer(500.0, 500.0);
        check(!a->gamePointerBlocked(), "the game pointer outside the hitbox is not blocked");
        a->noteGamePointer(20.0, 20.0);
        check(a->gamePointerBlocked(), "the game pointer inside the hitbox is blocked");
        check(!b->gamePointerBlocked(), "the other session's game pointer is its own");

        a->setBlocked(false);
        check(!a->blockedAt(20.0f, 20.0f), "an overlay that is off blocks nothing");

        a->destroy();
        b->destroy();
    }

    void withholdTests()
    {
        auto w = InputSession::create(SurfaceKind::Wayland, reinterpret_cast<void*>(0x1000), 7);
        check(w && w->kind() == SurfaceKind::Wayland, "Wayland session created without touching the display");

        withholdCalls.clear();
        w->initBlocker(true);
        w->setBlocked(true);
        check(withholdCalls.empty(), "a toast alone keeps the game's keys");
        w->setInteractive(true);
        check(withholdCalls.size() == 1 && withholdCalls[0], "an interactive menu withholds the game's keys");
        w->updatePointerPosition(1.0f, 1.0f);
        check(withholdCalls.size() == 1, "withholding is requested on a transition only");
        w->setBlocked(false);
        check(withholdCalls.size() == 2 && !withholdCalls[1], "closing the menu gives them back");

        w->setBlocked(true);
        w->destroy();
        check(withholdCalls.size() == 4 && !withholdCalls[3], "destroying a session that withholds gives the keys back");
        w->setInteractive(true);
        check(withholdCalls.size() == 4, "a destroyed session no longer requests anything");
    }

    void threadTests()
    {
        std::atomic<int> bad{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
        {
            threads.emplace_back([&bad, t] {
                for (int i = 0; i < 300; ++i)
                {
                    auto s = InputSession::create(SurfaceKind::X11, nullptr, static_cast<uintptr_t>(1000 + t * 1000 + i));
                    if (!s)
                        ++bad;
                    s->setBlocked(i % 2 == 0);
                    (void)InputSession::current();
                    if (i % 3 == 0)
                        s->destroy();
                }
            });
        }
        for (std::thread& thread : threads)
            thread.join();
        check(bad == 0, "concurrent create/current/destroy completes");
        check(InputSession::current() == nullptr, "every session is gone afterwards");
    }

    void facadeTests()
    {
        check(getSurfaceKind() == SurfaceKind::Unknown && !isX11() && !isWayland(), "no surface captured");

        const auto session = InputSession::create(SurfaceKind::X11, nullptr, 300);
        check(isX11() && getSurfaceKind() == SurfaceKind::X11, "an X11 session is the current one");
        check(InputSession::current() == session && session->nativeSurface() == 300, "it targets that window");

        initInputBlocker(true);
        setInputBlocked(true);
        setInputInteractive(true);
        check(isInputBlocked() && inputBlockingEnabled() && isInputBlockedAt(5.0f, 5.0f),
              "the free functions act on the current session");
        check(session->blockedAt(5.0f, 5.0f), "and that is the session itself");

        session->destroy();
        check(InputSession::current() == nullptr && !isX11(), "a destroyed session is no longer current");
    }

    // The device resolves its hotkeys when it is created, which an application
    // may do before it creates any surface: the lookup must not need a session.
    void keysymTests()
    {
        check(InputSession::current() == nullptr, "no session for the keysym lookup");
        check(convertToKeySym("Home") == 0xff50 && convertToKeySym("End") == 0xff57 && convertToKeySym("F10") == 0xffc7,
              "hotkey names resolve without a session");
        check(convertToKeySym("home") == 0xff50, "names are matched case-insensitively as a fallback");
        check(convertToKeySym("NotAKey") == 0, "an unknown name resolves to no key");
    }
} // namespace

int main()
{
    unsetenv("DISPLAY");

    creationTests();
    policyTests();
    blockingTests();
    withholdTests();
    threadTests();
    facadeTests();
    keysymTests();

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
