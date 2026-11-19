#include "overlay_renderer.hh"

#include "logical_device.hh"
#include "overlay_ui.hh"
#include "vk_handle.hh"
#include "logger.hh"

#include <algorithm>
#include <utility>

namespace VkHook
{
    namespace
    {
        // The one place this file releases GPU objects. Handing a destroy to the
        // deferred queue means the device-destroy flush decides when it runs, so
        // a renderer torn down during device teardown never calls into a
        // destroyed VkDevice. The integrator retargets this to the device's
        // RetireQueue in one edit.
        template<typename Destroy>
        void retire(DestroyPhase phase, Destroy&& destroy)
        {
            DeferredDestroyQueue::instance().push(phase, std::forward<Destroy>(destroy));
        }
    } // namespace

    std::unique_ptr<OverlayRenderer> OverlayRenderer::create(OverlayUi& ui, LogicalDevice* device, VkFormat format,
                                                             uint32_t imageCount)
    {
        if (!device || imageCount == 0)
            return nullptr;
        std::unique_ptr<OverlayRenderer> renderer(new OverlayRenderer(ui, device));
        if (!renderer->initialize(format, imageCount))
            return nullptr;
        return renderer;
    }

    OverlayRenderer::OverlayRenderer(OverlayUi& owner, LogicalDevice* device) : ui(owner), logicalDevice(device) {}

    OverlayRenderer::~OverlayRenderer()
    {
        if (attached)
            ui.detachRenderer(this);

        const VkDevice device = logicalDevice->device;
        const auto vkd = logicalDevice->vkd;

        if (commandPool != VK_NULL_HANDLE)
        {
            const VkCommandPool pool = commandPool;
            retire(DestroyPhase::Resource, [vkd, device, pool] { vkd.DestroyCommandPool(device, pool, nullptr); });
        }
        // framebuffers before the pass they were created from
        for (const VkFramebuffer framebuffer : framebuffers)
        {
            if (framebuffer != VK_NULL_HANDLE)
                retire(DestroyPhase::RenderPass,
                       [vkd, device, framebuffer] { vkd.DestroyFramebuffer(device, framebuffer, nullptr); });
        }
        if (renderPass != VK_NULL_HANDLE)
        {
            const VkRenderPass pass = renderPass;
            retire(DestroyPhase::RenderPass, [vkd, device, pass] { vkd.DestroyRenderPass(device, pass, nullptr); });
        }
        // Fences are the earliest phase: a command pool destroyed while a
        // submitted fence is still unsignalled is not allowed, and the pool
        // lands in Resource.
        for (const VkFence fence : fences)
        {
            if (fence != VK_NULL_HANDLE)
                retire(DestroyPhase::Sync, [vkd, device, fence] { vkd.DestroyFence(device, fence, nullptr); });
        }
    }

    bool OverlayRenderer::createRenderPass(VkFormat format)
    {
        VkAttachmentDescription attachment = {};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef = {};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        // Incoming: wait for prior color writes before the overlay renders.
        // srcAccessMask MUST include COLOR_ATTACHMENT_WRITE (or TRANSFER_WRITE) so
        // the implicit PRESENT_SRC -> COLOR_ATTACHMENT_OPTIMAL transition does not
        // race with the effect pass that just wrote the image. The semaphore
        // between the effect submit and the overlay submit orders execution, but
        // the dependency governs the transition barrier and must describe the
        // source access correctly.
        VkSubpassDependency dependencies[2] = {};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        // Outgoing: overlay writes and the layout transition must be complete and
        // VISIBLE to the presentation engine and to DMA-BUF screen capture.
        // MEMORY_READ is critical: without it GPU caches may not be flushed
        // before PipeWire or the compositor reads the image.
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

        VkRenderPassCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 2;
        info.pDependencies = dependencies;

        const VkResult vr = logicalDevice->vkd.CreateRenderPass(logicalDevice->device, &info, nullptr, &renderPass);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to create overlay render pass: " + std::to_string(vr));
            renderPass = VK_NULL_HANDLE;
            return false;
        }
        framebufferFormat = format;
        return true;
    }

    bool OverlayRenderer::initialize(VkFormat format, uint32_t imageCount)
    {
        const VkDevice device = logicalDevice->device;
        auto& vkd = logicalDevice->vkd;

        // Everything below is released by the destructor if a step fails, so a
        // failed create never leaks: the destructor only retires non-null handles.
        if (!createRenderPass(format))
            return false;

        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = logicalDevice->queueFamilyIndex;
        VkResult vr = vkd.CreateCommandPool(device, &poolInfo, nullptr, &commandPool);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to create overlay command pool: " + std::to_string(vr));
            commandPool = VK_NULL_HANDLE;
            return false;
        }

        commandBuffers.assign(imageCount, VK_NULL_HANDLE);
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = imageCount;
        vr = vkd.AllocateCommandBuffers(device, &allocInfo, commandBuffers.data());
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to allocate overlay command buffers: " + std::to_string(vr));
            commandBuffers.clear();
            return false;
        }

        // signalled initially so the first frame doesn't wait; fenceForSubmit
        // resets one only at the moment it is handed to a submit
        fences.assign(imageCount, VK_NULL_HANDLE);
        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < imageCount; ++i)
        {
            vr = vkd.CreateFence(device, &fenceInfo, nullptr, &fences[i]);
            if (vr != VK_SUCCESS)
            {
                Logger::err("Failed to create overlay fence " + std::to_string(i) + ": " + std::to_string(vr));
                return false;
            }
        }

        framebuffers.assign(imageCount, VK_NULL_HANDLE);
        framebufferViews.assign(imageCount, VK_NULL_HANDLE);
        framebufferExtents.assign(imageCount, VkExtent2D{0, 0});

        // Pool and fences are retired by the destructor on every failure path;
        // the UI registry is only joined once everything exists.
        if (!ui.attachRenderer(this, format, renderPass, imageCount))
            return false;
        attached = true;
        return true;
    }

    bool OverlayRenderer::waitIdle(uint64_t timeoutNs)
    {
        if (fences.empty())
            return true;
        const VkResult waited = logicalDevice->vkd.WaitForFences(
            logicalDevice->device, static_cast<uint32_t>(fences.size()), fences.data(), VK_TRUE, timeoutNs);
        return waited == VK_SUCCESS;
    }

    VkFence OverlayRenderer::fenceForSubmit(uint32_t imageIndex)
    {
        if (imageIndex >= fences.size())
            return VK_NULL_HANDLE;
        VkFence fence = fences[imageIndex];
        const VkResult reset = logicalDevice->vkd.ResetFences(logicalDevice->device, 1, &fence);
        if (reset != VK_SUCCESS)
        {
            // submitting with a signalled fence is a validation error, so submit without one
            Logger::err("Failed to reset overlay fence: " + std::to_string(reset));
            return VK_NULL_HANDLE;
        }
        return fence;
    }

    void OverlayRenderer::collectFences(std::vector<VkFence>& out) const
    {
        for (const VkFence fence : fences)
            if (fence != VK_NULL_HANDLE)
                out.push_back(fence);
    }

    VkCommandBuffer OverlayRenderer::record(uint32_t imageIndex, VkImageView imageView, uint32_t width, uint32_t height)
    {
        if (!ui.beginFrameCheck(framebufferFormat, renderPass))
            return VK_NULL_HANDLE;

        // A swapchain with more images than this renderer was built for must not
        // index past the per-image arrays.
        if (imageIndex >= commandBuffers.size())
        {
            if (!warnedImageIndex)
            {
                warnedImageIndex = true;
                Logger::warn("overlay: image index " + std::to_string(imageIndex) + " outside the "
                             + std::to_string(commandBuffers.size()) + " images it was built for; skipping its frame");
            }
            return VK_NULL_HANDLE;
        }

        auto& vkd = logicalDevice->vkd;
        const VkDevice device = logicalDevice->device;

        // Wait for the previous use of this command buffer. The timeout adapts
        // to the frame time: with triple buffering the fence was submitted two
        // or three frames ago and is almost always already signalled. It stays
        // signalled here; fenceForSubmit resets it right before the submit, so
        // none of the early returns below can strand it unsignalled.
        const VkResult fenceResult = vkd.WaitForFences(device, 1, &fences[imageIndex], VK_TRUE, ui.fenceTimeoutNs());
        if (fenceResult == VK_TIMEOUT)
        {
            Logger::warn("overlay fence wait timed out for image " + std::to_string(imageIndex));
            return VK_NULL_HANDLE;
        }
        if (fenceResult != VK_SUCCESS)
        {
            Logger::err("overlay fence wait failed: " + std::to_string(fenceResult));
            return VK_NULL_HANDLE;
        }

        // The framebuffer follows the image view and the surface size of this
        // image alone; a resize recreates it. Its fence was just waited, so
        // nothing in flight still uses the old one.
        VkExtent2D& extent = framebufferExtents[imageIndex];
        if (framebuffers[imageIndex] == VK_NULL_HANDLE || framebufferViews[imageIndex] != imageView
            || extent.width != width || extent.height != height)
        {
            if (framebuffers[imageIndex] != VK_NULL_HANDLE)
            {
                const VkFramebuffer old = framebuffers[imageIndex];
                const auto destroyFramebuffer = vkd.DestroyFramebuffer;
                retire(DestroyPhase::RenderPass,
                       [destroyFramebuffer, device, old] { destroyFramebuffer(device, old, nullptr); });
                framebuffers[imageIndex] = VK_NULL_HANDLE;
            }

            VkFramebufferCreateInfo fbInfo = {};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = renderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = &imageView;
            fbInfo.width = width;
            fbInfo.height = height;
            fbInfo.layers = 1;
            const VkResult vr = vkd.CreateFramebuffer(device, &fbInfo, nullptr, &framebuffers[imageIndex]);
            if (vr != VK_SUCCESS)
            {
                framebuffers[imageIndex] = VK_NULL_HANDLE;
                Logger::err("Failed to create overlay framebuffer: " + std::to_string(vr));
                return VK_NULL_HANDLE;
            }
            framebufferViews[imageIndex] = imageView;
            extent = {width, height};
        }

        // The UI frame comes first: it can fail without leaving a half-recorded
        // command buffer behind.
        ImDrawData* drawData = ui.buildFrame(width, height);
        if (!drawData)
            return VK_NULL_HANDLE;

        const VkCommandBuffer cmd = commandBuffers[imageIndex];
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult vr = vkd.BeginCommandBuffer(cmd, &beginInfo);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to begin overlay command buffer: " + std::to_string(vr));
            return VK_NULL_HANDLE;
        }

        VkRenderPassBeginInfo rpBegin = {};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = renderPass;
        rpBegin.framebuffer = framebuffers[imageIndex];
        rpBegin.renderArea.extent.width = width;
        rpBegin.renderArea.extent.height = height;
        vkd.CmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        ui.drawFrame(drawData, cmd);
        vkd.CmdEndRenderPass(cmd);

        vr = vkd.EndCommandBuffer(cmd);
        if (vr != VK_SUCCESS)
        {
            Logger::err("Failed to end overlay command buffer: " + std::to_string(vr));
            return VK_NULL_HANDLE;
        }
        return cmd;
    }
} // namespace VkHook
