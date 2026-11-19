#pragma once

// Process-wide layer book-keeping: the dispatch-key -> object maps and the one
// lock that guards them and the objects they own. Stage 1 keeps a single global
// lock; the registry that replaces it (AGENTS.md §3, layer/registry) will keep
// these lookups behind the same helpers.
//
// Rule for every hook: look up under the lock, copy what you need, release the
// lock, then call down the chain. Never forward a call to the next layer while
// holding globalLock.

#include "vulkan_include.hh"
#include "vkdispatch.hh"
#include "logical_device.hh"
#include "logical_swapchain.hh"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace VkHook
{
    extern std::mutex globalLock;
    using scoped_lock = std::lock_guard<std::mutex>;

    // layer book-keeping, keyed by the loader dispatch key of the object
    extern std::unordered_map<void*, InstanceDispatch>                           instanceDispatchMap;
    extern std::unordered_map<void*, VkInstance>                                 instanceMap;
    extern std::unordered_map<void*, uint32_t>                                   instanceVersionMap;
    extern std::unordered_map<void*, std::shared_ptr<LogicalDevice>>             deviceMap;
    extern std::unordered_map<VkSwapchainKHR, std::shared_ptr<LogicalSwapchain>> swapchainMap;

    // The loader stores the dispatch table pointer in the first word of every
    // dispatchable handle, and every object of one device shares it.
    template<typename DispatchableType>
    void* GetKey(DispatchableType inst)
    {
        return *(void**) inst;
    }

    // Caller holds globalLock. Null if the key is not a device this layer created.
    inline std::shared_ptr<LogicalDevice> findDeviceLocked(void* key)
    {
        auto it = deviceMap.find(key);
        return it == deviceMap.end() ? nullptr : it->second;
    }

    // Takes globalLock briefly. The returned reference keeps the device alive
    // while the caller forwards the call with the lock released.
    inline std::shared_ptr<LogicalDevice> findDevice(void* key)
    {
        scoped_lock l(globalLock);
        return findDeviceLocked(key);
    }

    inline std::shared_ptr<LogicalSwapchain> findSwapchainLocked(VkSwapchainKHR swapchain)
    {
        auto it = swapchainMap.find(swapchain);
        return it == swapchainMap.end() ? nullptr : it->second;
    }

    inline std::shared_ptr<LogicalSwapchain> findSwapchain(VkSwapchainKHR swapchain)
    {
        scoped_lock l(globalLock);
        return findSwapchainLocked(swapchain);
    }

    // A copy of everything the layer stores for an instance, taken under the lock
    // so the caller can use it after releasing it.
    struct InstanceSnapshot
    {
        InstanceDispatch dispatch;
        VkInstance       instance   = VK_NULL_HANDLE;
        uint32_t         apiVersion = 0;
        bool             found      = false;
    };
    InstanceSnapshot snapshotInstance(void* key);

    // A hook was handed a dispatchable object the layer has no state for.
    //
    // The loader only reaches a hook through the pointer GetDeviceProcAddr /
    // GetInstanceProcAddr returned, and those return hooks only after
    // CreateDevice / CreateInstance registered the object. Every dispatchable
    // child of a device carries the device's dispatch key, so the lookup can fail
    // only if the application uses an object after destroying its device (or
    // races vkDestroyDevice), which is undefined behaviour on its side. There is
    // no dispatch table left to call down through: the loader trampoline for the
    // same entry point would route straight back into this hook. So the call
    // fails loudly instead of pretending to succeed. The error is logged once per
    // entry point; the caller returns the result.
    VkResult failUnknownObject(const char* entryPoint, VkResult result);
} // namespace VkHook
