#include "present/swapchain.hh"

#include "layer/entry.hh"
#include "layer/state.hh"
#include "layer/env.hh"

#include "control/frontend.hh"
#include "depth/tracker.hh"
#include "diag/gpu_crash.hh"

#include "input_blocker.hh"
#include "wayland_display.hh"
#include "fake_swapchain.hh"
#include "command_buffer.hh"
#include "format.hh"
#include "vk_handle.hh"
#include "util.hh"
#include "logger.hh"
#include "settings_manager.hh"
#include "config.hh"
#include "imgui_overlay.hh"
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

namespace VkHook
{
    // Helper function to create effects for a swapchain
    // This centralizes the effect creation logic used by both initial swapchain setup and hot-reload
    static void createEffectsForSwapchain(
        LogicalSwapchain* logicalSwapchain,
        LogicalDevice* logicalDevice,
        Config* config,
        const std::vector<std::string>& effectStrings,
        bool checkEnabledState = true)
    {
        const bool useMutableFormat = logicalSwapchain->useMutableFormat;

        if (logicalSwapchain->imageCount == 0)
        {
            Logger::err("Cannot create effects for swapchain with imageCount=0");
            return;
        }

        const size_t requiredSlots = effectStrings.empty()
            ? 1
            : effectStrings.size() + (useMutableFormat ? 0u : 1u);
        const size_t requiredFakeImages = static_cast<size_t>(logicalSwapchain->imageCount) * requiredSlots;
        if (logicalSwapchain->fakeImages.size() < requiredFakeImages)
        {
            Logger::err("Insufficient fake images for effect chain: have "
                        + std::to_string(logicalSwapchain->fakeImages.size()) + ", need "
                        + std::to_string(requiredFakeImages));
            return;
        }

        // If no effects, add pass-through so rendering still works
        if (effectStrings.empty())
        {
            std::vector<VkImage> firstImages(logicalSwapchain->fakeImages.begin(),
                                             logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount);
            logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(
                logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent,
                firstImages, logicalSwapchain->images, config));
            return;
        }

        // New scope for each swapchain build/reload: history must not alias an
        // old chain (possibly still in flight) or another swapchain's images.
        ReshadeTexturePool texturePool(logicalDevice);
        for (uint32_t i = 0; i < effectStrings.size(); i++)
        {
            VKHOOK_LOG_DEBUG([&](std::string& s) { s = "creating effect " + std::to_string(i) + ": " + effectStrings[i]; });

            // Calculate input images for this effect
            std::vector<VkImage> firstImages(logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount * i,
                                             logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount * (i + 1));

            // Calculate output images - last effect writes to swapchain or final fake images
            std::vector<VkImage> secondImages;
            if (i == effectStrings.size() - 1)
            {
                secondImages = useMutableFormat
                    ? logicalSwapchain->images
                    : std::vector<VkImage>(logicalSwapchain->fakeImages.end() - logicalSwapchain->imageCount,
                                           logicalSwapchain->fakeImages.end());
            }
            else
            {
                secondImages = std::vector<VkImage>(logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount * (i + 1),
                                                    logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount * (i + 2));
            }

            // Check if effect should be skipped (disabled or failed)
            bool effectFailed = effectRegistry.hasEffectFailed(effectStrings[i]);
            bool effectDisabled = checkEnabledState && !effectRegistry.isEffectEnabled(effectStrings[i]);

            if (effectFailed || effectDisabled)
            {
                VKHOOK_LOG_DEBUG([&](std::string& s) {
                    s = "effect " + std::string(effectFailed ? "failed" : "disabled") + ", using pass-through: " + effectStrings[i];
                });
                logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent, firstImages, secondImages, config));
                continue;
            }

            if (isDlss5Effect(effectStrings[i], config))
            {
                try
                {
#ifdef VKHOOK_DLSS5
                    const std::vector<VkImage> motionSources(logicalSwapchain->fakeImages.begin(),
                        logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount);
                    logicalSwapchain->effects.push_back(std::make_shared<Dlss5Effect>(
                        logicalDevice, logicalSwapchain->format, logicalSwapchain->swapchainCreateInfo.imageColorSpace,
                        logicalSwapchain->imageExtent, firstImages, secondImages, motionSources,
                        &effectRegistry, config, effectStrings[i]));
#else
                    throw std::runtime_error("DLSS5 backend not built; configure Meson with -Ddlss5=enabled and its build dependencies");
#endif
                }
                catch (const std::exception& e)
                {
                    Logger::err("Failed to create DLSS5 effect: " + std::string(e.what()));
                    effectRegistry.setEffectError(effectStrings[i], e.what());
                    logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(
                        logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent, firstImages, secondImages, config));
                }
                continue;
            }

            // ReShade effect. A build that fails (compile error, driver refusal, any
            // exception) disables this effect only: it is replaced by a
            // pass-through and the registry records why. Nothing here may take the
            // process down; the embedded compiler reports failures as errors.
            std::string effectPath = effectRegistry.getEffectFilePath(effectStrings[i]);
            auto customDefs = effectRegistry.getPreprocessorDefs(effectStrings[i]);

            auto disableEffect = [&](const std::string& why)
            {
                Logger::err("Failed to create ReshadeEffect " + effectStrings[i] + ": " + why);
                effectRegistry.setEffectError(effectStrings[i], why);
                logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(
                    logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent, firstImages, secondImages, config));
            };

            try
            {
                auto reshadeEffect = std::make_shared<ReshadeEffect>(
                    logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent,
                    firstImages, secondImages, &effectRegistry, texturePool, effectStrings[i], effectPath, customDefs);
                logicalSwapchain->effects.push_back(reshadeEffect);
                if (reshadeEffect->getOutputWrites() == 0)
                {
                    Logger::debug("deterministic forwarding for zero-output-write effect: " + effectStrings[i]);
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
                std::vector<VkImage>(logicalSwapchain->fakeImages.end() - logicalSwapchain->imageCount, logicalSwapchain->fakeImages.end()),
                logicalSwapchain->images, config));
        }
    }

    // Helper function to reload effects for a swapchain (for hot-reload)
    static void reloadEffectsForSwapchain(LogicalSwapchain* logicalSwapchain, Config* config,
                                   const std::vector<std::string>& activeEffects = {})
    {
        LogicalDevice* logicalDevice = logicalSwapchain->logicalDevice;

        // the caller has already established that this swapchain's in-flight
        // effect submissions are done (depthRebuildFencesReady), so there is no
        // QueueWaitIdle here: freeing command buffers or destroying effect
        // resources while the GPU still references them is what the fence check
        // exists to prevent, and a global wait would stall the present thread.

        // Clear effects (command buffers will be freed by reallocateCommandBuffers)
        logicalSwapchain->effects.clear();
        logicalSwapchain->defaultTransfer.reset();

        // Retired effect handles are now in the deferred queue. The drain happens
        // once, after every swapchain of the device has been reloaded and the
        // queue is idled — see reloadAllSwapchains.

        // Use provided active effects list directly - no fallback to config
        // Registry is the single source of truth (initialized at first swapchain creation)
        std::vector<std::string> effectStrings = activeEffects;

        // Check if we have enough fake images for the effects
        // Fake images are allocated at swapchain creation based on maxEffectSlots
        if (effectStrings.size() > logicalSwapchain->maxEffectSlots)
        {
            Logger::warn("Cannot add more effects than maxEffectSlots (" +
                        std::to_string(effectStrings.size()) + " > " + std::to_string(logicalSwapchain->maxEffectSlots) +
                        "). Increase maxEffects in config.");
            effectStrings.resize(logicalSwapchain->maxEffectSlots);
        }

        Logger::info("reloading " + std::to_string(effectStrings.size()) + " effects");

        // Create effects using centralized helper
        createEffectsForSwapchain(logicalSwapchain, logicalDevice, config, effectStrings, true);

        // Create default transfer effect (needed for no-effect command buffers)
        logicalSwapchain->defaultTransfer = std::make_shared<TransferEffect>(
            logicalDevice,
            logicalSwapchain->format,
            logicalSwapchain->imageExtent,
            std::vector<VkImage>(logicalSwapchain->fakeImages.begin(), logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount),
            logicalSwapchain->images,
            config);

        // Free old command buffers and allocate/write new ones
        DepthState depth = getDepthState(logicalDevice);
        reallocateCommandBuffers(logicalDevice, logicalSwapchain, depth);

        Logger::info("effects reloaded successfully");
    }


    // Reload effects for all swapchains belonging to a device
    void reloadAllSwapchains(LogicalDevice* logicalDevice, const std::vector<std::string>& activeEffects)
    {
        bool reloaded = false;
        for (auto& [_, logicalSwapchain] : swapchainMap)
        {
            if (!logicalSwapchain->fakeImages.empty()
                && (!logicalDevice || logicalSwapchain->logicalDevice == logicalDevice))
            {
                reloadEffectsForSwapchain(logicalSwapchain.get(), config.get(), activeEffects);
                reloaded = true;
            }
        }

        // Every reload handed the retired effects' handles to the deferred queue.
        // The caller's fence gate proved the effect submissions finished, but the
        // queue is global and this is a free of device memory, not a command
        // buffer: idle the queue first so nothing the GPU still reads is
        // destroyed. Without this, a reload orphans a whole effect's images until
        // the next swapchain teardown (a VRAM leak); draining without the idle is
        // a device loss.
        if (reloaded && logicalDevice)
        {
            // only flush once the idle completed; a stuck queue means the
            // releases have to wait for a later safe point, never run now
            if (waitQueueIdleBounded(logicalDevice, "effect reload flush"))
                DeferredDestroyQueue::instance().flush();
        }
    }

    // Extent of a live, non-passthrough swapchain for this device, i.e. the
    // window size the layer is actually presenting to. Used by the overlay to
    // default a fresh manual depth pin to the matching buffer; callers must
    // hold globalLock.
    VkExtent2D currentSwapchainExtentLocked(LogicalDevice* logicalDevice)
    {
        for (const auto& [_, sc] : swapchainMap)
            if (sc && sc->logicalDevice == logicalDevice && !sc->passthrough && sc->imageCount > 0)
                return sc->imageExtent;
        return VkExtent2D{0, 0};
    }

namespace
{
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

        VkSwapchainCreateInfoKHR modifiedCreateInfo = *pCreateInfo;

        VkFormat format = modifiedCreateInfo.imageFormat;

        VkFormat srgbFormat  = isSRGB(format) ? format : convertToSRGB(format);
        VkFormat unormFormat = isSRGB(format) ? convertToUNORM(format) : format;
        Logger::debug(std::to_string(srgbFormat) + " " + std::to_string(unormFormat));

        VkFormat formats[] = {unormFormat, srgbFormat};

        VkImageFormatListCreateInfoKHR imageFormatListCreateInfo;
        bool hasMutableEnvOverride = false;
        bool useMutableFormat = getMutableSwapchainEnvOverride(hasMutableEnvOverride, logicalDevice->supportsMutableFormat);
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
        else if (hasMutableEnvOverride)
        {
            Logger::info(std::string("Mutable swapchain explicitly set via VKHOOK_ENABLE_MUTABLE_SWAPCHAIN=") + (useMutableFormat ? "1" : "0"));
        }

        if (useMutableFormat)
        {
            // Keep application-requested usage bits and add the ones VkHook needs.
            modifiedCreateInfo.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                             ; // mutable path renders final pass directly into swapchain
            modifiedCreateInfo.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
            // TODO what if the application already uses multiple formats for the swapchain?

            imageFormatListCreateInfo.sType           = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR;
            imageFormatListCreateInfo.pNext           = modifiedCreateInfo.pNext;
            imageFormatListCreateInfo.viewFormatCount = (srgbFormat == unormFormat) ? 1 : 2;
            imageFormatListCreateInfo.pViewFormats    = formats;

            modifiedCreateInfo.pNext = &imageFormatListCreateInfo;
        }

        // The overlay's backdrop capture blits out of the real swapchain image, so
        // it must be usable as a transfer source. This is a strict widening of the
        // app's requested usage and is valid on both swapchain paths.
        modifiedCreateInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        // Keep application-provided usage bits unchanged on the non-mutable path.
        // Some drivers/apps are sensitive to swapchain usage mutation.

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
        
        // Recreation (same handle) is detected AFTER vkCreateSwapchainKHR returns,
        // since *swapchain is uninitialized until then.
        auto logicalSwapchain = std::make_shared<LogicalSwapchain>();
        logicalSwapchain->logicalDevice      = logicalDevice;
        logicalSwapchain->swapchainCreateInfo = *pCreateInfo;
        logicalSwapchain->imageExtent         = modifiedCreateInfo.imageExtent;
        logicalSwapchain->format              = modifiedCreateInfo.imageFormat;
        logicalSwapchain->imageCount          = 0;
        logicalSwapchain->useMutableFormat    = useMutableFormat;

        // forwarded with the lock released
        VkResult result = logicalDevice->vkd.CreateSwapchainKHR(device, &modifiedCreateInfo, pAllocator, swapchain);

        scoped_lock l(globalLock);
        if (result == VK_SUCCESS && swapchain != nullptr && *swapchain != VK_NULL_HANDLE)
        {
            // NOW check if this handle already existed (recreation scenario)
            const VkSwapchainKHR handle = *swapchain;
            auto oldIt = swapchainMap.find(handle);
            
            // Since swapchain handles are opaque, we use the map key directly
            // If an entry with this key exists, we're recreating
            if (oldIt != swapchainMap.end() && oldIt->second)
            {
                Logger::warn("CreateSwapchainKHR: Recreating swapchain, destroying stale resources");

                // destroy the retired entry before dropping our last reference. overwriting
                // it leaked and raced every in-flight resource on each resize,
                // which is how rapid rebuild churn turned into device-lost.
                if (oldIt->second.get() != logicalSwapchain.get())
                    oldIt->second->destroy();

                // Replace the old entry
                swapchainMap[handle] = logicalSwapchain;
            }
            else
            {
                swapchainMap[handle] = logicalSwapchain;
            }
            
            // clear depth state so nothing stale survives into the new swapchain
            logicalSwapchain->depthResolveSourceView = VK_NULL_HANDLE;
            logicalSwapchain->depthReallocPending = true;
            
            logicalSwapchain->depthResolvePerImage.resize(modifiedCreateInfo.minImageCount);
            for (auto& perImg : logicalSwapchain->depthResolvePerImage) perImg.image = VK_NULL_HANDLE;
            logicalSwapchain->depthResolveTransformedPerImage.resize(modifiedCreateInfo.minImageCount);
            for (auto& perImg : logicalSwapchain->depthResolveTransformedPerImage) perImg.image = VK_NULL_HANDLE;
            
            // Also clear device-level depth state when swapchain is recreated (Roblox OTA scenario)
            logicalDevice->activeDepthState = {};
            logicalDevice->pinnedDepthImageView = VK_NULL_HANDLE;
            logicalDevice->depthReallocPending = false;
            // The previous generation's depth resources die with the old
            // swapchain: hold the depth path off until the app has re-created
            // and re-rendered its depth, so no deferred copy or resolve build
            // ever executes against a dying image (that was the NVRM Xid 109
            // context-switch timeout at resize).
            // the old generation's depth resources die with the swapchain;
            // without the hold the GPU executed against a freed image
            armDepthTransitionHoldLocked(logicalDevice, "swapchain rebuilt");
            depthRetryStates.erase(logicalDevice);
            Logger::debug("CreateSwapchainKHR: cleared device depth state for fresh detection");
        }
        else
        {
            Logger::err("vkCreateSwapchainKHR failed: " + std::to_string(result));
        }

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

        auto swapchainRef = findSwapchain(swapchain);
        if (!swapchainRef)
            return logicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, pCount, swapchainImages);
        LogicalSwapchain* logicalSwapchain = swapchainRef.get();

        // If the images got already requested once, return them again instead of
        // creating new images. Caller holds globalLock.
        auto serveCachedFakeImages = [&](VkResult& result) -> bool
        {
            if (logicalSwapchain->fakeImages.empty())
                return false;
            if (logicalSwapchain->fakeImages.size() < logicalSwapchain->imageCount)
            {
                Logger::err("fake image cache is smaller than imageCount");
                result = VK_ERROR_OUT_OF_HOST_MEMORY;
                return true;
            }
            *pCount = std::min<uint32_t>(*pCount, logicalSwapchain->imageCount);
            std::memcpy(swapchainImages, logicalSwapchain->fakeImages.data(), sizeof(VkImage) * (*pCount));
            result = *pCount < logicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
            return true;
        };
        {
            scoped_lock l(globalLock);
            VkResult cached = VK_SUCCESS;
            if (serveCachedFakeImages(cached))
                return cached;
        }

        // The real images are fetched from the next layer with the lock released.
        const uint32_t requestedImageCapacity = *pCount;
        uint32_t realImageCount = 0;
        VkResult getCountResult = logicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, &realImageCount, nullptr);
        if (isSwapchainDiagEnabled())
        {
            std::fprintf(stderr,
                         "VkHook diag: GetSwapchainImagesKHR count-query result=%d count=%u\n",
                         static_cast<int>(getCountResult),
                         static_cast<unsigned int>(realImageCount));
        }
        if (getCountResult != VK_SUCCESS && getCountResult != VK_INCOMPLETE)
        {
            Logger::err("vkGetSwapchainImagesKHR(count) failed: " + std::to_string(getCountResult));
            return getCountResult;
        }
        if (realImageCount == 0)
        {
            Logger::err("vkGetSwapchainImagesKHR returned zero images");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        std::vector<VkImage> realImages(realImageCount);
        uint32_t fetchedImageCount = realImageCount;
        VkResult getImagesResult = logicalDevice->vkd.GetSwapchainImagesKHR(device, swapchain, &fetchedImageCount, realImages.data());
        if (isSwapchainDiagEnabled())
        {
            std::fprintf(stderr,
                         "VkHook diag: GetSwapchainImagesKHR fetch result=%d fetched=%u\n",
                         static_cast<int>(getImagesResult),
                         static_cast<unsigned int>(fetchedImageCount));
        }
        if (getImagesResult != VK_SUCCESS && getImagesResult != VK_INCOMPLETE)
        {
            Logger::err("vkGetSwapchainImagesKHR(images) failed: " + std::to_string(getImagesResult));
            return getImagesResult;
        }
        if (fetchedImageCount == 0)
        {
            Logger::err("vkGetSwapchainImagesKHR fetched zero images");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        realImages.resize(fetchedImageCount);

        std::unique_lock<std::mutex> lock(globalLock);
        if (findSwapchainLocked(swapchain) != swapchainRef)
            return VK_ERROR_OUT_OF_DATE_KHR;  // destroyed while the images were fetched
        {
            VkResult cached = VK_SUCCESS;
            if (serveCachedFakeImages(cached))
                return cached;
        }
        logicalSwapchain->imageCount = fetchedImageCount;
        logicalSwapchain->images = std::move(realImages);

        // Swapchains the fake-image scheme cannot represent: zero-sized ones
        // (created transiently while a window or drawable is being rebuilt) and
        // layered ones. Serve the real images and pass presents through instead
        // of failing a fetch the game is not prepared to recover from.
        const bool zeroExtent = logicalSwapchain->imageExtent.width == 0 || logicalSwapchain->imageExtent.height == 0;
        const bool layered = logicalSwapchain->swapchainCreateInfo.imageArrayLayers > 1;
        if (zeroExtent || layered)
        {
            Logger::warn(std::string("swapchain is unusable for effects (")
                         + (zeroExtent ? "zero extent" : "multi-layer")
                         + "), serving real images and passing presents through");
            logicalSwapchain->passthrough = true;
        }

        // a surface neither backend processes gets the real images so it still
        // presents. wayland and x11 both get the fakes below; the present path
        // copies them into the real swapchain.
        if (logicalSwapchain->passthrough || (!isWayland() && !isX11()))
        {
            *pCount = std::min<uint32_t>(*pCount, logicalSwapchain->imageCount);
            std::memcpy(swapchainImages, logicalSwapchain->images.data(), sizeof(VkImage) * (*pCount));
            return *pCount < logicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
        }

        // Per-swapchain command pool, created before anything else so a driver
        // that refuses it degrades to passthrough before any state exists.
        {
            VkCommandPoolCreateInfo poolCI = {};
            poolCI.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolCI.flags            = 0;
            poolCI.queueFamilyIndex = logicalDevice->queueFamilyIndex;
            VkResult poolResult = logicalDevice->vkd.CreateCommandPool(
                logicalDevice->device, &poolCI, nullptr, &logicalSwapchain->commandPool);
            if (poolResult != VK_SUCCESS || logicalSwapchain->commandPool == VK_NULL_HANDLE)
            {
                Logger::err("Failed to create per-swapchain command pool (" + std::to_string(poolResult)
                            + "), serving real images and passing presents through");
                logicalSwapchain->commandPool = VK_NULL_HANDLE;
                logicalSwapchain->passthrough = true;
                *pCount = std::min<uint32_t>(*pCount, logicalSwapchain->imageCount);
                std::memcpy(swapchainImages, logicalSwapchain->images.data(), sizeof(VkImage) * (*pCount));
                return *pCount < logicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
            }
        }

        // Create image views for overlay rendering
        logicalSwapchain->imageViews.resize(logicalSwapchain->imageCount);
        for (uint32_t i = 0; i < logicalSwapchain->imageCount; i++)
        {
            VkImageViewCreateInfo viewInfo = {};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = logicalSwapchain->images[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = logicalSwapchain->format;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = 0;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.baseArrayLayer = 0;
            viewInfo.subresourceRange.layerCount = 1;
            VkResult viewResult = logicalDevice->vkd.CreateImageView(logicalDevice->device, &viewInfo, nullptr, &logicalSwapchain->imageViews[i]);
            if (viewResult != VK_SUCCESS)
                Logger::err("Failed to create swapchain image view " + std::to_string(i) + ": " + std::to_string(viewResult));
        }

        // Initialize registry from config on first run (before calculating effect slots)
        bool isFirstRun = !effectRegistry.isInitializedFromConfig();
        if (isFirstRun)
            effectRegistry.initializeSelectedEffectsFromConfig();

        const auto& selectedEffects = effectRegistry.getSelectedEffects();

        // Allow dynamic effect loading by allocating for more effects than configured.
        // Clamp maxEffects to a safe range to avoid pathological allocations.
        int32_t maxEffects = std::clamp(settingsManager.getMaxEffects(), 1, 200);
        size_t effectSlots = std::max(selectedEffects.size(), static_cast<size_t>(maxEffects));
        logicalSwapchain->maxEffectSlots = effectSlots;

        // create 1 more set of images when we can't use the swapchain itself
        const uint64_t slotCount = static_cast<uint64_t>(effectSlots) + (logicalSwapchain->useMutableFormat ? 0u : 1u);
        const uint64_t fakeImageCount64 = static_cast<uint64_t>(logicalSwapchain->imageCount) * slotCount;
        if (fakeImageCount64 == 0 || fakeImageCount64 > std::numeric_limits<uint32_t>::max())
        {
            Logger::err("Invalid fake image count computed: " + std::to_string(fakeImageCount64));
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        uint32_t fakeImageCount = static_cast<uint32_t>(fakeImageCount64);

        logicalSwapchain->fakeImages =
            createFakeSwapchainImages(logicalDevice, logicalSwapchain->swapchainCreateInfo, fakeImageCount, logicalSwapchain->fakeImageMemories);
        if (logicalSwapchain->fakeImages.empty())
        {
            // Do not fail the fetch: the game gets its real images and keeps
            // presenting, the layer simply chains no effects on this swapchain.
            Logger::err("Failed to create fake swapchain images; serving real images and passing presents through");
            logicalSwapchain->passthrough = true;
            *pCount = std::min<uint32_t>(*pCount, logicalSwapchain->imageCount);
            std::memcpy(swapchainImages, logicalSwapchain->images.data(), sizeof(VkImage) * (*pCount));
            return *pCount < logicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
        }
        Logger::debug("created fake swapchain images");

        if (!isFirstRun && !selectedEffects.empty())
        {
            // Resize with effects - use pass-through and debounce for smooth resize
            Logger::debug("using pass-through during resize, will restore effects after debounce");
            std::vector<VkImage> firstImages(logicalSwapchain->fakeImages.begin(),
                                             logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount);
            logicalSwapchain->effects.push_back(std::make_shared<TransferEffect>(
                logicalDevice, logicalSwapchain->format, logicalSwapchain->imageExtent,
                firstImages, logicalSwapchain->images, config.get()));

            resizeDebounce.pending = true;
            resizeDebounce.lastResizeTime = std::chrono::steady_clock::now();
        }
        else
        {
            // First run OR empty effects - create effects from registry
            createEffectsForSwapchain(logicalSwapchain, logicalDevice, config.get(), selectedEffects, true);
        }

        DepthState depth = getDepthState(logicalDevice);

        VKHOOK_LOG_DEBUG([&](std::string& s) {
            s = "selected effect count: " + std::to_string(selectedEffects.size())
                + ", effect count: " + std::to_string(logicalSwapchain->effects.size());
        });

        logicalSwapchain->commandBuffersEffect = allocateCommandBuffer(logicalDevice, logicalSwapchain->imageCount, logicalSwapchain->commandPool);
        Logger::debug("allocated ComandBuffers " + std::to_string(logicalSwapchain->commandBuffersEffect.size()) + " for swapchain "
                      + convertToString(swapchain));

        ensureDepthResolveResources(logicalSwapchain, depth);
        writeCommandBuffers(logicalDevice,
                            logicalSwapchain,
                            logicalSwapchain->effects,
                            logicalSwapchain->commandBuffersEffect,
                            depth);
        Logger::debug("wrote CommandBuffers");

        logicalSwapchain->semaphores = createSemaphores(logicalDevice, logicalSwapchain->imageCount);
        logicalSwapchain->overlaySemaphores = createSemaphores(logicalDevice, logicalSwapchain->imageCount);

        // Create per-image fences for effect CB submission tracking.
        // These ensure we don't update descriptor sets or free CBs while
        // the GPU is still using them (which causes VK_ERROR_DEVICE_LOST).
        logicalSwapchain->effectSubmitFences.resize(logicalSwapchain->imageCount, VK_NULL_HANDLE);
        logicalSwapchain->effectSubmitFenceUsed.resize(logicalSwapchain->imageCount, false);
        VkFenceCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        for (uint32_t i = 0; i < logicalSwapchain->imageCount; ++i)
        {
            VkResult fr = logicalDevice->vkd.CreateFence(logicalDevice->device, &fci, nullptr, &logicalSwapchain->effectSubmitFences[i]);
            if (fr != VK_SUCCESS)
            {
                Logger::err("Failed to create effect submit fence for image " + std::to_string(i) + ": " + std::to_string(fr));
                logicalSwapchain->effectSubmitFences[i] = VK_NULL_HANDLE;
            }
        }

        Logger::debug("created semaphores + fences");
        for (unsigned int i = 0; i < logicalSwapchain->imageCount; i++)
        {
            Logger::debug(std::to_string(i) + " written commandbuffer " + convertToString(logicalSwapchain->commandBuffersEffect[i]));
        }
        Logger::trace("vkGetSwapchainImagesKHR");

        logicalSwapchain->defaultTransfer = std::make_shared<TransferEffect>(
            logicalDevice,
            logicalSwapchain->format,
            logicalSwapchain->imageExtent,
            std::vector<VkImage>(logicalSwapchain->fakeImages.begin(), logicalSwapchain->fakeImages.begin() + logicalSwapchain->imageCount),
            logicalSwapchain->images,
            config.get());

        logicalSwapchain->commandBuffersNoEffect = allocateCommandBuffer(logicalDevice, logicalSwapchain->imageCount, logicalSwapchain->commandPool);

        writeCommandBuffers(logicalDevice,
                            logicalSwapchain,
                            {logicalSwapchain->defaultTransfer},
                            logicalSwapchain->commandBuffersNoEffect,
                            depth);

        for (unsigned int i = 0; i < logicalSwapchain->imageCount; i++)
        {
            Logger::debug(std::to_string(i) + " written commandbuffer " + convertToString(logicalSwapchain->commandBuffersNoEffect[i]));
        }

        // Create ImGui overlay at device level (if not already created)
        // This survives swapchain recreation during resize
        if (!logicalDevice->imguiOverlay)
        {
            if (!logicalDevice->overlayPersistentState)
                logicalDevice->overlayPersistentState = std::make_unique<OverlayPersistentState>();
            logicalDevice->imguiOverlay = std::make_unique<ImGuiOverlay>(
                logicalDevice, logicalSwapchain->format, logicalSwapchain->imageCount,
                logicalDevice->overlayPersistentState.get());
            // Set the effect registry pointer (single source of truth for enabled states)
            logicalDevice->imguiOverlay->setEffectRegistry(&effectRegistry);

            // Set game/profile info for auto-save
            logicalDevice->imguiOverlay->setGameProfile(detectedGameName, activeProfileName, activeProfilePath);

            // Initialize input blocking (grabs all input when overlay is visible)
            // the overlay is created once per device, so this runs once per device
            initInputBlocker(settingsManager.getOverlayBlockInput());
            setInputBlocked(logicalDevice->imguiOverlay->isVisible());
        }

        if (logicalSwapchain->fakeImages.size() < logicalSwapchain->imageCount)
        {
            Logger::err("fake image vector too small for swapchain copy");
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        *pCount = std::min<uint32_t>(requestedImageCapacity, logicalSwapchain->imageCount);
        std::memcpy(swapchainImages, logicalSwapchain->fakeImages.data(), sizeof(VkImage) * (*pCount));
        return requestedImageCapacity < logicalSwapchain->imageCount ? VK_INCOMPLETE : VK_SUCCESS;
    }

    VKAPI_ATTR void VKAPI_CALL VkHook_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
    {
        if (!swapchain)
            return;

        Logger::trace("vkDestroySwapchainKHR " + convertToString(swapchain));

        std::shared_ptr<LogicalDevice> deviceRef;
        {
            scoped_lock l(globalLock);
            // we need to delete the infos of the oldswapchain
            auto it = swapchainMap.find(swapchain);
            if (it != swapchainMap.end() && it->second)
                it->second->destroy();
            swapchainMap.erase(swapchain);
            deviceRef = findDeviceLocked(GetKey(device));
        }
        // forwarded with the lock released
        if (deviceRef)
            deviceRef->vkd.DestroySwapchainKHR(device, swapchain, pAllocator);
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
