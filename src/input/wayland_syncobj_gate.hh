#pragma once

#include <cstdint>
#include <memory>

namespace VkHook
{
    // Keeps a foreign window commit out of an in-flight present. Mesa's WSI
    // sets linux-drm-syncobj points inside its present call, and a commit from
    // another thread landing between a point and the attach is a protocol
    // error hyprland answers by killing the connection. Mesa does not route
    // through any marshalling we can see, so the presenter's side is bracketed
    // around the driver present call instead.
    //
    // Per surface there is one shared_timed_mutex. The presenter holds it
    // exclusively from beginPresent to endPresent; every other thread holds it
    // shared across the actual commit send (CommitScope). There is no
    // check-then-act: a commit either holds the lock while it is sent, in which
    // case beginPresent waits for it, or it waits behind the open present.
    // Both waits are bounded by a budget so a wedged thread cannot park the
    // other forever; when a budget runs out the lock is skipped and warned.
    namespace WaylandSyncobjGate
    {
        // surface id of the captured wayland window, 0 when not on wayland
        uint32_t waylandSurfaceId();

        // bracket around the driver's present call
        void beginPresent(uint32_t surfaceId);
        void endPresent(uint32_t surfaceId);

        // true while any present bracket is open (diagnostics)
        bool anyPresentOpen();

        // Held for the duration of one wl_surface.commit send. The presenter's
        // own commits (same thread, inside its bracket) pass straight through.
        class CommitScope
        {
        public:
            explicit CommitScope(uint32_t surfaceId);
            ~CommitScope();
            CommitScope(const CommitScope&) = delete;
            CommitScope& operator=(const CommitScope&) = delete;

        public:
            struct Surface;  // per-surface lock state; opaque outside the gate

        private:
            std::shared_ptr<Surface> m_surface;
            bool m_held = false;
        };

        // diagnostics: commits that were actually made to wait
        uint64_t heldCount();

        // test hooks
        void setWaitBudgetForTests(uint64_t microseconds);
        void resetForTests();
    }
} // namespace VkHook
