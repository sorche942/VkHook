// WaylandInput against a real libwayland client and a fake in-process server
// (libwayland-server on a socketpair). The server advertises a seat with a
// keyboard and a pointer, a compositor, a subcompositor and wl_shm, and accepts
// every request. This checks what no compositor-free test can:
//   - tearing the input down leaves no proxy attached to its event queue (libwayland
//     logs "queue destroyed while proxies still attached" otherwise: the old
//     cleanup destroyed the queue and seat before the pointer, and the display
//     wrapper after the queue);
//   - the capture subsurface is re-created on the new parent when the game
//     recreates its surface, and is destroyed again at teardown;
//   - the input can be brought up again after a shutdown.
#include "wayland_input.hh"
#include "wayland_interpose.hh"

#include <wayland-client.h>
#include <wayland-server.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace VkHook
{
    void registerOverlayProxy(wl_proxy*)
    {
    }

    void unregisterOverlayProxy(wl_proxy*)
    {
    }

    void withholdGameKeys(bool)
    {
    }

    // The game drew a 3840x2160 swapchain into a 2259x1271 window, as SDL does
    // on a 1.7 output scale with wp_viewport.
    void gameSurfaceLogicalSize(wl_proxy*, uint32_t bufferWidth, uint32_t bufferHeight, double& width, double& height)
    {
        width = bufferWidth * (2259.0 / 3840.0);
        height = bufferHeight * (1271.0 / 2160.0);
    }
} // namespace VkHook

namespace
{
    int failures = 0;

    void check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
            ++failures;
    }

    // ── client-side libwayland log capture ───────────────────────────────────

    std::mutex logMutex;
    std::vector<std::string> clientLog;

    void captureLog(const char* format, va_list args)
    {
        char buffer[512];
        vsnprintf(buffer, sizeof(buffer), format, args);
        std::lock_guard<std::mutex> lock(logMutex);
        clientLog.emplace_back(buffer);
    }

    size_t countLog(const char* needle)
    {
        std::lock_guard<std::mutex> lock(logMutex);
        size_t n = 0;
        for (const std::string& line : clientLog)
            if (line.find(needle) != std::string::npos)
                ++n;
        return n;
    }

    // ── fake server ──────────────────────────────────────────────────────────

    struct SubsurfaceEvent
    {
        bool created;
        uint32_t parentId;
    };

    struct Server
    {
        wl_display* display = nullptr;
        std::thread thread;
        std::atomic<bool> stop{false};
        std::mutex mutex;
        std::vector<SubsurfaceEvent> subsurfaceEvents;
        std::atomic<int> liveSubsurfaces{0};
        // the latest capture buffer and subsurface position the client sent
        std::atomic<int> bufferWidth{0}, bufferHeight{0};
        std::atomic<int> positionX{-1}, positionY{-1};
    };

    Server server;

    // Accepts every request. Requests that create an object get a server-side
    // resource driven by this same dispatcher; destructor requests destroy it.
    int dispatch(const void*, void* target, uint32_t, const wl_message* message, wl_argument* args)
    {
        wl_resource* resource = static_cast<wl_resource*>(target);
        wl_client* client = wl_resource_get_client(resource);
        const char* iface = wl_resource_get_class(resource);

        int arg = 0;
        for (const char* c = message->signature; *c; ++c)
        {
            if (*c >= '0' && *c <= '9')
                continue;
            if (*c == '?')
                continue;
            if (*c == 'n')
            {
                wl_resource* created = wl_resource_create(client, message->types[arg],
                                                          wl_resource_get_version(resource), args[arg].n);
                wl_resource_set_dispatcher(created, dispatch, nullptr, nullptr, nullptr);
                if (std::strcmp(message->types[arg]->name, "wl_subsurface") == 0)
                {
                    const uint32_t parent = wl_resource_get_id(reinterpret_cast<wl_resource*>(args[2].o));
                    std::lock_guard<std::mutex> lock(server.mutex);
                    server.subsurfaceEvents.push_back({true, parent});
                    ++server.liveSubsurfaces;
                }
            }
            else if (*c == 'h')
            {
                close(args[arg].h);
            }
            ++arg;
        }

        if (std::strcmp(iface, "wl_shm_pool") == 0 && std::strcmp(message->name, "create_buffer") == 0)
        {
            server.bufferWidth = args[2].i;
            server.bufferHeight = args[3].i;
        }
        if (std::strcmp(iface, "wl_subsurface") == 0 && std::strcmp(message->name, "set_position") == 0)
        {
            server.positionX = args[0].i;
            server.positionY = args[1].i;
        }

        if (std::strcmp(message->name, "destroy") == 0 || std::strcmp(message->name, "release") == 0)
        {
            if (std::strcmp(iface, "wl_subsurface") == 0)
            {
                std::lock_guard<std::mutex> lock(server.mutex);
                server.subsurfaceEvents.push_back({false, 0});
                --server.liveSubsurfaces;
            }
            wl_resource_destroy(resource);
        }
        return 0;
    }

    void bindGlobal(wl_client* client, void* data, uint32_t version, uint32_t id)
    {
        const wl_interface* iface = static_cast<const wl_interface*>(data);
        wl_resource* resource = wl_resource_create(client, iface, version, id);
        wl_resource_set_dispatcher(resource, dispatch, nullptr, nullptr, nullptr);
        if (iface == &wl_seat_interface)
            wl_resource_post_event(resource, 0, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    }

    int connectClient()
    {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
            return -1;

        server.display = wl_display_create();
        wl_global_create(server.display, &wl_compositor_interface, 4, const_cast<wl_interface*>(&wl_compositor_interface), bindGlobal);
        wl_global_create(server.display, &wl_subcompositor_interface, 1, const_cast<wl_interface*>(&wl_subcompositor_interface), bindGlobal);
        wl_global_create(server.display, &wl_shm_interface, 1, const_cast<wl_interface*>(&wl_shm_interface), bindGlobal);
        wl_global_create(server.display, &wl_seat_interface, 5, const_cast<wl_interface*>(&wl_seat_interface), bindGlobal);
        wl_client_create(server.display, fds[0]);

        server.thread = std::thread([] {
            wl_event_loop* loop = wl_display_get_event_loop(server.display);
            while (!server.stop.load())
            {
                wl_event_loop_dispatch(loop, 5);
                wl_display_flush_clients(server.display);
            }
        });
        return fds[1];
    }

    void stopServer()
    {
        server.stop = true;
        server.thread.join();
        wl_display_destroy(server.display);
    }

    // ── client helpers ───────────────────────────────────────────────────────

    wl_compositor* compositor = nullptr;

    void onGlobal(void*, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
    {
        if (std::strcmp(interface, wl_compositor_interface.name) == 0)
            compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface,
                                                                      version < 4 ? version : 4));
    }

    void onGlobalRemove(void*, wl_registry*, uint32_t)
    {
    }

    const wl_registry_listener registryListener = {onGlobal, onGlobalRemove};

    // Two "game" surfaces, as the game's own window would be.
    void makeGameSurfaces(wl_display* display, wl_surface** first, wl_surface** second)
    {
        wl_registry* registry = wl_display_get_registry(display);
        wl_registry_add_listener(registry, &registryListener, nullptr);
        wl_display_roundtrip(display);
        *first = wl_compositor_create_surface(compositor);
        *second = wl_compositor_create_surface(compositor);
        wl_display_roundtrip(display);
        wl_registry_destroy(registry);
    }

    // The detector must see the defect it guards against: destroy a queue while a
    // proxy created on it is alive, as the old teardown did.
    void negativeControl(wl_display* display)
    {
        const size_t before = countLog("destroyed while proxies still attached");
        wl_event_queue* queue = wl_display_create_queue(display);
        wl_display* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
        wl_registry* registry = wl_display_get_registry(wrapper);

        wl_event_queue_destroy(queue);   // wrong order: the registry is still attached
        const bool seen = countLog("destroyed while proxies still attached") > before;
        wl_registry_destroy(registry);
        wl_proxy_wrapper_destroy(wrapper);
        check(seen, "control: destroying the queue first is reported by libwayland");
    }

    std::vector<SubsurfaceEvent> subsurfaceEvents()
    {
        std::lock_guard<std::mutex> lock(server.mutex);
        return server.subsurfaceEvents;
    }

    // the server learns of a request only after its thread ran; wait for it
    template <typename Pred>
    bool eventually(Pred pred)
    {
        for (int i = 0; i < 400; ++i)
        {
            if (pred())
                return true;
            usleep(5000);
        }
        return pred();
    }
} // namespace

int main()
{
    wl_log_set_handler_client(captureLog);

    const int fd = connectClient();
    if (fd < 0)
    {
        std::printf("SKIP cannot create a socket pair\n");
        return 77;
    }
    wl_display* display = wl_display_connect_to_fd(fd);
    if (!display)
    {
        stopServer();
        std::printf("SKIP cannot connect to the fake server\n");
        return 77;
    }

    wl_surface *surfaceA = nullptr, *surfaceB = nullptr;
    makeGameSurfaces(display, &surfaceA, &surfaceB);
    const uint32_t idA = wl_proxy_get_id(reinterpret_cast<wl_proxy*>(surfaceA));
    const uint32_t idB = wl_proxy_get_id(reinterpret_cast<wl_proxy*>(surfaceB));

    negativeControl(display);

    {
        VkHook::WaylandInput input(display, reinterpret_cast<uintptr_t>(surfaceA));

        // bring up keyboard and pointer on the shared seat and queue
        input.mouseState();
        input.keyboardState();
        const VkHook::InputRect rect{10.0f, 10.0f, 200.0f, 100.0f};
        input.setCaptureRects(&rect, 1);

        check(eventually([] { return server.liveSubsurfaces.load() == 1; }),
              "the capture subsurface exists once input is up");
        {
            const auto events = subsurfaceEvents();
            check(!events.empty() && events[0].created && events[0].parentId == idA,
                  "it is attached to the game's surface");
        }

        // the game recreates its window: a new wl_surface on the same display
        input.setParentSurface(reinterpret_cast<uintptr_t>(surfaceB));
        wl_display_roundtrip(display);
        check(eventually([] { return subsurfaceEvents().size() >= 3; }),
              "recreating the surface destroys the old capture and creates a new one");
        {
            const auto events = subsurfaceEvents();
            check(events.size() == 3 && !events[1].created && events[2].created && events[2].parentId == idB,
                  "the new capture hangs off the new surface");
        }
        check(server.liveSubsurfaces.load() == 1, "exactly one capture subsurface is live");

        // The overlay's hitboxes are framebuffer pixels; the subsurface lives in
        // the window's surface-local units. A full-frame capture must cover the
        // window exactly: sized in pixels it grew the window 1.7x, and the game
        // answered with a bigger swapchain.
        input.setFramebufferExtent(3840, 2160);
        const VkHook::InputRect fullFrame{0.0f, 0.0f, 3840.0f, 2160.0f};
        input.setCaptureRects(&fullFrame, 1);
        wl_display_roundtrip(display);
        check(eventually([] { return server.bufferWidth.load() == 2259 && server.bufferHeight.load() == 1271; }),
              "a full-frame capture is the window's logical size, not its pixel size");
        check(eventually([] { return server.positionX.load() == 0 && server.positionY.load() == 0; }),
              "and sits at the window's origin");
        check(input.framebufferPerLogicalX() > 1.69 && input.framebufferPerLogicalX() < 1.71,
              "pointer coordinates are scaled by the framebuffer's pixels per logical unit");

        input.shutdown();
        check(eventually([] { return server.liveSubsurfaces.load() == 0; }),
              "shutdown destroys the capture subsurface");
        check(countLog("destroyed while proxies still attached") == 1,
              "shutdown leaves no proxy attached to the event queue (only the control was reported)");

        // the next poll re-creates everything, on the surface the game now uses
        input.mouseState();
        input.setCaptureRects(&rect, 1);
        check(eventually([] { return server.liveSubsurfaces.load() == 1; }),
              "input comes up again after a shutdown");
        {
            const auto events = subsurfaceEvents();
            check(events.back().created && events.back().parentId == idB, "and attaches to the current surface");
        }

        input.close();
        input.mouseState();
        check(eventually([] { return server.liveSubsurfaces.load() == 0; }), "close destroys the capture subsurface");
        check(countLog("destroyed while proxies still attached") == 1, "close leaves no proxy attached either");
        check(server.liveSubsurfaces.load() == 0, "a closed input is not re-created by a later poll");
    }

    check(countLog("destroyed while proxies still attached") == 1, "destruction logs no further warnings");

    wl_surface_destroy(surfaceA);
    wl_surface_destroy(surfaceB);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    stopServer();

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
