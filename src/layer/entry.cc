#include "layer/entry.hh"
#include "layer/state.hh"

#include "control/frontend.hh"
#include "logger.hh"

#include <cstring>
#include <dlfcn.h>

#define VKHOOK_LAYER_NAME "VK_LAYER_VKHOOK_post_processing"

#if defined(__GNUC__) && __GNUC__ >= 4
#define VK_SHADE_EXPORT __attribute__((visibility("default")))
#else
#error "Unsupported platform!"
#endif

namespace VkHook
{
namespace
{
    ///////////////////////////////////////////////////////////////////////////////////////////
    // Enumeration functions

    VkResult VKAPI_CALL VkHook_EnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties)
    {
        if (pPropertyCount)
            *pPropertyCount = 1;

        if (pProperties)
        {
            std::strcpy(pProperties->layerName, VKHOOK_LAYER_NAME);
            std::strcpy(pProperties->description, "a post processing layer");
            pProperties->implementationVersion = 1;
            pProperties->specVersion           = VK_MAKE_VERSION(1, 2, 0);
        }

        return VK_SUCCESS;
    }

    VkResult VKAPI_CALL VkHook_EnumerateDeviceLayerProperties(VkPhysicalDevice   /* physicalDevice */,
                                                                uint32_t*          pPropertyCount,
                                                                VkLayerProperties* pProperties)
    {
        return VkHook_EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
    }

    VkResult VKAPI_CALL VkHook_EnumerateInstanceExtensionProperties(const char*            pLayerName,
                                                                      uint32_t*              pPropertyCount,
                                                                      VkExtensionProperties* /* pProperties */)
    {
        if (pLayerName == NULL || std::strcmp(pLayerName, VKHOOK_LAYER_NAME))
        {
            return VK_ERROR_LAYER_NOT_PRESENT;
        }

        // don't expose any extensions
        if (pPropertyCount)
        {
            *pPropertyCount = 0;
        }
        return VK_SUCCESS;
    }

    VkResult VKAPI_CALL VkHook_EnumerateDeviceExtensionProperties(VkPhysicalDevice       physicalDevice,
                                                                    const char*            pLayerName,
                                                                    uint32_t*              pPropertyCount,
                                                                    VkExtensionProperties* pProperties)
    {
        // pass through any queries that aren't to us
        if (pLayerName == NULL || std::strcmp(pLayerName, VKHOOK_LAYER_NAME))
        {
            if (physicalDevice == VK_NULL_HANDLE)
            {
                return VK_SUCCESS;
            }

            // forward with the lock released
            const InstanceSnapshot snapshot = snapshotInstance(GetKey(physicalDevice));
            if (!snapshot.found || !snapshot.dispatch.EnumerateDeviceExtensionProperties)
                return VK_ERROR_INITIALIZATION_FAILED;
            return snapshot.dispatch.EnumerateDeviceExtensionProperties(
                physicalDevice, pLayerName, pPropertyCount, pProperties);
        }

        // don't expose any extensions
        if (pPropertyCount)
        {
            *pPropertyCount = 0;
        }
        return VK_SUCCESS;
    }

    const InterceptEntry kEnumerateIntercepts[] = {
        VKHOOK_INTERCEPT(Unconditional, EnumerateInstanceLayerProperties),
        VKHOOK_INTERCEPT(Unconditional, EnumerateInstanceExtensionProperties),
        VKHOOK_INTERCEPT(Unconditional, EnumerateDeviceLayerProperties),
        VKHOOK_INTERCEPT(Unconditional, EnumerateDeviceExtensionProperties),
    };

    PFN_vkGetInstanceProcAddr getNextInstanceProcAddr()
    {
        return reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(RTLD_NEXT, "vkGetInstanceProcAddr"));
    }

    PFN_vkGetDeviceProcAddr getNextDeviceProcAddr()
    {
        return reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(RTLD_NEXT, "vkGetDeviceProcAddr"));
    }

    // Looks `name` up in one module table. Returns true when the name is ours
    // (`fn` is null for a Downstream entry the next layer does not provide) and
    // false when the table has no such name or the entry is gated off.
    template <typename DownstreamResolvable>
    bool lookup(const InterceptTable& table, const char* name, bool depthCapture,
                DownstreamResolvable&& downstreamHas, PFN_vkVoidFunction& fn)
    {
        for (size_t i = 0; i < table.count; ++i)
        {
            const InterceptEntry& entry = table.entries[i];
            if (std::strcmp(name, entry.name) != 0)
                continue;
            switch (entry.gate)
            {
            case InterceptGate::Unconditional:
                fn = entry.fn;
                break;
            case InterceptGate::DepthCapture:
                // gated off: not intercepted, the next layer's function is returned
                if (!depthCapture)
                    return false;
                fn = entry.fn;
                break;
            case InterceptGate::Downstream:
                fn = downstreamHas() ? entry.fn : nullptr;
                break;
            }
            return true;
        }
        return false;
    }

    // Every module table, in lookup order. The instance and device chains share
    // one table: some games resolve device entry points through the instance
    // loader and the other way round.
    template <typename DownstreamResolvable>
    bool interceptLookup(const char* name, DownstreamResolvable&& downstreamHas, PFN_vkVoidFunction& fn)
    {
        const bool depthCapture = depthCaptureActive();
        const InterceptTable tables[] = {
            makeInterceptTable(kEnumerateIntercepts),
            instanceInterceptTable(),
            deviceInterceptTable(),
            swapchainInterceptTable(),
            presentInterceptTable(),
            submitInterceptTable(),
            depthInterceptTable(),
        };
        for (const InterceptTable& table : tables)
            if (lookup(table, name, depthCapture, downstreamHas, fn))
                return true;
        return false;
    }
} // namespace
} // namespace VkHook

extern "C"
{ // these are the entry points for the layer, so they need to be c-linkeable

    VK_SHADE_EXPORT PFN_vkVoidFunction VKAPI_CALL VkHook_GetDeviceProcAddr(VkDevice device, const char* pName);
    VK_SHADE_EXPORT PFN_vkVoidFunction VKAPI_CALL VkHook_GetInstanceProcAddr(VkInstance instance, const char* pName);

    VK_SHADE_EXPORT PFN_vkVoidFunction VKAPI_CALL VkHook_GetDeviceProcAddr(VkDevice device, const char* pName)
    {
        VkHook::initConfigs();

        // vkGetDeviceProcAddr needs to behave like vkGetInstanceProcAddr thanks to some games
        if (!std::strcmp(pName, "vkGetInstanceProcAddr"))
            return reinterpret_cast<PFN_vkVoidFunction>(&VkHook_GetInstanceProcAddr);
        if (!std::strcmp(pName, "vkGetDeviceProcAddr"))
            return reinterpret_cast<PFN_vkVoidFunction>(&VkHook_GetDeviceProcAddr);

        // The downstream resolver is copied out under the lock and called after
        // it is released.
        PFN_vkGetDeviceProcAddr downstream = nullptr;
        bool                    known      = false;
        if (device != VK_NULL_HANDLE)
        {
            auto dev = VkHook::findDevice(VkHook::GetKey(device));
            if (dev && dev->vkd.GetDeviceProcAddr)
            {
                known      = true;
                downstream = dev->vkd.GetDeviceProcAddr;
            }
        }

        PFN_vkVoidFunction fn = nullptr;
        // Preserve null for unsupported/disabled extension entry points.
        auto downstreamHas = [&] { return known && downstream(device, pName) != nullptr; };
        if (VkHook::interceptLookup(pName, downstreamHas, fn))
            return fn;

        if (known)
            return downstream(device, pName);

        PFN_vkGetDeviceProcAddr next = VkHook::getNextDeviceProcAddr();
        return next ? next(device, pName) : nullptr;
    }

    VK_SHADE_EXPORT PFN_vkVoidFunction VKAPI_CALL VkHook_GetInstanceProcAddr(VkInstance instance, const char* pName)
    {
        VkHook::initConfigs();

        if (!std::strcmp(pName, "vkGetInstanceProcAddr"))
            return reinterpret_cast<PFN_vkVoidFunction>(&VkHook_GetInstanceProcAddr);
        if (!std::strcmp(pName, "vkGetDeviceProcAddr"))
            return reinterpret_cast<PFN_vkVoidFunction>(&VkHook_GetDeviceProcAddr);

        PFN_vkGetInstanceProcAddr downstream = nullptr;
        if (instance != VK_NULL_HANDLE)
        {
            const VkHook::InstanceSnapshot snapshot = VkHook::snapshotInstance(VkHook::GetKey(instance));
            if (snapshot.found && snapshot.dispatch.GetInstanceProcAddr)
                downstream = snapshot.dispatch.GetInstanceProcAddr;
        }

        PFN_vkVoidFunction fn = nullptr;
        auto downstreamHas = [&] { return downstream != nullptr && downstream(instance, pName) != nullptr; };
        if (VkHook::interceptLookup(pName, downstreamHas, fn))
            return fn;

        if (downstream)
            return downstream(instance, pName);

        PFN_vkGetInstanceProcAddr next = VkHook::getNextInstanceProcAddr();
        return next ? next(instance, pName) : nullptr;
    }

} // extern "C"
