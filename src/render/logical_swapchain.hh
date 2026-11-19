#ifndef LOGICAL_SWAPCHAIN_HPP_INCLUDED
#define LOGICAL_SWAPCHAIN_HPP_INCLUDED
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>
#include <memory>

#include "effects/effect.hh"

#include "vulkan_include.hh"

#include "logical_device.hh"

namespace VkHook
{
    class Config;
    class InputSession;
    class OverlayRenderer;

    // for each swapchain, we have the Images and the other stuff we need to execute the compute shader
    // Per-image depth resolve resources — kept together so they
    // can never desynchronize across parallel vectors.
    struct DepthResolvePerImage
    {
        VkImage        image     = VK_NULL_HANDLE;
        VkImageView    imageView = VK_NULL_HANDLE;
        VkDeviceMemory memory    = VK_NULL_HANDLE;
        bool           initialized = false;
    };

    // Swapchain-lifetime resources and the per-image frame ring, shared by every
    // chain generation of one swapchain. The game holds the fake images, and the
    // semaphores are waited on by queued presents, so none of it can follow a
    // generation swap. Everything is retired through the device's RetireQueue
    // when the last generation of the swapchain goes away.
    struct SwapchainShared
    {
        LogicalDevice*             logicalDevice = nullptr;
        std::vector<VkImage>       fakeImages;
        std::vector<VkDeviceMemory> fakeImageMemories;
        std::vector<VkImageView>   imageViews;  // real images, for overlay rendering
        std::vector<VkSemaphore>   semaphores;
        std::vector<VkSemaphore>   overlaySemaphores;
        // One fence per image, signalled by that image's last layer submission.
        // Only the swapchain's present path (externally synchronised by the
        // application) touches fenceUsed.
        std::vector<VkFence>       effectFences;
        std::vector<uint8_t>       effectFenceUsed;
        // newest layer submission that reads or writes this swapchain's images
        std::atomic<uint64_t>      lastSerial{0};

        // The input session of the surface this swapchain presents to (null for a
        // windowing system the layer does not process). It outlives the swapchain's use of it.
        std::shared_ptr<InputSession> input;

        // This swapchain's overlay target: render pass, command buffers, fences
        // and framebuffers for its images, drawn with the device's OverlayUi.
        // Created with the first generation (its format and image count are fixed
        // for the swapchain's life) and reset before anything it renders into
        // is released.
        std::shared_ptr<OverlayRenderer> overlay;

        // First-generation build (vkGetSwapchainImagesKHR). The mutex guards the
        // state variable only and is never held while building; a concurrent
        // caller waits on the condition variable for the builder to finish.
        enum class Build { None, Building, Done };
        std::mutex              buildLock;
        std::condition_variable buildDone;
        Build                   build = Build::None;

        // Held by the thread that is rebuilding this swapchain's chain; a second
        // rebuild request skips instead of waiting (it never blocks).
        std::atomic<bool> rebuildBusy{false};

        // Releases every object now. `gpuIdle` says the caller proved the layer's
        // submissions on this swapchain complete (vkDestroySwapchainKHR waited
        // for its last serial): objects are then destroyed in place, before the
        // real swapchain goes away; otherwise they are retired. Idempotent: the
        // destructor releases whatever is left.
        void release(bool gpuIdle);

        explicit SwapchainShared(LogicalDevice* device) : logicalDevice(device) {}
        SwapchainShared(const SwapchainShared&)            = delete;
        SwapchainShared& operator=(const SwapchainShared&) = delete;
        ~SwapchainShared();
    };

    // Per chain-generation depth-resolve resources. A rebuild builds a fresh set
    // beside the live one and swaps whole generations (present/swapchain.cc).
    struct DepthResolveFields
    {
        std::vector<DepthResolvePerImage>       depthResolvePerImage;
        VkFormat                             depthResolveFormat = VK_FORMAT_UNDEFINED;
        VkExtent3D                           depthResolveExtent = {0, 0, 1};
        VkImageView                          depthResolveSourceView = VK_NULL_HANDLE;  // Track which view we resolved for
        bool                                 depthResolveUsesShader = false;
        // MSAA depth resolve path. When the active depth source is multisampled
        // and not in GENERAL layout, the layer resolves it down to 1 sample via
        // a depth-stencil resolve subpass (VkSubpassDescriptionDepthStencilResolve)
        // before effects sample it. The resolve target is depthResolvePerImage.
        bool                                 depthResolveIsMsaa = false;
        VkSampleCountFlagBits                depthResolveSourceSamples = VK_SAMPLE_COUNT_1_BIT;
        VkResolveModeFlagBits                depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        VkRenderPass                         depthResolveMsaaRenderPass = VK_NULL_HANDLE;
        std::vector<VkFramebuffer>           depthResolveMsaaFramebuffers;
        // msaa + transform: resolves into depthResolvePerImage, then a second
        // subpass writes the transformed copy effects read into
        // depthResolveTransformedPerImage — all inside one render pass.
        bool                                 depthResolveMsaaTransform = false;
        std::vector<DepthResolvePerImage>    depthResolveTransformedPerImage;
        VkRenderPass                         depthResolveTransformRenderPass = VK_NULL_HANDLE;
        std::vector<VkFramebuffer>           depthResolveTransformFramebuffers;
        VkSampler                            depthResolveSampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout                depthResolveDescriptorSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool                     depthResolveDescriptorPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet>         depthResolveDescriptorSets;
        VkRenderPass                         depthResolveRenderPass = VK_NULL_HANDLE;
        VkPipelineLayout                     depthResolvePipelineLayout = VK_NULL_HANDLE;
        VkPipeline                           depthResolvePipeline = VK_NULL_HANDLE;
        std::vector<VkFramebuffer>           depthResolveFramebuffers;
    };

    // One chain generation of a swapchain: the effects, their command buffers
    // and the depth-resolve resources those buffers reference (AGENTS.md
    // invariant 6; formal/VkHookFormal/ChainRebuild.lean). A rebuild constructs
    // the next generation with no lock held (cloneBase + build), the registry
    // swaps the pointer under the device's stateLock at a present boundary, and
    // destroying the old object retires its resources through the RetireQueue
    // once the last present thread using it drops its reference.
    struct LogicalSwapchain : DepthResolveFields
    {
        LogicalDevice*                       logicalDevice = nullptr;
        VkSwapchainKHR                       handle        = VK_NULL_HANDLE;
        // Deep copy: pNext is dropped (nothing after creation needs it) and
        // pQueueFamilyIndices points into ownedQueueFamilyIndices.
        VkSwapchainCreateInfoKHR             swapchainCreateInfo{};
        std::vector<uint32_t>                ownedQueueFamilyIndices;
        VkExtent2D                           imageExtent{};
        VkFormat                             format = VK_FORMAT_UNDEFINED;
        uint32_t                             imageCount = 0;
        bool                                 useMutableFormat = false;
        std::vector<VkImage>                 images;
        std::vector<VkImageView>             imageViews;  // for overlay rendering
        std::vector<VkImage>                 fakeImages;
        // real images served and presents passed through: zero-sized, layered,
        // a surface that cannot take TRANSFER_DST, or fake allocation failed.
        // Fixed once GetSwapchainImages answered; a swapchain is never both.
        bool                                 passthrough = false;
        size_t                               maxEffectSlots = 0;  // Max number of effects supported
        std::vector<VkSemaphore>             semaphores;
        std::vector<VkSemaphore>             overlaySemaphores;
        std::vector<VkFence>                 effectSubmitFences;
        std::vector<VkDeviceMemory>          fakeImageMemories;
        std::shared_ptr<SwapchainShared>     shared;

        // --- generation-owned (replaced by a rebuild) ---
        // per-generation pool (RESET_COMMAND_BUFFER: the buffers are re-recorded
        // every frame effects change)
        VkCommandPool                        commandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer>         commandBuffersEffect;
        // fake -> real copy only, recorded without depth: what CopyOnly submits
        std::vector<VkCommandBuffer>         commandBuffersNoEffect;
        std::vector<std::shared_ptr<Effect>> effects;
        std::shared_ptr<Effect>              defaultTransfer;
        uint64_t                             generation = 0;
        // names of the effects `effects` was built from (a depth rebuild builds
        // the same chain again)
        std::vector<std::string>             effectNames;

        bool depthReallocPending = false;

        LogicalSwapchain() = default;
        LogicalSwapchain(const LogicalSwapchain&)            = delete;
        LogicalSwapchain& operator=(const LogicalSwapchain&) = delete;
        ~LogicalSwapchain();

        // The game-facing swapchain was replaced by fake images; every present
        // then needs a layer submission (invariant 5).
        bool faked() const { return !passthrough && !fakeImages.empty(); }

        // The swapchain-lifetime part of this object, with an empty generation.
        std::shared_ptr<LogicalSwapchain> cloneBase() const;

        bool fenceUsed(uint32_t image) const
        {
            return shared && image < shared->effectFenceUsed.size() && shared->effectFenceUsed[image];
        }

        // Retires this generation's pool, command buffers and depth-resolve
        // resources and drops its effects. Called by the destructor.
        void releaseGeneration();
    };

    // Retires every depth-resolve object of the set (the pipeline, pass,
    // framebuffers, views, images and memory a recorded buffer may reference)
    // and resets it to empty.
    void retireDepthResolveFields(LogicalDevice* logicalDevice, DepthResolveFields& fields);
} // namespace VkHook

#endif // LOGICAL_SWAPCHAIN_HPP_INCLUDED
