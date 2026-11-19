#include "depth/capture.hh"

#include "depth/depth_shaders.hh"

#include "command_buffer.hh"
#include "graphics_pipeline.hh"
#include "image.hh"
#include "image_view.hh"
#include "logger.hh"

namespace VkHook
{
namespace
{
    // must match DepthParams in depth_resolve.frag.glsl: three int32
    struct PushConstants
    {
        int32_t invert;
        int32_t flip;
        int32_t flop;
    };
    static_assert(sizeof(PushConstants) == 12, "push constants must match the GLSL block");

    constexpr VkFormat kTargetFormat = VK_FORMAT_R32_SFLOAT;

    int32_t modeConstant(VkResolveModeFlagBits mode)
    {
        switch (mode)
        {
        case VK_RESOLVE_MODE_AVERAGE_BIT: return 1;
        case VK_RESOLVE_MODE_MIN_BIT:     return 2;
        case VK_RESOLVE_MODE_MAX_BIT:     return 3;
        default:                          return 0;
        }
    }

    template <typename Handle>
    void retireIf(LogicalDevice* device, VkObjectType type, Handle handle)
    {
        if (handle != VK_NULL_HANDLE)
            device->retire(type, handle);
    }

    VkImageMemoryBarrier depthBarrier(const DepthSource& source, VkImageLayout from, VkImageLayout to,
                                      VkAccessFlags srcAccess, VkAccessFlags dstAccess)
    {
        VkImageMemoryBarrier barrier = {};
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask       = srcAccess;
        barrier.dstAccessMask       = dstAccess;
        barrier.oldLayout           = from;
        barrier.newLayout           = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = source.image;
        barrier.subresourceRange    = {depthBarrierAspects(source.format), 0, 1, 0, 1};
        return barrier;
    }
} // namespace

    std::shared_ptr<DepthCapture> DepthCapture::create(LogicalDevice* device, VkExtent2D extent, uint32_t imageCount)
    {
        if (!device || imageCount == 0 || extent.width == 0 || extent.height == 0)
            return nullptr;
        std::shared_ptr<DepthCapture> capture(new DepthCapture(device));
        if (!capture->build(extent, imageCount))
        {
            Logger::err("depth capture: could not create the capture targets; effects will see no depth");
            return nullptr;  // the destructor retires what was made
        }
        return capture;
    }

    DepthCapture::~DepthCapture()
    {
        for (Variant& variant : variants)
            retireIf(device, VK_OBJECT_TYPE_PIPELINE, variant.pipeline);
        for (Target& target : targets)
        {
            retireIf(device, VK_OBJECT_TYPE_FRAMEBUFFER, target.framebuffer);
            retireIf(device, VK_OBJECT_TYPE_IMAGE_VIEW, target.view);
            retireIf(device, VK_OBJECT_TYPE_IMAGE, target.image);
        }
        retireIf(device, VK_OBJECT_TYPE_DEVICE_MEMORY, memory);
        retireIf(device, VK_OBJECT_TYPE_COMMAND_POOL, commandPool);  // frees the buffers
        retireIf(device, VK_OBJECT_TYPE_PIPELINE_LAYOUT, pipelineLayout);
        retireIf(device, VK_OBJECT_TYPE_DESCRIPTOR_POOL, descriptorPool);  // frees the sets
        retireIf(device, VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, setLayout);
        retireIf(device, VK_OBJECT_TYPE_SAMPLER, sampler);
        retireIf(device, VK_OBJECT_TYPE_RENDER_PASS, renderPass);
    }

    bool DepthCapture::build(VkExtent2D target, uint32_t imageCount)
    {
        extent = target;
        const auto& vkd = device->vkd;
        const VkDevice dev = device->device;

        // the pass clears to the far plane, then the draw (when there is a source)
        // overwrites every pixel; the target leaves it ready for sampling
        VkAttachmentDescription attachment = {};
        attachment.format         = kTargetFormat;
        attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout    = kReadLayout;
        VkAttachmentReference colorRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments    = &colorRef;
        VkSubpassDependency dependencies[2] = {};
        // the previous frame's effect reads finish before the target is rewritten
        dependencies[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass    = 0;
        dependencies[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dependencies[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = 0;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        // and the effects that follow in the same submission see the result
        dependencies[1].srcSubpass    = 0;
        dependencies[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo passInfo = {};
        passInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        passInfo.attachmentCount = 1;
        passInfo.pAttachments    = &attachment;
        passInfo.subpassCount    = 1;
        passInfo.pSubpasses      = &subpass;
        passInfo.dependencyCount = 2;
        passInfo.pDependencies   = dependencies;
        if (vkd.CreateRenderPass(dev, &passInfo, nullptr, &renderPass) != VK_SUCCESS)
        {
            renderPass = VK_NULL_HANDLE;
            return false;
        }

        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter    = VK_FILTER_NEAREST;
        samplerInfo.minFilter    = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkd.CreateSampler(dev, &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
        {
            sampler = VK_NULL_HANDLE;
            return false;
        }

        VkDescriptorSetLayoutBinding binding = {};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings    = &binding;
        if (vkd.CreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &setLayout) != VK_SUCCESS)
        {
            setLayout = VK_NULL_HANDLE;
            return false;
        }

        VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageCount};
        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets       = imageCount;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes    = &poolSize;
        if (vkd.CreateDescriptorPool(dev, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS)
        {
            descriptorPool = VK_NULL_HANDLE;
            return false;
        }
        std::vector<VkDescriptorSetLayout> layouts(imageCount, setLayout);
        std::vector<VkDescriptorSet>       sets(imageCount, VK_NULL_HANDLE);
        VkDescriptorSetAllocateInfo allocInfo = {};
        allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool     = descriptorPool;
        allocInfo.descriptorSetCount = imageCount;
        allocInfo.pSetLayouts        = layouts.data();
        if (vkd.AllocateDescriptorSets(dev, &allocInfo, sets.data()) != VK_SUCCESS)
            return false;

        VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
        pipelineLayout = createGraphicsPipelineLayout(device, {setLayout}, {push});
        if (pipelineLayout == VK_NULL_HANDLE)
            return false;

        const std::vector<VkImage> images = createImages(
            device, imageCount, {extent.width, extent.height, 1}, kTargetFormat,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory);
        if (images.size() != imageCount)
            return false;
        const std::vector<VkImageView> imageViews = createImageViews(device, kTargetFormat, images);
        targets.assign(imageCount, Target{});
        for (uint32_t i = 0; i < imageCount; ++i)
        {
            targets[i].image = images[i];
            targets[i].set   = sets[i];
            if (i < imageViews.size())
                targets[i].view = imageViews[i];
        }
        for (Target& t : targets)
            if (t.view == VK_NULL_HANDLE)
                return false;
        for (Target& t : targets)
        {
            VkFramebufferCreateInfo fb = {};
            fb.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fb.renderPass      = renderPass;
            fb.attachmentCount = 1;
            fb.pAttachments    = &t.view;
            fb.width           = extent.width;
            fb.height          = extent.height;
            fb.layers          = 1;
            if (vkd.CreateFramebuffer(dev, &fb, nullptr, &t.framebuffer) != VK_SUCCESS)
            {
                t.framebuffer = VK_NULL_HANDLE;
                return false;
            }
        }

        VkCommandPoolCreateInfo cpi = {};
        cpi.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpi.queueFamilyIndex = device->queueFamilyIndex;
        if (vkd.CreateCommandPool(dev, &cpi, nullptr, &commandPool) != VK_SUCCESS)
        {
            commandPool = VK_NULL_HANDLE;
            return false;
        }
        const std::vector<VkCommandBuffer> buffers = allocateCommandBuffer(device, imageCount, commandPool);
        if (buffers.size() != imageCount)
            return false;
        for (uint32_t i = 0; i < imageCount; ++i)
            targets[i].commandBuffer = buffers[i];
        return true;
    }

    VkPipeline DepthCapture::pipelineFor(VkSampleCountFlagBits samples, VkResolveModeFlagBits mode)
    {
        // the 1-sample build has no resolve mode
        if (samples == VK_SAMPLE_COUNT_1_BIT)
            mode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        for (const Variant& v : variants)
            if (v.samples == samples && v.mode == mode)
                return v.pipeline;

        const auto& vkd = device->vkd;
        VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
        const auto moduleOf = [&](const uint32_t* code, size_t bytes, VkShaderModule& out) {
            VkShaderModuleCreateInfo info = {};
            info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            info.codeSize = bytes;
            info.pCode    = code;
            return vkd.CreateShaderModule(device->device, &info, nullptr, &out) == VK_SUCCESS;
        };
        const bool msaa = samples != VK_SAMPLE_COUNT_1_BIT;
        bool ok = moduleOf(kFullScreenTriangleVert, sizeof(kFullScreenTriangleVert), vertex)
                  && (msaa ? moduleOf(kDepthResolveMsaaFrag, sizeof(kDepthResolveMsaaFrag), fragment)
                           : moduleOf(kDepthResolveFrag, sizeof(kDepthResolveFrag), fragment));

        VkPipeline pipeline = VK_NULL_HANDLE;
        if (ok)
        {
            const int32_t data[2] = {static_cast<int32_t>(samples), modeConstant(mode)};
            const VkSpecializationMapEntry entries[2] = {{0, 0, sizeof(int32_t)}, {1, sizeof(int32_t), sizeof(int32_t)}};
            VkSpecializationInfo specialization = {2, entries, sizeof(data), data};
            pipeline = createGraphicsPipeline(device, vertex, nullptr, "main", fragment, &specialization, "main",
                                              renderPass, pipelineLayout);
        }
        if (vertex != VK_NULL_HANDLE)
            vkd.DestroyShaderModule(device->device, vertex, nullptr);
        if (fragment != VK_NULL_HANDLE)
            vkd.DestroyShaderModule(device->device, fragment, nullptr);

        if (pipeline == VK_NULL_HANDLE && !pipelineFailureLogged)
        {
            pipelineFailureLogged = true;
            Logger::err("depth capture: pipeline for " + std::to_string(static_cast<int>(samples))
                        + " samples could not be created; that source is captured as far plane");
        }
        variants.push_back({samples, mode, pipeline});  // a failure is remembered too
        return pipeline;
    }

    VkCommandBuffer DepthCapture::record(uint32_t imageIndex, const DepthSource& source, const DepthCaptureParams& params)
    {
        if (imageIndex >= targets.size())
            return VK_NULL_HANDLE;
        Target& target = targets[imageIndex];
        const auto& vkd = device->vkd;
        const VkCommandBuffer cb = target.commandBuffer;

        const VkPipeline pipeline = source.valid ? pipelineFor(source.samples, params.resolveMode) : VK_NULL_HANDLE;
        const bool draw = pipeline != VK_NULL_HANDLE;

        // the sampled layout; a copy in layer storage is already in it
        const VkImageLayout readLayout = draw ? depthReadOnlyLayout(source.format, device->separateDepthStencilLayouts)
                                              : VK_IMAGE_LAYOUT_UNDEFINED;

        if (draw)
        {
            VkDescriptorImageInfo imageInfo = {sampler, source.view, readLayout};
            VkWriteDescriptorSet write = {};
            write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet          = target.set;
            write.dstBinding      = 0;
            write.descriptorCount = 1;
            write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo      = &imageInfo;
            vkd.UpdateDescriptorSets(device->device, 1, &write, 0, nullptr);
        }

        if (vkd.ResetCommandBuffer(cb, 0) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkd.BeginCommandBuffer(cb, &begin) != VK_SUCCESS)
            return VK_NULL_HANDLE;

        const bool transition = draw && !source.fromStorage;
        if (transition)
        {
            // from the tracked end-of-frame layout; ALL_COMMANDS on the source
            // side covers whatever the application's last writes were
            const VkImageMemoryBarrier toRead = depthBarrier(source, source.layout, readLayout,
                                                             VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
            vkd.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                   nullptr, 0, nullptr, 1, &toRead);
        }

        VkClearValue clear = {};
        clear.color.float32[0] = 1.0f;
        VkRenderPassBeginInfo pass = {};
        pass.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass        = renderPass;
        pass.framebuffer       = target.framebuffer;
        pass.renderArea.extent = extent;
        pass.clearValueCount   = 1;
        pass.pClearValues      = &clear;
        vkd.CmdBeginRenderPass(cb, &pass, VK_SUBPASS_CONTENTS_INLINE);
        if (draw)
        {
            vkd.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkd.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &target.set, 0, nullptr);
            cmdSetViewportScissor(device, cb, extent);
            const PushConstants constants = {params.invert ? 1 : 0, params.flip ? 1 : 0, params.flop ? 1 : 0};
            vkd.CmdPushConstants(cb, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
            vkd.CmdDraw(cb, 3, 1, 0, 0);
        }
        vkd.CmdEndRenderPass(cb);

        if (transition)
        {
            // balanced: the application's next frame finds its layout unchanged
            const VkImageMemoryBarrier back = depthBarrier(source, readLayout, source.layout, 0,
                                                           VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
            vkd.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                   nullptr, 0, nullptr, 1, &back);
        }
        return vkd.EndCommandBuffer(cb) == VK_SUCCESS ? cb : VK_NULL_HANDLE;
    }
} // namespace VkHook
