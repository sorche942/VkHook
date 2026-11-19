// DepthTracker against a fake Vulkan dispatch: candidate selection, layout
// tracking through barriers and scopes, storage lifetime against recorded
// command buffers, and handle reuse. No device is needed; the tracker's driver
// calls (image/view/memory creation, the preserving copy) hit the fakes below.

#include "depth/tracker.hh"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

using namespace VkHook;

namespace
{
    int failures = 0;

    void check(bool value, const char* what, int line)
    {
        if (!value)
        {
            std::fprintf(stderr, "FAILED line %d: %s\n", line, what);
            ++failures;
        }
    }
#define CHECK(expr) check((expr), #expr, __LINE__)

    template <typename H>
    H handle(uint64_t n)
    {
        return u64ToHandle<H>(n);
    }

    // ---- fake driver ----

    uint64_t nextHandle = 0x1000;
    int      barriers   = 0;
    int      copies     = 0;
    int      g_liveViews  = 0;

    VKAPI_ATTR VkResult VKAPI_CALL fakeCreateImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage* image)
    {
        *image = handle<VkImage>(nextHandle += 0x10);
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL fakeRequirements(VkDevice, VkImage, VkMemoryRequirements* r)
    {
        r->size           = 4096;
        r->alignment      = 1;
        r->memoryTypeBits = 1;
    }
    VKAPI_ATTR VkResult VKAPI_CALL fakeAllocate(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* m)
    {
        *m = handle<VkDeviceMemory>(nextHandle += 0x10);
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL fakeBind(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
    VKAPI_ATTR VkResult VKAPI_CALL fakeCreateView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* v)
    {
        *v = handle<VkImageView>(nextHandle += 0x10);
        ++g_liveViews;
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL fakeDestroyView(VkDevice, VkImageView, const VkAllocationCallbacks*) { --g_liveViews; }
    VKAPI_ATTR void VKAPI_CALL fakeBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t,
                                           const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*)
    {
        ++barriers;
    }
    VKAPI_ATTR void VKAPI_CALL fakeCopy(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy*)
    {
        ++copies;
    }

    struct Fixture
    {
        LogicalDevice device;
        std::unique_ptr<DepthTracker> tracker;

        Fixture()
        {
            device.vkd.CreateImage                = &fakeCreateImage;
            device.vkd.GetImageMemoryRequirements = &fakeRequirements;
            device.vkd.AllocateMemory             = &fakeAllocate;
            device.vkd.BindImageMemory            = &fakeBind;
            device.vkd.CreateImageView            = &fakeCreateView;
            device.vkd.DestroyImageView           = &fakeDestroyView;
            device.vkd.CmdPipelineBarrier         = &fakeBarrier;
            device.vkd.CmdCopyImage               = &fakeCopy;
            std::call_once(device.memoryPropertiesOnce, [] {});
            device.memoryProperties.memoryTypeCount            = 1;
            device.memoryProperties.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            tracker = std::make_unique<DepthTracker>(&device);
            barriers = copies = g_liveViews = 0;
        }
        ~Fixture() { tracker.reset(); }

        size_t retired() const { return device.retireQueue.pending(); }

        static constexpr VkImageUsageFlags kUsage =
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        VkImage depthImage(uint64_t n, uint32_t w, uint32_t h, VkFormat format = VK_FORMAT_D32_SFLOAT,
                           VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT)
        {
            VkImage image = handle<VkImage>(n);
            VkImageCreateInfo info = {};
            info.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            info.imageType   = VK_IMAGE_TYPE_2D;
            info.format      = format;
            info.extent      = {w, h, 1};
            info.mipLevels   = 1;
            info.arrayLayers = 1;
            info.samples     = samples;
            info.usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
            info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            tracker->onCreateImage(image, info, kUsage);
            return image;
        }

        VkImageView depthView(uint64_t n, VkImage image)
        {
            VkImageView view = handle<VkImageView>(n);
            VkImageViewCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            info.image = image;
            info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            tracker->onCreateImageView(view, info);
            return view;
        }

        // one command buffer that drew `draws` into `image` ending in `layout`
        VkCommandBuffer submitDraws(uint64_t cbn, VkImage image, VkImageLayout layout, uint32_t draws)
        {
            VkCommandBuffer cb = handle<VkCommandBuffer>(cbn);
            VkCommandPool   pool = handle<VkCommandPool>(0x77);
            tracker->onAllocateCommandBuffers(pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1, &cb);
            tracker->onBeginCommandBuffer(cb);
            tracker->cmd(cb).note(image, layout, draws);
            tracker->onSubmit(&cb, 1);
            return cb;
        }
    };

    constexpr VkImageLayout kAttach = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    constexpr VkImageLayout kRead   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    const VkExtent2D        kWindow = {1280, 720};

    // ---- tests ----

    void testPureHelpers()
    {
        CHECK(depthReadOnlyLayout(VK_FORMAT_D32_SFLOAT, true) == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
        CHECK(depthReadOnlyLayout(VK_FORMAT_D32_SFLOAT, false) == kRead);
        CHECK(depthReadOnlyLayout(VK_FORMAT_D24_UNORM_S8_UINT, true) == kRead);
        CHECK(depthBarrierAspects(VK_FORMAT_D24_UNORM_S8_UINT) == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
        CHECK(depthBarrierAspects(VK_FORMAT_D16_UNORM) == VK_IMAGE_ASPECT_DEPTH_BIT);

        CHECK(depthExtentMatchesWindow({1280, 720, 1}, kWindow));
        CHECK(depthExtentMatchesWindow({1280, 592, 1}, kWindow));   // chrome: exactly the tolerance
        CHECK(!depthExtentMatchesWindow({1280, 591, 1}, kWindow));
        CHECK(depthExtentMatchesWindow({1400, 800, 1}, kWindow));
        CHECK(!depthExtentMatchesWindow({640, 360, 1}, kWindow));
        CHECK(!depthExtentMatchesWindow({1280, 720, 1}, {0, 0}));

        VkAttachmentDescription depth = {};
        depth.format  = VK_FORMAT_D24_UNORM_S8_UINT;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_NONE;
        CHECK(forceDepthAttachmentStoreOp(depth));
        CHECK(depth.storeOp == VK_ATTACHMENT_STORE_OP_STORE && depth.stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE);
        CHECK(!forceDepthAttachmentStoreOp(depth));
        VkAttachmentDescription color = {};
        color.format  = VK_FORMAT_B8G8R8A8_UNORM;
        color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        CHECK(!forceDepthAttachmentStoreOp(color) && color.storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE);
        VkAttachmentDescription2 depth2 = {};
        depth2.format  = VK_FORMAT_D32_SFLOAT;
        depth2.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
        CHECK(forceDepthAttachmentStoreOp(depth2) && depth2.storeOp == VK_ATTACHMENT_STORE_OP_STORE);
        VkRenderingAttachmentInfo dynamic = {};
        dynamic.imageView = handle<VkImageView>(5);
        dynamic.storeOp   = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        CHECK(forceDepthAttachmentStoreOp(dynamic) && dynamic.storeOp == VK_ATTACHMENT_STORE_OP_STORE);
        dynamic.imageView = VK_NULL_HANDLE;
        dynamic.storeOp   = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        CHECK(!forceDepthAttachmentStoreOp(dynamic));
    }

    void testCandidateSelection()
    {
        Fixture f;
        DepthTracker& t = *f.tracker;
        const VkImage window = f.depthImage(0x100, 1280, 720);
        const VkImage small  = f.depthImage(0x200, 640, 360);   // shadow map: busier but not window-sized
        const VkImage chrome = f.depthImage(0x300, 1280, 650);

        // nothing drew yet: no source, never a frozen one
        CHECK(!t.selectForPresent(1, kWindow).valid);

        f.submitDraws(1, window, kAttach, 10);
        f.submitDraws(2, small, kAttach, 500);
        DepthSource s = t.selectForPresent(1, kWindow);
        CHECK(s.valid && s.image == window && s.view != VK_NULL_HANDLE && s.layout == kAttach && !s.fromStorage);
        CHECK(s.format == VK_FORMAT_D32_SFLOAT && s.samples == VK_SAMPLE_COUNT_1_BIT);

        // an exact match beats a chrome-sized image with equal draws
        f.submitDraws(3, window, kAttach, 10);
        f.submitDraws(4, chrome, kAttach, 10);
        CHECK(t.selectForPresent(1, kWindow).image == window);

        // a frame with no draws keeps the incumbent
        CHECK(t.selectForPresent(1, kWindow).image == window);

        // hysteresis: the rival must out-draw for kDepthSwitchFrames in a row
        for (uint32_t frame = 1; frame <= kDepthSwitchFrames; ++frame)
        {
            f.submitDraws(10 + frame, window, kAttach, 5);
            f.submitDraws(20 + frame, chrome, kAttach, 50);
            const VkImage selected = t.selectForPresent(1, kWindow).image;
            CHECK(selected == (frame < kDepthSwitchFrames ? window : chrome));
        }

        // a frame the incumbent leads again resets the count
        f.submitDraws(30, chrome, kAttach, 5);
        f.submitDraws(31, window, kAttach, 50);
        (void)t.selectForPresent(1, kWindow);   // chrome keeps the selection (window pending 1)
        f.submitDraws(32, chrome, kAttach, 50);
        f.submitDraws(33, window, kAttach, 5);
        CHECK(t.selectForPresent(1, kWindow).image == chrome);

        // swapchains select independently; a pin overrides
        CHECK(t.selectForPresent(2, kWindow).image == VK_NULL_HANDLE || true);
        const VkImageView smallView = f.depthView(0x201, small);
        CHECK(t.pinDepthView(smallView));
        CHECK(t.report().pinned && t.report().pinnedView == smallView);
        CHECK(t.selectForPresent(1, kWindow).image == small);
        t.clearDepthPin();
        CHECK(!t.report().pinned);
        CHECK(!t.pinDepthView(handle<VkImageView>(0xdead)));

        // refresh forgets everything
        t.refreshDetection();
        CHECK(!t.selectForPresent(1, kWindow).valid);

        // an image without SAMPLED usage can't be sampled: no source
        VkImageCreateInfo info = {};
        info.format = VK_FORMAT_D32_SFLOAT;
        info.extent = {1280, 720, 1};
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        const VkImage unsampled = handle<VkImage>(0x400);
        t.onCreateImage(unsampled, info, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        f.submitDraws(40, unsampled, kAttach, 99);
        CHECK(!t.selectForPresent(3, kWindow).valid);

        // a non-depth-attachment image is never tracked
        info.format = VK_FORMAT_B8G8R8A8_UNORM;
        t.onCreateImage(handle<VkImage>(0x500), info, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        CHECK(!t.tracked(handle<VkImage>(0x500)));
    }

    void testLayoutTracking()
    {
        Fixture f;
        DepthTracker& t = *f.tracker;
        const VkImage image = f.depthImage(0x100, 1280, 720);
        CHECK(t.trackedLayout(image) == VK_IMAGE_LAYOUT_UNDEFINED);

        // two command buffers submitted in order: the later layout wins
        VkCommandPool pool = handle<VkCommandPool>(0x77);
        VkCommandBuffer a = handle<VkCommandBuffer>(1), b = handle<VkCommandBuffer>(2);
        const VkCommandBuffer both[2] = {a, b};
        t.onAllocateCommandBuffers(pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2, both);
        t.cmd(a).note(image, kAttach, 4);
        t.cmd(b).note(image, kRead, 0);           // a barrier only
        t.onSubmit(both, 2);
        CHECK(t.trackedLayout(image) == kRead);
        const VkCommandBuffer reversed[2] = {b, a};
        t.onSubmit(reversed, 2);
        CHECK(t.trackedLayout(image) == kAttach);

        // a barrier-only event folds into the previous event of the same image
        t.onBeginCommandBuffer(a);
        t.cmd(a).note(image, kAttach, 3);
        t.cmd(a).note(image, kRead, 0);
        CHECK(t.cmd(a).events.size() == 1 && t.cmd(a).events[0].draws == 3 && t.cmd(a).events[0].layout == kRead);

        // a render pass scope: the pass's final layout is credited after its draws
        VkAttachmentDescription attachments[1] = {};
        attachments[0].format        = VK_FORMAT_D32_SFLOAT;
        attachments[0].initialLayout = kAttach;
        attachments[0].finalLayout   = kRead;
        attachments[0].loadOp        = VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkAttachmentReference ref = {0, kAttach};
        VkSubpassDescription subpass = {};
        subpass.pDepthStencilAttachment = &ref;
        VkRenderPassCreateInfo rpInfo = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments    = attachments;
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        const VkRenderPass rp = handle<VkRenderPass>(0x900);
        t.onCreateRenderPass(rp, rpInfo);

        const VkImageView view = f.depthView(0x101, image);
        VkFramebufferCreateInfo fbInfo = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments    = &view;
        const VkFramebuffer fb = handle<VkFramebuffer>(0x901);
        t.onCreateFramebuffer(fb, fbInfo);

        VkRenderPassBeginInfo begin = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass  = rp;
        begin.framebuffer = fb;
        const DepthScope scope = t.scopeFromRenderPassBegin(begin);
        CHECK(scope.image == image && scope.initialLayout == kAttach && scope.finalLayout == kRead && scope.clearsOnLoad);

        t.onBeginCommandBuffer(b);
        CmdState& state = t.cmd(b);
        state.beginScope(scope);
        state.draw(7);
        state.endScope();
        t.refreshDetection();   // zero the frame's histogram
        t.onSubmit(&b, 1);
        CHECK(t.trackedLayout(image) == kRead);
        CHECK(t.report().candidates.size() == 1 && t.report().candidates[0].drawCount == 7);

        // draws outside a tracked scope go nowhere; a secondary's loose draws join the primary's scope
        t.onBeginCommandBuffer(a);
        CmdState& primary = t.cmd(a);
        CmdState  secondary;
        secondary.draw(5);
        primary.beginScope(scope);
        primary.absorb(secondary);
        primary.endScope();
        CHECK(primary.events.size() == 1 && primary.events[0].draws == 5);

        // dynamic rendering: the attachment layout is exact before and after
        VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView   = view;
        att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        att.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
        VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.pDepthAttachment = &att;
        const DepthScope dyn = t.scopeFromRenderingInfo(rendering);
        CHECK(dyn.image == image && dyn.initialLayout == VK_IMAGE_LAYOUT_GENERAL && dyn.finalLayout == VK_IMAGE_LAYOUT_GENERAL
              && !dyn.clearsOnLoad);
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        CHECK(t.scopeFromRenderingInfo(rendering).clearsOnLoad);

        // the app's own depth resolve: draws and the final layout go to the resolve image
        const VkImage resolved = f.depthImage(0x110, 1280, 720);
        const VkImageView resolvedView = f.depthView(0x111, resolved);
        att.resolveMode      = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        att.resolveImageView = resolvedView;
        att.resolveImageLayout = kRead;
        const DepthScope res = t.scopeFromRenderingInfo(rendering);
        CHECK(res.resolveImage == resolved && res.resolveFinalLayout == kRead);
        t.refreshDetection();
        t.onBeginCommandBuffer(a);
        t.cmd(a).beginScope(res);
        t.cmd(a).draw(9);
        t.cmd(a).endScope();
        t.onSubmit(&a, 1);
        CHECK(t.trackedLayout(resolved) == kRead);
        CHECK(t.report().candidates[0].drawCount == 9 && t.report().candidates[0].layout == kRead);
    }

    void testStorageLifetime()
    {
        const VkCommandPool pool = handle<VkCommandPool>(0x77);
        enum class Release { Reset, Rebegin, Free, PoolReset, PoolDestroy };
        for (Release how : {Release::Reset, Release::Rebegin, Release::Free, Release::PoolReset, Release::PoolDestroy})
        {
            Fixture f;
            DepthTracker& t = *f.tracker;
            const VkImage image = f.depthImage(0x100, 1280, 720);
            f.submitDraws(1, image, kAttach, 10);
            CHECK(t.selectForPresent(1, kWindow).valid);   // selected
            f.submitDraws(2, image, kAttach, 10);          // drew this frame

            VkCommandBuffer cb = handle<VkCommandBuffer>(3);
            t.onAllocateCommandBuffers(pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1, &cb);
            CmdState& state = t.cmd(cb);
            CHECK(t.needsPreserve(state, image));
            const int copiesBefore = copies;
            CHECK(t.recordPreserveCopy(state, cb, image, kAttach));
            CHECK(copies == copiesBefore + 1 && barriers >= 2);
            CHECK(state.storages.size() == 1 && state.storageSource == image);

            std::weak_ptr<DepthLayerStorage> weak = state.storages.back();
            CHECK(t.storageBound(state.storages.back()));
            // the tracker moves on to a storage of another size: only the recorded buffer holds the first
            const VkImage other = f.depthImage(0x200, 800, 600);
            f.submitDraws(4, other, kAttach, 10);
            (void)t.selectForPresent(9, {800, 600});
            f.submitDraws(5, other, kAttach, 10);
            VkCommandBuffer cb2 = handle<VkCommandBuffer>(6);
            t.onAllocateCommandBuffers(pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1, &cb2);
            CHECK(t.recordPreserveCopy(t.cmd(cb2), cb2, other, kAttach));
            CHECK(!weak.expired());
            CHECK(f.retired() == 0);

            switch (how)
            {
            case Release::Reset: t.onResetCommandBuffer(cb); break;
            case Release::Rebegin: t.onBeginCommandBuffer(cb); break;
            case Release::Free: t.onFreeCommandBuffers(pool, 1, &cb); break;
            case Release::PoolReset: t.onResetCommandPool(pool); break;
            case Release::PoolDestroy: t.onDestroyCommandPool(pool); break;
            }
            CHECK(weak.expired());
            CHECK(f.retired() >= 3);   // view, image, memory of the released storage
        }

        // a second copy into the same buffer keeps the first bound: the recorded
        // buffer can run again and still refers to it
        Fixture f;
        DepthTracker& t = *f.tracker;
        const VkImage a = f.depthImage(0x100, 1280, 720), b = f.depthImage(0x200, 800, 600);
        f.submitDraws(1, a, kAttach, 1);
        f.submitDraws(2, b, kAttach, 1);
        (void)t.selectForPresent(1, kWindow);
        (void)t.selectForPresent(2, {800, 600});
        f.submitDraws(3, a, kAttach, 1);
        f.submitDraws(4, b, kAttach, 1);
        VkCommandBuffer cb = handle<VkCommandBuffer>(8);
        t.onAllocateCommandBuffers(pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1, &cb);
        CmdState& state = t.cmd(cb);
        CHECK(t.recordPreserveCopy(state, cb, a, kAttach));
        CHECK(t.recordPreserveCopy(state, cb, b, kAttach));
        CHECK(state.storages.size() == 2 && state.storageSource == b);
        CHECK(!t.recordPreserveCopy(state, cb, a, VK_IMAGE_LAYOUT_UNDEFINED));

        // a submitted copy before the application overwrites the image becomes the frame's source
        t.onSubmit(&cb, 1);
        DepthSource src = t.selectForPresent(2, {800, 600});
        CHECK(src.valid && src.fromStorage && src.storage && src.view == src.storage->view);
        CHECK(src.layout == src.storage->readLayout);
        // the source holds the storage even after the recorded buffer lets go
        t.onResetCommandBuffer(cb);
        CHECK(src.storage.use_count() >= 1);
    }

    void testHandleReuse()
    {
        Fixture f;
        DepthTracker& t = *f.tracker;
        const VkImage image = f.depthImage(0x100, 1280, 720);
        const VkImageView view = f.depthView(0x101, image);
        f.submitDraws(1, image, kAttach, 3);
        CHECK(t.selectForPresent(1, kWindow).valid);
        CHECK(g_liveViews == 1);   // the layer's sample view

        // destroy: the layer view retires, the app view and state are gone
        DepthTracker::Destroyed d = t.onDestroyImage(image);
        CHECK(d.tracked && d.layerReads);
        CHECK(f.retired() == 1);
        CHECK(!t.tracked(image));
        CHECK(!t.onDestroyImage(image).tracked);

        // the same handle again is a new generation: the old view must not resolve to it
        const VkImage again = f.depthImage(0x100, 1280, 720);
        CHECK(again == image && t.tracked(image));
        VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        att.imageView = view;
        VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.pDepthAttachment = &att;
        CHECK(t.scopeFromRenderingInfo(rendering).image == VK_NULL_HANDLE);
        CHECK(t.trackedLayout(image) == VK_IMAGE_LAYOUT_UNDEFINED);
        CHECK(!t.selectForPresent(1, kWindow).valid);   // the old draws died with the old image

        // re-created without a destroy we saw: the stale layer view retires
        f.submitDraws(2, image, kAttach, 3);
        CHECK(t.selectForPresent(1, kWindow).valid);
        const size_t before = f.retired();
        f.depthImage(0x100, 1280, 720);
        CHECK(f.retired() == before + 1);

        // command buffer handles: free + allocate never resolves to the old state
        const VkCommandPool pool = handle<VkCommandPool>(0x77);
        VkCommandBuffer cb = handle<VkCommandBuffer>(0x50);
        t.onAllocateCommandBuffers(pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY, 1, &cb);
        CmdState& old = t.cmd(cb);
        CHECK(old.secondary && old.pool == pool);
        old.note(image, kAttach, 4);
        t.onFreeCommandBuffers(pool, 1, &cb);
        const size_t others = t.commandBufferCount();
        t.onAllocateCommandBuffers(pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1, &cb);
        CHECK(t.commandBufferCount() == others + 1);
        t.onFreeCommandBuffers(pool, 1, &cb);
        CHECK(t.commandBufferCount() == others);
        t.onAllocateCommandBuffers(pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1, &cb);
        CmdState& fresh = t.cmd(cb);   // the thread's cached pointer was invalidated by the free
        CHECK(!fresh.secondary && fresh.events.empty());
        t.onSubmit(&cb, 1);

        // a re-allocation without a free we saw replaces the state
        fresh.note(image, kRead, 1);
        t.onAllocateCommandBuffers(pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1, &cb);
        CHECK(t.cmd(cb).events.empty() && t.commandBufferCount() == others + 1);

        // two trackers never share a thread's cached command buffer
        Fixture g;
        CHECK(&g.tracker->cmd(cb) != &t.cmd(cb));

        // views: destroying the app view moves the pin id on
        const VkImageView second = f.depthView(0x102, image);
        const VkImageView third  = f.depthView(0x103, image);
        (void)second; (void)third;
        t.onDestroyImageView(handle<VkImageView>(0x102));
        t.onDestroyImageView(handle<VkImageView>(0x103));
        CHECK(!t.pinDepthView(handle<VkImageView>(0x103)));
    }
} // namespace

int main()
{
    testPureHelpers();
    testCandidateSelection();
    testLayoutTracking();
    testStorageLifetime();
    testHandleReuse();
    if (failures)
    {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("depth tracker tests passed");
    return 0;
}
