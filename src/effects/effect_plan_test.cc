// Headless tests for the pure planning pieces of the effect runtime and for the
// Vulkan-facing helpers that can be driven through a fake dispatch table:
//  - the per-image uniform ring (layout, isolation of slices, lazy defaults);
//  - the descriptor pool budget;
//  - the decision to skip the redundant forwarding copy after an effect;
//  - the shared render pass description;
//  - the batched texture upload (one staging buffer, one submission).

#include "descriptor_budget.hh"
#include "effect_renderpass.hh"
#include "output_forward.hh"
#include "texture_upload.hh"
#include "uniform_ring.hh"
#include "gpu/retire_device.hh"

#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace VkHook
{
    // LogicalDevice owns these; headless tests never construct them.
    struct OverlayPersistentState {};
    class ImGuiOverlay {};
} // namespace VkHook

using namespace VkHook;

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

    // ---- uniform ring ----

    void test_ring_layout()
    {
        const UniformRing ring(3, 100);
        expect(ring.slotCount() == 3, "one slot per swapchain image");
        expect(ring.stride() == 256, "a 100-byte slice is padded to the offset alignment");
        expect(ring.totalBytes() == 768, "the buffer spans every slice");
        expect(ring.offset(0) == 0 && ring.offset(1) == 256 && ring.offset(2) == 512, "slices are stride apart");
        for (uint32_t slot = 0; slot < 3; ++slot)
            expect(ring.offset(slot) % UniformRing::kOffsetAlignment == 0, "every slice offset is bindable");
        expect(ring.contains(2) && !ring.contains(3), "contains bounds the image index");

        const UniformRing exact(2, 512);
        expect(exact.stride() == 512 && exact.totalBytes() == 1024, "an aligned size is not padded further");
        const UniformRing big(2, 513);
        expect(big.stride() == 768, "one byte over rounds up to the next alignment step");

        const UniformRing empty(4, 0);
        expect(empty.slotCount() == 0 && empty.totalBytes() == 0, "no uniforms means no slots");
        expect(!empty.contains(0), "an empty ring contains no image");
    }

    void test_ring_slices_do_not_alias()
    {
        // A frame in flight on image 1 must not see the CPU rewrite for image 0:
        // write distinct patterns through each slice and check the others.
        const UniformRing   ring(3, 100);
        std::vector<uint8_t> memory(ring.totalBytes(), 0);
        for (uint32_t slot = 0; slot < 3; ++slot)
            std::memset(ring.slice(memory.data(), slot), 0x10 * (slot + 1), ring.slotBytes());

        std::memset(ring.slice(memory.data(), 0), 0xEE, ring.slotBytes()); // image 0's next frame
        bool othersIntact = true;
        for (uint32_t slot = 1; slot < 3; ++slot)
        {
            const auto* p = static_cast<const uint8_t*>(ring.slice(memory.data(), slot));
            for (uint32_t i = 0; i < ring.slotBytes(); ++i)
                othersIntact &= p[i] == 0x10 * (slot + 1);
        }
        expect(othersIntact, "rewriting image 0's slice leaves images 1 and 2 untouched");
        expect(ring.slice(memory.data(), 1) == memory.data() + 256, "slice pointers follow the offsets");
    }

    void test_ring_defaults_are_per_slice_and_lazy()
    {
        UniformRing ring(2, 64);
        expect(ring.needsDefaults(0, 7) && ring.needsDefaults(1, 7), "a fresh slice needs the defaults");
        ring.markDefaults(0, 7);
        expect(!ring.needsDefaults(0, 7), "a refreshed slice does not need them again");
        expect(ring.needsDefaults(1, 7), "another slice still does: it may have been in flight");
        expect(ring.needsDefaults(0, 8), "a new bindings generation invalidates the slice");
        ring.markDefaults(1, 8);
        expect(!ring.needsDefaults(1, 8), "slice 1 catches up on its own update");
        expect(ring.needsDefaults(0, 8), "slice 0 is refreshed only by its own update");
    }

    // ---- descriptor budget ----

    void test_budget_sizes_from_real_needs()
    {
        // 3 images, 2 sampled + 1 storage binding, input set only, with uniforms.
        const DescriptorBudget b = computeDescriptorBudget(3, 2, 1, samplerSetGroups(0), true);
        expect(b.maxSets == 6, "3 image/sampler sets + 3 uniform sets");
        expect(b.sampledImages == 6, "3 sets x 2 sampled bindings");
        expect(b.storageImages == 3, "3 sets x 1 storage binding");
        expect(b.uniformBuffers == 3, "one uniform buffer descriptor per image");

        const auto sizes = b.poolSizes();
        uint32_t total = 0;
        for (const auto& s : sizes)
            total += s.descriptorCount;
        expect(sizes.size() == 3 && total == 12, "pool sizes sum to the descriptors the sets hold");
    }

    void test_budget_follows_output_writes()
    {
        expect(samplerSetGroups(0) == 1 && samplerSetGroups(1) == 1, "up to one output write needs only the input set");
        expect(samplerSetGroups(2) == 2, "two output writes add the back buffer set");
        expect(samplerSetGroups(3) == 3 && samplerSetGroups(9) == 3, "three or more add the output set, no further");

        const DescriptorBudget b = computeDescriptorBudget(4, 1, 0, samplerSetGroups(3), false);
        expect(b.maxSets == 12, "4 images x 3 groups, no uniform sets");
        expect(b.uniformBuffers == 0, "no uniforms, no uniform descriptors");
    }

    void test_budget_has_no_empty_pool_sizes()
    {
        const DescriptorBudget b = computeDescriptorBudget(3, 2, 0, 1, false);
        const auto sizes = b.poolSizes();
        bool anyEmpty = false;
        for (const auto& s : sizes)
            anyEmpty |= s.descriptorCount == 0;
        expect(sizes.size() == 1 && !anyEmpty, "a descriptor type nothing uses is left out (Vulkan forbids zero counts)");
        expect(sizes[0].type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && sizes[0].descriptorCount == 6, "sampled pool size");
    }

    // ---- redundant forwarding copy ----

    reshadefx::module makeModule(std::vector<reshadefx::pass_info> passes, std::vector<reshadefx::texture_info> textures = {})
    {
        reshadefx::module m;
        reshadefx::technique_info t;
        t.passes = std::move(passes);
        m.techniques.push_back(std::move(t));
        m.textures = std::move(textures);
        return m;
    }

    reshadefx::pass_info graphicsPass(const char* target)
    {
        reshadefx::pass_info p;
        p.vs_entry_point = "VS";
        p.ps_entry_point = "PS";
        p.render_target_names[0] = target;
        return p;
    }

    reshadefx::texture_info texture(const char* name, const char* semantic)
    {
        reshadefx::texture_info t;
        t.unique_name = name;
        t.semantic = semantic;
        return t;
    }

    void test_forward_skipped_when_output_already_holds_the_result()
    {
        expect(!reshadeOutputNeedsForward(makeModule({}), 0), "an effect with no passes leaves its seed copy as the output");
        expect(!reshadeOutputNeedsForward(makeModule({graphicsPass("texLut")}, {texture("texLut", "")}), 0),
               "graphics passes into private textures leave the seed copy as the output");
        expect(!reshadeOutputNeedsForward(makeModule({graphicsPass("")}), 1), "back buffer writers never need forwarding");
        expect(!reshadeOutputNeedsForward(reshadefx::module{}, 0), "a module without techniques is not forwarded");
    }

    void test_forward_kept_when_the_input_is_modified()
    {
        reshadefx::pass_info compute;
        compute.cs_entry_point = "CS";
        expect(reshadeOutputNeedsForward(makeModule({compute}), 0), "a compute pass can write the input in place");
        expect(reshadeOutputNeedsForward(makeModule({graphicsPass("texLut"), compute}, {texture("texLut", "")}), 0),
               "one compute pass among graphics passes is enough");
        expect(reshadeOutputNeedsForward(makeModule({graphicsPass("texBack")}, {texture("texBack", "COLOR")}), 0),
               "rendering into a COLOR-semantic texture writes the input image");
        expect(reshadeOutputNeedsForward(makeModule({graphicsPass("texDepth")}, {texture("texDepth", "DEPTH")}), 0),
               "rendering into a DEPTH-semantic texture writes the input image");
        reshadefx::pass_info multi = graphicsPass("texLut");
        multi.render_target_names[3] = "texBack";
        expect(reshadeOutputNeedsForward(makeModule({multi}, {texture("texLut", ""), texture("texBack", "COLOR")}), 0),
               "any of the eight targets can alias the input");
    }

    // ---- render pass description ----

    void test_render_pass_references()
    {
        EffectRenderPassDesc desc;
        desc.attachments = {effectColorAttachment(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_LOAD),
                            effectColorAttachment(VK_FORMAT_R16_SFLOAT, VK_ATTACHMENT_LOAD_OP_CLEAR),
                            effectStencilAttachment(VK_FORMAT_D24_UNORM_S8_UINT, true)};
        desc.hasDepthStencil = true;
        const EffectRenderPassInfo info(desc);
        const auto&                ci = info.createInfo();
        expect(ci.attachmentCount == 3 && ci.subpassCount == 1 && ci.dependencyCount == 2, "three attachments, one subpass, two dependencies");
        expect(info.subpass().colorAttachmentCount == 2, "the depth/stencil attachment is not a colour attachment");
        expect(info.subpass().pDepthStencilAttachment && info.subpass().pDepthStencilAttachment->attachment == 2 &&
                   info.subpass().pDepthStencilAttachment->layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
               "depth/stencil is referenced after the colour attachments");
        expect(info.subpass().pColorAttachments[1].attachment == 1 &&
                   info.subpass().pColorAttachments[1].layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               "colour attachments are referenced in order");
        expect(ci.pAttachments[0].initialLayout == VK_IMAGE_LAYOUT_GENERAL && ci.pAttachments[0].finalLayout == VK_IMAGE_LAYOUT_GENERAL,
               "effect images stay in GENERAL across the pass");
        expect(ci.pAttachments[1].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR, "the load op is the caller's");
        expect(ci.pAttachments[2].stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR, "the first stencil access clears");
        expect(ci.pDependencies[0].srcSubpass == VK_SUBPASS_EXTERNAL && ci.pDependencies[1].dstSubpass == VK_SUBPASS_EXTERNAL,
               "dependencies bracket the pass");

        EffectRenderPassDesc colorOnly;
        colorOnly.attachments = {effectColorAttachment(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_LOAD)};
        const EffectRenderPassInfo plain(colorOnly);
        expect(plain.subpass().colorAttachmentCount == 1 && plain.subpass().pDepthStencilAttachment == nullptr,
               "no depth/stencil attachment, no depth reference");
        expect(effectStencilAttachment(VK_FORMAT_D24_UNORM_S8_UINT, false).stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD,
               "later stencil accesses load");
    }

    // ---- texture upload ----

    template <class T> T handle(uintptr_t value)
    {
        if constexpr (std::is_pointer_v<T>)
            return reinterpret_cast<T>(value);
        else
            return static_cast<T>(value);
    }

    struct FakeDispatchable
    {
        void* loaderData = nullptr;
    };

    struct Recorder
    {
        std::vector<VkBufferImageCopy>  copies;
        std::vector<VkImage>            cleared;
        int                             blits = 0;
        int                             buffersCreated = 0;
        int                             commandBuffersAllocated = 0;
        int                             buffersDestroyed = 0;
        int                             memoryFreed = 0;
        int                             poolsDestroyed = 0;
        int                             begins = 0;
        int                             ends = 0;
        int                             submits = 0;
        VkDeviceSize                    bufferSize = 0;
        std::vector<uint8_t>            hostMemory;
        std::vector<VkImageMemoryBarrier> barriers;
    };
    Recorder* g_rec = nullptr;

    VKAPI_ATTR VkResult VKAPI_CALL createBuffer(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*, VkBuffer* out)
    {
        ++g_rec->buffersCreated;
        g_rec->bufferSize = info->size;
        *out = handle<VkBuffer>(0x1000 + g_rec->buffersCreated);
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL getBufferRequirements(VkDevice, VkBuffer, VkMemoryRequirements* out)
    {
        *out = {};
        out->size = g_rec->bufferSize;
        out->alignment = 16;
        out->memoryTypeBits = 1;
    }
    VKAPI_ATTR VkResult VKAPI_CALL allocateMemory(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* out)
    {
        g_rec->hostMemory.assign(static_cast<size_t>(info->allocationSize), 0);
        *out = handle<VkDeviceMemory>(0x2000);
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL bindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return VK_SUCCESS; }
    VKAPI_ATTR VkResult VKAPI_CALL mapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void** out)
    {
        *out = g_rec->hostMemory.data();
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL unmapMemory(VkDevice, VkDeviceMemory) {}
    VKAPI_ATTR void VKAPI_CALL destroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) { ++g_rec->buffersDestroyed; }
    VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) { ++g_rec->memoryFreed; }
    VKAPI_ATTR VkResult VKAPI_CALL allocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* out)
    {
        ++g_rec->commandBuffersAllocated;
        static FakeDispatchable object;
        *out = reinterpret_cast<VkCommandBuffer>(&object);
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL beginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*)
    {
        ++g_rec->begins;
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL endCommandBuffer(VkCommandBuffer)
    {
        ++g_rec->ends;
        return VK_SUCCESS;
    }
    VKAPI_ATTR VkResult VKAPI_CALL createCommandPool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool* out)
    {
        *out = handle<VkCommandPool>(0x77);
        return VK_SUCCESS;
    }
    VKAPI_ATTR void VKAPI_CALL destroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) { ++g_rec->poolsDestroyed; }
    VKAPI_ATTR void VKAPI_CALL cmdBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t,
                                          const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*, uint32_t imageCount,
                                          const VkImageMemoryBarrier* images)
    {
        for (uint32_t i = 0; i < imageCount; ++i)
            g_rec->barriers.push_back(images[i]);
    }
    VKAPI_ATTR void VKAPI_CALL cmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout, uint32_t count, const VkBufferImageCopy* regions)
    {
        for (uint32_t i = 0; i < count; ++i)
            g_rec->copies.push_back(regions[i]);
    }
    VKAPI_ATTR void VKAPI_CALL cmdClear(VkCommandBuffer, VkImage image, VkImageLayout, const VkClearColorValue*, uint32_t, const VkImageSubresourceRange*)
    {
        g_rec->cleared.push_back(image);
    }
    VKAPI_ATTR void VKAPI_CALL cmdBlit(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageBlit*, VkFilter)
    {
        ++g_rec->blits;
    }
    VKAPI_ATTR void VKAPI_CALL memoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* out)
    {
        *out = {};
        out->memoryTypeCount = 1;
        out->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

    struct Fixture
    {
        Recorder         recorder;
        LogicalDevice    device{};
        FakeDispatchable deviceObject;

        Fixture()
        {
            g_rec = &recorder;
            bindRetireDestroyer(device);
            device.device = reinterpret_cast<VkDevice>(&deviceObject);
            device.physicalDevice = handle<VkPhysicalDevice>(0xBEEF0001);
            device.vkd.CreateBuffer = createBuffer;
            device.vkd.GetBufferMemoryRequirements = getBufferRequirements;
            device.vkd.AllocateMemory = allocateMemory;
            device.vkd.BindBufferMemory = bindBufferMemory;
            device.vkd.MapMemory = mapMemory;
            device.vkd.UnmapMemory = unmapMemory;
            device.vkd.DestroyBuffer = destroyBuffer;
            device.vkd.FreeMemory = freeMemory;
            device.vkd.AllocateCommandBuffers = allocateCommandBuffers;
            device.vkd.BeginCommandBuffer = beginCommandBuffer;
            device.vkd.EndCommandBuffer = endCommandBuffer;
            device.vkd.CreateCommandPool = createCommandPool;
            device.vkd.DestroyCommandPool = destroyCommandPool;
            device.vkd.CmdPipelineBarrier = cmdBarrier;
            device.vkd.CmdCopyBufferToImage = cmdCopyBufferToImage;
            device.vkd.CmdClearColorImage = cmdClear;
            device.vkd.CmdBlitImage = cmdBlit;
            device.vki.GetPhysicalDeviceMemoryProperties = memoryProperties;
        }
    };

    std::vector<uint8_t> pixels(size_t count, uint8_t value) { return std::vector<uint8_t>(count, value); }

    void test_upload_staging_layout()
    {
        TextureUploadBatch batch;
        batch.addUpload(handle<VkImage>(1), {4, 4, 1}, 1, pixels(10, 1));
        batch.addClear(handle<VkImage>(2), 1);
        batch.addUpload(handle<VkImage>(3), {2, 2, 1}, 1, pixels(100, 3));
        expect(batch.size() == 3 && !batch.empty(), "every texture is one item");
        expect(batch.stagingOffset(0) == 0, "the first upload starts the buffer");
        expect(batch.stagingOffset(2) >= 10 && batch.stagingOffset(2) % TextureUploadBatch::kStagingAlignment == 0,
               "later uploads follow the previous one at an aligned offset");
        expect(batch.stagingBytes() == batch.stagingOffset(2) + 100, "the buffer ends at the last upload");

        std::vector<uint8_t> staging(batch.stagingBytes(), 0xAA);
        batch.fill(staging.data());
        expect(staging[0] == 1 && staging[9] == 1 && staging[batch.stagingOffset(2)] == 3 && staging[batch.stagingOffset(2) + 99] == 3,
               "pixels land at their offsets");
        expect(staging[10] == 0xAA, "padding between uploads is left alone");
    }

    void test_upload_is_one_submission()
    {
        Fixture f;
        TextureUploadBatch batch;
        batch.addUpload(handle<VkImage>(1), {64, 64, 1}, 3, pixels(64 * 64 * 4, 7));
        batch.addUpload(handle<VkImage>(3), {8, 8, 1}, 1, pixels(8 * 8 * 4, 9));
        batch.addClear(handle<VkImage>(2), 2);
        batch.addClear(handle<VkImage>(4), 1);

        int      submissions = 0;
        VkCommandBuffer submitted = VK_NULL_HANDLE;
        const bool ok = batch.submit(&f.device, [&](VkCommandBuffer commands) {
            ++submissions;
            submitted = commands;
            // Everything is already recorded and the staging buffer still exists.
            expect(f.recorder.ends == 1, "the command buffer is ended before it is submitted");
            expect(f.recorder.buffersDestroyed == 0, "the staging buffer lives until the GPU is done");
            return true;
        });

        expect(ok, "a completed batch reports success");
        expect(submissions == 1 && submitted != VK_NULL_HANDLE, "four textures cost exactly one submission");
        expect(f.recorder.buffersCreated == 1, "one staging buffer for all textures");
        expect(f.recorder.commandBuffersAllocated == 1 && f.recorder.begins == 1, "one command buffer for all textures");
        expect(f.recorder.copies.size() == 2, "each upload is one buffer-to-image copy");
        expect(f.recorder.copies[0].bufferOffset == batch.stagingOffset(0) && f.recorder.copies[1].bufferOffset == batch.stagingOffset(1),
               "copies read from the planned offsets");
        expect(f.recorder.cleared.size() == 2, "each unusable texture is cleared, not left undefined");
        expect(f.recorder.blits == 2, "mips are generated for the mipmapped upload only (3 levels = 2 blits)");
        expect(f.recorder.buffersDestroyed == 1 && f.recorder.memoryFreed == 1 && f.recorder.poolsDestroyed == 1,
               "staging objects are released once the wait returned");
        expect(f.recorder.hostMemory[batch.stagingOffset(0)] == 7 && f.recorder.hostMemory[batch.stagingOffset(1)] == 9,
               "the staging memory holds the pixels");

        bool toGeneral = false;
        for (const auto& b : f.recorder.barriers)
            toGeneral |= b.image == handle<VkImage>(2) && b.newLayout == VK_IMAGE_LAYOUT_GENERAL;
        bool undefinedFirst = !f.recorder.barriers.empty() && f.recorder.barriers[0].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED;
        expect(toGeneral, "textures end in GENERAL");
        expect(undefinedFirst, "a texture that is overwritten starts from UNDEFINED");
    }

    void test_failed_wait_defers_release()
    {
        Fixture f;
        TextureUploadBatch batch;
        batch.addUpload(handle<VkImage>(1), {4, 4, 1}, 1, pixels(64, 1));
        const bool ok = batch.submit(&f.device, [](VkCommandBuffer) { return false; });
        expect(!ok, "a batch the GPU did not finish reports failure");
        expect(f.recorder.buffersDestroyed == 0 && f.recorder.poolsDestroyed == 0,
               "objects the GPU may still read are not released inline");
        expect(f.device.retireQueue.pending() == 3, "they are retired instead");
        f.device.retireQueue.drainAll();
        expect(f.recorder.buffersDestroyed == 1 && f.recorder.memoryFreed == 1 && f.recorder.poolsDestroyed == 1,
               "and released when the retire queue drains");
    }

    void test_empty_batch_touches_nothing()
    {
        Fixture f;
        TextureUploadBatch batch;
        bool called = false;
        expect(batch.submit(&f.device, [&](VkCommandBuffer) { called = true; return true; }), "an empty batch succeeds");
        expect(!called && f.recorder.buffersCreated == 0 && f.recorder.commandBuffersAllocated == 0,
               "an empty batch creates no buffer, records nothing and submits nothing");
    }
} // namespace

int main()
{
    test_ring_layout();
    test_ring_slices_do_not_alias();
    test_ring_defaults_are_per_slice_and_lazy();
    test_budget_sizes_from_real_needs();
    test_budget_follows_output_writes();
    test_budget_has_no_empty_pool_sizes();
    test_forward_skipped_when_output_already_holds_the_result();
    test_forward_kept_when_the_input_is_modified();
    test_render_pass_references();
    test_upload_staging_layout();
    test_upload_is_one_submission();
    test_failed_wait_defers_release();
    test_empty_batch_touches_nothing();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
