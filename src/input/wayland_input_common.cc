#include "logger.hh"
#include "wayland_input.hh"
#include "wayland_interpose.hh"

#include "viewporter-client-protocol.h"

#include <wayland-client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace VkHook
{
    struct WaylandSeatListeners
    {
        static void capabilities(void* data, wl_seat* /*seat*/, uint32_t caps)
        {
            static_cast<WaylandInput*>(data)->onSeatCapabilities(caps);
        }

        static void name(void* /*data*/, wl_seat* /*seat*/, const char* /*name*/)
        {
        }

        static void global(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
        {
            static_cast<WaylandInput*>(data)->onRegistryGlobal(registry, name, interface, version);
        }

        static void globalRemove(void* /*data*/, wl_registry* /*registry*/, uint32_t /*name*/)
        {
        }

        static constexpr wl_seat_listener seat = {
            .capabilities = capabilities,
            .name = name,
        };

        static constexpr wl_registry_listener registry = {
            .global = global,
            .global_remove = globalRemove,
        };
    };

    WaylandInput::WaylandInput(wl_display* display, uintptr_t parentSurface)
        : m_display(display)
        , m_parentAtomic(parentSurface)
    {
        m_attachment.setParent(parentSurface);
    }

    WaylandInput::~WaylandInput()
    {
        shutdown();
    }

    void WaylandInput::onSeatCapabilities(uint32_t capabilities)
    {
        m_seatCapabilities = capabilities;
        bindDevices();
    }

    // Binds the devices a poll asked for and the seat offers. The seat reports
    // capabilities once, during the second roundtrip of ensureCommon, and again
    // if a device appears later; a device wanted after the first report binds
    // from the stored set.
    void WaylandInput::bindDevices()
    {
        if (m_wantKeyboard && (m_seatCapabilities & WL_SEAT_CAPABILITY_KEYBOARD))
            bindKeyboard();
        if (m_wantPointer && (m_seatCapabilities & WL_SEAT_CAPABILITY_POINTER))
            bindPointer();
    }

    void WaylandInput::onRegistryGlobal(wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
    {
        // Each bound global is pushed on the release stack as it is created, so
        // teardown unwinds them newest first, all before the registry and queue.
        if (std::strcmp(interface, wl_seat_interface.name) == 0)
        {
            if (m_seat)
                return;
            m_seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface,
                                                            version < 5 ? version : 5));
            m_releases.push([this] {
                wl_seat_destroy(m_seat);
                m_seat = nullptr;
            });
            wl_seat_add_listener(m_seat, &WaylandSeatListeners::seat, this);
            Logger::debug("Wayland: shared seat bound");
        }
        else if (std::strcmp(interface, wl_compositor_interface.name) == 0)
        {
            if (m_compositor)
                return;
            m_compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface,
                                                                        version < 4 ? version : 4));
            m_releases.push([this] {
                wl_compositor_destroy(m_compositor);
                m_compositor = nullptr;
            });
        }
        else if (std::strcmp(interface, wl_subcompositor_interface.name) == 0)
        {
            if (m_subcompositor)
                return;
            m_subcompositor = static_cast<wl_subcompositor*>(
                wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
            m_releases.push([this] {
                wl_subcompositor_destroy(m_subcompositor);
                m_subcompositor = nullptr;
            });
        }
        else if (std::strcmp(interface, wl_shm_interface.name) == 0)
        {
            if (m_shm)
                return;
            m_shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
            m_releases.push([this] {
                wl_shm_destroy(m_shm);
                m_shm = nullptr;
            });
        }
        else if (std::strcmp(interface, wp_viewporter_interface.name) == 0)
        {
            if (m_viewporter)
                return;
            m_viewporter = static_cast<wp_viewporter*>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
            m_releases.push([this] {
                wp_viewporter_destroy(m_viewporter);
                m_viewporter = nullptr;
            });
        }
    }

    // ── Input-capture surface ────────────────────────────────────────────────

    void WaylandInput::destroyInputBuffer()
    {
        if (m_inputBuffer)
        {
            wl_buffer_destroy(m_inputBuffer);
            m_inputBuffer = nullptr;
        }
        if (m_inputPool)
        {
            wl_shm_pool_destroy(m_inputPool);
            m_inputPool = nullptr;
        }
        if (m_inputBufferData)
        {
            munmap(m_inputBufferData, m_inputBufferSize);
            m_inputBufferData = nullptr;
        }
        if (m_inputShmFd >= 0)
        {
            ::close(m_inputShmFd);
            m_inputShmFd = -1;
        }
        m_inputBufferSize = 0;
        m_inputBufferWidth = 0;
        m_inputBufferHeight = 0;
    }

    // A fully transparent ARGB buffer. It exists only so the surface is mapped
    // (an unmapped surface receives no input); nothing is ever drawn into it.
    // With wp_viewporter it is a single pixel scaled up to the hitbox; without
    // it the buffer is allocated at the hitbox size.
    bool WaylandInput::ensureInputBuffer(int hitboxWidth, int hitboxHeight)
    {
        const int width = m_inputViewport ? 1 : hitboxWidth;
        const int height = m_inputViewport ? 1 : hitboxHeight;
        if (m_inputBuffer && m_inputBufferWidth == width && m_inputBufferHeight == height)
            return true;

        const size_t size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;

        char name[64];
        std::snprintf(name, sizeof(name), "/vkhook-input-%d", static_cast<int>(getpid()));
        const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
        {
            Logger::warn("Wayland: shm_open failed for the input buffer");
            return false;
        }
        shm_unlink(name);

        if (ftruncate(fd, static_cast<off_t>(size)) != 0)
        {
            ::close(fd);
            return false;
        }

        void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED)
        {
            ::close(fd);
            return false;
        }
        std::memset(data, 0, size);

        wl_shm_pool* pool = wl_shm_create_pool(m_shm, fd, static_cast<int32_t>(size));
        wl_buffer* buffer = pool
            ? wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888)
            : nullptr;
        if (!buffer)
        {
            if (pool)
                wl_shm_pool_destroy(pool);
            munmap(data, size);
            ::close(fd);
            return false;
        }

        // swap only once the replacement is complete: tearing the old buffer
        // down first would leave the surface unmapped for an instant, and an
        // unmapped surface loses the pointer focus, which reads as a frozen
        // cursor until the user moves again.
        destroyInputBuffer();

        m_inputBuffer = buffer;
        m_inputPool = pool;
        m_inputBufferData = data;
        m_inputBufferSize = size;
        m_inputShmFd = fd;
        m_inputBufferWidth = width;
        m_inputBufferHeight = height;
        return true;
    }

    void WaylandInput::flush()
    {
        wl_display_flush(m_display);
    }

    // Children before parents: the buffer and viewport hang off the surface, and
    // the surface must outlive its subsurface role object's destruction request.
    void WaylandInput::destroyCapture()
    {
        destroyInputBuffer();
        if (m_inputViewport)
        {
            wp_viewport_destroy(m_inputViewport);
            m_inputViewport = nullptr;
        }
        if (m_inputRegion)
        {
            wl_region_destroy(m_inputRegion);
            m_inputRegion = nullptr;
        }
        if (m_inputSubsurface)
        {
            wl_subsurface_destroy(m_inputSubsurface);
            m_inputSubsurface = nullptr;
        }
        if (m_inputSurface)
        {
            wl_surface_destroy(m_inputSurface);
            m_inputSurface = nullptr;
        }
        m_inputSurfaceX = 0.0f;
        m_inputSurfaceY = 0.0f;
        m_publishedRects.clear();
    }

    bool WaylandInput::createCapture(uintptr_t parent)
    {
        if (m_inputSurface || !m_compositor || !m_subcompositor || !m_shm || parent == 0)
            return false;

        m_inputSurface = wl_compositor_create_surface(m_compositor);
        if (!m_inputSurface)
            return false;

        m_inputSubsurface = wl_subcompositor_get_subsurface(m_subcompositor, m_inputSurface,
                                                            reinterpret_cast<wl_surface*>(parent));
        if (!m_inputSubsurface)
        {
            destroyCapture();
            return false;
        }

        // apply our commits on their own rather than waiting on the parent's
        wl_subsurface_set_desync(m_inputSubsurface);

        if (m_viewporter)
            m_inputViewport = wp_viewporter_get_viewport(m_viewporter, m_inputSurface);

        // start with an empty region: everything passes through until the
        // overlay publishes a hitbox.
        m_inputRegion = wl_compositor_create_region(m_compositor);
        wl_surface_set_input_region(m_inputSurface, m_inputRegion);
        wl_surface_commit(m_inputSurface);
        flush();

        Logger::info("Wayland: input capture surface created");
        return true;
    }

    // Brings the capture subsurface in line with the game's current surface. A
    // subsurface is bound to its parent for life, so a recreated game window
    // needs a fresh one; the hitboxes are republished by publishCaptureRects().
    void WaylandInput::reconcileCapture()
    {
        const CaptureAttachment::Step step = m_attachment.next();
        if (step == CaptureAttachment::Step::None)
            return;

        if (step == CaptureAttachment::Step::Detach || step == CaptureAttachment::Step::Reattach)
            destroyCapture();

        bool ok = true;
        if (step == CaptureAttachment::Step::Attach || step == CaptureAttachment::Step::Reattach)
            ok = createCapture(m_attachment.parent());

        m_attachment.completed(step, ok);
        if (m_attachment.takeRepublish())
            m_publishedRects.clear();
    }

    void WaylandInput::setParentSurface(uintptr_t parent)
    {
        std::lock_guard<std::mutex> lock(m_lifecycle);
        m_parentAtomic.store(parent, std::memory_order_release);
        m_attachment.setParent(parent);
        if (m_commonReady)
        {
            reconcileCapture();
            publishCaptureRects();
        }
    }

    uint32_t WaylandInput::parentSurfaceId() const
    {
        const uintptr_t parent = m_parentAtomic.load(std::memory_order_acquire);
        return parent ? wl_proxy_get_id(reinterpret_cast<wl_proxy*>(parent)) : 0;
    }

    bool WaylandInput::hasFocus() const
    {
        return m_keyboardFocus.load(std::memory_order_acquire) || m_pointerFocus.load(std::memory_order_acquire);
    }

    void WaylandInput::setCaptureRects(const InputRect* rects, int count)
    {
        std::vector<InputRect> valid;
        valid.reserve(count > 0 ? static_cast<size_t>(count) : 0);
        for (int i = 0; i < count; ++i)
        {
            if (rects[i].width > 0.0f && rects[i].height > 0.0f)
                valid.push_back(rects[i]);
        }

        std::lock_guard<std::mutex> lock(m_lifecycle);
        m_desiredRects = std::move(valid);
        if (!m_commonReady)
            return;
        reconcileCapture();
        publishCaptureRects();
    }

    void WaylandInput::setFramebufferExtent(uint32_t width, uint32_t height)
    {
        const uintptr_t parent = m_parentAtomic.load(std::memory_order_acquire);
        if (!parent || width == 0 || height == 0)
            return;
        double logicalWidth = 0.0, logicalHeight = 0.0;
        gameSurfaceLogicalSize(reinterpret_cast<wl_proxy*>(parent), width, height, logicalWidth, logicalHeight);
        if (logicalWidth <= 0.0 || logicalHeight <= 0.0)
            return;
        const double perX = width / logicalWidth;
        const double perY = height / logicalHeight;
        const bool changedX = m_fbPerLogicalX.exchange(perX, std::memory_order_acq_rel) != perX;
        const bool changedY = m_fbPerLogicalY.exchange(perY, std::memory_order_acq_rel) != perY;
        if (changedX || changedY)
        {
            Logger::debug([&] {
                return "Wayland: framebuffer " + std::to_string(width) + "x" + std::to_string(height) + " in a "
                    + std::to_string(logicalWidth) + "x" + std::to_string(logicalHeight) + " surface";
            });
        }
    }

    void WaylandInput::publishCaptureRects()
    {
        if (!m_inputSurface)
            return;

        // the overlay calls this every frame; only churn the compositor when the
        // hitbox set or the scale it maps by actually changes.
        const std::vector<InputRect>& valid = m_desiredRects;
        const double perX = framebufferPerLogicalX();
        const double perY = framebufferPerLogicalY();
        if (valid == m_publishedRects && perX == m_publishedPerLogicalX && perY == m_publishedPerLogicalY)
            return;
        m_publishedRects = valid;
        m_publishedPerLogicalX = perX;
        m_publishedPerLogicalY = perY;

        // a region is immutable once handed to set_input_region, so rebuild it.
        if (m_inputRegion)
            wl_region_destroy(m_inputRegion);
        m_inputRegion = wl_compositor_create_region(m_compositor);
        if (!m_inputRegion)
            return;

        if (valid.empty())
        {
            m_inputSurfaceX = 0.0f;
            m_inputSurfaceY = 0.0f;
            // nothing to capture: unmap the surface outright (and keep the
            // region empty) so the pointer passes straight through to the game
            wl_surface_attach(m_inputSurface, nullptr, 0, 0);
            wl_surface_set_input_region(m_inputSurface, m_inputRegion);
            wl_surface_commit(m_inputSurface);
            flush();
            return;
        }

        // Hitboxes are in framebuffer pixels; the subsurface and its region are
        // surface-local. Outward rounding keeps every hitbox covered, and a
        // capture surface no larger than the window keeps the window's size.
        struct LogicalRect
        {
            int x0, y0, x1, y1;
        };
        auto toLogical = [&](const InputRect& r) {
            return LogicalRect{static_cast<int>(std::floor(r.x / perX)), static_cast<int>(std::floor(r.y / perY)),
                               static_cast<int>(std::ceil((r.x + r.width) / perX)),
                               static_cast<int>(std::ceil((r.y + r.height) / perY))};
        };

        // the capture surface is one rectangle, so cover the bounding box of
        // every hitbox and add each one to the region. popups stick out past the
        // main window, and this is what keeps them from leaking to the game.
        LogicalRect bounds = toLogical(valid[0]);
        for (const InputRect& r : valid)
        {
            const LogicalRect l = toLogical(r);
            bounds.x0 = std::min(bounds.x0, l.x0);
            bounds.y0 = std::min(bounds.y0, l.y0);
            bounds.x1 = std::max(bounds.x1, l.x1);
            bounds.y1 = std::max(bounds.y1, l.y1);
        }

        const int w = bounds.x1 - bounds.x0;
        const int h = bounds.y1 - bounds.y0;
        if (w <= 0 || h <= 0 || !ensureInputBuffer(w, h))
            return;

        m_inputSurfaceX = static_cast<float>(bounds.x0);
        m_inputSurfaceY = static_cast<float>(bounds.y0);

        for (const InputRect& r : valid)
        {
            const LogicalRect l = toLogical(r);
            wl_region_add(m_inputRegion, l.x0 - bounds.x0, l.y0 - bounds.y0, l.x1 - l.x0, l.y1 - l.y0);
        }
        wl_subsurface_set_position(m_inputSubsurface, bounds.x0, bounds.y0);
        if (m_inputViewport)
            wp_viewport_set_destination(m_inputViewport, w, h);
        wl_surface_set_input_region(m_inputSurface, m_inputRegion);
        wl_surface_attach(m_inputSurface, m_inputBuffer, 0, 0);
        wl_surface_commit(m_inputSurface);
        flush();
    }

    void WaylandInput::close()
    {
        std::lock_guard<std::mutex> lock(m_lifecycle);
        m_closed = true;
        shutdownLocked();
    }

    // Creates the private queue, the registry and the globals, in that order.
    // Every created object is pushed on m_releases, so shutdown destroys them in
    // reverse: devices and globals first, then the registry, then the display
    // wrapper, and the event queue last, when no proxy is attached to it any more.
    bool WaylandInput::ensureCommon(bool wantKeyboard, bool wantPointer)
    {
        m_wantKeyboard = m_wantKeyboard || wantKeyboard;
        m_wantPointer = m_wantPointer || wantPointer;

        if (m_commonReady)
        {
            bindDevices();
            return m_seat != nullptr;
        }

        if (m_closed || !m_display)
            return false;

        m_queue = wl_display_create_queue(m_display);
        if (!m_queue)
            return false;
        m_releases.push([this] {
            wl_event_queue_destroy(m_queue);
            m_queue = nullptr;
        });

        m_wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(m_display));
        if (!m_wrapper)
        {
            m_releases.releaseAll();
            return false;
        }
        m_releases.push([this] {
            wl_proxy_wrapper_destroy(m_wrapper);
            m_wrapper = nullptr;
        });
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m_wrapper), m_queue);

        m_registry = wl_display_get_registry(m_wrapper);
        m_releases.push([this] {
            wl_registry_destroy(m_registry);
            m_registry = nullptr;
        });
        wl_registry_add_listener(m_registry, &WaylandSeatListeners::registry, this);

        // Roundtrip to discover globals (seat)
        wl_display_roundtrip_queue(m_display, m_queue);
        // Second roundtrip to get seat capabilities (keyboard + pointer)
        wl_display_roundtrip_queue(m_display, m_queue);

        m_releases.push([this] {
            destroyCapture();
            m_attachment.completed(CaptureAttachment::Step::Detach, true);
        });
        reconcileCapture();
        publishCaptureRects();

        if (m_seat)
        {
            m_commonReady = true;
            Logger::info("Wayland: shared input resources initialized");
            return true;
        }

        Logger::warn("Wayland: no seat found");
        shutdownLocked();
        return false;
    }

    void WaylandInput::dispatchPending()
    {
        // Skip if already dispatched this frame
        const uint64_t currentFrame = m_frameId.load(std::memory_order_acquire);
        if (m_lastDispatchedFrame == currentFrame)
            return;
        m_lastDispatchedFrame = currentFrame;

        if (!m_queue)
            return;

        // drain our queue only; the socket is read by the game's event loop,
        // and taking the display read lock from here spun its runloop into a
        // freeze during compositor resizes
        wl_display_dispatch_queue_pending(m_display, m_queue);
    }

    void WaylandInput::beginFrame()
    {
        m_frameId.fetch_add(1, std::memory_order_release);
    }

    void WaylandInput::shutdown()
    {
        std::lock_guard<std::mutex> lock(m_lifecycle);
        shutdownLocked();
    }

    void WaylandInput::shutdownLocked()
    {
        // Keyboard and pointer are on the stack above the seat they came from,
        // so they are released before it, and the queue goes last.
        const bool hadObjects = !m_releases.empty();
        m_releases.releaseAll();
        // send the destroy requests now rather than whenever the game next flushes
        if (hadObjects)
            wl_display_flush(m_display);
        releaseKeyboard();
        {
            std::lock_guard<std::mutex> lock(m_mouseMutex);
            m_pointerFocusSurface = nullptr;
            m_leftButton = m_rightButton = m_middleButton = false;
            m_scroll = m_continuousScroll = m_discreteScroll = m_highResolutionScroll = 0.0f;
            m_hasDiscreteScroll = m_hasHighResolutionScroll = false;
        }

        m_commonReady = false;
        m_wantKeyboard = false;
        m_wantPointer = false;
        m_seatCapabilities = 0;
        m_keyboardFocus.store(false, std::memory_order_release);
        m_pointerFocus.store(false, std::memory_order_release);
    }
} // namespace VkHook
