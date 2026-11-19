#pragma once

// DepthTracker: per-device bookkeeping of the application's depth images
// (AGENTS.md invariants 4, 7 and 11; formal/VkHookFormal/ChainRebuild.lean).
//
// What it knows, and when:
//   * every depth image with DEPTH_STENCIL_ATTACHMENT usage, in one map keyed
//     by VkImage; views and framebuffers resolve to (VkImage, generation);
//   * per command buffer a CmdState, reached through the allocate/free/pool
//     hooks. Recording hooks touch only that command buffer's state (the
//     application externally synchronises command buffers), logging
//     {image, layout after, draws} events. Nothing is decided at record time;
//   * at vkQueueSubmit* the events of the submitted command buffers are folded
//     into the images in submission order: the tracked layout and the
//     per-frame draw histogram;
//   * at present the window-sized image with the most draws is selected, with a
//     few frames of hysteresis.
//
// The layer never records its own pipelines, descriptor sets or render passes
// into application command buffers. The one injection is a transfer-only
// vkCmdCopyImage of a depth image about to be overwritten into layer-owned
// storage (DepthLayerStorage). A storage object is shared by the tracker and
// by every recorded application command buffer bound to it, and is retired
// only when the last of them lets go (reset, re-begin, free, pool reset, pool
// destroy) and the layer submissions that read it have completed.
//
// Locking: `cmdMutex` guards the command-buffer and pool maps, `mutex` guards
// every other map and the per-image state. Order: cmdMutex, then mutex. Neither
// is held across a driver call, a GPU wait or a log write. Draw hooks take no
// lock when the thread's cached CmdState is current.

#include "vulkan_include.hh"
#include "logical_device.hh"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace VkHook
{
    // Frames a competing image must lead before the selection switches to it.
    inline constexpr uint32_t kDepthSwitchFrames = 3;
    // The game may render under or up to client-drawn window chrome.
    inline constexpr int64_t kDepthChromeTolerancePx = 128;

    // ---- pure helpers (no device) ----

    // The layout a depth image is sampled in. DEPTH_READ_ONLY_OPTIMAL needs
    // separateDepthStencilLayouts, and only describes an image without stencil.
    VkImageLayout depthReadOnlyLayout(VkFormat format, bool separateDepthStencilLayouts);
    // Every aspect a barrier of a combined format must cover (H9).
    VkImageAspectFlags depthBarrierAspects(VkFormat format);
    // Exact window size, or both axes within the chrome tolerance of it.
    bool depthExtentMatchesWindow(const VkExtent3D& depth, const VkExtent2D& window);

    // Rewrite helpers for render pass creation / dynamic rendering: the layer
    // reads depth after the pass, so a DONT_CARE/NONE store becomes STORE.
    bool forceDepthAttachmentStoreOp(VkAttachmentDescription& attachment);
    bool forceDepthAttachmentStoreOp(VkAttachmentDescription2& attachment);
    bool forceDepthAttachmentStoreOp(VkRenderingAttachmentInfo& attachment);

    // Layer-owned depth image a recorded application command buffer copies into.
    struct DepthLayerStorage
    {
        LogicalDevice*        device = nullptr;
        VkImage               image  = VK_NULL_HANDLE;
        VkImageView           view   = VK_NULL_HANDLE;
        VkDeviceMemory        memory = VK_NULL_HANDLE;
        VkFormat              format = VK_FORMAT_UNDEFINED;
        VkExtent3D            extent = {0, 0, 1};
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        // the layout every injected copy leaves the image in
        VkImageLayout         readLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        DepthLayerStorage() = default;
        DepthLayerStorage(const DepthLayerStorage&)            = delete;
        DepthLayerStorage& operator=(const DepthLayerStorage&) = delete;
        // Retires the objects through the device's RetireQueue: layer
        // submissions that read them may still be in flight.
        ~DepthLayerStorage();
    };

    // One entry of a command buffer's log. draws == 0: a pure layout change.
    struct CmdEvent
    {
        VkImage       image  = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // the depth aspect's layout after the event
        uint32_t      draws  = 0;
    };

    // What one render pass / rendering scope does to its depth attachment.
    struct DepthScope
    {
        VkImage       image         = VK_NULL_HANDLE;  // tracked depth image drawn into, or null
        VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // layout at begin (exact)
        VkImageLayout finalLayout   = VK_IMAGE_LAYOUT_UNDEFINED;  // layout after the scope (exact)
        bool          clearsOnLoad  = false;  // loadOp CLEAR or DONT_CARE: the old contents die at begin
        // the application's own resolve target (1-sample, directly usable) and its layout after
        VkImage       resolveImage       = VK_NULL_HANDLE;
        VkImageLayout resolveFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    };

    // Per command buffer. Only the thread recording the buffer touches it; the
    // submit fold reads it under cmdMutex once the application submitted it.
    struct CmdState
    {
        VkCommandPool pool      = VK_NULL_HANDLE;
        bool          secondary = false;

        DepthScope    scope;
        bool          inScope    = false;
        uint32_t      scopeDraws = 0;
        // draws outside any tracked scope: a secondary buffer's, merged into
        // the primary's scope by vkCmdExecuteCommands
        uint32_t      looseDraws = 0;

        std::vector<CmdEvent> events;

        // Every storage a copy recorded into this buffer targets: the recorded
        // buffer can run again at any time, so each stays alive until the buffer
        // is reset, re-begun or freed. The last one holds the newest copy, of
        // `storageSource`.
        std::vector<std::shared_ptr<DepthLayerStorage>> storages;
        VkImage                                         storageSource = VK_NULL_HANDLE;

        void draw(uint32_t count) { (inScope ? scopeDraws : looseDraws) += count; }
        // appends or, when the last event is the same image, folds into it
        void note(VkImage image, VkImageLayout layout, uint32_t draws);
        void beginScope(const DepthScope& s);
        void endScope();
        // a secondary buffer executed inside this one
        void absorb(const CmdState& secondaryState);
        // reset / re-begin: forgets the log and releases the storage binding
        void reset();
    };

    // A depth image the capture can read this frame.
    struct DepthSource
    {
        bool                  valid  = false;
        VkImage               image  = VK_NULL_HANDLE;
        VkImageView           view   = VK_NULL_HANDLE;  // layer-owned 2D view of the depth aspect
        VkFormat              format = VK_FORMAT_UNDEFINED;
        VkExtent3D            extent = {0, 0, 1};
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        // the depth aspect's layout at the end of the frame (the storage's read layout when copied)
        VkImageLayout         layout = VK_IMAGE_LAYOUT_UNDEFINED;
        bool                  fromStorage = false;
        // keeps the storage alive until the layer submission that reads it is queued
        std::shared_ptr<DepthLayerStorage> storage;
    };

    // What the overlay shows. Read under the tracker's lock and copied out.
    struct DepthReport
    {
        struct Entry
        {
            VkImageView           appView = VK_NULL_HANDLE;  // the application's view: the pin id
            VkFormat              format  = VK_FORMAT_UNDEFINED;
            VkExtent3D            extent  = {0, 0, 1};
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
            VkImageLayout         layout  = VK_IMAGE_LAYOUT_UNDEFINED;
            bool                  transient = false;  // the application asked for TRANSIENT_ATTACHMENT
            uint32_t              drawCount = 0;      // draws in the last completed frame
        };
        std::vector<Entry> candidates;  // busiest first
        Entry              active;      // the selected image, or empty
        bool               hasActive = false;
        bool               pinned    = false;
        VkImageView        pinnedView = VK_NULL_HANDLE;
    };

    class DepthTracker
    {
    public:
        explicit DepthTracker(LogicalDevice* device);
        ~DepthTracker();
        DepthTracker(const DepthTracker&)            = delete;
        DepthTracker& operator=(const DepthTracker&) = delete;

        // ---- object lifetime hooks (create paths run after the driver call) ----

        // `usage` is what the driver was given; `transient` whether the
        // application's create info asked for TRANSIENT_ATTACHMENT.
        void onCreateImage(VkImage image, const VkImageCreateInfo& original, VkImageUsageFlags usage);
        struct Destroyed
        {
            bool tracked     = false;
            bool layerReads  = false;  // the layer created a view of it: a layer submission may still read it
        };
        Destroyed onDestroyImage(VkImage image);
        void onCreateImageView(VkImageView view, const VkImageViewCreateInfo& info);
        void onDestroyImageView(VkImageView view);
        void onCreateRenderPass(VkRenderPass renderPass, const VkRenderPassCreateInfo& info);
        void onCreateRenderPass(VkRenderPass renderPass, const VkRenderPassCreateInfo2& info);
        void onDestroyRenderPass(VkRenderPass renderPass);
        void onCreateFramebuffer(VkFramebuffer framebuffer, const VkFramebufferCreateInfo& info);
        void onDestroyFramebuffer(VkFramebuffer framebuffer);

        // ---- command buffer lifetime hooks (H8) ----

        void onAllocateCommandBuffers(VkCommandPool pool, VkCommandBufferLevel level, uint32_t count,
                                      const VkCommandBuffer* buffers);
        void onFreeCommandBuffers(VkCommandPool pool, uint32_t count, const VkCommandBuffer* buffers);
        void onBeginCommandBuffer(VkCommandBuffer commandBuffer);
        void onResetCommandBuffer(VkCommandBuffer commandBuffer);
        void onResetCommandPool(VkCommandPool pool);
        void onDestroyCommandPool(VkCommandPool pool);

        // ---- recording (no shared lock when the thread's cache is current) ----

        CmdState& cmd(VkCommandBuffer commandBuffer);

        DepthScope scopeFromRenderPassBegin(const VkRenderPassBeginInfo& begin);
        DepthScope scopeFromRenderingInfo(const VkRenderingInfo& info);

        // True when `image` is a selected depth image that already drew this
        // frame (committed submissions or `state`'s own log): overwriting it now
        // would lose the frame's depth before present.
        bool needsPreserve(const CmdState& state, VkImage image);
        // Records the transfer-only copy of `image` (in `layout`, exact) into
        // layer storage, between the render scopes of `commandBuffer`. False
        // when no storage could be made; nothing was recorded then.
        bool recordPreserveCopy(CmdState& state, VkCommandBuffer commandBuffer, VkImage image, VkImageLayout layout);

        // ---- submit ----

        // Folds the logs of `count` submitted command buffers, in order.
        void onSubmit(const VkCommandBuffer* buffers, uint32_t count);

        // ---- present ----

        // Chooses the depth source for the swapchain `key` (any stable id) whose
        // window is `window`, and closes the frame for the images of that size.
        // Creates the layer's sample view when it is missing (a driver call, no
        // lock held). `valid` is false when no usable image exists: the caller
        // then captures a cleared target, never freezes.
        DepthSource selectForPresent(uint64_t key, const VkExtent2D& window);
        // Forgets a swapchain's selection.
        void forgetSelection(uint64_t key);

        // ---- control plane (overlay commands, applied at a present boundary) ----

        bool pinDepthView(VkImageView view);   // false: not a tracked depth view
        void clearDepthPin();
        void refreshDetection();               // forgets selections, the pin and the frame's draws
        DepthReport report();

        bool tracked(VkImage image);
        VkImageLayout trackedLayout(VkImage image);
        size_t commandBufferCount();
        bool storageBound(const std::shared_ptr<DepthLayerStorage>& storage);

    private:
        struct TrackedDepth
        {
            VkFormat              format  = VK_FORMAT_UNDEFINED;
            VkExtent3D            extent  = {0, 0, 1};
            VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
            VkImageUsageFlags     usage   = 0;
            bool                  transient = false;
            VkImageLayout         layout  = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageView           layerView = VK_NULL_HANDLE;
            VkImageView           appView   = VK_NULL_HANDLE;
            uint64_t              generation = 0;
            uint32_t              frameDraws = 0;
            uint32_t              lastDraws  = 0;
        };
        struct ImageRef
        {
            VkImage  image      = VK_NULL_HANDLE;
            uint64_t generation = 0;
        };
        struct RenderPassDepth
        {
            uint32_t      depthIndex   = UINT32_MAX;
            uint32_t      resolveIndex = UINT32_MAX;
            VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageLayout finalLayout   = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageLayout resolveFinalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            bool          clearsOnLoad = false;
        };
        struct Selection
        {
            VkImage  image   = VK_NULL_HANDLE;
            VkImage  pending = VK_NULL_HANDLE;
            uint32_t pendingFrames = 0;
        };

        // all below: `mutex` held
        const TrackedDepth* findLocked(const ImageRef& ref) const;
        ImageRef refForViewLocked(VkImageView view) const;
        DepthScope scopeLocked(const RenderPassDepth& rp, const ImageRef& depth, const ImageRef& resolve) const;
        bool isSelectedLocked(VkImage image) const;

        std::shared_ptr<DepthLayerStorage> ensureStorage(VkFormat format, const VkExtent3D& extent,
                                                         VkSampleCountFlagBits samples);
        std::shared_ptr<DepthLayerStorage> makeStorage(VkFormat format, const VkExtent3D& extent,
                                                       VkSampleCountFlagBits samples);

        LogicalDevice* device;
        const uint64_t id;

        mutable std::shared_mutex mutex;
        uint64_t                                  nextGeneration = 1;
        std::unordered_map<VkImage, TrackedDepth> images;
        std::unordered_map<VkImageView, ImageRef> views;
        std::unordered_map<VkFramebuffer, std::vector<ImageRef>> framebuffers;
        std::unordered_map<VkRenderPass, RenderPassDepth>        renderPasses;
        std::unordered_map<uint64_t, Selection>   selections;
        VkImage                                   pinned = VK_NULL_HANDLE;
        VkImage                                   lastSelected = VK_NULL_HANDLE;
        std::shared_ptr<DepthLayerStorage>        storage;       // the storage new copies target
        VkImage                                   storageFrameSource = VK_NULL_HANDLE;  // a submitted copy this frame
        std::shared_ptr<DepthLayerStorage>        storageFrame;

        std::mutex                                                cmdMutex;
        std::unordered_map<VkCommandBuffer, std::unique_ptr<CmdState>> commandBuffers;
        std::unordered_map<VkCommandPool, std::vector<VkCommandBuffer>> pools;
        // bumped when a CmdState is destroyed: invalidates every thread's cached pointer
        std::atomic<uint64_t> epoch{1};
    };
} // namespace VkHook
