#include "layer/registry.hh"

#include "logger.hh"

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace VkHook
{
namespace
{
    template <typename Value>
    class KeyedRegistry
    {
    public:
        using Map = std::unordered_map<void*, Value>;

        Value find(void* key) const
        {
            auto snapshot = slot.load();
            if (!snapshot)
                return nullptr;
            auto it = snapshot->find(key);
            return it == snapshot->end() ? nullptr : it->second;
        }

        void insert(void* key, Value value)
        {
            std::lock_guard<std::mutex> l(writeLock);
            auto next = std::make_shared<Map>(copy());
            (*next)[key] = std::move(value);
            slot.store(std::move(next));
        }

        bool empty() const
        {
            auto snapshot = slot.load();
            return !snapshot || snapshot->empty();
        }

        void erase(void* key)
        {
            std::lock_guard<std::mutex> l(writeLock);
            auto next = std::make_shared<Map>(copy());
            next->erase(key);
            slot.store(std::move(next));
        }

    private:
        Map copy() const
        {
            auto snapshot = slot.load();
            return snapshot ? *snapshot : Map{};
        }

        CowSlot<Map> slot;
        std::mutex   writeLock;  // create/destroy only; lookups never take it
    };

    KeyedRegistry<std::shared_ptr<const InstanceState>>& instances()
    {
        static KeyedRegistry<std::shared_ptr<const InstanceState>> registry;
        return registry;
    }

    KeyedRegistry<std::shared_ptr<LogicalDevice>>& devices()
    {
        static KeyedRegistry<std::shared_ptr<LogicalDevice>> registry;
        return registry;
    }
} // namespace

    bool noInstancesRegistered() { return instances().empty(); }

    void registerInstance(void* key, std::shared_ptr<const InstanceState> state) { instances().insert(key, std::move(state)); }
    void unregisterInstance(void* key) { instances().erase(key); }
    void registerDevice(void* key, std::shared_ptr<LogicalDevice> device) { devices().insert(key, std::move(device)); }
    void unregisterDevice(void* key) { devices().erase(key); }

    std::shared_ptr<LogicalDevice> findDevice(void* key) { return devices().find(key); }
    std::shared_ptr<const InstanceState> findInstance(void* key) { return instances().find(key); }

    InstanceSnapshot snapshotInstance(void* key)
    {
        InstanceSnapshot snap;
        auto state = findInstance(key);
        if (!state)
            return snap;
        snap.found      = true;
        snap.dispatch   = state->dispatch;
        snap.instance   = state->instance;
        snap.apiVersion = state->apiVersion;
        return snap;
    }

    std::shared_ptr<LogicalSwapchain> findSwapchain(LogicalDevice& device, VkSwapchainKHR swapchain)
    {
        std::lock_guard<std::mutex> l(device.stateLock);
        auto it = device.swapchains.find(swapchain);
        return it == device.swapchains.end() ? nullptr : it->second;
    }

    std::vector<std::shared_ptr<LogicalSwapchain>> deviceSwapchains(LogicalDevice& device)
    {
        std::vector<std::shared_ptr<LogicalSwapchain>> out;
        std::lock_guard<std::mutex> l(device.stateLock);
        out.reserve(device.swapchains.size());
        for (const auto& [_, swapchain] : device.swapchains)
            if (swapchain)
                out.push_back(swapchain);
        return out;
    }

    VkResult failUnknownObject(const char* entryPoint, VkResult result)
    {
        static std::mutex                      reportedLock;
        static std::unordered_set<std::string> reported;
        {
            std::lock_guard<std::mutex> l(reportedLock);
            if (!reported.insert(entryPoint).second)
                return result;
        }
        Logger::err(std::string(entryPoint) + ": called with an object the layer does not track "
                    "(device already destroyed?); returning VkResult " + std::to_string(result));
        return result;
    }

namespace
{
    VKAPI_ATTR VkResult VKAPI_CALL internalQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence)
    {
        auto device = findDevice(GetKey(queue));
        if (!device)
            return failUnknownObject("vkQueueSubmit", VK_ERROR_DEVICE_LOST);
        return device->queues.submit(queue, count, submits, fence);
    }

    VKAPI_ATTR VkResult VKAPI_CALL internalQueueWaitIdle(VkQueue queue)
    {
        auto device = findDevice(GetKey(queue));
        if (!device)
            return failUnknownObject("vkQueueWaitIdle", VK_ERROR_DEVICE_LOST);
        return device->queues.waitIdle(queue);
    }

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL internalDeviceProcAddr(VkDevice device, const char* name)
    {
        if (std::string_view(name) == "vkQueueSubmit")
            return reinterpret_cast<PFN_vkVoidFunction>(&internalQueueSubmit);
        if (std::string_view(name) == "vkQueueWaitIdle")
            return reinterpret_cast<PFN_vkVoidFunction>(&internalQueueWaitIdle);
        auto state = findDevice(GetKey(device));
        if (!state || !state->downstreamGDPA)
            return nullptr;
        return state->downstreamGDPA(device, name);
    }
} // namespace

    PFN_vkGetDeviceProcAddr layerDeviceProcAddr()
    {
        return &internalDeviceProcAddr;
    }
} // namespace VkHook
