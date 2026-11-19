#pragma once

namespace VkHook
{
    // Same teardown as cleanupWaylandKeyboard(): the keyboard and pointer share
    // one seat and queue, so they are released together. Idempotent.
    void cleanupWaylandMouse();
} // namespace VkHook
