// Wayland event interposition for input blocking.
//
// On Wayland, there is no XGrabPointer equivalent. The game and our overlay
// both receive pointer/keyboard events from the compositor for the same
// surface. This module interposes on wl_proxy_add_listener and
// wl_proxy_add_dispatcher (the underlying C functions that the generated
// protocol helpers use) and wraps the game's pointer/keyboard callbacks.
//
// Blocking is a hitbox, not a global gate: the overlay publishes its rects,
// and only events positioned inside them are withheld from the game. Pointer
// motion/button/axis carry or inherit a position; keyboard has none, so it
// follows the last pointer position.
//
// Overlay-owned proxies are registered via registerOverlayProxy() and are
// always passed through unwrapped.

#include "wayland_interpose.hh"
#include "input_blocker.hh"
#include "wayland_display.hh"
#include "wayland_syncobj_gate.hh"
#include "wayland_interpose_patch.hh"
#include "logger.hh"

// Forward-declare to avoid pulling in mouse_input_wayland.hh (which
// transitively includes mouse_input.hh → X11 headers in some targets).
namespace VkHook { void mirrorButtonState(uint32_t button, bool pressed); }

#include <wayland-client.h>
#include <dlfcn.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <unistd.h>
#include <vector>

namespace VkHook
{
    namespace
    {
        // Vulkan layers are typically loaded RTLD_LOCAL, which prevents global
        // symbol interposition in normal implicit-layer mode. Promote this
        // already-loaded object to RTLD_GLOBAL so our wl_proxy wrappers are
        // visible process-wide.
        __attribute__((constructor)) void promoteSelfToGlobalScope()
        {
            Dl_info info{};
            if (dladdr(reinterpret_cast<void*>(&promoteSelfToGlobalScope), &info) == 0 || !info.dli_fname)
                return;

            void* handle = dlopen(info.dli_fname, RTLD_NOLOAD | RTLD_NOW | RTLD_GLOBAL);
            if (!handle)
                handle = dlopen(info.dli_fname, RTLD_NOW | RTLD_GLOBAL);

            if (handle)
                Logger::debug("Wayland interpose: promoted VkHook library to RTLD_GLOBAL");
        }
    } // namespace

    static std::mutex proxyMutex;
    static std::unordered_set<wl_proxy*> overlayProxies;

    void registerOverlayProxy(wl_proxy* proxy)
    {
        std::lock_guard<std::mutex> lock(proxyMutex);
        overlayProxies.insert(proxy);
    }

    void unregisterOverlayProxy(wl_proxy* proxy)
    {
        std::lock_guard<std::mutex> lock(proxyMutex);
        overlayProxies.erase(proxy);
    }

    static bool isOverlayProxy(wl_proxy* proxy)
    {
        std::lock_guard<std::mutex> lock(proxyMutex);
        return overlayProxies.count(proxy) > 0;
    }
} // namespace VkHook

// Last pointer position seen on a game pointer, in wl_surface-local coords.
// Events without coordinates (button, axis, key) are tested against this.
static std::atomic<double> g_lastPointerX{-1.0};
static std::atomic<double> g_lastPointerY{-1.0};

static bool pointerBlocked()
{
    return VkHook::isInputBlockedAt((float)g_lastPointerX.load(std::memory_order_acquire),
                                     (float)g_lastPointerY.load(std::memory_order_acquire));
}

// ── Game listener storage ────────────────────────────────────────────────────
//
// The game's listener table is copied when it registers, only as far as the
// version the game bound: a client built against older headers owns a shorter
// table, and reading the full current struct would run past it. Entries for
// events the game never knew stay null and are skipped.
//
// Registered with the game's own user data, so wl_proxy_get_user_data returns
// what the game stored and the wrappers can hand `data` straight back.
//
// Nothing here calls into the game while gameDataMutex is held: handlers run
// arbitrary game code, which may itself add listeners (re-entering this
// module) or block. Wrappers look an entry up under the lock, then call out
// with a shared_ptr snapshot after releasing it.

struct GamePointer
{
    wl_pointer_listener listener{};
};

struct GameKeyboard
{
    wl_keyboard_listener listener{};
};

struct GameDispatcher
{
    wl_dispatcher_func_t original = nullptr;
    const void* dispatcherData = nullptr;
};

static std::mutex gameDataMutex;
static std::unordered_map<wl_proxy*, std::shared_ptr<const GamePointer>> gamePointers;
static std::unordered_map<wl_proxy*, std::shared_ptr<const GameKeyboard>> gameKeyboards;
static std::unordered_map<wl_proxy*, std::shared_ptr<const GameDispatcher>> gamePointerDispatchers;
static std::unordered_map<wl_proxy*, std::shared_ptr<const GameDispatcher>> gameKeyboardDispatchers;

// Keys the game currently holds, tracked even while blocked so a hitbox
// crossing can release exactly those and re-press whatever is still held.
static std::unordered_map<wl_proxy*, std::unordered_set<uint32_t>> gameHeldKeys;

template <typename Map>
static auto lookupEntry(Map& map, wl_proxy* proxy) -> typename Map::mapped_type
{
    std::lock_guard<std::mutex> lock(gameDataMutex);
    auto it = map.find(proxy);
    return it == map.end() ? typename Map::mapped_type{} : it->second;
}

// ── Withheld keys ────────────────────────────────────────────────────────────
//
// The input blocker asks, from the present thread, for the game's held keys to
// be released (overlay took the keyboard) or re-pressed (it gave it back). The
// game's key handlers belong to the thread that dispatches the game's event
// queue, so they must not run on the present thread. The request is parked in
// an atomic and carried out by whichever wrapper runs next on the game's
// dispatch thread, before that event is processed. The cost is a bounded delay:
// the release lands with the next event the compositor sends the game, which
// while the pointer or keyboard is in use is immediate.

static std::atomic<bool> g_withholdWanted{false};
static std::atomic<bool> g_withholdApplied{false};

static void synthesizeKey(wl_proxy* kb, uint32_t key, uint32_t state)
{
    std::shared_ptr<const GameKeyboard> listener;
    std::shared_ptr<const GameDispatcher> dispatcher;
    {
        std::lock_guard<std::mutex> lock(gameDataMutex);
        if (auto it = gameKeyboards.find(kb); it != gameKeyboards.end())
            listener = it->second;
        if (auto it = gameKeyboardDispatchers.find(kb); it != gameKeyboardDispatchers.end())
            dispatcher = it->second;
    }
    // serial and time are zero: these are not compositor events
    if (listener && listener->listener.key)
        listener->listener.key(wl_proxy_get_user_data(kb), reinterpret_cast<wl_keyboard*>(kb), 0, 0, key, state);
    if (dispatcher && dispatcher->original)
    {
        union wl_argument args[4] = {};
        args[2].u = key;
        args[3].u = state;
        dispatcher->original(dispatcher->dispatcherData, kb, 3, &wl_keyboard_interface.events[3], args);
    }
}

static void applyPendingWithhold()
{
    bool wanted = g_withholdWanted.load(std::memory_order_acquire);
    bool applied = g_withholdApplied.load(std::memory_order_acquire);
    if (wanted == applied)
        return;
    // one thread carries out each transition
    if (!g_withholdApplied.compare_exchange_strong(applied, wanted, std::memory_order_acq_rel))
        return;

    std::vector<std::pair<wl_proxy*, uint32_t>> keys;
    {
        std::lock_guard<std::mutex> lock(gameDataMutex);
        for (const auto& [kb, held] : gameHeldKeys)
            for (uint32_t key : held)
                keys.emplace_back(kb, key);
    }

    const uint32_t state = wanted ? WL_KEYBOARD_KEY_STATE_RELEASED : WL_KEYBOARD_KEY_STATE_PRESSED;
    for (const auto& [kb, key] : keys)
        synthesizeKey(kb, key, state);

    if (!keys.empty())
        VkHook::Logger::debug(std::string("Wayland interpose: ") + (wanted ? "released " : "re-pressed ")
                              + std::to_string(keys.size()) + " held game key(s)");
}

static void noteKey(wl_proxy* kb, uint32_t key, uint32_t state)
{
    std::lock_guard<std::mutex> lock(gameDataMutex);
    std::unordered_set<uint32_t>& held = gameHeldKeys[kb];
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
        held.insert(key);
    else
        held.erase(key);
}

static void forgetKeys(wl_proxy* kb)
{
    std::lock_guard<std::mutex> lock(gameDataMutex);
    gameHeldKeys.erase(kb);
}

// ── Wrapper pointer listener ─────────────────────────────────────────────────

static void wp_enter(void* data, wl_pointer* p, uint32_t serial, wl_surface* s, wl_fixed_t x, wl_fixed_t y)
{
    applyPendingWithhold();
    g_lastPointerX.store(wl_fixed_to_double(x), std::memory_order_release);
    g_lastPointerY.store(wl_fixed_to_double(y), std::memory_order_release);
    auto game = lookupEntry(gamePointers, (wl_proxy*)p);
    if (game && game->listener.enter)
        game->listener.enter(data, p, serial, s, x, y);
}

static void wp_leave(void* data, wl_pointer* p, uint32_t serial, wl_surface* s)
{
    applyPendingWithhold();
    // Always forward leave — the game needs to know it lost focus
    auto game = lookupEntry(gamePointers, (wl_proxy*)p);
    if (game && game->listener.leave)
        game->listener.leave(data, p, serial, s);
}

static void wp_motion(void* data, wl_pointer* p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    applyPendingWithhold();
    const double fx = wl_fixed_to_double(x);
    const double fy = wl_fixed_to_double(y);
    g_lastPointerX.store(fx, std::memory_order_release);
    g_lastPointerY.store(fy, std::memory_order_release);
    if (VkHook::isInputBlockedAt((float)fx, (float)fy))
        return;
    auto game = lookupEntry(gamePointers, (wl_proxy*)p);
    if (game && game->listener.motion)
        game->listener.motion(data, p, time, x, y);
}

static void wp_button(void* data, wl_pointer* p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    applyPendingWithhold();
    // Always mirror button state to overlay — the game's pointer receives
    // releases via Wayland's implicit grab that our overlay pointer never sees.
    if (VkHook::isWayland())
        VkHook::mirrorButtonState(button, state == WL_POINTER_BUTTON_STATE_PRESSED);

    if (pointerBlocked())
        return;
    auto game = lookupEntry(gamePointers, (wl_proxy*)p);
    if (game && game->listener.button)
        game->listener.button(data, p, serial, time, button, state);
}

static void wp_axis(void* data, wl_pointer* p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    applyPendingWithhold();
    if (pointerBlocked())
        return;
    auto game = lookupEntry(gamePointers, (wl_proxy*)p);
    if (game && game->listener.axis)
        game->listener.axis(data, p, time, axis, value);
}

static void wp_frame(void* data, wl_pointer* p)
{
    applyPendingWithhold();
    if (pointerBlocked())
        return;
    auto game = lookupEntry(gamePointers, (wl_proxy*)p);
    if (game && game->listener.frame)
        game->listener.frame(data, p);
}

static void wp_axis_source(void* data, wl_pointer* p, uint32_t source)
{
    applyPendingWithhold();
    if (pointerBlocked())
        return;
    auto game = lookupEntry(gamePointers, (wl_proxy*)p);
    if (game && game->listener.axis_source)
        game->listener.axis_source(data, p, source);
}

static void wp_axis_stop(void* data, wl_pointer* p, uint32_t time, uint32_t axis)
{
    applyPendingWithhold();
    if (pointerBlocked())
        return;
    auto game = lookupEntry(gamePointers, (wl_proxy*)p);
    if (game && game->listener.axis_stop)
        game->listener.axis_stop(data, p, time, axis);
}

static void wp_axis_discrete(void* data, wl_pointer* p, uint32_t axis, int32_t discrete)
{
    applyPendingWithhold();
    if (pointerBlocked())
        return;
    auto game = lookupEntry(gamePointers, (wl_proxy*)p);
    if (game && game->listener.axis_discrete)
        game->listener.axis_discrete(data, p, axis, discrete);
}

static void wp_axis_value120(void* data, wl_pointer* p, uint32_t axis, int32_t value120)
{
    applyPendingWithhold();
    if (pointerBlocked())
        return;
    auto game = lookupEntry(gamePointers, (wl_proxy*)p);
    if (game && game->listener.axis_value120)
        game->listener.axis_value120(data, p, axis, value120);
}

// wl_pointer v9
static void wp_axis_relative_direction(void* data, wl_pointer* p, uint32_t axis, uint32_t direction)
{
    applyPendingWithhold();
    if (pointerBlocked())
        return;
    auto game = lookupEntry(gamePointers, (wl_proxy*)p);
    if (game && game->listener.axis_relative_direction)
        game->listener.axis_relative_direction(data, p, axis, direction);
}

// wl_pointer v11: a compositor-initiated warp of the pointer position
static void wp_warp(void* data, wl_pointer* p, wl_fixed_t x, wl_fixed_t y)
{
    applyPendingWithhold();
    g_lastPointerX.store(wl_fixed_to_double(x), std::memory_order_release);
    g_lastPointerY.store(wl_fixed_to_double(y), std::memory_order_release);
    if (VkHook::isInputBlockedAt((float)wl_fixed_to_double(x), (float)wl_fixed_to_double(y)))
        return;
    auto game = lookupEntry(gamePointers, (wl_proxy*)p);
    if (game && game->listener.warp)
        game->listener.warp(data, p, x, y);
}

// Every event of every version libwayland 1.26 knows has an entry: a missing
// one is a null (or out-of-table) function pointer that libwayland calls or
// aborts on as soon as the compositor sends that event to a newer client.
static const wl_pointer_listener wrapperPointerListener = {
    .enter = wp_enter,
    .leave = wp_leave,
    .motion = wp_motion,
    .button = wp_button,
    .axis = wp_axis,
    .frame = wp_frame,
    .axis_source = wp_axis_source,
    .axis_stop = wp_axis_stop,
    .axis_discrete = wp_axis_discrete,
    .axis_value120 = wp_axis_value120,
    .axis_relative_direction = wp_axis_relative_direction,
    .warp = wp_warp,
};

// ── Wrapper keyboard listener ────────────────────────────────────────────────

static void wk_keymap(void* data, wl_keyboard* kb, uint32_t format, int32_t fd, uint32_t size)
{
    applyPendingWithhold();
    // Always forward keymap — needed for initialization
    auto game = lookupEntry(gameKeyboards, (wl_proxy*)kb);
    if (game && game->listener.keymap)
        game->listener.keymap(data, kb, format, fd, size);
    else
        close(fd);  // the protocol hands us ownership of the descriptor
}

static void wk_enter(void* data, wl_keyboard* kb, uint32_t serial, wl_surface* s, wl_array* keys)
{
    applyPendingWithhold();
    // Forward enter so the game's keyboard state stays consistent; individual
    // keys are withheld in wk_key while the pointer is over the overlay.
    auto game = lookupEntry(gameKeyboards, (wl_proxy*)kb);
    if (game && game->listener.enter)
        game->listener.enter(data, kb, serial, s, keys);
}

static void wk_leave(void* data, wl_keyboard* kb, uint32_t serial, wl_surface* s)
{
    applyPendingWithhold();
    // Always forward leave
    auto game = lookupEntry(gameKeyboards, (wl_proxy*)kb);
    if (game && game->listener.leave)
        game->listener.leave(data, kb, serial, s);
    // the compositor releases every key on focus loss; so must our record of
    // them, or a later withhold would "release" keys the game never saw held
    forgetKeys((wl_proxy*)kb);
}

static void wk_key(void* data, wl_keyboard* kb, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    applyPendingWithhold();
    noteKey((wl_proxy*)kb, key, state);

    // keys follow the pointer hitbox plus the overlay's own key consumption
    if (pointerBlocked() || VkHook::isOverlayConsumingKeys())
        return;
    auto game = lookupEntry(gameKeyboards, (wl_proxy*)kb);
    if (game && game->listener.key)
        game->listener.key(data, kb, serial, time, key, state);
}

static void wk_modifiers(void* data, wl_keyboard* kb, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    applyPendingWithhold();
    // Always forward modifiers — games need modifier state for keybinds
    auto game = lookupEntry(gameKeyboards, (wl_proxy*)kb);
    if (game && game->listener.modifiers)
        game->listener.modifiers(data, kb, serial, depressed, latched, locked, group);
}

static void wk_repeat_info(void* data, wl_keyboard* kb, int32_t rate, int32_t delay)
{
    applyPendingWithhold();
    // Always forward repeat info
    auto game = lookupEntry(gameKeyboards, (wl_proxy*)kb);
    if (game && game->listener.repeat_info)
        game->listener.repeat_info(data, kb, rate, delay);
}

static const wl_keyboard_listener wrapperKeyboardListener = {
    .keymap = wk_keymap,
    .enter = wk_enter,
    .leave = wk_leave,
    .key = wk_key,
    .modifiers = wk_modifiers,
    .repeat_info = wk_repeat_info,
};

// ── Wrapper dispatcher callbacks ────────────────────────────────────────────

static int wd_pointer(const void* data, void* target, uint32_t opcode, const struct wl_message* msg, union wl_argument* args)
{
    auto* proxy = static_cast<wl_proxy*>(target);
    applyPendingWithhold();

    // enter/motion/warp carry surface coords; remember the latest for the
    // events that do not (button, axis, frame).
    if (opcode == 0) // enter: serial, surface, x, y
    {
        g_lastPointerX.store(wl_fixed_to_double(args[2].f), std::memory_order_release);
        g_lastPointerY.store(wl_fixed_to_double(args[3].f), std::memory_order_release);
    }
    else if (opcode == 2) // motion: time, x, y
    {
        g_lastPointerX.store(wl_fixed_to_double(args[1].f), std::memory_order_release);
        g_lastPointerY.store(wl_fixed_to_double(args[2].f), std::memory_order_release);
    }
    else if (opcode == 11) // warp: x, y
    {
        g_lastPointerX.store(wl_fixed_to_double(args[0].f), std::memory_order_release);
        g_lastPointerY.store(wl_fixed_to_double(args[1].f), std::memory_order_release);
    }

    if (opcode == 3 && VkHook::isWayland())
        VkHook::mirrorButtonState(args[2].u, args[3].u == WL_POINTER_BUTTON_STATE_PRESSED);

    // forward enter/leave so the game tracks the pointer, withhold everything
    // positional while it sits over the overlay (motion .. warp).
    if (opcode >= 2 && opcode <= 11 && pointerBlocked())
        return 0;

    auto game = lookupEntry(gamePointerDispatchers, proxy);
    if (game && game->original)
        return game->original(game->dispatcherData, target, opcode, msg, args);
    return 0;
}

static int wd_keyboard(const void* data, void* target, uint32_t opcode, const struct wl_message* msg, union wl_argument* args)
{
    auto* proxy = static_cast<wl_proxy*>(target);
    applyPendingWithhold();

    if (opcode == 3) // key: serial, time, key, state
        noteKey(proxy, args[2].u, args[3].u);
    else if (opcode == 2) // leave
        forgetKeys(proxy);

    // keyboard has no coordinates; it follows the hitbox and the overlay's
    // own key consumption
    if (opcode == 3 && (pointerBlocked() || VkHook::isOverlayConsumingKeys()))
        return 0;

    auto game = lookupEntry(gameKeyboardDispatchers, proxy);
    if (game && game->original)
        return game->original(game->dispatcherData, target, opcode, msg, args);
    return 0;
}

// ── Symbol interposition ─────────────────────────────────────────────────────
//
// wl_pointer_add_listener is a static inline in <wayland-client-protocol.h>
// that calls wl_proxy_add_listener. We interpose on the underlying C function
// so that even inlined calls are caught.

using AddListenerFn = int (*)(struct wl_proxy*, void (**)(void), void*);
using DlsymFn = void* (*)(void*, const char*);

static DlsymFn getRealDlsym()
{
    static DlsymFn fn = nullptr;
    if (fn)
        return fn;

    // Prefer a versioned lookup to avoid self-recursion when we interpose dlsym.
    fn = reinterpret_cast<DlsymFn>(dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5"));
    if (!fn)
        fn = reinterpret_cast<DlsymFn>(dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34"));
    return fn;
}

template <typename FnType>
static FnType resolveWaylandSymbol(const char* symbol)
{
    DlsymFn realDlsym = getRealDlsym();
    if (!realDlsym)
        return nullptr;

    // Preferred path for normal implicit-layer usage.
    void* sym = realDlsym(RTLD_NEXT, symbol);
    if (sym)
        return reinterpret_cast<FnType>(sym);

    // Fallback for preload / early-load scenarios where RTLD_NEXT may not
    // reach libwayland-client yet.
    static void* waylandHandle = nullptr;
    if (!waylandHandle)
    {
        waylandHandle = dlopen("libwayland-client.so.0", RTLD_NOLOAD | RTLD_NOW);
        if (!waylandHandle)
            waylandHandle = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    }

    if (waylandHandle)
    {
        sym = realDlsym(waylandHandle, symbol);
        if (sym)
            return reinterpret_cast<FnType>(sym);
    }

    return nullptr;
}

static AddListenerFn getRealAddListener()
{
    static AddListenerFn fn = resolveWaylandSymbol<AddListenerFn>("wl_proxy_add_listener");
    return fn;
}

using AddDispatcherFn = int (*)(struct wl_proxy*, wl_dispatcher_func_t, const void*, void*);
using MarshalArrayFlagsFn = wl_proxy* (*)(struct wl_proxy*, uint32_t, const struct wl_interface*, uint32_t, uint32_t, union wl_argument*);

static AddDispatcherFn getRealAddDispatcher()
{
    static AddDispatcherFn fn = resolveWaylandSymbol<AddDispatcherFn>("wl_proxy_add_dispatcher");
    return fn;
}

static MarshalArrayFlagsFn getRealMarshalArrayFlags(void* caller)
{
    // the installer records the real entry of every gated libwayland copy;
    // pick the copy the caller lives in
    if (void* gated = VkHook::waylandMarshalArrayFlagsForCaller(caller))
        return reinterpret_cast<MarshalArrayFlagsFn>(gated);

    // not gated (e.g. reached through symbol interposition): fall back to a
    // handle-scoped lookup, which can never recurse into this wrapper
    static std::atomic<MarshalArrayFlagsFn> fallback{nullptr};
    MarshalArrayFlagsFn fn = fallback.load(std::memory_order_acquire);
    if (fn)
        return fn;

    static void* waylandHandle = nullptr;
    if (!waylandHandle)
        waylandHandle = dlopen("libwayland-client.so.0", RTLD_NOLOAD | RTLD_NOW);
    if (!waylandHandle)
        waylandHandle = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
    if (DlsymFn real = getRealDlsym(); waylandHandle && real)
        fn = reinterpret_cast<MarshalArrayFlagsFn>(real(waylandHandle, "wl_proxy_marshal_array_flags"));

    if (fn)
        fallback.store(fn, std::memory_order_release);
    return fn;
}

extern "C" __attribute__((visibility("default")))
int wl_proxy_add_listener(struct wl_proxy* proxy,
                          void (**implementation)(void),
                          void* data)
{
    AddListenerFn real = getRealAddListener();
    if (!real)
        return -1;

    const char* cls = wl_proxy_get_class(proxy);
    if (!cls)
        return real(proxy, implementation, data);

    // Skip overlay-owned proxies — they use their own listeners
    if (VkHook::isOverlayProxy(proxy))
        return real(proxy, implementation, data);

    if (std::strcmp(cls, "wl_pointer") == 0)
    {
        auto game = std::make_shared<GamePointer>();
        // only the entries the bound version defines are the game's to read
        const uint32_t version = wl_proxy_get_version(proxy);
        const size_t entries = version >= 11 ? 12 : version >= 9 ? 11 : version >= 8 ? 10 : version >= 5 ? 9 : 5;
        std::memcpy(&game->listener, implementation, entries * sizeof(void (*)(void)));
        {
            std::lock_guard<std::mutex> lock(gameDataMutex);
            gamePointers[proxy] = std::move(game);
        }

        VkHook::Logger::debug("Wayland interpose: wrapped game pointer listener (v" + std::to_string(version) + ")");
        // the game's own user data stays on the proxy
        return real(proxy, (void (**)(void))&wrapperPointerListener, data);
    }

    if (std::strcmp(cls, "wl_keyboard") == 0)
    {
        auto game = std::make_shared<GameKeyboard>();
        const uint32_t version = wl_proxy_get_version(proxy);
        const size_t entries = version >= 4 ? 6 : 5;
        std::memcpy(&game->listener, implementation, entries * sizeof(void (*)(void)));
        {
            std::lock_guard<std::mutex> lock(gameDataMutex);
            gameKeyboards[proxy] = std::move(game);
            gameHeldKeys.erase(proxy);  // a recycled address must not inherit old keys
        }

        VkHook::Logger::debug("Wayland interpose: wrapped game keyboard listener (v" + std::to_string(version) + ")");
        return real(proxy, (void (**)(void))&wrapperKeyboardListener, data);
    }

    return real(proxy, implementation, data);
}

extern "C" __attribute__((visibility("default")))
int wl_proxy_add_dispatcher(struct wl_proxy* proxy,
                            wl_dispatcher_func_t dispatcher_func,
                            const void* dispatcher_data,
                            void* data);

extern "C" __attribute__((visibility("default")))
wl_proxy* wl_proxy_marshal_array_flags(struct wl_proxy* proxy,
                                       uint32_t opcode,
                                       const struct wl_interface* iface,
                                       uint32_t version,
                                       uint32_t flags,
                                       union wl_argument* args)
{
    MarshalArrayFlagsFn real = getRealMarshalArrayFlags(__builtin_return_address(0));
    if (!real)
        return nullptr;

    // a destroying request (wl_keyboard.release, wl_pointer.release) ends the
    // proxy: forget the game's listener and held keys before the address can be
    // reused
    if (flags & WL_MARSHAL_FLAG_DESTROY)
    {
        const char* cls = wl_proxy_get_class(proxy);
        if (cls && (std::strcmp(cls, "wl_keyboard") == 0 || std::strcmp(cls, "wl_pointer") == 0))
        {
            std::lock_guard<std::mutex> lock(gameDataMutex);
            gamePointers.erase(proxy);
            gamePointerDispatchers.erase(proxy);
            gameKeyboards.erase(proxy);
            gameKeyboardDispatchers.erase(proxy);
            gameHeldKeys.erase(proxy);
        }
    }

    // A window commit must not interleave with a present's explicit-sync
    // points (see wayland_syncobj_gate.hh). The gate's shared lock is held for
    // the whole send, so a present cannot open between the check and the
    // marshal; there is no unlocked window to race.
    if (opcode == 6)
    {
        const char* cls = wl_proxy_get_class(proxy);
        if (cls && std::strcmp(cls, "wl_surface") == 0)
        {
            VkHook::WaylandSyncobjGate::CommitScope commit(wl_proxy_get_id(proxy));
            return real(proxy, opcode, iface, version, flags, args);
        }
    }

    return real(proxy, opcode, iface, version, flags, args);
}

extern "C" __attribute__((visibility("default")))
void* dlsym(void* handle, const char* symbol)
{
    DlsymFn realDlsym = getRealDlsym();
    if (!realDlsym)
        return nullptr;

    void* resolved = realDlsym(handle, symbol);

    // Keep behavior unchanged unless VkHook is explicitly enabled.
    // Cache the check — this function is called for every dlsym in the process.
    static const bool VkHookEnabled = []{
        const char* e = std::getenv("ENABLE_VKHOOK");
        return e && std::strcmp(e, "1") == 0;
    }();
    if (!VkHookEnabled || !resolved)
        return resolved;

    bool wantsListener = (std::strcmp(symbol, "wl_proxy_add_listener") == 0);
    bool wantsDispatcher = (std::strcmp(symbol, "wl_proxy_add_dispatcher") == 0);
    bool wantsMarshalArrayFlags = (std::strcmp(symbol, "wl_proxy_marshal_array_flags") == 0);
    if (!wantsListener && !wantsDispatcher && !wantsMarshalArrayFlags)
        return resolved;

    Dl_info info{};
    if (dladdr(resolved, &info) == 0 || !info.dli_fname)
        return resolved;

    std::string module(info.dli_fname);
    std::transform(module.begin(), module.end(), module.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // Only redirect lookups that currently resolve to libwayland-client.
    if (module.find("libwayland-client") == std::string::npos)
    {
        return resolved;
    }

    if (wantsListener)
        return reinterpret_cast<void*>(&wl_proxy_add_listener);
    if (wantsDispatcher)
        return reinterpret_cast<void*>(&wl_proxy_add_dispatcher);
    return reinterpret_cast<void*>(&wl_proxy_marshal_array_flags);
}

extern "C" __attribute__((visibility("default")))
int wl_proxy_add_dispatcher(struct wl_proxy* proxy,
                            wl_dispatcher_func_t dispatcher_func,
                            const void* dispatcher_data,
                            void* data)
{
    AddDispatcherFn real = getRealAddDispatcher();
    if (!real)
        return -1;

    const char* cls = wl_proxy_get_class(proxy);
    if (!cls)
        return real(proxy, dispatcher_func, dispatcher_data, data);

    // Skip overlay-owned proxies — they use their own dispatchers
    if (VkHook::isOverlayProxy(proxy))
        return real(proxy, dispatcher_func, dispatcher_data, data);

    if (std::strcmp(cls, "wl_pointer") == 0)
    {
        auto game = std::make_shared<GameDispatcher>();
        game->original = dispatcher_func;
        game->dispatcherData = dispatcher_data;
        {
            std::lock_guard<std::mutex> lock(gameDataMutex);
            gamePointerDispatchers[proxy] = std::move(game);
        }

        VkHook::Logger::debug("Wayland interpose: wrapped game pointer dispatcher");
        return real(proxy, wd_pointer, nullptr, data);
    }

    if (std::strcmp(cls, "wl_keyboard") == 0)
    {
        auto game = std::make_shared<GameDispatcher>();
        game->original = dispatcher_func;
        game->dispatcherData = dispatcher_data;
        {
            std::lock_guard<std::mutex> lock(gameDataMutex);
            gameKeyboardDispatchers[proxy] = std::move(game);
            gameHeldKeys.erase(proxy);
        }

        VkHook::Logger::debug("Wayland interpose: wrapped game keyboard dispatcher");
        return real(proxy, wd_keyboard, nullptr, data);
    }

    return real(proxy, dispatcher_func, dispatcher_data, data);
}

namespace VkHook
{
    void withholdGameKeys(bool withhold)
    {
        // Only records the request. The game's handlers must run on the thread
        // that dispatches the game's queue, never here on the present thread,
        // so the next wrapper to run there carries it out.
        g_withholdWanted.store(withhold, std::memory_order_release);
    }
} // namespace VkHook

namespace VkHook::WaylandSyncobjGate
{
    uint32_t waylandSurfaceId()
    {
        if (!VkHook::isWayland())
            return 0;

        wl_surface* surface = VkHook::getWaylandSurface();
        return surface ? wl_proxy_get_id(reinterpret_cast<struct wl_proxy*>(surface)) : 0;
    }
} // namespace VkHook::WaylandSyncobjGate

