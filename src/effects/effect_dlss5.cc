#include "effect_dlss5.hh"
#include "effect_registry.hh"
#include "logical_device.hh"
#include "layer/registry.hh"
#include "config.hh"
#include "config_serializer.hh"
#include "logger.hh"
#include "dlss5_paths.hh"
#include "dlss5_pipeline.hh"
#include "nr_model.h"
#include <algorithm>
#include <dlfcn.h>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace VkHook
{
    namespace
    {
        std::string pathOption(Config* config, const char* key, const char* environment, const std::string& fallback)
        {
            const char* value = std::getenv(environment);
            return config->getOption<std::string>(key, value && *value ? value : fallback);
        }

        // The optical-flow input reduction per axis; 0 lets the pipeline choose from the frame size.
        uint32_t flowDownscaleOption(Config* config)
        {
            const std::string value = config->getOption<std::string>("dlss5FlowResolution", "auto");
            if (value == "full") return 1;
            if (value == "half") return 2;
            if (value == "quarter") return 4;
            if (value != "auto")
                Logger::warn("dlss5FlowResolution = " + value + " is not auto, full, half or quarter; using auto");
            return 0;
        }

        std::string installedAssets(const char* kind, const char* relativePath, const char* systemPath)
        {
            // Resolve loader-created symlinks before looking beside the library:
            // pressure-vessel remaps the original installation under /run/parent.
            Dl_info module{};
            if (dladdr(reinterpret_cast<void*>(&installedAssets), &module) && module.dli_fname)
            {
                std::error_code error;
                auto library = std::filesystem::canonical(module.dli_fname, error);
                if (error) library = module.dli_fname;
                const auto directory = library.parent_path();
                // User-local installs keep assets beside each architecture's .so.
                const auto adjacent = directory / "dlss5" / kind;
                if (std::filesystem::is_directory(adjacent)) return adjacent.string();
                // Preserve Meson's libdir/datadir relationship after relocation.
                const auto installed = (directory / relativePath).lexically_normal();
                if (std::filesystem::is_directory(installed)) return installed.string();
            }
            return systemPath;
        }
    }

    namespace
    {
        // The loaded network and its weights on the GPU. The model is read from
        // disk, hash-checked and uploaded once per (device, model path); every
        // chain rebuild reuses it instead of paying that again. It owns the
        // context its buffers were created through, so it stays valid however
        // many effects come and go. An effect still builds its own context for
        // its kernels, graph and descriptors, which die with the effect.
        struct ModelEntry
        {
            std::unique_ptr<vk::Context> context;
            std::unique_ptr<nr::Model>   model; // after the context: destroyed first
            // The model uploads weights lazily while a graph is built.
            std::mutex use;
        };

        // Keyed by the device explicitly: the network's Vulkan context and weights
        // belong to one VkDevice, so two devices (or a device recreated at the same
        // address after a destroy) never share an entry.
        using ModelKey = std::pair<VkDevice, std::string>;

        // Process-wide, because the effects are built from the layer's hooks with no
        // per-device object to hang this on (the registry is a core-owned global
        // until core makes it per-device; once it does, this moves into it). The
        // lifetime is bounded by the device key: an entry is created by the first
        // DLSS5 effect built on a device and is released by clearDlss5ModelCache at
        // vkDestroyDevice, while the device is alive. Nothing is released at process
        // exit, deliberately: the devices are already gone and freeing GPU objects
        // then would call into a dead driver.
        struct ModelCache
        {
            std::mutex                                   mutex;
            std::map<ModelKey, std::shared_ptr<ModelEntry>> entries;
        };

        // Constructed on first use and never destroyed (see ModelCache). Not
        // function-local mutable state of a hook: it is only reached through
        // acquireModel/clearDlss5ModelCache with an explicit device key.
        ModelCache& modelCache()
        {
            static ModelCache* cache = new ModelCache;
            return *cache;
        }

        std::shared_ptr<ModelEntry> acquireModel(LogicalDevice* device, const std::string& modelPath)
        {
            ModelCache&    cache = modelCache();
            const ModelKey key{device->device, modelPath};
            {
                std::lock_guard<std::mutex> lock(cache.mutex);
                const auto                  found = cache.entries.find(key);
                if (found != cache.entries.end())
                    return found->second;
            }

            // Loaded outside the lock: reading and hashing the weights is slow
            // and must not hold up another device's lookup.
            auto entry = std::make_shared<ModelEntry>();
            entry->context = std::make_unique<vk::Context>(device->instance, device->physicalDevice, device->device,
                device->queueFamilyIndex, 0, device->downstreamGIPA, device->downstreamGDPA);
            entry->model = std::make_unique<nr::Model>(*entry->context, modelPath);

            std::lock_guard<std::mutex> lock(cache.mutex);
            // A concurrent build may have won; keep one and drop the other.
            return cache.entries.emplace(key, std::move(entry)).first->second;
        }
    } // namespace

    void clearDlss5ModelCache(VkDevice device)
    {
        std::vector<std::shared_ptr<ModelEntry>> released;
        {
            ModelCache&                 cache = modelCache();
            std::lock_guard<std::mutex> lock(cache.mutex);
            for (auto it = cache.entries.begin(); it != cache.entries.end();)
            {
                if (it->first.first == device)
                {
                    released.push_back(std::move(it->second));
                    it = cache.entries.erase(it);
                }
                else
                    ++it;
            }
        }
        // Destroyed here, outside the lock, while the device is still alive.
        released.clear();
    }

    struct Dlss5Effect::State
    {
        EffectRegistry* registry = nullptr;
        std::string name;
        std::unique_ptr<vk::Context> context;
        std::shared_ptr<ModelEntry> model;
        // After the context and the model: destroyed first.
        std::unique_ptr<Dlss5Pipeline> pipeline;
    };

    Dlss5Effect::Dlss5Effect(LogicalDevice* device, VkFormat format, VkColorSpaceKHR colorSpace,
                             VkExtent2D extent, const std::vector<VkImage>& input,
                             const std::vector<VkImage>& output, const std::vector<VkImage>& motionSources,
                             EffectRegistry* registry,
                             Config* config, const std::string& name)
        : state(std::make_unique<State>())
    {
        if (!device->dlss5Supported)
            throw std::runtime_error(device->dlss5UnavailableReason);
        const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
        if ((!bgra && format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB)
            || colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            throw std::runtime_error("DLSS5 requires an SDR RGBA8/BGRA8 swapchain; HDR is not a display proxy");
        const std::string modelPath = pathOption(config, "dlss5ModelPath", "VKHOOK_DLSS5_MODEL",
            (std::filesystem::path(ConfigSerializer::getBaseConfigDir()) / "models/dlss5").string());
        if (!std::filesystem::is_regular_file(std::filesystem::path(modelPath) / "manifest.json"))
            throw std::runtime_error("DLSS5 model missing: extract your DLL with scripts/extract-dlss5-weights.py and set dlss5ModelPath or VKHOOK_DLSS5_MODEL");
        auto& s = *state;
        s.registry = registry;
        s.name = name;
        s.context = std::make_unique<vk::Context>(device->instance, device->physicalDevice, device->device,
            device->queueFamilyIndex, 0, device->downstreamGIPA, layerDeviceProcAddr());
        s.model = acquireModel(device, modelPath);
        if (!device->dlss5OpticalFlowQueue)
            Logger::warn("DLSS5 temporal unavailable: no enabled NVIDIA optical-flow feature/usable queue; using no history");

        Dlss5Pipeline::CreateInfo info;
        info.context = s.context.get();
        info.queues = &device->queues;
        info.model = s.model->model.get();
        info.modelUse = &s.model->use;
        info.opticalQueue = device->dlss5OpticalFlowQueue;
        info.opticalFamily = device->dlss5OpticalFlowQueueFamily;
        info.flowDownscale = flowDownscaleOption(config);
        info.extent = extent;
        info.format = format;
        info.input = input;
        info.output = output;
        info.motionSources = motionSources;
        info.shaderPath = pathOption(config, "dlss5ShaderPath", "VKHOOK_DLSS5_SHADER_DIR",
            installedAssets("shaders", DLSS5_SHADER_RELATIVE_DIR, DLSS5_SHADER_DIR));
        info.ptxPath = pathOption(config, "dlss5PtxPath", "VKHOOK_DLSS5_PTX_DIR",
            installedAssets("ptx", DLSS5_PTX_RELATIVE_DIR, DLSS5_PTX_DIR));
        info.gipa = device->downstreamGIPA;
        info.gdpa = layerDeviceProcAddr();
        s.pipeline = std::make_unique<Dlss5Pipeline>(info);
        for (uint32_t index = 0; index < input.size(); ++index)
            updateFrame(index);
        const uint32_t flowDownscale = s.pipeline->flowDownscale();
        Logger::info("DLSS5: loaded the " + std::string(s.pipeline->networkName()) + " network ("
                     + std::to_string(s.pipeline->blockCount()) + " blocks, "
                     + std::to_string(s.pipeline->pooledActivationBytes() >> 20) + " MiB of activations), PTX neural rendering at "
                     + std::to_string(extent.width) + "x" + std::to_string(extent.height)
                     + (flowDownscale ? " (SDR, bidirectional NVIDIA hardware optical flow at 1/" + std::to_string(flowDownscale)
                                            + " resolution)"
                                      : std::string(" (SDR, no temporal history)")));
    }

    Dlss5Effect::~Dlss5Effect() = default;

    bool Dlss5Effect::updateFrame(uint32_t imageIndex)
    {
        auto& s = *state;
        Dlss5Parameters& parameters = s.pipeline->parameters();
        auto value = [&](const char* name, float fallback, float minimum, float maximum) {
            auto param = s.registry->getParameter(s.name, name);
            if (!param || (*param)->getType() != ParamType::Float) return fallback;
            const float v = static_cast<const FloatParam*>(*param)->value;
            return std::isfinite(v) ? std::clamp(v, minimum, maximum) : fallback;
        };
        parameters.intensity = value("intensity", 1, 0, 1);
        parameters.localTone = value("localTone", 1, 0, 1);
        parameters.localStructure = value("localStructure", 1, 0, 1);
        parameters.skinStructure = value("skinStructure", -1, -1, 1);
        const auto autoMask = s.registry->getParameter(s.name, "autoMask");
        parameters.autoMask = !autoMask || (*autoMask)->getType() != ParamType::Bool
            || static_cast<const BoolParam*>(*autoMask)->value;
        parameters.style = value("style", 0, 0, 128);
        const auto seed = s.registry->getParameter(s.name, "seed");
        parameters.seed = seed && (*seed)->getType() == ParamType::Uint
            ? static_cast<const UintParam*>(*seed)->value : 0u;
        const auto temporal = s.registry->getParameter(s.name, "temporal");
        parameters.temporal = !temporal || (*temporal)->getType() != ParamType::Bool
            || static_cast<const BoolParam*>(*temporal)->value;
        parameters.historyStrength = value("historyStrength", 1, 0, 1);
        const auto debug = s.registry->getParameter(s.name, "debugView");
        parameters.debugView = debug && (*debug)->getType() == ParamType::Int
            ? static_cast<uint32_t>(std::clamp(static_cast<const IntParam*>(*debug)->value, 0, 4)) : 0u;
        return s.pipeline->commitParameters(imageIndex);
    }

    VkResult Dlss5Effect::prepareSubmission(uint32_t imageIndex, uint64_t frameSerial,
                                            VkSubmitInfo& submission) noexcept
    {
        return state->pipeline->prepare(imageIndex, frameSerial, submission);
    }

    void Dlss5Effect::applyEffect(uint32_t imageIndex, VkCommandBuffer commands)
    {
        state->pipeline->record(imageIndex, commands);
    }
}
