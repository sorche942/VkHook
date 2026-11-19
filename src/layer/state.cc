#include "layer/state.hh"

#include "logger.hh"

#include <string>
#include <unordered_set>

namespace VkHook
{
    std::mutex globalLock;

    std::unordered_map<void*, InstanceDispatch>                           instanceDispatchMap;
    std::unordered_map<void*, VkInstance>                                 instanceMap;
    std::unordered_map<void*, uint32_t>                                   instanceVersionMap;
    std::unordered_map<void*, std::shared_ptr<LogicalDevice>>             deviceMap;
    std::unordered_map<VkSwapchainKHR, std::shared_ptr<LogicalSwapchain>> swapchainMap;

    InstanceSnapshot snapshotInstance(void* key)
    {
        InstanceSnapshot snap;
        scoped_lock      l(globalLock);
        auto             it = instanceDispatchMap.find(key);
        if (it == instanceDispatchMap.end())
            return snap;
        snap.found      = true;
        snap.dispatch   = it->second;
        auto instIt     = instanceMap.find(key);
        if (instIt != instanceMap.end())
            snap.instance = instIt->second;
        auto verIt = instanceVersionMap.find(key);
        if (verIt != instanceVersionMap.end())
            snap.apiVersion = verIt->second;
        return snap;
    }

    VkResult failUnknownObject(const char* entryPoint, VkResult result)
    {
        static std::mutex                   reportedLock;
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
} // namespace VkHook
