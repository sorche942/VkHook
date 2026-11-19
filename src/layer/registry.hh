#pragma once

// Layer registry: InstanceState / LogicalDevice lookup by dispatch key.
//
// Lookups are lock-free: the maps are copy-on-write snapshots published through
// an atomic pointer, so every hook (vkCmd*, submit, begin) resolves its device
// without a mutex. Registration happens only in vkCreate*/vkDestroy* and takes
// a writer mutex that no lookup ever touches.
//
// There is no process-wide lock and no process-wide swapchain map. Mutable
// per-device state sits behind LogicalDevice::stateLock (short, never held across
// a GPU wait, file I/O, compile, or a call into the next layer), and a device's
// swapchains hang off the device.

#include "vulkan_include.hh"
#include "vkdispatch.hh"
#include "logical_device.hh"
#include "logical_swapchain.hh"

#include <memory>

namespace VkHook
{
    // The loader stores the dispatch table pointer in the first word of every
    // dispatchable handle, and every object of one device shares it.
    template<typename DispatchableType>
    void* GetKey(DispatchableType inst)
    {
        return *(void**) inst;
    }

    class InputSession;

    struct InstanceState
    {
        InstanceDispatch dispatch;
        VkInstance       instance   = VK_NULL_HANDLE;
        uint32_t         apiVersion = 0;
    };

    void registerInstance(void* key, std::shared_ptr<const InstanceState> state);
    void unregisterInstance(void* key);
    void registerDevice(void* key, std::shared_ptr<LogicalDevice> device);
    void unregisterDevice(void* key);
    // True once every instance the layer saw was destroyed (final teardown).
    bool noInstancesRegistered();

    // Null if the key is not an object this layer created. The returned
    // reference keeps the state alive while the caller forwards the call.
    std::shared_ptr<LogicalDevice>       findDevice(void* key);
    std::shared_ptr<const InstanceState> findInstance(void* key);

    // A copy of everything the layer stores for an instance.
    struct InstanceSnapshot
    {
        InstanceDispatch dispatch;
        VkInstance       instance   = VK_NULL_HANDLE;
        uint32_t         apiVersion = 0;
        bool             found      = false;
    };
    InstanceSnapshot snapshotInstance(void* key);

    // The input session of a surface: created by vkCreate{Wayland,Xlib,Xcb}SurfaceKHR,
    // destroyed by vkDestroySurfaceKHR. A swapchain keeps the session of the
    // surface it was created on, so it outlives nothing it depends on.
    void registerSurface(VkSurfaceKHR surface, std::shared_ptr<InputSession> session);
    // Forgets the surface and returns its session (null when none was registered).
    std::shared_ptr<InputSession> unregisterSurface(VkSurfaceKHR surface);
    std::shared_ptr<InputSession> findSurfaceSession(VkSurfaceKHR surface);

    // Takes device.stateLock for the lookup only.
    std::shared_ptr<LogicalSwapchain> findSwapchain(LogicalDevice& device, VkSwapchainKHR swapchain);

    // Every swapchain of the device, copied under stateLock.
    std::vector<std::shared_ptr<LogicalSwapchain>> deviceSwapchains(LogicalDevice& device);

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

    // vkGetDeviceProcAddr for the layer's own helper libraries (the DLSS5
    // context). Queue entry points resolve to the layer's locked, fence-wait
    // implementations so nothing outside gpu/queue touches a queue unlocked;
    // everything else resolves through the next layer.
    PFN_vkGetDeviceProcAddr layerDeviceProcAddr();
} // namespace VkHook
