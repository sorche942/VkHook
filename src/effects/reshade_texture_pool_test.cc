#include "reshade_texture_pool.hh"

#include "image.hh"
#include "logical_device.hh"
#include "gpu/retire.hh"
#include "gpu/timeline.hh"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <vector>

// The headless executable never creates overlay objects. Complete these
// forward-declared owners so LogicalDevice's null unique_ptrs can be destroyed
// without linking the unrelated ImGui renderer.
namespace VkHook
{
    struct OverlayPersistentState {};
    class ImGuiOverlay {};
}

using namespace VkHook;

namespace
{
    int failures = 0;
    int checks = 0;
    uintptr_t nextHandle = 1;
    unsigned imageCalls = 0;
    unsigned viewCalls = 0;
    unsigned initCalls = 0;
    unsigned failView = 0;
    bool failImage = false;
    bool failInit = false;
    VkDevice expectedDevice = VK_NULL_HANDLE;
    VkImageUsageFlags lastUsage = 0;
    VkExtent3D lastExtent = {};
    VkFormat lastFormat = VK_FORMAT_UNDEFINED;
    uint32_t lastMips = 0;
    Timeline timeline;
    RetireQueue retireQueue{timeline};
    TimelineLane lane = 0;

    struct View
    {
        VkImage image;
        VkFormat format;
        VkImageViewType type;
        uint32_t levels;
    };
    std::unordered_map<VkImage, VkDeviceMemory> images;
    std::unordered_map<VkDeviceMemory, bool> memories;
    std::unordered_map<VkImageView, View> views;
    std::unordered_map<VkImage, unsigned> initializations;
    std::vector<char> releases;

    void expect(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
        {
            ++failures;
            std::printf("FAILED: %s\n", message);
        }
    }

    template<class T> T handle()
    {
        if constexpr (std::is_pointer_v<T>)
            return reinterpret_cast<T>(nextHandle++);
        else
            return static_cast<T>(nextHandle++);
    }

    VKAPI_ATTR VkResult VKAPI_CALL createImageView(VkDevice device, const VkImageViewCreateInfo* info,
                                                  const VkAllocationCallbacks*, VkImageView* output)
    {
        expect(device == expectedDevice, "view creation uses owning device");
        expect(images.count(info->image) == 1, "view references a live image");
        expect(info->subresourceRange.baseMipLevel == 0 && info->subresourceRange.baseArrayLayer == 0
                   && info->subresourceRange.layerCount == 1 && info->subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT,
               "view covers color starting at mip zero");
        ++viewCalls;
        if (failView == viewCalls)
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        *output = handle<VkImageView>();
        views.emplace(*output, View{info->image, info->format, info->viewType, info->subresourceRange.levelCount});
        return VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL destroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks*)
    {
        expect(device == expectedDevice, "view destruction retains device");
        const auto found = views.find(view);
        expect(found != views.end(), "each view destroyed exactly once");
        if (found != views.end())
        {
            expect(images.count(found->second.image) == 1, "view destroyed before its image");
            views.erase(found);
        }
        releases.push_back('v');
    }

    VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks*)
    {
        expect(device == expectedDevice, "image destruction retains device");
        expect(std::none_of(views.begin(), views.end(), [image](const auto& view) { return view.second.image == image; }),
               "all dependent views destroyed before image");
        const auto found = images.find(image);
        expect(found != images.end(), "each image destroyed exactly once");
        if (found != images.end())
        {
            expect(memories.count(found->second) == 1, "image destroyed before its backing memory");
            images.erase(found);
        }
        releases.push_back('i');
    }

    VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks*)
    {
        expect(device == expectedDevice, "memory destruction retains device");
        expect(std::none_of(images.begin(), images.end(), [memory](const auto& image) { return image.second == memory; }),
               "memory freed after every backed image");
        expect(memories.erase(memory) == 1, "each allocation freed exactly once");
        releases.push_back('m');
    }

    // Stands in for the device's destroy function: only the types a texture owns.
    void destroyRetired(void*, const RetiredObject& object)
    {
        switch (object.type)
        {
        case VK_OBJECT_TYPE_IMAGE_VIEW:
            destroyImageView(expectedDevice, u64ToHandle<VkImageView>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_IMAGE:
            destroyImage(expectedDevice, u64ToHandle<VkImage>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_DEVICE_MEMORY:
            freeMemory(expectedDevice, u64ToHandle<VkDeviceMemory>(object.handle), nullptr);
            break;
        default:
            expect(false, "texture retires only views, images and memory");
        }
    }

    void retireToQueue(void*, VkObjectType type, uint64_t handle, uint64_t extra)
    {
        retireQueue.retire(type, handle, extra);
    }

    ReshadeRetireHook testHook() { return {retireToQueue, nullptr}; }

    void reset()
    {
        retireQueue.drainAll();
        timeline.nextSerial(lane); // submitted serial > 0 so collect(submitted - 1) is "GPU not done"
        expect(images.empty() && memories.empty() && views.empty(), "previous case leaves no live GPU handles");
        imageCalls = viewCalls = initCalls = failView = 0;
        failImage = failInit = false;
        releases.clear();
        initializations.clear();
    }

    void finish(unsigned expectedViews, unsigned expectedImages = 1)
    {
        expect(releases.empty(), "destruction is deferred until the GPU serial passes");
        expect(retireQueue.pending() == expectedViews + 2 * expectedImages,
               "one queued release per distinct owned handle");
        expect(retireQueue.collect(timeline.submittedSerial() - 1) == 0 && releases.empty(),
               "nothing is destroyed before the serial it was retired under completes");
        retireQueue.collect(timeline.submittedSerial());
        expect(images.empty() && memories.empty() && views.empty(), "flush releases every owned handle");
        expect(std::count(releases.begin(), releases.end(), 'v') == expectedViews, "exact view release count");
        expect(std::count(releases.begin(), releases.end(), 'i') == expectedImages, "exact image release count");
        expect(std::count(releases.begin(), releases.end(), 'm') == expectedImages, "exact memory release count");
        const auto count = releases.size();
        retireQueue.collect(timeline.submittedSerial());
        expect(releases.size() == count && retireQueue.pending() == 0,
               "second flush never repeats destruction");
    }

    const ReshadeTextureDesc standard = {{128, 64, 1}, VK_FORMAT_R8G8B8A8_UNORM, 4, VK_IMAGE_VIEW_TYPE_2D};

    void aliasAndLifetime(LogicalDevice& device)
    {
        reset();
        std::shared_ptr<ReshadeTexture> consumer;
        VkImage producerImage = VK_NULL_HANDLE;
        {
            ReshadeTexturePool pool(&device, testHook());
            auto producer = pool.acquire("DH_MotionVectors", standard);
            producerImage = producer->image;
            consumer = pool.acquire("DH_MotionVectors", standard);
            expect(consumer == producer && consumer->image == producerImage, "consumer retains producer's exact image and owner");
            expect(imageCalls == 1 && viewCalls == 4 && initCalls == 1,
                   "reuse performs no image allocation, view creation, or initialization");
            expect(lastExtent.width == 128 && lastExtent.height == 64 && lastExtent.depth == 1
                       && lastFormat == standard.format && lastMips == 4, "image receives complete descriptor");
            expect(lastUsage == (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                 | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
                   "2D generated image supports sampling, compute, copies, and rendering");
            expect(views.at(consumer->unormView).format == VK_FORMAT_R8G8B8A8_UNORM
                       && views.at(consumer->srgbView).format == VK_FORMAT_R8G8B8A8_SRGB,
                   "sampled views use corresponding UNORM and SRGB formats");
            expect(views.at(consumer->unormView).levels == 4 && views.at(consumer->srgbView).levels == 4,
                   "sampled views span all mips");
            expect(views.at(consumer->unormRenderView).levels == 1 && views.at(consumer->srgbRenderView).levels == 1,
                   "attachment views expose only base level");
            producer.reset();
            retireQueue.collect(timeline.submittedSerial());
            expect(images.count(producerImage) == 1 && releases.empty(), "dropping producer cannot release consumer's image");
            consumer.reset();
            retireQueue.collect(timeline.submittedSerial());
            expect(images.count(producerImage) == 1 && releases.empty(), "pool retains texture with no external consumers");
            consumer = pool.acquire("DH_MotionVectors", standard);
            expect(consumer->image == producerImage && imageCalls == 1 && initCalls == 1,
                   "later consumer receives retained producer image without reinitializing");
        }
        retireQueue.collect(timeline.submittedSerial());
        expect(images.count(producerImage) == 1 && releases.empty(), "dropping pool cannot release consumer's image");
        consumer.reset();
        finish(4);
        expect(releases == std::vector<char>({'v', 'v', 'v', 'v', 'i', 'm'}), "single texture releases views then image then memory");
    }

    void mismatchAndScope(LogicalDevice& device)
    {
        reset();
        {
            ReshadeTexturePool pool(&device, testHook());
            auto first = pool.acquire("shared", standard);
            ReshadeTextureDesc variants[] = {standard, standard, standard, standard, standard, standard};
            ++variants[0].extent.width;
            ++variants[1].extent.height;
            ++variants[2].extent.depth;
            variants[3].format = VK_FORMAT_R16G16_SFLOAT;
            ++variants[4].mipLevels;
            variants[5].viewType = VK_IMAGE_VIEW_TYPE_3D;
            for (const auto& variant : variants)
            {
                bool rejected = false;
                try { pool.acquire("shared", variant); }
                catch (const std::runtime_error&) { rejected = true; }
                expect(rejected, "each incompatible descriptor field is rejected");
                expect(imageCalls == 1 && viewCalls == 4 && initCalls == 1, "mismatch rejected before allocating or initializing");
                expect(pool.acquire("shared", standard) == first, "mismatch preserves existing texture");
            }
            auto otherName = pool.acquire("other", standard);
            ReshadeTexturePool otherPool(&device, testHook());
            auto otherScope = otherPool.acquire("shared", standard);
            expect(first->image != otherName->image && first->image != otherScope->image
                       && otherName->image != otherScope->image, "different names and chain scopes never alias");
        }
        finish(12, 3);
    }

    // Instances X and X.2 of one effect file must not share private textures
    // (history included), while the first instance of another file that declares
    // the same name still shares with the first, as ReShade does across files.
    void instancesOfSameEffect(LogicalDevice& device)
    {
        reset();
        {
            ReshadeTexturePool pool(&device, testHook());
            const uint32_t first = pool.beginEffect("Temporal.fx");
            const uint32_t second = pool.beginEffect("Temporal.fx");
            const uint32_t otherFile = pool.beginEffect("Other.fx");
            expect(first == 0 && second == 1 && otherFile == 0, "instance ordinals count per effect file");

            auto firstHistory = pool.acquire("VHistory", standard, first);
            auto secondHistory = pool.acquire("VHistory", standard, second);
            expect(firstHistory != secondHistory && firstHistory->image != secondHistory->image,
                   "two instances of one file get separate private textures");
            expect(imageCalls == 2, "each instance allocates its own image");
            expect(pool.acquire("VHistory", standard, first) == firstHistory
                       && pool.acquire("VHistory", standard, second) == secondHistory,
                   "an instance keeps getting its own texture");

            auto consumer = pool.acquire("VHistory", standard, otherFile);
            expect(consumer == firstHistory, "another file's first instance shares by name with the first instance");
            expect(imageCalls == 2, "cross-file sharing allocates nothing");
        }
        finish(8, 2);
    }

    void deduplicateViews(LogicalDevice& device)
    {
        for (const VkFormat format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16_SFLOAT})
            for (const uint32_t mips : {1u, 4u})
            {
                reset();
                const bool sameFormat = format == VK_FORMAT_R16G16_SFLOAT;
                const unsigned expected = (sameFormat ? 1u : 2u) * (mips == 1 ? 1u : 2u);
                {
                    ReshadeTexturePool pool(&device, testHook());
                    auto texture = pool.acquire("dedup", {{32, 16, 1}, format, mips, VK_IMAGE_VIEW_TYPE_2D});
                    expect(viewCalls == expected, "only distinct format/mip combinations create views");
                    expect((texture->unormView == texture->srgbView) == sameFormat, "identical sampled formats share view");
                    expect((texture->unormRenderView == texture->srgbRenderView) == sameFormat, "identical attachment formats share view");
                    expect((texture->unormView == texture->unormRenderView) == (mips == 1), "single mip shares sampled and render view");
                }
                finish(expected);
            }
        reset();
        {
            ReshadeTexturePool pool(&device, testHook());
            auto texture = pool.acquire("volume", {{16, 16, 8}, VK_FORMAT_R16G16_SFLOAT, 1, VK_IMAGE_VIEW_TYPE_3D});
            expect(!(lastUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT), "3D texture excludes unsupported attachment usage");
            expect(lastExtent.depth == 8 && views.at(texture->unormView).type == VK_IMAGE_VIEW_TYPE_3D,
                   "volume preserves depth and view type");
        }
        finish(1);
    }

    void constructionFailures(LogicalDevice& device)
    {
        // Fail each of the four distinct view acquisitions, then initialization.
        for (unsigned stage = 0; stage <= 5; ++stage)
        {
            reset();
            failImage = stage == 0;
            failView = stage >= 1 && stage <= 4 ? stage : 0;
            failInit = stage == 5;
            ReshadeTexturePool pool(&device, testHook());
            bool threw = false;
            try { pool.acquire("partial", standard); }
            catch (const std::runtime_error&) { threw = true; }
            expect(threw, "failed construction reports failure");
            finish(stage == 0 ? 0 : stage == 5 ? 4 : stage - 1, stage == 0 ? 0 : 1);
            failImage = failInit = false;
            failView = 0;
            releases.clear();
            auto retry = pool.acquire("partial", standard);
            expect(images.count(retry->image) == 1, "failed acquire does not poison name in pool");
            expect(pool.acquire("partial", standard) == retry, "successful retry becomes canonical owner");
        }
        reset();
    }
}

// Only the GPU image allocation and initialization boundary is replaced. Pool
// lookup, format conversion, view creation dispatch, ownership and deferred
// release ordering all use the production implementations.
namespace VkHook
{
    VkImage createSingleImage(LogicalDevice*, VkExtent3D extent, VkFormat format, VkImageUsageFlags usage,
                              VkMemoryPropertyFlags properties, VkDeviceMemory& memory, uint32_t mipLevels)
    {
        ++imageCalls;
        lastUsage = usage;
        lastExtent = extent;
        lastFormat = format;
        lastMips = mipLevels;
        if (failImage)
            return VK_NULL_HANDLE;
        memory = handle<VkDeviceMemory>();
        memories.emplace(memory, true);
        VkImage image = handle<VkImage>();
        images.emplace(image, memory);
        return image;
    }

    void changeImageLayout(LogicalDevice*, const std::vector<VkImage>& initializedImages, uint32_t mipLevels)
    {
        ++initCalls;
        expect(initializedImages.size() == 1 && mipLevels == lastMips, "initialization covers exactly the new image and all mips");
        for (auto image : initializedImages)
        {
            expect(images.count(image) == 1, "initialization receives a live image");
            expect(++initializations[image] == 1, "image is initialized once, never on reuse");
        }
        if (failInit)
            throw std::runtime_error("injected initialization failure");
    }
}

int main()
{
    LogicalDevice device = {};
    device.device = handle<VkDevice>();
    expectedDevice = device.device;
    lane = timeline.addLane();
    retireQueue.bind(destroyRetired, nullptr);
    device.vkd.CreateImageView = createImageView;
    device.vkd.DestroyImageView = destroyImageView;
    device.vkd.DestroyImage = destroyImage;
    device.vkd.FreeMemory = freeMemory;

    aliasAndLifetime(device);
    mismatchAndScope(device);
    instancesOfSameEffect(device);
    deduplicateViews(device);
    constructionFailures(device);
    std::printf("ReShade texture pool: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
