#include "layer/entry.hh"
#include "layer/registry.hh"
#include "layer/guard.hh"
#include "layer/device_plan.hh"
#include "persist.hh"

#include "input_session.hh"
#include "wayland_interpose_patch.hh"
#include "logger.hh"
#include "util.hh"

#include <algorithm>
#include <memory>

#define VK_USE_PLATFORM_WAYLAND_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XCB_KHR
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include <wayland-client.h>
#include "vulkan/vulkan_wayland.h"
#include "vulkan/vulkan_xlib.h"
#include "vulkan/vulkan_xcb.h"

namespace VkHook
{
namespace
{
    VkResult VKAPI_CALL VkHook_CreateInstance(const VkInstanceCreateInfo*  pCreateInfo,
                                                const VkAllocationCallbacks* pAllocator,
                                                VkInstance*                  pInstance)
    {
        VkLayerInstanceCreateInfo* layerCreateInfo = (VkLayerInstanceCreateInfo*) pCreateInfo->pNext;

        // step through the chain of pNext until we get to the link info
        while (layerCreateInfo
               && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO))
        {
            layerCreateInfo = (VkLayerInstanceCreateInfo*) layerCreateInfo->pNext;
        }

        Logger::trace("vkCreateInstance");

        // reach the app's wayland marshalling before any window commit can race
        guardLayerWork(nullptr, "vkCreateInstance", [] { installWaylandInterposeHooks(); });

        if (layerCreateInfo == nullptr)
        {
            // No loader instance create info
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        PFN_vkGetInstanceProcAddr gpa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
        // move chain on for next layer
        layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

        PFN_vkCreateInstance createFunc = (PFN_vkCreateInstance) gpa(VK_NULL_HANDLE, "vkCreateInstance");

        InstanceCaps caps;
#ifdef VKHOOK_DLSS5
        auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gpa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        if (!enumerateVersion || enumerateVersion(&caps.loaderApiVersion) != VK_SUCCESS)
            caps.loaderApiVersion = VK_API_VERSION_1_0;
        caps.wantApi13 = true;
#endif

        const std::unique_ptr<InstancePlan> plan = planInstanceCreate(caps, *pCreateInfo);
        const VkInstanceCreateInfo&         modifiedCreateInfo = plan->createInfo();
        VkResult ret                        = createFunc(&modifiedCreateInfo, pAllocator, pInstance);

        if (ret != VK_SUCCESS)
        {
            // *pInstance is only valid on VK_SUCCESS, and the maps key on the handle
            Logger::err("vkCreateInstance failed: " + std::to_string(ret));
            return ret;
        }

        // fetch our own dispatch table for the functions we need, into the next layer
        InstanceDispatch dispatchTable;
        fillDispatchTableInstance(*pInstance, gpa, &dispatchTable);

        // store the table by key
        {
            auto state        = std::make_shared<InstanceState>();
            state->dispatch   = dispatchTable;
            state->instance   = *pInstance;
            state->apiVersion = modifiedCreateInfo.pApplicationInfo->apiVersion;
            registerInstance(GetKey(*pInstance), std::move(state));
        }

        return ret;
    }

    void VKAPI_CALL VkHook_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
    {
        if (!instance)
            return;

        Logger::trace("vkDestroyInstance");

        const InstanceSnapshot snapshot = snapshotInstance(GetKey(instance));
        if (!snapshot.found)
        {
            // Not created through this layer; nothing we can safely do
            Logger::trace("vkDestroyInstance: instance not tracked by this layer, skipping");
            return;
        }

        // forward first, then forget the instance
        snapshot.dispatch.DestroyInstance(instance, pAllocator);

        unregisterInstance(GetKey(instance));

        // final teardown: no instance left, so no device can queue a write again
        if (noInstancesRegistered())
            PersistenceQueue::instance().shutdown();
    }

    // Looks the instance up under the lock and resolves the next layer's
    // implementation of `name`; the lock is released before the caller uses it.
    PFN_vkVoidFunction resolveNextInstanceProc(VkInstance instance, const char* name)
    {
        const InstanceSnapshot snapshot = snapshotInstance(GetKey(instance));
        if (!snapshot.found)
        {
            failUnknownObject(name, VK_ERROR_INITIALIZATION_FAILED);
            return nullptr;
        }
        return snapshot.dispatch.GetInstanceProcAddr(snapshot.instance, name);
    }

    ///////////////////////////////////////////////////////////////////////////////////////////
    // Wayland / X11 surface interception: capture the window for input

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_CreateWaylandSurfaceKHR(
        VkInstance                              instance,
        const VkWaylandSurfaceCreateInfoKHR*    pCreateInfo,
        const VkAllocationCallbacks*            pAllocator,
        VkSurfaceKHR*                           pSurface)
    {
        Logger::trace("vkCreateWaylandSurfaceKHR");

        // Forward to the real implementation via the next layer
        auto nextFunc = (PFN_vkCreateWaylandSurfaceKHR) resolveNextInstanceProc(instance, "vkCreateWaylandSurfaceKHR");
        if (!nextFunc)
            return VK_ERROR_EXTENSION_NOT_PRESENT;

        const VkResult result = nextFunc(instance, pCreateInfo, pAllocator, pSurface);
        if (result != VK_SUCCESS || !pCreateInfo)
            return result;

        // The surface's input session: the game's wl_display and wl_surface, captured
        // only for a surface that was actually created
        guardLayerWork(nullptr, "vkCreateWaylandSurfaceKHR", [&] {
            registerSurface(*pSurface, InputSession::create(SurfaceKind::Wayland, pCreateInfo->display,
                                                            reinterpret_cast<uintptr_t>(pCreateInfo->surface)));
        });
        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_CreateXlibSurfaceKHR(
        VkInstance                           instance,
        const VkXlibSurfaceCreateInfoKHR*    pCreateInfo,
        const VkAllocationCallbacks*         pAllocator,
        VkSurfaceKHR*                        pSurface)
    {
        Logger::trace("vkCreateXlibSurfaceKHR");

        auto nextFunc = (PFN_vkCreateXlibSurfaceKHR) resolveNextInstanceProc(instance, "vkCreateXlibSurfaceKHR");
        if (!nextFunc)
            return VK_ERROR_EXTENSION_NOT_PRESENT;

        const VkResult result = nextFunc(instance, pCreateInfo, pAllocator, pSurface);
        if (result == VK_SUCCESS && pCreateInfo)
            guardLayerWork(nullptr, "vkCreateXlibSurfaceKHR", [&] {
                registerSurface(*pSurface, InputSession::create(SurfaceKind::X11, pCreateInfo->dpy,
                                                                static_cast<uintptr_t>(pCreateInfo->window)));
            });
        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_CreateXcbSurfaceKHR(
        VkInstance                          instance,
        const VkXcbSurfaceCreateInfoKHR*    pCreateInfo,
        const VkAllocationCallbacks*        pAllocator,
        VkSurfaceKHR*                       pSurface)
    {
        Logger::trace("vkCreateXcbSurfaceKHR");

        auto nextFunc = (PFN_vkCreateXcbSurfaceKHR) resolveNextInstanceProc(instance, "vkCreateXcbSurfaceKHR");
        if (!nextFunc)
            return VK_ERROR_EXTENSION_NOT_PRESENT;

        const VkResult result = nextFunc(instance, pCreateInfo, pAllocator, pSurface);
        if (result == VK_SUCCESS && pCreateInfo)
            guardLayerWork(nullptr, "vkCreateXcbSurfaceKHR", [&] {
                registerSurface(*pSurface, InputSession::create(SurfaceKind::X11, pCreateInfo->connection,
                                                                static_cast<uintptr_t>(pCreateInfo->window)));
            });
        return result;
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_DestroySurfaceKHR(VkInstance                   instance,
                                                          VkSurfaceKHR                 surface,
                                                          const VkAllocationCallbacks* pAllocator)
    {
        Logger::trace("vkDestroySurfaceKHR");

        // the session goes first: its grabs and Wayland objects belong to the window
        // the application is about to destroy
        guardLayerWork(nullptr, "vkDestroySurfaceKHR", [&] {
            if (const std::shared_ptr<InputSession> session = unregisterSurface(surface))
                session->destroy();
        });

        auto nextFunc = (PFN_vkDestroySurfaceKHR) resolveNextInstanceProc(instance, "vkDestroySurfaceKHR");
        if (nextFunc)
            nextFunc(instance, surface, pAllocator);
    }

    const InterceptEntry kInstanceIntercepts[] = {
        VKHOOK_INTERCEPT(Unconditional, CreateInstance),
        VKHOOK_INTERCEPT(Unconditional, DestroyInstance),
        VKHOOK_INTERCEPT(Unconditional, CreateWaylandSurfaceKHR),
        VKHOOK_INTERCEPT(Unconditional, CreateXlibSurfaceKHR),
        VKHOOK_INTERCEPT(Unconditional, CreateXcbSurfaceKHR),
        VKHOOK_INTERCEPT(Unconditional, DestroySurfaceKHR),
    };
} // namespace

    InterceptTable instanceInterceptTable()
    {
        return makeInterceptTable(kInstanceIntercepts);
    }
} // namespace VkHook
