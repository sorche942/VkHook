#include "vkdispatch.hh"

#include "vkfuncs.hh"
#include "wayland_syncobj_gate.hh"
#include "wayland_interpose_patch.hh"

#include <atomic>

namespace
{
    std::atomic<PFN_vkQueuePresentKHR> realQueuePresent{nullptr};

    VKAPI_ATTR VkResult VKAPI_CALL gatedQueuePresent(VkQueue queue, const VkPresentInfoKHR* presentInfo)
    {
        VkHook::installWaylandInterposeHooks();

        const uint32_t surfaceId = VkHook::WaylandSyncobjGate::waylandSurfaceId();
        if (surfaceId)
            VkHook::WaylandSyncobjGate::beginPresent(surfaceId);

        const VkResult result = realQueuePresent.load(std::memory_order_acquire)(queue, presentInfo);

        if (surfaceId)
            VkHook::WaylandSyncobjGate::endPresent(surfaceId);
        return result;
    }
} // namespace

namespace VkHook
{

    void fillDispatchTableInstance(VkInstance instance, PFN_vkGetInstanceProcAddr gipa, InstanceDispatch* table)
    {
        table->GetInstanceProcAddr = gipa;
#define FORVKFUNC(func) \
    do \
    { \
        if (!table->func) \
            table->func = (PFN_vk##func) gipa(instance, "vk" #func); \
    } while (false);
        VK_INSTANCE_FUNCS
#undef FORVKFUNC
    }

    void fillDispatchTableDevice(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, DeviceDispatch* table)
    {
        table->GetDeviceProcAddr = gdpa;
#define FORVKFUNC(func) \
    do \
    { \
        if (!table->func) \
            table->func = (PFN_vk##func) gdpa(device, "vk" #func); \
    } while (false);
        VK_DEVICE_FUNCS
#undef FORVKFUNC

        // bracket the driver present: Mesa sets explicit-sync points inside it,
        // and window commits from other threads must not land in between
        PFN_vkQueuePresentKHR real = table->QueuePresentKHR;
        if (real)
        {
            PFN_vkQueuePresentKHR expected = nullptr;
            realQueuePresent.compare_exchange_strong(expected, real);
            table->QueuePresentKHR = &gatedQueuePresent;
        }
    }

} // namespace VkHook
