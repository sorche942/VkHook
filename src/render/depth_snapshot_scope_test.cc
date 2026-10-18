// Exercise the production snapshot recorder, replacing only Vulkan dispatch.
// In particular, a usable source must not allow a nested shader render pass.
#include "command_buffer.hh"
#include "logical_swapchain.hh"
#include "settings_manager.hh"

#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace VKIntox
{
    // Match the other headless LogicalDevice tests: these owners stay null.
    struct OverlayPersistentState {};
    class ImGuiOverlay {};

    // Use the real inline settings accessors without loading a user config.
    SettingsManager settingsManager{};
}

using namespace VKIntox;

namespace
{
    int checks = 0;
    int failures = 0;

    void expect(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::printf("FAILED: %s\n", message);
        }
    }

    // Non-dispatchable Vulkan handles are integers on i686 and pointers on x64.
    template<class T> T handle(uintptr_t value)
    {
        if constexpr (std::is_pointer_v<T>)
            return reinterpret_cast<T>(value);
        else
            return static_cast<T>(value);
    }

    struct Fixture
    {
        LogicalDevice device{};
        LogicalSwapchain swapchain{};
        DepthState depth{};
        VkCommandBuffer cb = handle<VkCommandBuffer>(1);
        std::string events;
        std::vector<VkImageMemoryBarrier> barriers;
        std::vector<VkPipelineStageFlags> sourceStages;
        std::vector<VkPipelineStageFlags> destinationStages;

        explicit Fixture(bool shader)
        {
            device.device = handle<VkDevice>(2);
            depth.image = handle<VkImage>(3);
            depth.imageView = handle<VkImageView>(4);
            depth.format = VK_FORMAT_D24_UNORM_S8_UINT;
            depth.extent = {3840, 2160, 1};
            depth.observedLayout = VK_IMAGE_LAYOUT_GENERAL;
            device.depthImages.push_back(depth.image);
            device.depthImageExtents.emplace(depth.image, depth.extent);
            device.depthImageMetadata.emplace(depth.image, DepthImageMetadata{
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL});
            device.depthViewStates.emplace(depth.imageView, depth);
            swapchain.logicalDevice = &device;
            swapchain.depthResolveUsesShader = shader;
            swapchain.depthResolveExtent = depth.extent;
            swapchain.depthResolveFormat = shader ? VK_FORMAT_R32_SFLOAT : depth.format;
            swapchain.depthResolvePerImage.resize(1);
            swapchain.depthResolvePerImage[0].image = handle<VkImage>(5);
            swapchain.depthResolveDescriptorSets.push_back(handle<VkDescriptorSet>(6));
            swapchain.depthResolveFramebuffers.push_back(handle<VkFramebuffer>(7));
            swapchain.depthResolveRenderPass = handle<VkRenderPass>(8);
            swapchain.depthResolvePipeline = handle<VkPipeline>(9);
            swapchain.depthResolvePipelineLayout = handle<VkPipelineLayout>(10);
            swapchain.depthResolveSampler = handle<VkSampler>(11);
        }

        void record()
        {
            recordDepthResolveSnapshot(&device, &swapchain, cb, 0, depth);
        }

        void resetTrace()
        {
            events.clear();
            barriers.clear();
            sourceStages.clear();
            destinationStages.clear();
        }
    };

    Fixture* current = nullptr;

    VKAPI_ATTR void VKAPI_CALL pipelineBarrier(VkCommandBuffer,
        VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkDependencyFlags,
        uint32_t, const VkMemoryBarrier*, uint32_t,
        const VkBufferMemoryBarrier*, uint32_t imageCount, const VkImageMemoryBarrier* images)
    {
        current->events += 'B';
        for (uint32_t i = 0; i < imageCount; ++i)
        {
            current->barriers.push_back(images[i]);
            current->sourceStages.push_back(sourceStage);
            current->destinationStages.push_back(destinationStage);
        }
    }

    VKAPI_ATTR void VKAPI_CALL updateDescriptorSets(VkDevice, uint32_t,
        const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*)
    {
        current->events += 'U';
    }

    VKAPI_ATTR void VKAPI_CALL beginRenderPass(VkCommandBuffer,
        const VkRenderPassBeginInfo*, VkSubpassContents)
    {
        current->events += 'R';
    }

    VKAPI_ATTR void VKAPI_CALL bindDescriptorSets(VkCommandBuffer,
        VkPipelineBindPoint, VkPipelineLayout, uint32_t,
        uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*)
    {
        current->events += 'S';
    }

    VKAPI_ATTR void VKAPI_CALL bindPipeline(VkCommandBuffer,
        VkPipelineBindPoint, VkPipeline)
    {
        current->events += 'P';
    }

    VKAPI_ATTR void VKAPI_CALL pushConstants(VkCommandBuffer, VkPipelineLayout,
        VkShaderStageFlags, uint32_t, uint32_t, const void*)
    {
        current->events += 'K';
    }

    VKAPI_ATTR void VKAPI_CALL draw(VkCommandBuffer, uint32_t,
        uint32_t, uint32_t, uint32_t)
    {
        current->events += 'D';
    }

    VKAPI_ATTR void VKAPI_CALL endRenderPass(VkCommandBuffer)
    {
        current->events += 'E';
    }

    VKAPI_ATTR void VKAPI_CALL copyImage(VkCommandBuffer, VkImage,
        VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy*)
    {
        current->events += 'C';
    }

    void installDispatch(Fixture& fixture)
    {
        current = &fixture;
        auto& dispatch = fixture.device.vkd;
        dispatch.CmdPipelineBarrier = pipelineBarrier;
        dispatch.UpdateDescriptorSets = updateDescriptorSets;
        dispatch.CmdBeginRenderPass = beginRenderPass;
        dispatch.CmdBindDescriptorSets = bindDescriptorSets;
        dispatch.CmdBindPipeline = bindPipeline;
        dispatch.CmdPushConstants = pushConstants;
        dispatch.CmdDraw = draw;
        dispatch.CmdEndRenderPass = endRenderPass;
        dispatch.CmdCopyImage = copyImage;
    }

    void expectSnapshot(const Fixture& fixture, bool initiallyInitialized)
    {
        const bool shader = fixture.swapchain.depthResolveUsesShader;
        if (shader)
            expect(fixture.events.find('R') < fixture.events.find('D')
                       && fixture.events.find('D') < fixture.events.find('E')
                       && fixture.events.find('E') != std::string::npos,
                   "legal shader snapshot draws inside its own render pass");
        else
            expect(fixture.events.find('C') != std::string::npos
                       && fixture.events.find('R') == std::string::npos,
                   "legal transfer snapshot copies depth without a render pass");
        expect(fixture.swapchain.depthResolvePerImage[0].initialized,
               "legal snapshot marks its destination initialized");
        expect(fixture.barriers.size() == (shader ? 2u : 3u),
               "GENERAL snapshot emits the expected source and destination barriers");
        if (fixture.barriers.size() < 2)
            return;
        const auto& source = fixture.barriers[0];
        expect(source.image == fixture.depth.image && source.oldLayout == VK_IMAGE_LAYOUT_GENERAL
                   && source.newLayout == VK_IMAGE_LAYOUT_GENERAL
                   && source.subresourceRange.aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
               "source barrier preserves GENERAL and covers depth plus stencil");
        const auto& destination = fixture.barriers[1];
        const auto readOnly = shader ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                    : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        expect(destination.image == fixture.swapchain.depthResolvePerImage[0].image
                   && destination.oldLayout == (initiallyInitialized ? readOnly : VK_IMAGE_LAYOUT_UNDEFINED)
                   && destination.newLayout == (shader ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                                       : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
               "destination transition honors its preexisting initialization state");
        if (!shader && fixture.barriers.size() == 3)
            expect(fixture.barriers[2].image == destination.image
                       && fixture.barriers[2].oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                       && fixture.barriers[2].newLayout == readOnly,
                   "copy transitions the destination back to sampleable depth");
    }

    void scopeBoundary(bool shader, bool initiallyInitialized, bool trackedDepthUsable)
    {
        Fixture fixture(shader);
        installDispatch(fixture);
        auto& scope = fixture.device.commandBufferDepthStates[fixture.cb];
        scope.inRenderScope = true;
        if (trackedDepthUsable)
            scope.depthState = fixture.depth;
        // No direct snapshot target, just as in the crashing application's pass.
        fixture.swapchain.depthResolvePerImage[0].initialized = initiallyInitialized;
        fixture.record();
        expect(fixture.events.empty() && fixture.barriers.empty(),
               "active render scope emits no Vulkan calls, even without tracked depth");
        expect(fixture.swapchain.depthResolvePerImage[0].initialized == initiallyInitialized,
               "active render scope preserves destination initialization");
        expect(scope.inRenderScope && fixture.device.commandBufferDepthStates.size() == 1,
               "rejected snapshot leaves application scope tracking intact");

        // Keep the same source and destination: only crossing the scope boundary
        // makes recording legal. This prevents an always-return guard from passing.
        scope.inRenderScope = false;
        fixture.resetTrace();
        fixture.record();
        expectSnapshot(fixture, initiallyInitialized);
    }

    void untrackedCommandBuffer(bool shader)
    {
        Fixture fixture(shader);
        installDispatch(fixture);
        // An unrelated application's active pass must not suppress this layer CB.
        fixture.device.commandBufferDepthStates[handle<VkCommandBuffer>(99)].inRenderScope = true;
        fixture.record();
        expectSnapshot(fixture, false);
        expect(fixture.device.commandBufferDepthStates.count(fixture.cb) == 0
                   && fixture.device.commandBufferDepthStates.size() == 1,
               "layer-owned command buffer needs no tracking entry and creates none");
    }

    void transferDestinationSnapshot()
    {
        Fixture fixture(false);
        installDispatch(fixture);
        fixture.depth.observedLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        fixture.record();
        expect(fixture.events.find('C') != std::string::npos
                   && fixture.swapchain.depthResolvePerImage[0].initialized,
               "out-of-pass clear snapshot copies depth");
        expect(fixture.barriers.size() == 4, "transfer snapshot emits both source transitions");
        if (fixture.barriers.size() != 4)
            return;
        const auto& source = fixture.barriers.front();
        expect(source.image == fixture.depth.image
                   && source.oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                   && source.newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                   && source.srcAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT
                   && source.dstAccessMask == VK_ACCESS_TRANSFER_READ_BIT
                   && fixture.sourceStages.front() == VK_PIPELINE_STAGE_TRANSFER_BIT
                   && fixture.destinationStages.front() == VK_PIPELINE_STAGE_TRANSFER_BIT,
               "snapshot synchronizes the preceding clear's transfer writes before reading");
        const auto& restore = fixture.barriers.back();
        expect(restore.image == fixture.depth.image
                   && restore.oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                   && restore.newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                   && restore.srcAccessMask == VK_ACCESS_TRANSFER_READ_BIT
                   && restore.dstAccessMask == VK_ACCESS_TRANSFER_WRITE_BIT
                   && fixture.sourceStages.back() == VK_PIPELINE_STAGE_TRANSFER_BIT
                   && fixture.destinationStages.back() == VK_PIPELINE_STAGE_TRANSFER_BIT,
               "snapshot restores the supplied transfer layout and write synchronization");
    }
}

int main()
{
    settingsManager.setDepthSourceChannel(0);
    settingsManager.setDepthInvert(false);
    settingsManager.setDepthFlip(false);
    settingsManager.setDepthFlop(false);
    for (bool shader : {false, true})
    {
        for (bool initialized : {false, true})
            for (bool trackedDepthUsable : {false, true})
                scopeBoundary(shader, initialized, trackedDepthUsable);
        untrackedCommandBuffer(shader);
    }
    transferDestinationSnapshot();
    std::printf("Depth snapshot scope: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
