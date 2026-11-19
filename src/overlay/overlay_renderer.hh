#ifndef VKHOOK_OVERLAY_RENDERER_HH_INCLUDED
#define VKHOOK_OVERLAY_RENDERER_HH_INCLUDED

// OverlayRenderer: what one swapchain needs to put the overlay on screen.
//
// It owns the render pass for that swapchain's format and, per swapchain
// image, a command buffer, a fence and a framebuffer. The UI itself comes from
// the device's OverlayUi, which every renderer of the device shares.
//
// How core uses it (the wiring is present/swapchain.cc and present/present.cc):
//
//   construct  when a swapchain is created (after its image views exist):
//                swapchain->overlay = OverlayRenderer::create(ui, device,
//                                         swapchain->format, swapchain->imageCount);
//              A null result means the overlay is unavailable for this
//              swapchain; present without it.
//   destroy    when the swapchain is destroyed or replaced, before the device's
//              OverlayUi and before the swapchain's image views are retired.
//              The destructor hands every GPU object to the retire helper in
//              overlay_renderer.cc and never waits.
//   record     once per present, on the present thread, after the effect
//              submission that wrote `imageView`:
//                VkCommandBuffer cmd = overlay->record(index, imageView, w, h, input);
//              `input` is the swapchain's InputSession (SwapchainShared::input),
//              or null for no input this frame; the overlay reads the pointer
//              and keyboard from it and publishes its hitboxes and blocking to it.
//              A null result means there is nothing to draw; present the effect
//              semaphore directly. Otherwise submit `cmd` on the present queue,
//              waiting on the effect semaphore and signalling the overlay one,
//              with the fence from fenceForSubmit(index):
//                VkFence fence = overlay->fenceForSubmit(index);
//                vkQueueSubmit(queue, 1, &submit, fence);
//              fenceForSubmit resets the fence, so call it exactly once per
//              submit and only for a non-null record() result.
//   drain      collectFences() lists the fences of in-flight overlay work, for
//              callers that must wait before destroying shared images.
//
// Lifetime: the OverlayUi must outlive every renderer made from it. A swapchain
// that changes format or image count is a new swapchain and gets a new renderer.

#include <cstdint>
#include <memory>
#include <vector>

#include "vulkan_include.hh"

namespace VkHook
{
    struct LogicalDevice;
    class InputSession;
    class OverlayUi;

    class OverlayRenderer
    {
    public:
        static std::unique_ptr<OverlayRenderer> create(OverlayUi& ui, LogicalDevice* device, VkFormat format,
                                                       uint32_t imageCount);
        ~OverlayRenderer();
        OverlayRenderer(const OverlayRenderer&) = delete;
        OverlayRenderer& operator=(const OverlayRenderer&) = delete;

        // Records this frame's UI for swapchain image `imageIndex`, or returns
        // VK_NULL_HANDLE when there is nothing to draw, the index is outside
        // the per-image resources, or the previous use of this image's command
        // buffer has not finished.
        VkCommandBuffer record(uint32_t imageIndex, VkImageView imageView, uint32_t width, uint32_t height,
                               InputSession* input = nullptr);

        // The fence for the submit that executes record()'s command buffer.
        VkFence fenceForSubmit(uint32_t imageIndex);

        void collectFences(std::vector<VkFence>& out) const;

        VkFormat format() const { return framebufferFormat; }
        uint32_t imageCount() const { return static_cast<uint32_t>(commandBuffers.size()); }

        // Waits (bounded) for every command buffer of this renderer to finish.
        // OverlayUi uses it before it rebuilds ImGui's pipeline.
        bool waitIdle(uint64_t timeoutNs);

    private:
        OverlayRenderer(OverlayUi& ui, LogicalDevice* device);
        bool initialize(VkFormat format, uint32_t imageCount);
        bool createRenderPass(VkFormat format);

        OverlayUi& ui;
        LogicalDevice* logicalDevice;
        VkFormat framebufferFormat = VK_FORMAT_UNDEFINED;
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> commandBuffers;
        std::vector<VkFence> fences;  // signalled whenever the image's command buffer is idle
        std::vector<VkFramebuffer> framebuffers;
        std::vector<VkImageView> framebufferViews;  // the view each framebuffer was made for
        std::vector<VkExtent2D> framebufferExtents;
        bool attached = false;
        bool warnedImageIndex = false;
    };
} // namespace VkHook

#endif // VKHOOK_OVERLAY_RENDERER_HH_INCLUDED
