#include "effect_renderpass.hh"

namespace VkHook
{
    VkAttachmentDescription effectColorAttachment(VkFormat format, VkAttachmentLoadOp loadOp)
    {
        VkAttachmentDescription attachment = {};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = loadOp;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        return attachment;
    }

    VkAttachmentDescription effectStencilAttachment(VkFormat format, bool clearStencil)
    {
        VkAttachmentDescription attachment = {};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.stencilLoadOp = clearStencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        return attachment;
    }

    EffectRenderPassInfo::EffectRenderPassInfo(const EffectRenderPassDesc& desc)
        : m_attachments(desc.attachments)
    {
        const uint32_t colorCount = static_cast<uint32_t>(m_attachments.size()) - (desc.hasDepthStencil ? 1u : 0u);
        for (uint32_t i = 0; i < colorCount; ++i)
            m_references.push_back({i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
        if (desc.hasDepthStencil)
            m_references.push_back({colorCount, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL});

        m_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        m_subpass.colorAttachmentCount = colorCount;
        m_subpass.pColorAttachments = m_references.data();
        m_subpass.pDepthStencilAttachment = desc.hasDepthStencil ? &m_references.back() : nullptr;

        constexpr VkPipelineStageFlags neighbourStages = VK_PIPELINE_STAGE_TRANSFER_BIT |
                                                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

        m_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        m_dependencies[0].dstSubpass = 0;
        m_dependencies[0].srcStageMask = neighbourStages;
        m_dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        m_dependencies[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                                          VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        m_dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        m_dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        m_dependencies[1].srcSubpass = 0;
        m_dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        m_dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        m_dependencies[1].dstStageMask = neighbourStages;
        m_dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        m_dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        m_dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

        m_createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        m_createInfo.attachmentCount = static_cast<uint32_t>(m_attachments.size());
        m_createInfo.pAttachments = m_attachments.data();
        m_createInfo.subpassCount = 1;
        m_createInfo.pSubpasses = &m_subpass;
        m_createInfo.dependencyCount = 2;
        m_createInfo.pDependencies = m_dependencies;
    }

    VkResult createEffectRenderPass(LogicalDevice* logicalDevice, const EffectRenderPassDesc& desc, VkRenderPass& renderPass)
    {
        const EffectRenderPassInfo info(desc);
        return logicalDevice->vkd.CreateRenderPass(logicalDevice->device, &info.createInfo(), nullptr, &renderPass);
    }
} // namespace VkHook
