#pragma once
#include "effect.hh"
#include <memory>
#include <string>

namespace VKIntox
{
    struct LogicalDevice;
    class Config;
    class EffectRegistry;

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
