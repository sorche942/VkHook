#pragma once
#include "effect.hh"
#include <memory>
#include <string>

namespace VkHook
{
    struct LogicalDevice;
    class Config;
    class EffectRegistry;

    // Releases the loaded DLSS5 network and weights cached for `device`. The
    // cache holds GPU buffers created on the device, so this must run while the
    // device is still alive and idle: at vkDestroyDevice, after vkDeviceWaitIdle
    // and before the real destroy. Effects built from the cache must already be
    // gone. Safe to call for a device that never used DLSS5.
    void clearDlss5ModelCache(VkDevice device);

    class Dlss5Effect final : public Effect
    {
    public:
        Dlss5Effect(LogicalDevice* device, VkFormat format, VkColorSpaceKHR colorSpace,
                    VkExtent2D extent, const std::vector<VkImage>& input,
                    const std::vector<VkImage>& output, const std::vector<VkImage>& motionSources,
                    EffectRegistry* registry,
                    Config* config, const std::string& name);
        ~Dlss5Effect() override;
        void applyEffect(uint32_t imageIndex, VkCommandBuffer commandBuffer) override;
        bool updateFrame(uint32_t imageIndex) override;
        VkResult prepareSubmission(uint32_t imageIndex, uint64_t frameSerial,
                                   VkSubmitInfo& submission) noexcept override;
    private:
        struct State;
        std::unique_ptr<State> state;
    };
}
