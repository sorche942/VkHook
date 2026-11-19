#ifndef EFFECT_RENDERPASS_HPP_INCLUDED
#define EFFECT_RENDERPASS_HPP_INCLUDED

#include <vector>

#include "vulkan_include.hh"

#include "logical_device.hh"

namespace VkHook
{
    // The one way effects build a render pass: a single subpass over colour
    // attachments in GENERAL (the layout every effect image is kept in) plus an
    // optional depth/stencil attachment, bracketed by the external dependencies
    // that order it against the transfer, shader and attachment work of the
    // neighbouring passes.
    //
    // `attachments` lists the colour attachments first, then the depth/stencil
    // attachment when `hasDepthStencil`. The subpass references colour attachment
    // i in COLOR_ATTACHMENT_OPTIMAL and the depth/stencil one in
    // DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
    struct EffectRenderPassDesc
    {
        std::vector<VkAttachmentDescription> attachments;
        bool                                 hasDepthStencil = false;
    };

    // A colour attachment that stays in GENERAL before and after the pass.
    VkAttachmentDescription effectColorAttachment(VkFormat format, VkAttachmentLoadOp loadOp);

    // A depth/stencil attachment whose stencil is cleared when `clearStencil`
    // and loaded otherwise; depth is not used. Stays in
    // DEPTH_STENCIL_ATTACHMENT_OPTIMAL.
    VkAttachmentDescription effectStencilAttachment(VkFormat format, bool clearStencil);

    // The create info for `desc`, with the subpass and dependency arrays owned by
    // the returned object (the info points into it, so keep it alive across the
    // create call).
    class EffectRenderPassInfo
    {
    public:
        explicit EffectRenderPassInfo(const EffectRenderPassDesc& desc);
        EffectRenderPassInfo(const EffectRenderPassInfo&) = delete;
        EffectRenderPassInfo& operator=(const EffectRenderPassInfo&) = delete;

        const VkRenderPassCreateInfo& createInfo() const { return m_createInfo; }
        const std::vector<VkAttachmentReference>& references() const { return m_references; }
        const VkSubpassDescription& subpass() const { return m_subpass; }
        const VkSubpassDependency* dependencies() const { return m_dependencies; }

    private:
        std::vector<VkAttachmentDescription> m_attachments;
        std::vector<VkAttachmentReference>   m_references;
        VkSubpassDescription                 m_subpass = {};
        VkSubpassDependency                  m_dependencies[2] = {};
        VkRenderPassCreateInfo               m_createInfo = {};
    };

    VkResult createEffectRenderPass(LogicalDevice* logicalDevice, const EffectRenderPassDesc& desc, VkRenderPass& renderPass);
} // namespace VkHook

#endif // EFFECT_RENDERPASS_HPP_INCLUDED
