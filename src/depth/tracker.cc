#include "depth/tracker.hh"

#include "format.hh"
#include "image_view.hh"
#include "logger.hh"
#include "memory.hh"

#include <algorithm>
#include <utility>

namespace VkHook
{
namespace
{
    std::atomic<uint64_t> g_nextTrackerId{1};

    // The last command buffer this thread looked up. Recording hooks of one
    // thread hit the same command buffer for thousands of draws; the cache turns
    // each of them into three compares. Valid only while the tracker's epoch is
    // unchanged (a CmdState was destroyed), so a freed and re-allocated handle
    // can never resolve to a stale state.
    struct CmdCache
    {
        uint64_t        tracker = 0;
        VkCommandBuffer buffer  = VK_NULL_HANDLE;
        uint64_t        epoch   = 0;
        CmdState*       state   = nullptr;
    };
    thread_local CmdCache tlsCmdCache;

    bool unsupportedStore(VkAttachmentStoreOp op)
    {
        return op == VK_ATTACHMENT_STORE_OP_DONT_CARE || op == VK_ATTACHMENT_STORE_OP_NONE;
    }

    bool overwritesOnLoad(VkAttachmentLoadOp op)
    {
        return op == VK_ATTACHMENT_LOAD_OP_CLEAR || op == VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }

    bool hasUsableLayout(VkImageLayout layout)
    {
        return layout != VK_IMAGE_LAYOUT_UNDEFINED && layout != VK_IMAGE_LAYOUT_PREINITIALIZED;
    }

    template <typename Attachment>
    bool forceStoreImpl(Attachment& attachment)
    {
        if (!isDepthFormat(attachment.format))
            return false;
        bool changed = false;
        if (unsupportedStore(attachment.storeOp))
        {
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed            = true;
        }
        if (isStencilFormat(attachment.format) && unsupportedStore(attachment.stencilStoreOp))
        {
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            changed                   = true;
        }
        return changed;
    }

    VkImageMemoryBarrier imageBarrier(VkImage image, VkImageAspectFlags aspects, VkImageLayout from, VkImageLayout to,
                                      VkAccessFlags srcAccess, VkAccessFlags dstAccess)
    {
        VkImageMemoryBarrier barrier = {};
        barrier.sType                = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask        = srcAccess;
        barrier.dstAccessMask        = dstAccess;
        barrier.oldLayout            = from;
        barrier.newLayout            = to;
        barrier.srcQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex  = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                = image;
        barrier.subresourceRange     = {aspects, 0, 1, 0, 1};
        return barrier;
    }
} // namespace

    // ---------------------------------------------------------------- pure helpers

    VkImageLayout depthReadOnlyLayout(VkFormat format, bool separateDepthStencilLayouts)
    {
        return (separateDepthStencilLayouts && !isStencilFormat(format)) ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                                                                         : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    }

    VkImageAspectFlags depthBarrierAspects(VkFormat format)
    {
        return isStencilFormat(format) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                       : VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    bool depthExtentMatchesWindow(const VkExtent3D& depth, const VkExtent2D& window)
    {
        if (window.width == 0 || window.height == 0)
            return false;
        if (depth.width == window.width && depth.height == window.height)
            return true;
        const int64_t dw = static_cast<int64_t>(depth.width) - static_cast<int64_t>(window.width);
        const int64_t dh = static_cast<int64_t>(depth.height) - static_cast<int64_t>(window.height);
        return dw >= -kDepthChromeTolerancePx && dw <= kDepthChromeTolerancePx && dh >= -kDepthChromeTolerancePx
               && dh <= kDepthChromeTolerancePx;
    }

    bool forceDepthAttachmentStoreOp(VkAttachmentDescription& attachment)  { return forceStoreImpl(attachment); }
    bool forceDepthAttachmentStoreOp(VkAttachmentDescription2& attachment) { return forceStoreImpl(attachment); }

    bool forceDepthAttachmentStoreOp(VkRenderingAttachmentInfo& attachment)
    {
        if (attachment.imageView == VK_NULL_HANDLE || !unsupportedStore(attachment.storeOp))
            return false;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        return true;
    }

    // ---------------------------------------------------------------- DepthLayerStorage

    DepthLayerStorage::~DepthLayerStorage()
    {
        if (!device)
            return;
        device->retire(VK_OBJECT_TYPE_IMAGE_VIEW, view);
        device->retire(VK_OBJECT_TYPE_IMAGE, image);
        device->retire(VK_OBJECT_TYPE_DEVICE_MEMORY, memory);
    }

    // ---------------------------------------------------------------- CmdState

    void CmdState::note(VkImage image, VkImageLayout layout, uint32_t draws)
    {
        if (!events.empty() && events.back().image == image)
        {
            events.back().layout = layout;
            events.back().draws += draws;
            return;
        }
        events.push_back({image, layout, draws});
    }

    void CmdState::beginScope(const DepthScope& s)
    {
        scope      = s;
        inScope    = true;
        scopeDraws = 0;
    }

    void CmdState::endScope()
    {
        if (!inScope)
            return;
        inScope = false;
        if (scope.image != VK_NULL_HANDLE)
        {
            if (scope.resolveImage != VK_NULL_HANDLE)
            {
                // the application resolved into a 1-sample image: that is the depth to use
                note(scope.image, scope.finalLayout, 0);
                note(scope.resolveImage, scope.resolveFinalLayout, scopeDraws);
            }
            else
            {
                note(scope.image, scope.finalLayout, scopeDraws);
            }
        }
        scope      = {};
        scopeDraws = 0;
    }

    void CmdState::absorb(const CmdState& secondaryState)
    {
        draw(secondaryState.looseDraws);
        for (const CmdEvent& event : secondaryState.events)
            note(event.image, event.layout, event.draws);
    }

    void CmdState::reset()
    {
        events.clear();  // keeps the capacity: re-recording allocates nothing
        scope         = {};
        inScope       = false;
        scopeDraws    = 0;
        looseDraws    = 0;
        storages.clear();
        storageSource = VK_NULL_HANDLE;
    }

    // ---------------------------------------------------------------- DepthTracker

    DepthTracker::DepthTracker(LogicalDevice* dev) : device(dev), id(g_nextTrackerId.fetch_add(1, std::memory_order_relaxed)) {}

    DepthTracker::~DepthTracker()
    {
        // The device is going away (vkDestroyDevice drained the GPU): the layer
        // views go through the RetireQueue like every other layer object.
        for (auto& [image, tracked] : images)
            if (tracked.layerView != VK_NULL_HANDLE)
                device->retire(VK_OBJECT_TYPE_IMAGE_VIEW, tracked.layerView);
    }

    // ---- object lifetime

    void DepthTracker::onCreateImage(VkImage image, const VkImageCreateInfo& original, VkImageUsageFlags usage)
    {
        if (!isDepthFormat(original.format) || !(usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
            return;

        VkImageView staleView = VK_NULL_HANDLE;
        {
            std::unique_lock lock(mutex);
            TrackedDepth& tracked = images[image];
            staleView             = tracked.layerView;  // a handle reused without a destroy we saw
            tracked               = TrackedDepth{};
            tracked.format        = original.format;
            tracked.extent        = original.extent;
            tracked.samples       = original.samples;
            tracked.usage         = usage;
            tracked.transient     = (original.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0;
            tracked.layout        = original.initialLayout;
            tracked.generation    = nextGeneration++;
        }
        if (staleView != VK_NULL_HANDLE)
            device->retire(VK_OBJECT_TYPE_IMAGE_VIEW, staleView);
    }

    DepthTracker::Destroyed DepthTracker::onDestroyImage(VkImage image)
    {
        Destroyed out;
        VkImageView layerView = VK_NULL_HANDLE;
        {
            std::unique_lock lock(mutex);
            auto it = images.find(image);
            if (it == images.end())
                return out;
            out.tracked    = true;
            layerView      = it->second.layerView;
            out.layerReads = layerView != VK_NULL_HANDLE;
            images.erase(it);

            for (auto& [key, selection] : selections)
            {
                if (selection.image == image)
                    selection = {};
                else if (selection.pending == image)
                    selection.pending = VK_NULL_HANDLE, selection.pendingFrames = 0;
            }
            if (pinned == image)
                pinned = VK_NULL_HANDLE;
            if (lastSelected == image)
                lastSelected = VK_NULL_HANDLE;
            if (storageFrameSource == image)
            {
                storageFrameSource = VK_NULL_HANDLE;
                storageFrame.reset();
            }
        }
        if (layerView != VK_NULL_HANDLE)
            device->retire(VK_OBJECT_TYPE_IMAGE_VIEW, layerView);
        return out;
    }

    void DepthTracker::onCreateImageView(VkImageView view, const VkImageViewCreateInfo& info)
    {
        if (!(info.subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT))
            return;
        std::unique_lock lock(mutex);
        auto it = images.find(info.image);
        if (it == images.end())
            return;
        views[view] = ImageRef{info.image, it->second.generation};
        if (it->second.appView == VK_NULL_HANDLE)
            it->second.appView = view;
    }

    void DepthTracker::onDestroyImageView(VkImageView view)
    {
        std::unique_lock lock(mutex);
        auto it = views.find(view);
        if (it == views.end())
            return;
        const ImageRef ref = it->second;
        views.erase(it);
        auto imageIt = images.find(ref.image);
        if (imageIt == images.end() || imageIt->second.generation != ref.generation || imageIt->second.appView != view)
            return;
        // the pin id moves to another live view of the same image, if any
        imageIt->second.appView = VK_NULL_HANDLE;
        for (const auto& [otherView, otherRef] : views)
        {
            if (otherRef.image == ref.image && otherRef.generation == ref.generation)
            {
                imageIt->second.appView = otherView;
                break;
            }
        }
    }

    namespace
    {
        template <typename Info, typename ResolveAttachmentOf>
        void scanRenderPass(const Info& info, ResolveAttachmentOf resolveOf, uint32_t& depthIndex, uint32_t& resolveIndex)
        {
            for (uint32_t s = 0; s < info.subpassCount; ++s)
            {
                const auto& subpass = info.pSubpasses[s];
                if (!subpass.pDepthStencilAttachment || subpass.pDepthStencilAttachment->attachment == VK_ATTACHMENT_UNUSED)
                    continue;
                depthIndex = subpass.pDepthStencilAttachment->attachment;
                resolveIndex = resolveOf(subpass);
                return;
            }
        }
    } // namespace

    void DepthTracker::onCreateRenderPass(VkRenderPass renderPass, const VkRenderPassCreateInfo& info)
    {
        RenderPassDepth rp;
        uint32_t        resolve = UINT32_MAX;
        scanRenderPass(
            info, [](const VkSubpassDescription&) { return UINT32_MAX; }, rp.depthIndex, resolve);
        if (rp.depthIndex >= info.attachmentCount || !isDepthFormat(info.pAttachments[rp.depthIndex].format))
            return;
        const VkAttachmentDescription& depth = info.pAttachments[rp.depthIndex];
        rp.initialLayout = depth.initialLayout;
        rp.finalLayout   = depth.finalLayout;
        rp.clearsOnLoad  = overwritesOnLoad(depth.loadOp);
        std::unique_lock lock(mutex);
        renderPasses[renderPass] = rp;
    }

    void DepthTracker::onCreateRenderPass(VkRenderPass renderPass, const VkRenderPassCreateInfo2& info)
    {
        RenderPassDepth rp;
        uint32_t        resolve = UINT32_MAX;
        scanRenderPass(
            info,
            [&](const VkSubpassDescription2& subpass) {
                for (const VkBaseInStructure* next = static_cast<const VkBaseInStructure*>(subpass.pNext); next;
                     next                          = next->pNext)
                {
                    if (next->sType != VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE)
                        continue;
                    const auto* r = reinterpret_cast<const VkSubpassDescriptionDepthStencilResolve*>(next);
                    if (r->depthResolveMode != VK_RESOLVE_MODE_NONE && r->pDepthStencilResolveAttachment
                        && r->pDepthStencilResolveAttachment->attachment != VK_ATTACHMENT_UNUSED)
                        return r->pDepthStencilResolveAttachment->attachment;
                }
                return UINT32_MAX;
            },
            rp.depthIndex, resolve);
        if (rp.depthIndex >= info.attachmentCount || !isDepthFormat(info.pAttachments[rp.depthIndex].format))
            return;
        const VkAttachmentDescription2& depth = info.pAttachments[rp.depthIndex];
        rp.initialLayout = depth.initialLayout;
        rp.finalLayout   = depth.finalLayout;
        rp.clearsOnLoad  = overwritesOnLoad(depth.loadOp);
        if (resolve < info.attachmentCount)
        {
            rp.resolveIndex       = resolve;
            rp.resolveFinalLayout = info.pAttachments[resolve].finalLayout;
        }
        std::unique_lock lock(mutex);
        renderPasses[renderPass] = rp;
    }

    void DepthTracker::onDestroyRenderPass(VkRenderPass renderPass)
    {
        std::unique_lock lock(mutex);
        renderPasses.erase(renderPass);
    }

    void DepthTracker::onCreateFramebuffer(VkFramebuffer framebuffer, const VkFramebufferCreateInfo& info)
    {
        if ((info.flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT) || !info.pAttachments)
            return;  // the views arrive with every vkCmdBeginRenderPass instead
        std::unique_lock lock(mutex);
        std::vector<ImageRef> refs(info.attachmentCount);
        bool any = false;
        for (uint32_t i = 0; i < info.attachmentCount; ++i)
        {
            refs[i] = refForViewLocked(info.pAttachments[i]);
            any |= refs[i].image != VK_NULL_HANDLE;
        }
        if (any)
            framebuffers[framebuffer] = std::move(refs);
    }

    void DepthTracker::onDestroyFramebuffer(VkFramebuffer framebuffer)
    {
        std::unique_lock lock(mutex);
        framebuffers.erase(framebuffer);
    }

    // ---- command buffers

    void DepthTracker::onAllocateCommandBuffers(VkCommandPool pool, VkCommandBufferLevel level, uint32_t count,
                                                const VkCommandBuffer* buffers)
    {
        std::vector<std::unique_ptr<CmdState>> replaced;
        {
            std::lock_guard<std::mutex> lock(cmdMutex);
            auto& list = pools[pool];
            for (uint32_t i = 0; i < count; ++i)
            {
                auto state       = std::make_unique<CmdState>();
                state->pool      = pool;
                state->secondary = level == VK_COMMAND_BUFFER_LEVEL_SECONDARY;
                auto& slot       = commandBuffers[buffers[i]];
                if (slot)
                    replaced.push_back(std::move(slot));  // a handle reused without a free we saw
                else
                    list.push_back(buffers[i]);
                slot = std::move(state);
            }
            if (!replaced.empty())
                epoch.fetch_add(1, std::memory_order_release);
        }
    }

    void DepthTracker::onFreeCommandBuffers(VkCommandPool pool, uint32_t count, const VkCommandBuffer* buffers)
    {
        std::vector<std::unique_ptr<CmdState>> dead;  // destroyed after the lock: may retire storage
        {
            std::lock_guard<std::mutex> lock(cmdMutex);
            auto poolIt = pools.find(pool);
            for (uint32_t i = 0; i < count; ++i)
            {
                auto it = commandBuffers.find(buffers[i]);
                if (it == commandBuffers.end())
                    continue;
                dead.push_back(std::move(it->second));
                commandBuffers.erase(it);
                if (poolIt != pools.end())
                {
                    auto& list = poolIt->second;
                    list.erase(std::remove(list.begin(), list.end(), buffers[i]), list.end());
                }
            }
            epoch.fetch_add(1, std::memory_order_release);
        }
    }

    void DepthTracker::onBeginCommandBuffer(VkCommandBuffer commandBuffer)
    {
        cmd(commandBuffer).reset();
    }

    void DepthTracker::onResetCommandBuffer(VkCommandBuffer commandBuffer)
    {
        cmd(commandBuffer).reset();
    }

    void DepthTracker::onResetCommandPool(VkCommandPool pool)
    {
        std::vector<std::shared_ptr<DepthLayerStorage>> released;
        {
            std::lock_guard<std::mutex> lock(cmdMutex);
            auto poolIt = pools.find(pool);
            if (poolIt == pools.end())
                return;
            for (VkCommandBuffer buffer : poolIt->second)
            {
                auto it = commandBuffers.find(buffer);
                if (it == commandBuffers.end())
                    continue;
                for (auto& held : it->second->storages)
                    released.push_back(std::move(held));
                it->second->reset();
            }
        }
    }

    void DepthTracker::onDestroyCommandPool(VkCommandPool pool)
    {
        std::vector<std::unique_ptr<CmdState>> dead;
        {
            std::lock_guard<std::mutex> lock(cmdMutex);
            auto poolIt = pools.find(pool);
            if (poolIt == pools.end())
                return;
            for (VkCommandBuffer buffer : poolIt->second)
            {
                auto it = commandBuffers.find(buffer);
                if (it == commandBuffers.end())
                    continue;
                dead.push_back(std::move(it->second));
                commandBuffers.erase(it);
            }
            pools.erase(poolIt);
            epoch.fetch_add(1, std::memory_order_release);
        }
    }

    CmdState& DepthTracker::cmd(VkCommandBuffer commandBuffer)
    {
        CmdCache& cache = tlsCmdCache;
        const uint64_t currentEpoch = epoch.load(std::memory_order_acquire);
        if (cache.tracker == id && cache.buffer == commandBuffer && cache.epoch == currentEpoch)
            return *cache.state;

        CmdState* state = nullptr;
        {
            std::lock_guard<std::mutex> lock(cmdMutex);
            auto& slot = commandBuffers[commandBuffer];
            if (!slot)
                slot = std::make_unique<CmdState>();  // allocated through a path the layer did not see
            state = slot.get();
        }
        cache = {id, commandBuffer, currentEpoch, state};
        return *state;
    }

    // ---- scopes

    DepthTracker::ImageRef DepthTracker::refForViewLocked(VkImageView view) const
    {
        auto it = views.find(view);
        return it == views.end() ? ImageRef{} : it->second;
    }

    const DepthTracker::TrackedDepth* DepthTracker::findLocked(const ImageRef& ref) const
    {
        if (ref.image == VK_NULL_HANDLE)
            return nullptr;
        auto it = images.find(ref.image);
        return it != images.end() && it->second.generation == ref.generation ? &it->second : nullptr;
    }

    DepthScope DepthTracker::scopeLocked(const RenderPassDepth& rp, const ImageRef& depth, const ImageRef& resolve) const
    {
        DepthScope scope;
        if (findLocked(depth))
        {
            scope.image         = depth.image;
            scope.initialLayout = rp.initialLayout;
            scope.finalLayout   = rp.finalLayout;
            scope.clearsOnLoad  = rp.clearsOnLoad;
        }
        if (rp.resolveIndex != UINT32_MAX && findLocked(resolve))
        {
            scope.resolveImage       = resolve.image;
            scope.resolveFinalLayout = rp.resolveFinalLayout;
        }
        return scope;
    }

    DepthScope DepthTracker::scopeFromRenderPassBegin(const VkRenderPassBeginInfo& begin)
    {
        std::shared_lock lock(mutex);
        auto rpIt = renderPasses.find(begin.renderPass);
        if (rpIt == renderPasses.end())
            return {};
        const RenderPassDepth& rp = rpIt->second;

        ImageRef depth, resolve;
        const VkRenderPassAttachmentBeginInfo* imageless = nullptr;
        for (const VkBaseInStructure* next = static_cast<const VkBaseInStructure*>(begin.pNext); next; next = next->pNext)
            if (next->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO)
                imageless = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(next);

        if (imageless)
        {
            if (rp.depthIndex < imageless->attachmentCount)
                depth = refForViewLocked(imageless->pAttachments[rp.depthIndex]);
            if (rp.resolveIndex < imageless->attachmentCount)
                resolve = refForViewLocked(imageless->pAttachments[rp.resolveIndex]);
        }
        else
        {
            auto fbIt = framebuffers.find(begin.framebuffer);
            if (fbIt == framebuffers.end())
                return {};
            if (rp.depthIndex < fbIt->second.size())
                depth = fbIt->second[rp.depthIndex];
            if (rp.resolveIndex < fbIt->second.size())
                resolve = fbIt->second[rp.resolveIndex];
        }
        return scopeLocked(rp, depth, resolve);
    }

    DepthScope DepthTracker::scopeFromRenderingInfo(const VkRenderingInfo& info)
    {
        const VkRenderingAttachmentInfo* attachment = info.pDepthAttachment;
        if (!attachment || attachment->imageView == VK_NULL_HANDLE)
            return {};

        std::shared_lock lock(mutex);
        DepthScope scope;
        const ImageRef depth = refForViewLocked(attachment->imageView);
        if (findLocked(depth))
        {
            scope.image         = depth.image;
            scope.initialLayout = attachment->imageLayout;  // the image is in this layout during the scope
            scope.finalLayout   = attachment->imageLayout;  // and stays in it afterwards
            scope.clearsOnLoad  = overwritesOnLoad(attachment->loadOp);
        }
        if (attachment->resolveMode != VK_RESOLVE_MODE_NONE && attachment->resolveImageView != VK_NULL_HANDLE)
        {
            const ImageRef resolve = refForViewLocked(attachment->resolveImageView);
            if (findLocked(resolve))
            {
                scope.resolveImage       = resolve.image;
                scope.resolveFinalLayout = attachment->resolveImageLayout;
            }
        }
        return scope;
    }

    bool DepthTracker::isSelectedLocked(VkImage image) const
    {
        if (pinned == image)
            return true;
        for (const auto& [key, selection] : selections)
            if (selection.image == image)
                return true;
        return false;
    }

    // ---- preserving copy

    bool DepthTracker::needsPreserve(const CmdState& state, VkImage image)
    {
        std::shared_lock lock(mutex);
        auto it = images.find(image);
        if (it == images.end() || !isSelectedLocked(image))
            return false;
        if (it->second.frameDraws > 0)
            return true;
        for (const CmdEvent& event : state.events)
            if (event.image == image && event.draws > 0)
                return true;
        return false;
    }

    std::shared_ptr<DepthLayerStorage> DepthTracker::makeStorage(VkFormat format, const VkExtent3D& extent,
                                                                 VkSampleCountFlagBits samples)
    {
        auto made     = std::make_shared<DepthLayerStorage>();
        made->device  = device;
        made->format  = format;
        made->extent  = extent;
        made->samples = samples;
        made->readLayout = depthReadOnlyLayout(format, device->separateDepthStencilLayouts);

        VkImageCreateInfo info = {};
        info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType     = VK_IMAGE_TYPE_2D;
        info.format        = format;
        info.extent        = {extent.width, extent.height, 1};
        info.mipLevels     = 1;
        info.arrayLayers   = 1;
        info.samples       = samples;
        info.tiling        = VK_IMAGE_TILING_OPTIMAL;
        info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // `made`'s destructor retires whatever exists when a step fails
        if (device->vkd.CreateImage(device->device, &info, nullptr, &made->image) != VK_SUCCESS)
        {
            made->image = VK_NULL_HANDLE;
            return nullptr;
        }
        VkMemoryRequirements requirements = {};
        device->vkd.GetImageMemoryRequirements(device->device, made->image, &requirements);
        const uint32_t type = findMemoryTypeIndex(device, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type == kInvalidMemoryTypeIndex)
            return nullptr;
        VkMemoryAllocateInfo allocation = {};
        allocation.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize  = requirements.size;
        allocation.memoryTypeIndex = type;
        if (device->vkd.AllocateMemory(device->device, &allocation, nullptr, &made->memory) != VK_SUCCESS)
        {
            made->memory = VK_NULL_HANDLE;
            return nullptr;
        }
        if (device->vkd.BindImageMemory(device->device, made->image, made->memory, 0) != VK_SUCCESS)
            return nullptr;
        made->view = createSingleImageView(device, format, made->image, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (made->view == VK_NULL_HANDLE)
            return nullptr;
        return made;
    }

    std::shared_ptr<DepthLayerStorage> DepthTracker::ensureStorage(VkFormat format, const VkExtent3D& extent,
                                                                   VkSampleCountFlagBits samples)
    {
        const auto matches = [&](const std::shared_ptr<DepthLayerStorage>& s) {
            return s && s->format == format && s->extent.width == extent.width && s->extent.height == extent.height
                   && s->samples == samples;
        };
        {
            std::shared_lock lock(mutex);
            if (matches(storage))
                return storage;
        }

        // the driver calls happen with no lock held, on the recording thread
        std::shared_ptr<DepthLayerStorage> made = makeStorage(format, extent, samples);
        if (!made)
            return nullptr;
        std::unique_lock lock(mutex);
        if (matches(storage))
            return storage;  // another thread was first; `made` retires itself
        storage = made;
        return made;
    }

    bool DepthTracker::recordPreserveCopy(CmdState& state, VkCommandBuffer commandBuffer, VkImage image, VkImageLayout layout)
    {
        if (!hasUsableLayout(layout))
            return false;
        VkFormat              format  = VK_FORMAT_UNDEFINED;
        VkExtent3D            extent  = {};
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        {
            std::shared_lock lock(mutex);
            auto it = images.find(image);
            if (it == images.end())
                return false;
            format  = it->second.format;
            extent  = it->second.extent;
            samples = it->second.samples;
        }

        std::shared_ptr<DepthLayerStorage> target = ensureStorage(format, extent, samples);
        if (!target)
            return false;

        const VkImageAspectFlags aspects = depthBarrierAspects(format);
        const auto& vkd = device->vkd;

        // before: the source leaves its layout for TRANSFER_SRC; the storage is
        // overwritten whole, so it starts UNDEFINED. ALL_COMMANDS on the source
        // side also orders this after the previous frame's capture read of it.
        VkImageMemoryBarrier before[2] = {
            imageBarrier(image, aspects, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
            imageBarrier(target->image, aspects, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         0, VK_ACCESS_TRANSFER_WRITE_BIT),
        };
        vkd.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                               nullptr, 0, nullptr, 2, before);

        VkImageCopy region = {};
        region.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        region.extent         = {extent.width, extent.height, 1};
        vkd.CmdCopyImage(commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        // after: the source returns to the layout the application expects, the
        // storage becomes sampleable
        VkImageMemoryBarrier after[2] = {
            imageBarrier(image, aspects, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout, 0,
                         VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
            imageBarrier(target->image, aspects, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, target->readLayout,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
        };
        vkd.CmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                               nullptr, 0, nullptr, 2, after);

        const auto bound = std::find(state.storages.begin(), state.storages.end(), target);
        if (bound != state.storages.end())
            state.storages.erase(bound);
        state.storages.push_back(std::move(target));  // newest last
        state.storageSource = image;
        return true;
    }

    // ---- submit

    void DepthTracker::onSubmit(const VkCommandBuffer* buffers, uint32_t count)
    {
        if (count == 0 || !buffers)
            return;
        std::lock_guard<std::mutex> commandLock(cmdMutex);
        std::unique_lock<std::shared_mutex> lock(mutex, std::defer_lock);
        for (uint32_t i = 0; i < count; ++i)
        {
            auto it = commandBuffers.find(buffers[i]);
            if (it == commandBuffers.end())
                continue;
            const CmdState& state = *it->second;
            if (state.events.empty() && state.storages.empty())
                continue;
            if (!lock.owns_lock())
                lock.lock();
            for (const CmdEvent& event : state.events)
            {
                auto imageIt = images.find(event.image);
                if (imageIt == images.end())
                    continue;
                imageIt->second.layout = event.layout;
                imageIt->second.frameDraws += event.draws;
            }
            if (!state.storages.empty() && state.storageSource != VK_NULL_HANDLE)
            {
                storageFrame       = state.storages.back();
                storageFrameSource = state.storageSource;
            }
        }
    }

    // ---- present

    DepthSource DepthTracker::selectForPresent(uint64_t key, const VkExtent2D& window)
    {
        DepthSource out;
        VkImage     chosen   = VK_NULL_HANDLE;
        TrackedDepth snapshot;
        std::shared_ptr<DepthLayerStorage> frameStorage;
        uint64_t generation = 0;
        {
            std::unique_lock lock(mutex);
            if (pinned != VK_NULL_HANDLE && images.count(pinned))
            {
                chosen = pinned;
            }
            else
            {
                VkImage  best      = VK_NULL_HANDLE;
                uint32_t bestDraws = 0;
                bool     bestExact = false;
                for (const auto& [image, tracked] : images)
                {
                    if (tracked.frameDraws == 0 || !depthExtentMatchesWindow(tracked.extent, window))
                        continue;
                    const bool exact = tracked.extent.width == window.width && tracked.extent.height == window.height;
                    if (tracked.frameDraws > bestDraws || (tracked.frameDraws == bestDraws && exact && !bestExact))
                    {
                        best      = image;
                        bestDraws = tracked.frameDraws;
                        bestExact = exact;
                    }
                }

                Selection& selection = selections[key];
                auto currentIt = selection.image != VK_NULL_HANDLE ? images.find(selection.image) : images.end();
                if (currentIt != images.end() && depthExtentMatchesWindow(currentIt->second.extent, window))
                {
                    // the incumbent stays unless a rival out-draws it for several frames in a row
                    if (best != VK_NULL_HANDLE && best != selection.image && bestDraws > currentIt->second.frameDraws)
                    {
                        selection.pendingFrames = selection.pending == best ? selection.pendingFrames + 1 : 1;
                        selection.pending       = best;
                        if (selection.pendingFrames >= kDepthSwitchFrames)
                        {
                            selection.image         = best;
                            selection.pending       = VK_NULL_HANDLE;
                            selection.pendingFrames = 0;
                        }
                    }
                    else
                    {
                        selection.pending       = VK_NULL_HANDLE;
                        selection.pendingFrames = 0;
                    }
                }
                else
                {
                    selection = {};
                    selection.image = best;
                }
                chosen = selection.image;
            }

            // close the frame for every image of this window's size
            for (auto& [image, tracked] : images)
            {
                if (!depthExtentMatchesWindow(tracked.extent, window))
                    continue;
                tracked.lastDraws  = tracked.frameDraws;
                tracked.frameDraws = 0;
            }

            lastSelected = chosen;
            frameStorage = std::move(storageFrame);
            const VkImage storageSource = storageFrameSource;
            storageFrame.reset();
            storageFrameSource = VK_NULL_HANDLE;

            if (chosen == VK_NULL_HANDLE)
                return out;
            auto it = images.find(chosen);
            if (it == images.end())
                return out;
            snapshot   = it->second;
            generation = it->second.generation;

            if (frameStorage && storageSource == chosen)
            {
                // the frame's depth was copied before the application overwrote it
                out.valid       = true;
                out.fromStorage = true;
                out.image       = frameStorage->image;
                out.view        = frameStorage->view;
                out.format      = frameStorage->format;
                out.extent      = frameStorage->extent;
                out.samples     = frameStorage->samples;
                out.layout      = frameStorage->readLayout;
                out.storage     = std::move(frameStorage);
                return out;
            }
        }

        if (!hasUsableLayout(snapshot.layout) || !(snapshot.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
            return out;

        VkImageView view = snapshot.layerView;
        if (view == VK_NULL_HANDLE)
        {
            // no lock held across the driver call; a concurrent creator's view wins
            const VkImageView created = createSingleImageView(device, snapshot.format, chosen, VK_IMAGE_VIEW_TYPE_2D,
                                                              VK_IMAGE_ASPECT_DEPTH_BIT);
            if (created == VK_NULL_HANDLE)
                return out;
            std::unique_lock lock(mutex);
            auto it = images.find(chosen);
            if (it == images.end() || it->second.generation != generation)
            {
                device->vkd.DestroyImageView(device->device, created, nullptr);  // never used by the GPU
                return out;
            }
            if (it->second.layerView == VK_NULL_HANDLE)
                it->second.layerView = created;
            else
                device->vkd.DestroyImageView(device->device, created, nullptr);
            view = it->second.layerView;
        }

        out.valid   = true;
        out.image   = chosen;
        out.view    = view;
        out.format  = snapshot.format;
        out.extent  = snapshot.extent;
        out.samples = snapshot.samples;
        out.layout  = snapshot.layout;
        return out;
    }

    void DepthTracker::forgetSelection(uint64_t key)
    {
        std::unique_lock lock(mutex);
        selections.erase(key);
    }

    // ---- control plane

    bool DepthTracker::pinDepthView(VkImageView view)
    {
        std::unique_lock lock(mutex);
        const ImageRef ref = refForViewLocked(view);
        if (!findLocked(ref))
            return false;
        pinned = ref.image;
        return true;
    }

    void DepthTracker::clearDepthPin()
    {
        std::unique_lock lock(mutex);
        pinned = VK_NULL_HANDLE;
    }

    void DepthTracker::refreshDetection()
    {
        std::unique_lock lock(mutex);
        selections.clear();
        pinned       = VK_NULL_HANDLE;
        lastSelected = VK_NULL_HANDLE;
        for (auto& [image, tracked] : images)
            tracked.frameDraws = tracked.lastDraws = 0;
        storageFrame.reset();
        storageFrameSource = VK_NULL_HANDLE;
    }

    DepthReport DepthTracker::report()
    {
        DepthReport out;
        std::shared_lock lock(mutex);
        const auto entryOf = [](const TrackedDepth& t) {
            DepthReport::Entry e;
            e.appView   = t.appView;
            e.format    = t.format;
            e.extent    = t.extent;
            e.samples   = t.samples;
            e.layout    = t.layout;
            e.transient = t.transient;
            e.drawCount = std::max(t.lastDraws, t.frameDraws);
            return e;
        };
        for (const auto& [image, tracked] : images)
            out.candidates.push_back(entryOf(tracked));
        std::sort(out.candidates.begin(), out.candidates.end(),
                  [](const DepthReport::Entry& a, const DepthReport::Entry& b) { return a.drawCount > b.drawCount; });

        const VkImage active = pinned != VK_NULL_HANDLE ? pinned : lastSelected;
        auto it = images.find(active);
        if (it != images.end())
        {
            out.active    = entryOf(it->second);
            out.hasActive = true;
        }
        if (pinned != VK_NULL_HANDLE && it != images.end())
        {
            out.pinned     = true;
            out.pinnedView = it->second.appView;
        }
        return out;
    }

    bool DepthTracker::tracked(VkImage image)
    {
        std::shared_lock lock(mutex);
        return images.count(image) != 0;
    }

    VkImageLayout DepthTracker::trackedLayout(VkImage image)
    {
        std::shared_lock lock(mutex);
        auto it = images.find(image);
        return it == images.end() ? VK_IMAGE_LAYOUT_UNDEFINED : it->second.layout;
    }

    size_t DepthTracker::commandBufferCount()
    {
        std::lock_guard<std::mutex> lock(cmdMutex);
        return commandBuffers.size();
    }

    bool DepthTracker::storageBound(const std::shared_ptr<DepthLayerStorage>& target)
    {
        std::lock_guard<std::mutex> lock(cmdMutex);
        for (const auto& [buffer, state] : commandBuffers)
            if (std::find(state->storages.begin(), state->storages.end(), target) != state->storages.end())
                return true;
        return false;
    }
} // namespace VkHook
