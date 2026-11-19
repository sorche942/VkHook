#include "present/swapchain.hh"

#include "layer/entry.hh"
#include "layer/registry.hh"
#include "layer/guard.hh"
#include "layer/env.hh"

#include "control/frontend.hh"
#include "depth/capture.hh"
#include "depth/tracker.hh"
#include "diag/gpu_crash.hh"

#include "input_blocker.hh"
#include "wayland_display.hh"
#include "fake_swapchain.hh"
#include "command_buffer.hh"
#include "format.hh"
#include "util.hh"
#include "logger.hh"
#include "store.hh"
#include "config.hh"
#include "input_session.hh"
#include "overlay/overlay_renderer.hh"
#include "effects/effect.hh"
#include "effects/effect_reshade.hh"
#include "effects/effect_transfer.hh"
#include "effects/effect_registry.hh"
#include "effects/dlss5_config.hh"
#ifdef VKHOOK_DLSS5
#include "effects/effect_dlss5.hh"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include <chrono>
#include <future>
#include <optional>

namespace VkHook
{
    // One chain entry, after its one id parse.
    struct ChainEntry
    {
        std::string             name;
        std::optional<EffectId> id;       // nullopt: a legacy name
        bool                    dlss5 = false;
        // index in the compile result when the entry was submitted, else -1
        int                     compileIndex = -1;
        std::shared_ptr<const EffectCompileResult> compiled;
    };
    using ChainPlan = std::vector<ChainEntry>;

    // A chain build whose ReShade effects are compiling on the registry's worker;
    // see advanceChainBuild.
    struct PendingChain
    {
        ChainPlan                              plan;
        std::shared_future<ChainCompileResult> future;
    };

    ChainPlan planChain(const std::vector<std::string>& names, Config* config)
    {
        ChainPlan plan;
        plan.reserve(names.size());
        for (const std::string& name : names)
        {
            ChainEntry entry;
            entry.name  = name;
            entry.id    = EffectId::parse(name);
            entry.dlss5 = entry.id ? entry.id->isBuiltin() : isDlss5Effect(name, config);
            plan.push_back(std::move(entry));
        }
        return plan;
    }

    // The compile request for the ReShade entries of `plan` that will be built (not
    // disabled, not failed, shader found), at `swapchain`'s extent and colour depth;
    // each submitted entry records its index in the result.
    ChainCompileSpec compileSpecFor(EffectRegistry& registry, const LogicalSwapchain& swapchain, ChainPlan& plan)
    {
        const uint32_t colorBitDepth = convertToUNORM(swapchain.format) == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ? 10 : 8;
        ChainCompileSpec spec;
        for (ChainEntry& entry : plan)
        {
            if (entry.dlss5 || !entry.id || registry.hasEffectFailed(entry.name) || !registry.isEffectEnabled(entry.name))
                continue;
            EffectCompileSpec effect;
            effect.path = registry.getEffectFilePath(entry.name);
            if (effect.path.empty())
                continue;  // the effect looks for the file itself and reports it
            effect.id    = *entry.id;
            effect.setup = registry.preprocessSetup(swapchain.imageExtent.width, swapchain.imageExtent.height,
                                                    colorBitDepth, registry.getPreprocessorDefs(entry.name));
            effect.codegen = registry.codegenOptions();
            entry.compileIndex = static_cast<int>(spec.effects.size());
            spec.effects.push_back(std::move(effect));
        }
        return spec;
    }

    // Creates the effects of a swapchain's chain from `plan`.
    // This centralizes the effect creation logic used by both initial swapchain setup and hot-reload
    static void createEffectsForSwapchain(
        LogicalSwapchain* logicalSwapchain,
        LogicalDevice* logicalDevice,
        Config* config,
        const ChainPlan& plan)
    {
        const bool useMutableFormat = logicalSwapchain->useMutableFormat;
        EffectRegistry& effectRegistry = deviceControl(logicalDevice).effectRegistry;

        if (logicalSwapchain->imageCount == 0)
        {
            Logger::err("Cannot create effects for swapchain with imageCount=0");
            return;
        }

        // If no effects, add pass-through so rendering still works
        if (plan.empty())
        {
            logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(
                logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent,
                logicalSwapchain->fakeImages, logicalSwapchain->images, config));
            return;
        }

        // New scope for each swapchain build/reload: history must not alias an
        // old chain (possibly still in flight) or another swapchain's images.
        ReshadeTexturePool texturePool(logicalDevice);
        // Effect 0 reads what the game rendered; every other effect reads what
        // the previous one wrote. The chain alternates between two images, so
        // its length costs no memory. Effect i writes ping when i is even,
        // pong when odd; the last effect writes the real image where the
        // swapchain can take it directly.
        const auto pingPong = [&](size_t effect) -> const std::vector<VkImage>& {
            return effect % 2 == 0 ? logicalSwapchain->pingImages : logicalSwapchain->pongImages;
        };
        for (uint32_t i = 0; i < plan.size(); i++)
        {
            const ChainEntry& entry = plan[i];
            const std::string& effectName = entry.name;
            VKHOOK_LOG_DEBUG([&](std::string& s) { s = "creating effect " + std::to_string(i) + ": " + effectName; });

            const std::vector<VkImage>& firstImages = i == 0 ? logicalSwapchain->fakeImages : pingPong(i - 1);
            const std::vector<VkImage>& secondImages =
                (i == plan.size() - 1 && useMutableFormat) ? logicalSwapchain->images : pingPong(i);

            // Check if effect should be skipped (disabled or failed)
            bool effectFailed = effectRegistry.hasEffectFailed(effectName);
            bool effectDisabled = !effectRegistry.isEffectEnabled(effectName);

            if (effectFailed || effectDisabled)
            {
                VKHOOK_LOG_DEBUG([&](std::string& s) {
                    s = "effect " + std::string(effectFailed ? "failed" : "disabled") + ", using pass-through: " + effectName;
                });
                logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent, firstImages, secondImages, config));
                continue;
            }

            if (entry.dlss5)
            {
                try
                {
#ifdef VKHOOK_DLSS5
                    const std::vector<VkImage> motionSources(logicalSwapchain->fakeImages.begin(),
                        logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount);
                    logicalSwapchain->effects.push_back(std::make_shared<Dlss5Effect>(
                        logicalDevice, logicalSwapchain->format, logicalSwapchain->swapchainCreateInfo.imageColorSpace,
                        logicalSwapchain->imageExtent, firstImages, secondImages, motionSources,
                        &effectRegistry, config, effectName));
#else
                    throw std::runtime_error("DLSS5 backend not built; configure Meson with -Ddlss5=enabled and its build dependencies");
#endif
                }
                catch (const std::exception& e)
                {
                    Logger::err("Failed to create DLSS5 effect: " + std::string(e.what()));
                    effectRegistry.setEffectError(effectName, e.what());
                    logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(
                        logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent, firstImages, secondImages, config));
                }
                continue;
            }

            // ReShade effect. A build that fails (compile error, driver refusal, any
            // exception) disables this effect only: it is replaced by a
            // pass-through and the registry records why. Nothing here may take the
            // process down; the embedded compiler reports failures as errors.
            std::string effectPath = effectRegistry.getEffectFilePath(effectName);
            auto customDefs = effectRegistry.getPreprocessorDefs(effectName);

            auto disableEffect = [&](const std::string& why)
            {
                Logger::err("Failed to create ReshadeEffect " + effectName + ": " + why);
                effectRegistry.setEffectError(effectName, why);
                logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(
                    logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent, firstImages, secondImages, config));
            };

            try
            {
                // `entry.compiled` is what the registry's worker compiled for this entry
                // (null: the effect compiles it itself through the registry's cache)
                auto reshadeEffect = std::make_shared<ReshadeEffect>(
                    logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent,
                    firstImages, secondImages, &effectRegistry, texturePool, effectName, effectPath, customDefs,
                    entry.compiled);
                logicalSwapchain->effects.push_back(reshadeEffect);
                if (reshadeEffect->requiresOutputForward())
                {
                    Logger::debug("forwarding the in-place result of effect: " + effectName);
                    logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent,
                                           firstImages, secondImages, config));
                }
            }
            catch (const std::exception& e)
            {
                disableEffect(e.what());
            }
            catch (...)
            {
                disableEffect("unknown exception during effect creation");
            }
        }

        // If device doesn't support mutable format, add final transfer to swapchain
        if (!useMutableFormat)
        {
            logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(
                logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent,
                pingPong(plan.size() - 1), logicalSwapchain->images, config));
        }
    }


namespace
{
    // Builds the chain of `next`, an unpublished generation no other thread can
    // see: its own RESET-flag command pool, the effects, and the effect and the
    // copy-only command buffers. No lock is held and nothing waits for the GPU.
    // On false, `next` is incomplete and must be dropped (its destructor retires
    // whatever was created).
    bool buildGeneration(LogicalDevice* logicalDevice, LogicalSwapchain& next, ChainPlan plan)
    {
        if (next.imageCount == 0 || next.fakeImages.empty())
            return false;

        VkCommandPoolCreateInfo poolCI = {};
        poolCI.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolCI.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolCI.queueFamilyIndex = logicalDevice->queueFamilyIndex;
        const VkResult poolResult = logicalDevice->vkd.CreateCommandPool(logicalDevice->device, &poolCI, nullptr, &next.commandPool);
        if (poolResult != VK_SUCCESS || next.commandPool == VK_NULL_HANDLE)
        {
            Logger::err("Failed to create the chain command pool (" + std::to_string(poolResult) + ")");
            next.commandPool = VK_NULL_HANDLE;
            return false;
        }

        createEffectsForSwapchain(&next, logicalDevice, deviceControl(logicalDevice).config.get(), plan);
        next.effectNames.clear();
        for (const ChainEntry& entry : plan)
            next.effectNames.push_back(entry.name);
        if (next.effects.empty())
            return false;

        // fake -> real copy, recorded without depth: what a CopyOnly present submits
        next.defaultTransfer = std::make_shared<TransferEffect>(
            logicalDevice, next.format, next.imageExtent,
            std::vector<VkImage>(next.fakeImages.begin(), next.fakeImages.begin() + next.imageCount),
            next.images, deviceControl(logicalDevice).config.get());

        next.commandBuffersEffect   = allocateCommandBuffer(logicalDevice, next.imageCount, next.commandPool);
        next.commandBuffersNoEffect = allocateCommandBuffer(logicalDevice, next.imageCount, next.commandPool);
        if (next.commandBuffersEffect.size() != next.imageCount || next.commandBuffersNoEffect.size() != next.imageCount)
        {
            Logger::err("Failed to allocate the chain command buffers");
            return false;
        }

        writeCommandBuffers(logicalDevice, &next, next.effects, next.commandBuffersEffect);
        writeCommandBuffers(logicalDevice, &next, {next.defaultTransfer}, next.commandBuffersNoEffect);
        return true;
    }

    bool installGeneration(LogicalDevice& logicalDevice, VkSwapchainKHR handle,
                           const std::shared_ptr<LogicalSwapchain>& expected, std::shared_ptr<LogicalSwapchain> next);

    // Builds the next generation from `plan` and swaps it in; the caller holds
    // `rebuildBusy`. No lock is held while building and nothing waits for the GPU.
    bool installChain(LogicalDevice* logicalDevice, const std::shared_ptr<LogicalSwapchain>& current, ChainPlan plan)
    {
        bool swapped = false;
        guardLayerWork(logicalDevice, "chain rebuild", [&] {
            auto next = current->cloneBase();
            Logger::info("rebuilding the chain: " + std::to_string(plan.size()) + " effects, generation "
                         + std::to_string(next->generation));
            if (!buildGeneration(logicalDevice, *next, std::move(plan)))
            {
                Logger::err("chain rebuild failed; keeping the current generation");
                return;
            }
            swapped = installGeneration(*logicalDevice, current->handle, current, std::move(next));
        });
        return swapped;
    }

    // Starts the build of `names` for `current`'s swapchain. The ReShade effects
    // compile on the registry's worker; advanceChainBuild finishes the build at a
    // later present, once the compile is done. A chain with nothing to compile is
    // built at once. A newer request replaces an unfinished one.
    bool beginChainBuild(LogicalDevice* logicalDevice, const std::shared_ptr<LogicalSwapchain>& current,
                         const std::vector<std::string>& names)
    {
        SwapchainShared& shared = *current->shared;
        EffectRegistry& registry = deviceControl(logicalDevice).effectRegistry;

        auto pending = std::make_shared<PendingChain>();
        pending->plan = planChain(names, deviceControl(logicalDevice).config.get());
        ChainCompileSpec spec = compileSpecFor(registry, *current, pending->plan);
        // one channel per swapchain: a newer request supersedes this swapchain's older one only
        spec.channel = reinterpret_cast<uint64_t>(&shared);

        {
            std::lock_guard<std::mutex> l(shared.pendingLock);
            shared.pendingChain.reset();
            shared.chainPending.store(false, std::memory_order_release);
        }
        if (spec.effects.empty())
        {
            if (shared.rebuildBusy.exchange(true, std::memory_order_acq_rel))
                return false;  // another thread is finishing a chain of this swapchain
            const bool swapped = installChain(logicalDevice, current, std::move(pending->plan));
            shared.rebuildBusy.store(false, std::memory_order_release);
            return swapped;
        }

        pending->future = registry.compiler().submit(std::move(spec));
        std::lock_guard<std::mutex> l(shared.pendingLock);
        shared.pendingChain = std::move(pending);
        shared.chainPending.store(true, std::memory_order_release);
        return true;
    }

    // Swaps `next` in for `expected`. False (and `next` dropped, retiring its
    // objects) when the swapchain was destroyed or replaced in the meantime.
    // The old generation is released after the lock is dropped.
    bool installGeneration(LogicalDevice& logicalDevice, VkSwapchainKHR handle,
                           const std::shared_ptr<LogicalSwapchain>& expected, std::shared_ptr<LogicalSwapchain> next)
    {
        std::shared_ptr<LogicalSwapchain> old;
        {
            std::lock_guard<std::mutex> l(logicalDevice.stateLock);
            auto it = logicalDevice.swapchains.find(handle);
            if (it == logicalDevice.swapchains.end() || it->second != expected)
                return false;
            old         = std::move(it->second);
            it->second  = std::move(next);
        }
        return true;
    }

    std::vector<std::shared_ptr<LogicalSwapchain>> fakedSwapchains(LogicalDevice& logicalDevice)
    {
        std::vector<std::shared_ptr<LogicalSwapchain>> out = deviceSwapchains(logicalDevice);
        out.erase(std::remove_if(out.begin(), out.end(), [](const auto& sc) { return !sc->faked() || !sc->shared; }), out.end());
        return out;
    }
} // namespace

    bool rebuildSwapchainChain(LogicalDevice* logicalDevice, const std::shared_ptr<LogicalSwapchain>& current,
                               const std::vector<std::string>* effects)
    {
        if (!logicalDevice || !current || !current->faked() || !current->shared)
            return false;
        return beginChainBuild(logicalDevice, current, effects ? *effects : current->effectNames);
    }

    void advanceChainBuild(LogicalDevice* logicalDevice, const std::shared_ptr<LogicalSwapchain>& current)
    {
        if (!logicalDevice || !current || !current->faked() || !current->shared)
            return;
        SwapchainShared& shared = *current->shared;

        // The first chain waits for the registry's initial build: its selection is not
        // known before.
        if (shared.chainWanted.load(std::memory_order_acquire))
        {
            EffectRegistry& registry = deviceControl(logicalDevice).effectRegistry;
            if (registry.isInitializedFromConfig() && !registry.initializing() &&
                shared.chainWanted.exchange(false, std::memory_order_acq_rel))
                beginChainBuild(logicalDevice, current, registry.getSelectedEffects());
        }

        if (!shared.chainPending.load(std::memory_order_acquire))
            return;
        std::shared_ptr<PendingChain> pending;
        {
            std::lock_guard<std::mutex> l(shared.pendingLock);
            pending = shared.pendingChain;
        }
        if (!pending || pending->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;

        // another thread finishing this swapchain's chain right now: try next present
        if (shared.rebuildBusy.exchange(true, std::memory_order_acq_rel))
            return;
        {
            std::lock_guard<std::mutex> l(shared.pendingLock);
            if (shared.pendingChain != pending)
            {
                shared.rebuildBusy.store(false, std::memory_order_release);
                return;  // replaced by a newer request meanwhile
            }
            shared.pendingChain.reset();
            shared.chainPending.store(false, std::memory_order_release);
        }
        const ChainCompileResult& compiled = pending->future.get();
        if (!compiled.cancelled)
        {
            for (ChainEntry& entry : pending->plan)
                if (entry.compileIndex >= 0 && static_cast<size_t>(entry.compileIndex) < compiled.effects.size())
                    entry.compiled = std::make_shared<const EffectCompileResult>(compiled.effects[entry.compileIndex]);
            installChain(logicalDevice, current, std::move(pending->plan));
        }
        shared.rebuildBusy.store(false, std::memory_order_release);
    }

    void reloadAllSwapchains(LogicalDevice* logicalDevice, const std::vector<std::string>& activeEffects)
    {
        if (!logicalDevice)
            return;
        for (const auto& sc : fakedSwapchains(*logicalDevice))
            rebuildSwapchainChain(logicalDevice, sc, &activeEffects);
    }

    VkExtent2D currentSwapchainExtentLocked(LogicalDevice* logicalDevice)
    {
        for (const auto& [_, sc] : logicalDevice->swapchains)
            if (sc && !sc->passthrough && sc->imageCount > 0)
                return sc->imageExtent;
        return VkExtent2D{0, 0};
    }

namespace
{
    // Serves `count` of `source` into the application's array (vkGetSwapchainImagesKHR rules).
    VkResult serveImages(const std::vector<VkImage>& source, uint32_t imageCount, uint32_t* pCount, VkImage* out)
    {
        if (source.size() < imageCount)
        {
            Logger::err("swapchain image cache is smaller than imageCount");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        const uint32_t requested = *pCount;
        *pCount = std::min<uint32_t>(requested, imageCount);
        std::memcpy(out, source.data(), sizeof(VkImage) * (*pCount));
        return requested < imageCount ? VK_INCOMPLETE : VK_SUCCESS;
    }

    VkResult serveGeneration(const LogicalSwapchain& sc, uint32_t* pCount, VkImage* out)
    {
        return serveImages(sc.faked() ? sc.fakeImages : sc.images, sc.imageCount, pCount, out);
    }

    // The device's OverlayUi, created with its first swapchain and shared by every
    // swapchain's renderer. The overlay grabs the surface's input while it is up.
    OverlayUi* ensureOverlayUi(LogicalDevice* logicalDevice, InputSession* input)
    {
        OverlayControl& overlay = deviceControl(logicalDevice).overlay;
        if (OverlayUi* existing = overlay.ui())
            return existing;

        OverlayUi* ui = overlay.ensureUi();
        if (ui && input)
        {
            // Input blocking (grabs all input when the overlay is visible)
            input->initBlocker(ConfigStore::instance().getOverlayBlockInput());
            input->setBlocked(ui->isVisible());
        }
        return ui;
    }

    // Builds the first generation of a swapchain from its real images. Returns
    // the generation (faked or passthrough) or null with `error` set.
    std::shared_ptr<LogicalSwapchain> buildFirstGeneration(LogicalDevice* logicalDevice, VkDevice device,
                                                           VkSwapchainKHR swapchain,
                                                           const std::shared_ptr<LogicalSwapchain>& skeleton,
                                                           VkResult& error)
    {
        uint32_t realImageCount = 0;
        VkResult getCountResult = logicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, &realImageCount, nullptr);
        if (isSwapchainDiagEnabled())
        {
            std::fprintf(stderr, "VkHook diag: GetSwapchainImagesKHR count-query result=%d count=%u\n",
                         static_cast<int>(getCountResult), static_cast<unsigned int>(realImageCount));
        }
        if (getCountResult != VK_SUCCESS && getCountResult != VK_INCOMPLETE)
        {
            Logger::err("vkGetSwapchainImagesKHR(count) failed: " + std::to_string(getCountResult));
            error = getCountResult;
            return nullptr;
        }
        if (realImageCount == 0)
        {
            Logger::err("vkGetSwapchainImagesKHR returned zero images");
            error = VK_ERROR_INITIALIZATION_FAILED;
            return nullptr;
        }

        std::vector<VkImage> realImages(realImageCount);
        uint32_t fetchedImageCount = realImageCount;
        VkResult getImagesResult = logicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, &fetchedImageCount, realImages.data());
        if (isSwapchainDiagEnabled())
        {
            std::fprintf(stderr, "VkHook diag: GetSwapchainImagesKHR fetch result=%d fetched=%u\n",
                         static_cast<int>(getImagesResult), static_cast<unsigned int>(fetchedImageCount));
        }
        if (getImagesResult != VK_SUCCESS && getImagesResult != VK_INCOMPLETE)
        {
            Logger::err("vkGetSwapchainImagesKHR(images) failed: " + std::to_string(getImagesResult));
            error = getImagesResult;
            return nullptr;
        }
        if (fetchedImageCount == 0)
        {
            Logger::err("vkGetSwapchainImagesKHR fetched zero images");
            error = VK_ERROR_INITIALIZATION_FAILED;
            return nullptr;
        }
        realImages.resize(fetchedImageCount);

        SwapchainShared& shared = *skeleton->shared;
        auto next = skeleton->cloneBase();
        next->imageCount = fetchedImageCount;
        next->images     = std::move(realImages);

        // The generation that serves the real images and passes presents through.
        auto passthroughGeneration = [&](const char* why) {
            Logger::warn(std::string("swapchain is unusable for effects (") + why
                         + "), serving real images and passing presents through");
            shared.release(false);  // nothing of the partial build was ever used
            auto pt = skeleton->cloneBase();
            pt->imageCount  = next->imageCount;
            pt->images      = next->images;
            pt->passthrough = true;
            // In a present that mixes this swapchain with a faked one, the layer
            // hands the present a semaphore it signals (accountPresentWaits).
            shared.semaphores = createSemaphores(logicalDevice, pt->imageCount);
            pt->semaphores    = shared.semaphores;
            return pt;
        };

        // Swapchains the fake-image scheme cannot represent: zero-sized ones
        // (created transiently while a window or drawable is being rebuilt),
        // layered ones, and ones whose surface cannot take TRANSFER_DST.
        const bool zeroExtent = next->imageExtent.width == 0 || next->imageExtent.height == 0;
        const bool layered    = next->swapchainCreateInfo.imageArrayLayers > 1;
        if (skeleton->passthrough || zeroExtent || layered)
            return passthroughGeneration(skeleton->passthrough ? "surface lacks TRANSFER_DST" : zeroExtent ? "zero extent" : "multi-layer");
        // a surface neither backend processes gets the real images so it still presents
        if (!shared.input || !shared.input->alive())
            return passthroughGeneration("unsupported windowing system");

        // Image views of the real images, for overlay rendering
        shared.imageViews.assign(next->imageCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < next->imageCount; i++)
        {
            VkImageViewCreateInfo viewInfo = {};
            viewInfo.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image                           = next->images[i];
            viewInfo.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format                          = next->format;
            viewInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel   = 0;
            viewInfo.subresourceRange.levelCount     = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount     = 1;
            VkResult viewResult = logicalDevice->vkd.CreateImageView(logicalDevice->device, &viewInfo, nullptr, &shared.imageViews[i]);
            if (viewResult != VK_SUCCESS)
                Logger::err("Failed to create swapchain image view " + std::to_string(i) + ": " + std::to_string(viewResult));
        }

        // The first swapchain of the device starts its chain as soon as the registry's
        // initial build (on its worker) has the selection; later ones are resizes
        DeviceControlState& control = deviceControl(logicalDevice);
        const bool isFirstRun = !control.firstChainStarted.exchange(true, std::memory_order_acq_rel);

        // Three sets of images however long the chain is: the game-facing fakes and
        // the two ping-pong images the effects alternate between (H21).
        constexpr uint64_t kImageSets = 3;
        const uint64_t fakeImageCount64 = static_cast<uint64_t>(next->imageCount) * kImageSets;

        shared.fakeImages = createFakeSwapchainImages(logicalDevice, next->swapchainCreateInfo,
                                                      static_cast<uint32_t>(fakeImageCount64), shared.fakeImageMemories);
        if (shared.fakeImages.empty())
        {
            // Do not fail the fetch: the game gets its real images and keeps
            // presenting, the layer simply chains no effects on this swapchain.
            Logger::err("Failed to create fake swapchain images");
            return passthroughGeneration("fake images could not be allocated");
        }
        Logger::debug("created fake swapchain images");

        shared.semaphores        = createSemaphores(logicalDevice, next->imageCount);
        shared.overlaySemaphores = createSemaphores(logicalDevice, next->imageCount);
        if (shared.semaphores.empty() || shared.overlaySemaphores.empty())
            return passthroughGeneration("semaphores could not be created");

        // Per-image fences, signalled by that image's last layer submission: the
        // present path waits on them before re-recording that image's buffers.
        shared.effectFences.assign(next->imageCount, VK_NULL_HANDLE);
        shared.effectFenceUsed.assign(next->imageCount, 0);
        VkFenceCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        for (uint32_t i = 0; i < next->imageCount; ++i)
        {
            const VkResult fr = logicalDevice->vkd.CreateFence(logicalDevice->device, &fci, nullptr, &shared.effectFences[i]);
            if (fr != VK_SUCCESS)
            {
                Logger::err("Failed to create effect submit fence for image " + std::to_string(i) + ": " + std::to_string(fr));
                return passthroughGeneration("fences could not be created");
            }
        }

        // This swapchain's overlay target. A swapchain without one presents without
        // the overlay; it is not a reason to give up the effects.
        guardLayerWork(logicalDevice, "overlay renderer", [&] {
            OverlayUi* ui = ensureOverlayUi(logicalDevice, shared.input.get());
            if (ui)
                shared.overlay = OverlayRenderer::create(*ui, logicalDevice, next->format, next->imageCount);
        });

        // the swapchain-lifetime objects are in `shared`; the generation carries value copies
        next->imageViews         = shared.imageViews;
        next->fakeImages.assign(shared.fakeImages.begin(), shared.fakeImages.begin() + next->imageCount);
        next->pingImages.assign(shared.fakeImages.begin() + next->imageCount, shared.fakeImages.begin() + 2 * next->imageCount);
        next->pongImages.assign(shared.fakeImages.begin() + 2 * next->imageCount, shared.fakeImages.end());
        next->fakeImageMemories  = shared.fakeImageMemories;
        next->semaphores         = shared.semaphores;
        next->overlaySemaphores  = shared.overlaySemaphores;
        next->effectSubmitFences = shared.effectFences;

        // The first generation carries no effects (a pass-through chain): their
        // shaders compile on the registry's worker and present swaps the chain in
        // when they are done. A later swapchain (a resize) waits out the debounce.
        if (isFirstRun)
        {
            shared.chainWanted.store(true, std::memory_order_release);
        }
        else if (!control.effectRegistry.getSelectedEffects().empty())
        {
            // Resize with effects - use pass-through and debounce for smooth resize
            Logger::debug("using pass-through during resize, will restore effects after debounce");
            control.noteResize();
        }

        // the capture targets the effects will sample (only while depth capture is on)
        if (logicalDevice->depth)
            shared.depthCapture = DepthCapture::create(logicalDevice, next->imageExtent, next->imageCount);

        if (!buildGeneration(logicalDevice, *next, ChainPlan{}))
            return passthroughGeneration("the effect chain could not be built");

        Logger::debug("built chain generation " + std::to_string(next->generation) + " with "
                      + std::to_string(next->effects.size()) + " effects");
        return next;
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_CreateSwapchainKHR(VkDevice                        device,
                                                               const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                               const VkAllocationCallbacks*    pAllocator,
                                                               VkSwapchainKHR*                 swapchain)
    {
        Logger::trace("vkCreateSwapchainKHR");

        auto deviceRef = findDevice(GetKey(device));
        if (!deviceRef)
            return failUnknownObject("vkCreateSwapchainKHR", VK_ERROR_DEVICE_LOST);
        LogicalDevice* logicalDevice = deviceRef.get();

        // What the layer asks of the swapchain, decided before the real call. A
        // failure here forwards the app's request untouched: no effects, no hold.
        VkSwapchainCreateInfoKHR modifiedCreateInfo = *pCreateInfo;
        VkImageFormatListCreateInfoKHR imageFormatListCreateInfo = {};
        VkFormat formats[2] = {};
        bool useMutableFormat = false;
        bool fakeable         = false;
        std::shared_ptr<LogicalSwapchain> skeleton;
        guardLayerWork(logicalDevice, "vkCreateSwapchainKHR", [&] {
            const VkFormat format = modifiedCreateInfo.imageFormat;

            const VkFormat srgbFormat  = isSRGB(format) ? format : convertToSRGB(format);
            const VkFormat unormFormat = isSRGB(format) ? convertToUNORM(format) : format;
            Logger::debug(std::to_string(srgbFormat) + " " + std::to_string(unormFormat));
            formats[0] = unormFormat;
            formats[1] = srgbFormat;

            // The layer copies its result into the real images and the overlay
            // captures out of them: ask the surface what it allows, and only
            // fake a swapchain whose images can be a transfer destination.
            VkSurfaceCapabilitiesKHR surfaceCaps = {};
            VkResult capsResult = VK_ERROR_EXTENSION_NOT_PRESENT;
            if (logicalDevice->vki.GetPhysicalDeviceSurfaceCapabilitiesKHR)
                capsResult = logicalDevice->vki.GetPhysicalDeviceSurfaceCapabilitiesKHR(
                    logicalDevice->physicalDevice, pCreateInfo->surface, &surfaceCaps);
            const VkImageUsageFlags supported = capsResult == VK_SUCCESS ? surfaceCaps.supportedUsageFlags : 0;
            fakeable = (supported & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
            if (!fakeable)
            {
                Logger::warn("surface does not support TRANSFER_DST swapchain images (caps result "
                             + std::to_string(capsResult) + "); this swapchain stays pass-through");
                return;
            }

            bool hasMutableEnvOverride = false;
            useMutableFormat = getMutableSwapchainEnvOverride(hasMutableEnvOverride, logicalDevice->supportsMutableFormat);
            if (useMutableFormat && !logicalDevice->supportsMutableFormat)
            {
                Logger::warn("Mutable swapchain forced on via VKHOOK_ENABLE_MUTABLE_SWAPCHAIN, but device does not support it. Falling back to disabled.");
                useMutableFormat = false;
            }
            else if (useMutableFormat &&
                     pNextChainContainsSType(modifiedCreateInfo.pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO))
            {
                Logger::warn("Application already provides VkImageFormatListCreateInfo in swapchain pNext; disabling mutable override for compatibility.");
                useMutableFormat = false;
            }
            else if (useMutableFormat && !(supported & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
            {
                Logger::warn("Surface does not allow COLOR_ATTACHMENT swapchain images; disabling the mutable swapchain.");
                useMutableFormat = false;
            }
            else if (hasMutableEnvOverride)
            {
                Logger::info(std::string("Mutable swapchain explicitly set via VKHOOK_ENABLE_MUTABLE_SWAPCHAIN=") + (useMutableFormat ? "1" : "0"));
            }

            if (useMutableFormat)
            {
                // the mutable path renders its final pass directly into the swapchain
                modifiedCreateInfo.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                modifiedCreateInfo.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
                // TODO what if the application already uses multiple formats for the swapchain?

                imageFormatListCreateInfo.sType           = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR;
                imageFormatListCreateInfo.pNext           = modifiedCreateInfo.pNext;
                imageFormatListCreateInfo.viewFormatCount = (srgbFormat == unormFormat) ? 1 : 2;
                imageFormatListCreateInfo.pViewFormats    = formats;

                modifiedCreateInfo.pNext = &imageFormatListCreateInfo;
            }

            // The layer's copy writes the real images; the overlay's backdrop capture
            // blits out of them when the surface allows it (a strict widening of the
            // app's usage, valid on both swapchain paths).
            modifiedCreateInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (supported & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                modifiedCreateInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        });

        if (isSwapchainDiagEnabled())
        {
            std::fprintf(stderr,
                         "VkHook diag: CreateSwapchainKHR mutable=%d reqUsage=0x%x finalUsage=0x%x reqFlags=0x%x finalFlags=0x%x format=%u extent=%ux%u minImages=%u\n",
                         useMutableFormat ? 1 : 0,
                         static_cast<unsigned int>(pCreateInfo->imageUsage),
                         static_cast<unsigned int>(modifiedCreateInfo.imageUsage),
                         static_cast<unsigned int>(pCreateInfo->flags),
                         static_cast<unsigned int>(modifiedCreateInfo.flags),
                         static_cast<unsigned int>(modifiedCreateInfo.imageFormat),
                         static_cast<unsigned int>(modifiedCreateInfo.imageExtent.width),
                         static_cast<unsigned int>(modifiedCreateInfo.imageExtent.height),
                         static_cast<unsigned int>(modifiedCreateInfo.minImageCount));
        }

        Logger::debug("format " + std::to_string(modifiedCreateInfo.imageFormat));

        // The registered generation 0: the swapchain's identity with an empty chain.
        // The create-info is deep-copied (pQueueFamilyIndices points at storage the
        // application may free), and pNext is dropped: nothing after creation needs it.
        guardLayerWork(logicalDevice, "vkCreateSwapchainKHR", [&] {
            skeleton = std::make_shared<LogicalSwapchain>();
            skeleton->logicalDevice        = logicalDevice;
            skeleton->swapchainCreateInfo  = *pCreateInfo;
            skeleton->swapchainCreateInfo.pNext = nullptr;
            if (pCreateInfo->pQueueFamilyIndices && pCreateInfo->queueFamilyIndexCount > 0)
                skeleton->ownedQueueFamilyIndices.assign(pCreateInfo->pQueueFamilyIndices,
                                                         pCreateInfo->pQueueFamilyIndices + pCreateInfo->queueFamilyIndexCount);
            skeleton->swapchainCreateInfo.pQueueFamilyIndices =
                skeleton->ownedQueueFamilyIndices.empty() ? nullptr : skeleton->ownedQueueFamilyIndices.data();
            skeleton->imageExtent       = modifiedCreateInfo.imageExtent;
            skeleton->format            = modifiedCreateInfo.imageFormat;
            skeleton->imageCount        = 0;
            skeleton->useMutableFormat  = useMutableFormat;
            skeleton->passthrough       = !fakeable;
            skeleton->shared            = std::make_shared<SwapchainShared>(logicalDevice);
            skeleton->shared->input     = findSurfaceSession(pCreateInfo->surface);
        });

        // forwarded with no lock held
        const VkResult result = logicalDevice->vkd.CreateSwapchainKHR(device, fakeable ? &modifiedCreateInfo : pCreateInfo,
                                                                      pAllocator, swapchain);
        if (result != VK_SUCCESS || swapchain == nullptr || *swapchain == VK_NULL_HANDLE)
        {
            Logger::err("vkCreateSwapchainKHR failed: " + std::to_string(result));
            return result;
        }

        if (!skeleton)
            return result;  // the layer could not track it; the app still gets a working swapchain

        guardLayerWork(logicalDevice, "vkCreateSwapchainKHR", [&] {
            skeleton->handle = *swapchain;

            // The usable extent is known now; effects compile against it.
            deviceControl(logicalDevice).effectRegistry.setCompileExtent(skeleton->imageExtent.width, skeleton->imageExtent.height);

            std::shared_ptr<LogicalSwapchain> replaced;
            {
                std::lock_guard<std::mutex> l(logicalDevice->stateLock);
                // the same handle again means a recreation: the previous entry's
                // resources retire when its last reference goes
                auto it = logicalDevice->swapchains.find(*swapchain);
                if (it != logicalDevice->swapchains.end())
                {
                    Logger::warn("CreateSwapchainKHR: Recreating swapchain, retiring stale resources");
                    replaced = std::move(it->second);
                }
                logicalDevice->swapchains[*swapchain] = skeleton;
            }
        });

        return result;
    }

    VKAPI_ATTR VkResult VKAPI_CALL VkHook_GetSwapchainImagesKHR(VkDevice       device,
                                                                  VkSwapchainKHR swapchain,
                                                                  uint32_t*      pCount,
                                                                  VkImage*       swapchainImages)
    {
        if (pCount == nullptr)
            return VK_ERROR_INITIALIZATION_FAILED;
        VKHOOK_LOG_TRACE([&](std::string& s) { s = "vkGetSwapchainImagesKHR " + std::to_string(*pCount); });

        auto deviceRef = findDevice(GetKey(device));
        if (!deviceRef)
            return failUnknownObject("vkGetSwapchainImagesKHR", VK_ERROR_DEVICE_LOST);
        LogicalDevice* logicalDevice = deviceRef.get();

        if (swapchainImages == nullptr)
            return logicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, swapchainImages);

        auto current = findSwapchain(*logicalDevice, swapchain);
        if (!current || !current->shared)
            return logicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, swapchainImages);
        SwapchainShared& shared = *current->shared;

        // Only one thread builds the first generation; others wait for it (a
        // condition variable wait, no lock held and no GPU wait).
        {
            std::unique_lock<std::mutex> l(shared.buildLock);
            shared.buildDone.wait(l, [&] { return shared.build != SwapchainShared::Build::Building; });
            if (shared.build == SwapchainShared::Build::Done)
            {
                l.unlock();
                auto latest = findSwapchain(*logicalDevice, swapchain);
                if (!latest)
                    return VK_ERROR_OUT_OF_DATE_KHR;  // destroyed meanwhile
                return serveGeneration(*latest, pCount, swapchainImages);
            }
            shared.build = SwapchainShared::Build::Building;
        }

        bool built = false;
        struct Finish
        {
            SwapchainShared& shared;
            bool&            built;
            ~Finish()
            {
                std::lock_guard<std::mutex> l(shared.buildLock);
                // a failed build may be retried by the next call
                shared.build = built ? SwapchainShared::Build::Done : SwapchainShared::Build::None;
                shared.buildDone.notify_all();
            }
        } finish{shared, built};

        VkResult error = VK_SUCCESS;
        std::shared_ptr<LogicalSwapchain> first;
        try
        {
            first = buildFirstGeneration(logicalDevice, device, swapchain, current, error);
        }
        catch (const std::exception& e)
        {
            reportHookException(logicalDevice, "vkGetSwapchainImagesKHR", e.what());
        }
        catch (...)
        {
            reportHookException(logicalDevice, "vkGetSwapchainImagesKHR", nullptr);
        }

        if (!first)
        {
            if (error != VK_SUCCESS)
                return error;
            // The layer failed: the game gets the real images and presents pass through.
            shared.release(false);
            built = true;
            {
                std::lock_guard<std::mutex> l(logicalDevice->stateLock);
                current->passthrough = true;
            }
            return logicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, swapchainImages);
        }

        if (!installGeneration(*logicalDevice, swapchain, current, first))
            return VK_ERROR_OUT_OF_DATE_KHR;  // destroyed while the images were fetched
        built = true;

        if (first->faked())
            Logger::trace("vkGetSwapchainImagesKHR");
        return serveGeneration(*first, pCount, swapchainImages);
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
    {
        if (!swapchain)
            return;

        Logger::trace("vkDestroySwapchainKHR " + convertToString(swapchain));

        auto deviceRef = findDevice(GetKey(device));
        if (!deviceRef)
        {
            failUnknownObject("vkDestroySwapchainKHR", VK_ERROR_DEVICE_LOST);
            return;
        }
        LogicalDevice* logicalDevice = deviceRef.get();

        std::shared_ptr<LogicalSwapchain> removed;
        {
            std::lock_guard<std::mutex> l(logicalDevice->stateLock);
            auto it = logicalDevice->swapchains.find(swapchain);
            if (it != logicalDevice->swapchains.end())
            {
                removed = std::move(it->second);
                logicalDevice->swapchains.erase(it);
            }
        }

        // The layer's submissions that read or write this swapchain's images and
        // semaphores finish before they go: a bounded fence wait on the newest of
        // them, with no lock held. If that cannot be proven, the objects retire
        // through the RetireQueue instead of being destroyed here.
        if (removed && removed->shared)
        {
            guardLayerWork(logicalDevice, "vkDestroySwapchainKHR", [&] {
                const uint64_t serial = removed->shared->lastSerial.load(std::memory_order_acquire);
                const bool idle = serial == 0 || logicalDevice->queues.waitSerial(serial, 2000) == VK_SUCCESS;
                removed->shared->release(idle);
            });
        }

        if (logicalDevice->depth)
            logicalDevice->depth->forgetSelection(handleToU64(swapchain));
        logicalDevice->vkd.DestroySwapchainKHR(device, swapchain, pAllocator);
        // `removed` (the last generation) retires its pool, effects and depth resources here
    }

    const InterceptEntry kSwapchainIntercepts[] = {
        VKHOOK_INTERCEPT(Unconditional, CreateSwapchainKHR),
        VKHOOK_INTERCEPT(Unconditional, GetSwapchainImagesKHR),
        VKHOOK_INTERCEPT(Unconditional, DestroySwapchainKHR),
    };
} // namespace

    InterceptTable swapchainInterceptTable()
    {
        return makeInterceptTable(kSwapchainIntercepts);
    }
} // namespace VkHook
