#pragma once

#include <cstdint>

namespace VkHook
{
    // Which game surface the Wayland capture subsurface is bound to, as a pure
    // state machine with no libwayland in it. A wl_subsurface is bound to its
    // parent for life, so when the game recreates its window (a new wl_surface
    // on the same display) the old capture is orphaned and must be replaced by
    // one bound to the new parent. The owner reads next(), performs that step
    // against the compositor, and reports it with completed().
    class CaptureAttachment
    {
    public:
        enum class Step
        {
            None,      // the live capture already matches the parent
            Attach,    // no capture exists: create one on the parent
            Detach,    // the parent is gone: destroy the capture
            Reattach,  // the parent changed: destroy the capture, create one on the new parent
        };

        // The surface the game presents to now; 0 when there is none.
        void setParent(uintptr_t parent)
        {
            m_parent = parent;
        }

        uintptr_t parent() const
        {
            return m_parent;
        }

        // The parent the live capture is bound to; 0 when there is no capture.
        uintptr_t bound() const
        {
            return m_bound;
        }

        Step next() const
        {
            if (m_bound == m_parent)
                return Step::None;
            if (m_parent == 0)
                return Step::Detach;
            return m_bound == 0 ? Step::Attach : Step::Reattach;
        }

        // Records a step the owner carried out. A failed Attach or Reattach
        // leaves no capture (Reattach destroys the old one first), so next()
        // asks for Attach again.
        void completed(Step step, bool ok)
        {
            switch (step)
            {
                case Step::None:
                    return;
                case Step::Detach:
                    m_bound = 0;
                    return;
                case Step::Attach:
                case Step::Reattach:
                    m_bound = ok ? m_parent : 0;
                    m_republish = ok;
                    return;
            }
        }

        // The capture is the owner's fresh surface with an empty input region,
        // so the hitboxes last requested must be published again. Returns true
        // once per (re)attach.
        bool takeRepublish()
        {
            const bool pending = m_republish;
            m_republish = false;
            return pending;
        }

    private:
        uintptr_t m_parent = 0;
        uintptr_t m_bound = 0;
        bool m_republish = false;
    };
} // namespace VkHook
