#ifndef GRAPHICS_PIPELINE_HPP_INCLUDED
#define GRAPHICS_PIPELINE_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <memory>

#include "vulkan_include.hh"

#include "logical_device.hh"

namespace VkHook
{
    VkPipelineLayout createGraphicsPipelineLayout(LogicalDevice* logicalDevice, 
                                                   std::vector<VkDescriptorSetLayout> descriptorSetLayouts,
                                                   std::vector<VkPushConstantRange> pushConstantRanges = {});

    // The device's shared pipeline cache, created on first use. Every pipeline the
    // layer builds goes through it, so a chain reload reuses the work the driver
    // already did. Null when the cache cannot be created (pipelines are then
    // built uncached). Not thread-safe: callers build pipelines on one thread per
    // device at a time.
    VkPipelineCache devicePipelineCache(LogicalDevice* logicalDevice);

    // A full-screen-style pipeline with dynamic viewport and scissor: it does not
    // depend on the target size, so one pipeline serves every swapchain extent.
    // Every command buffer that binds it must call cmdSetViewportScissor first.
    VkPipeline createGraphicsPipeline(LogicalDevice*        logicalDevice,
                                      VkShaderModule        vertexModule,
                                      VkSpecializationInfo* vertexSpecializationInfo,
                                      std::string           vertexEntryPoint,
                                      VkShaderModule        fragmentModule,
                                      VkSpecializationInfo* fragmentSpecializationInfo,
                                      std::string           fragmentEntryPoint,
                                      VkRenderPass          renderPass,
                                      VkPipelineLayout      pipelineLayout,
                                      uint32_t              subpass = 0);

    // Viewport and scissor covering `extent` from the origin. Inline so the
    // headless recorder tests need not link the pipeline builders.
    inline void cmdSetViewportScissor(LogicalDevice* logicalDevice, VkCommandBuffer commandBuffer, VkExtent2D extent)
    {
        VkViewport viewport;
        viewport.x        = 0.0f;
        viewport.y        = 0.0f;
        viewport.width    = static_cast<float>(extent.width);
        viewport.height   = static_cast<float>(extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        VkRect2D scissor;
        scissor.offset = {0, 0};
        scissor.extent = extent;
        logicalDevice->vkd.CmdSetViewport(commandBuffer, 0, 1, &viewport);
        logicalDevice->vkd.CmdSetScissor(commandBuffer, 0, 1, &scissor);
    }

} // namespace VkHook

#endif // GRAPHICS_PIPELINE_HPP_INCLUDED
