#include "effect_dlss5.hh"
#include "effect_registry.hh"
#include "logical_device.hh"
#include "layer/registry.hh"
#include "config.hh"
#include "config_serializer.hh"
#include "logger.hh"
#include "dlss5_paths.hh"
#include "nr_graph.h"
#include "dlss5_optical_flow.hh"
#include "dlss5_parameters.hh"
#include "reference.h"
#include <algorithm>
#include <dlfcn.h>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
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

        void memoryBarrier(LogicalDevice* device, VkCommandBuffer commands)
        {
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
            device->vkd.CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
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

        using ModelKey = std::pair<VkDevice, std::string>;

        struct ModelCache
        {
            std::mutex                                   mutex;
            std::map<ModelKey, std::shared_ptr<ModelEntry>> entries;
        };

        // Deliberately never destroyed: at process exit the devices are already
        // gone, and releasing GPU objects then would call into a dead driver.
        // clearDlss5ModelCache is what releases an entry, at device destruction.
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
        LogicalDevice* device = nullptr;
        EffectRegistry* registry = nullptr;
        std::string name;
        VkExtent2D extent{};
        std::vector<VkImage> input, output;
        std::unique_ptr<vk::Context> context;
        std::shared_ptr<ModelEntry> model;
        std::unique_ptr<nr::Kernels> kernels;
        std::unique_ptr<nr::Graph> graph;
        nr::Activation* features = nullptr;
        std::unique_ptr<Dlss5OpticalFlow> flow;
        vk::Buffer packedInput{}, packedOutput{};
        vk::Buffer previousInput{}, history{}, reprojection{};
        Dlss5Parameters parameters{}, submittedParameters{};
        uint64_t lastFrameSerial = 0;
        std::chrono::steady_clock::time_point lastSubmission{};
        std::vector<uint8_t> recordedTemporal;
        std::vector<vk::Buffer> parameterBuffers;
        std::vector<VkDescriptorSet> preprocessSets, compositeSets, reprojectSets;
        vk::Pipeline preprocess{}, composite{}, reproject{};
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer network = VK_NULL_HANDLE;

        ~State()
        {
            // The swapchain's submission fence/teardown gate owns synchronization.
            if (commandPool) device->vkd.DestroyCommandPool(device->device, commandPool, nullptr);
            if (!context) return;
            context->destroyPipeline(preprocess);
            context->destroyPipeline(composite);
            context->destroyPipeline(reproject);
            context->destroyBuffer(previousInput);
            context->destroyBuffer(history);
            context->destroyBuffer(reprojection);
            for (auto& buffer : parameterBuffers) context->destroyBuffer(buffer);
            context->destroyBuffer(packedInput);
            context->destroyBuffer(packedOutput);
        }
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
        if (input.empty() || input.size() != output.size())
            throw std::runtime_error("DLSS5 input/output image counts differ");
        const auto geometry = nr::Geometry::fromValid(extent.width, extent.height);
        const std::string modelPath = pathOption(config, "dlss5ModelPath", "VKHOOK_DLSS5_MODEL",
            (std::filesystem::path(ConfigSerializer::getBaseConfigDir()) / "models/dlss5").string());
        if (!std::filesystem::is_regular_file(std::filesystem::path(modelPath) / "manifest.json"))
            throw std::runtime_error("DLSS5 model missing: extract your DLL with scripts/extract-dlss5-weights.py and set dlss5ModelPath or VKHOOK_DLSS5_MODEL");
        const std::string shaderPath = pathOption(config, "dlss5ShaderPath", "VKHOOK_DLSS5_SHADER_DIR",
            installedAssets("shaders", DLSS5_SHADER_RELATIVE_DIR, DLSS5_SHADER_DIR));
        const std::string ptxPath = pathOption(config, "dlss5PtxPath", "VKHOOK_DLSS5_PTX_DIR",
            installedAssets("ptx", DLSS5_PTX_RELATIVE_DIR, DLSS5_PTX_DIR));
        auto& s = *state;
        s.device = device;
        s.registry = registry;
        s.name = name;
        s.extent = extent;
        s.input = input;
        s.output = output;
        s.context = std::make_unique<vk::Context>(device->instance, device->physicalDevice, device->device,
            device->queueFamilyIndex, 0, device->downstreamGIPA, layerDeviceProcAddr());
        if (device->dlss5OpticalFlowQueue)
        {
            s.flow = std::make_unique<Dlss5OpticalFlow>(*s.context, device->dlss5OpticalFlowQueueFamily,
                device->dlss5OpticalFlowQueue, extent, format, motionSources, shaderPath,
                device->downstreamGIPA, layerDeviceProcAddr());
        }
        else Logger::warn("DLSS5 temporal unavailable: no enabled NVIDIA optical-flow feature/usable queue; using no history");
        s.model = acquireModel(device, modelPath);
        s.kernels = std::make_unique<nr::Kernels>(*s.context, shaderPath, ptxPath);
        // A foreign renderer may overlap long graphics workloads with this pass.
        // Keep PTX fusions, but use Vulkan barriers instead of spinning counter
        // chains whose co-residency assumptions belong to the standalone demo.
        s.kernels->setChainEnabled(false);
        s.kernels->setSiluTable(ref::siluTable());
        {
            std::lock_guard<std::mutex> lock(s.model->use);
            s.graph = std::make_unique<nr::Graph>(*s.context, *s.model->model, *s.kernels, geometry, nr::Graph::Options{});
        }
        s.features = s.graph->allocate("input features", geometry.fullWidth * geometry.fullHeight, 16, nr::Format::F32);
        const VkDeviceSize pixelBytes = VkDeviceSize(extent.width) * extent.height * sizeof(uint32_t);
        s.packedInput = s.context->createBuffer(pixelBytes, false, "DLSS5 packed input");
        s.packedOutput = s.context->createBuffer(pixelBytes, false, "DLSS5 packed output");
        s.parameters.width = extent.width;
        s.parameters.height = extent.height;
        s.parameters.fullWidth = geometry.fullWidth;
        s.parameters.fullHeight = geometry.fullHeight;
        s.parameters.bgra = bgra ? 1u : 0u;
        if (s.flow)
        {
            s.parameters.flowWidth = s.flow->flowWidth();
            s.parameters.flowHeight = s.flow->flowHeight();
            s.previousInput = s.context->createBuffer(pixelBytes, false, "DLSS5 previous input");
            s.history = s.context->createBuffer(pixelBytes * 4, false, "DLSS5 neural history");
            s.reprojection = s.context->createBuffer(16 + pixelBytes * 4, false, "DLSS5 reprojected history");
            s.reproject = s.context->createComputePipeline(s.context->loadShaderModule(shaderPath + "/dlss5_reproject.spv"), {}, "DLSS5 reprojection");
        }

        // Record the expensive graph once. Swapchain command buffers reuse this
        // secondary; depth descriptor rebuilds never allocate another network.
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        // the command buffers are re-recorded, which needs the reset flag
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = device->queueFamilyIndex;
        VK_CHECK(device->vkd.CreateCommandPool(device->device, &poolInfo, nullptr, &s.commandPool));
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = s.commandPool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        allocation.commandBufferCount = 1;
        VK_CHECK(allocateLayerCommandBuffers(device, &allocation, &s.network));
        initializeDispatchTable(s.network, device->device);
        VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        begin.pInheritanceInfo = &inheritance;
        VK_CHECK(device->vkd.BeginCommandBuffer(s.network, &begin));
        s.graph->record(s.network, *s.features);
        VK_CHECK(device->vkd.EndCommandBuffer(s.network));
        vk::SpecConstants constants;
        s.preprocess = s.context->createComputePipeline(s.context->loadShaderModule(shaderPath + "/dlss5_preprocess.spv"), constants, "DLSS5 input");
        s.composite = s.context->createComputePipeline(s.context->loadShaderModule(shaderPath + "/dlss5_composite.spv"), constants, "DLSS5 output");
        s.parameterBuffers.resize(input.size());
        s.preprocessSets.resize(input.size());
        s.compositeSets.resize(input.size());
        s.reprojectSets.resize(input.size());
        s.recordedTemporal.resize(input.size(), 2);
        for (uint32_t index = 0; index < input.size(); ++index)
        {
            s.parameterBuffers[index] = s.context->createBuffer(sizeof(Dlss5Parameters), true, "DLSS5 parameters");
            const vk::Buffer* bindings[vk::kGenericBindings]{};
            bindings[0] = &s.packedInput;
            bindings[1] = &s.parameterBuffers[index];
            bindings[2] = s.flow ? &s.reprojection : nullptr;
            bindings[7] = &s.features->buffer;
            s.preprocessSets[index] = s.context->allocateSet(bindings);
            bindings[2] = &s.graph->head().buffer;
            bindings[3] = &s.features->buffer;
            bindings[4] = s.flow ? &s.reprojection : nullptr;
            bindings[5] = s.flow ? &s.history : nullptr;
            bindings[6] = s.flow ? &s.flow->forward() : nullptr;
            bindings[7] = &s.packedOutput;
            s.compositeSets[index] = s.context->allocateSet(bindings);
            if (s.flow)
            {
                bindings[2] = &s.previousInput;
                bindings[3] = &s.history;
                bindings[4] = &s.flow->forward();
                bindings[5] = &s.flow->backward();
                bindings[6] = &s.flow->cost();
                bindings[7] = &s.reprojection;
                s.reprojectSets[index] = s.context->allocateSet(bindings);
            }
            updateFrame(index);
        }
        Logger::info("DLSS5: loaded the " + std::string(s.graph->layout().name) + " network ("
                     + std::to_string(s.model->model->blockCount()) + " blocks, "
                     + std::to_string(s.graph->pooledBytes() >> 20) + " MiB of activations), PTX neural rendering at "
                     + std::to_string(extent.width) + "x" + std::to_string(extent.height)
                     + (s.flow ? " (SDR, bidirectional NVIDIA hardware optical flow)" : " (SDR, no temporal history)"));
    }

    Dlss5Effect::~Dlss5Effect() = default;

    bool Dlss5Effect::updateFrame(uint32_t imageIndex)
    {
        auto& s = *state;
        auto value = [&](const char* name, float fallback, float minimum, float maximum) {
            auto param = s.registry->getParameter(s.name, name);
            if (!param || (*param)->getType() != ParamType::Float) return fallback;
            const float v = static_cast<const FloatParam*>(*param)->value;
            return std::isfinite(v) ? std::clamp(v, minimum, maximum) : fallback;
        };
        s.parameters.intensity = value("intensity", 1, 0, 1);
        s.parameters.localTone = value("localTone", 1, 0, 1);
        s.parameters.localStructure = value("localStructure", 1, 0, 1);
        s.parameters.skinStructure = value("skinStructure", -1, -1, 1);
        const auto autoMask = s.registry->getParameter(s.name, "autoMask");
        s.parameters.autoMask = !autoMask || (*autoMask)->getType() != ParamType::Bool
            || static_cast<const BoolParam*>(*autoMask)->value;
        s.parameters.style = value("style", 0, 0, 128);
        const auto seed = s.registry->getParameter(s.name, "seed");
        s.parameters.seed = seed && (*seed)->getType() == ParamType::Uint
            ? static_cast<const UintParam*>(*seed)->value : 0u;
        const auto temporal = s.registry->getParameter(s.name, "temporal");
        s.parameters.temporal = s.flow && (!temporal || (*temporal)->getType() != ParamType::Bool
            || static_cast<const BoolParam*>(*temporal)->value);
        s.parameters.historyStrength = value("historyStrength", 1, 0, 1);
        const auto debug = s.registry->getParameter(s.name, "debugView");
        s.parameters.debugView = debug && (*debug)->getType() == ParamType::Int
            ? static_cast<uint32_t>(std::clamp(static_cast<const IntParam*>(*debug)->value, 0, 4)) : 0u;
        std::memcpy(s.parameterBuffers.at(imageIndex).mapped, &s.parameters, sizeof(s.parameters));
        return s.recordedTemporal.at(imageIndex) != s.parameters.temporal;
    }

    VkResult Dlss5Effect::prepareSubmission(uint32_t imageIndex, uint64_t frameSerial,
                                            VkSubmitInfo& submission) noexcept
    {
        auto& s = *state;
        const auto now = std::chrono::steady_clock::now();
        const bool sameControls = std::memcmp(&s.parameters, &s.submittedParameters,
                                             offsetof(Dlss5Parameters, temporal)) == 0
            && s.parameters.temporal == s.submittedParameters.temporal
            && s.parameters.historyStrength == s.submittedParameters.historyStrength
            && s.parameters.autoMask == s.submittedParameters.autoMask
            && s.parameters.style == s.submittedParameters.style;
        s.parameters.historyValid = s.parameters.temporal && s.lastFrameSerial != 0
            && frameSerial == s.lastFrameSerial + 1 && sameControls
            && now - s.lastSubmission < std::chrono::milliseconds(250);
        std::memcpy(s.parameterBuffers[imageIndex].mapped, &s.parameters, sizeof(s.parameters));
        if (!s.parameters.temporal)
        {
            s.lastFrameSerial = 0;
            return VK_SUCCESS;
        }
        const VkResult result = s.flow->prepare(imageIndex, submission);
        if (result == VK_SUCCESS)
        {
            s.lastFrameSerial = frameSerial;
            s.lastSubmission = now;
            s.submittedParameters = s.parameters;
        }
        else s.lastFrameSerial = 0;
        return result;
    }

    void Dlss5Effect::applyEffect(uint32_t imageIndex, VkCommandBuffer commands)
    {
        auto& s = *state;
        auto& vk = s.device->vkd;
        // Scratch is shared across frames, but all effect submissions use this
        // device's single queue. Serialize before any frame overwrites it.
        memoryBarrier(s.device, commands);
        s.recordedTemporal.at(imageIndex) = static_cast<uint8_t>(s.parameters.temporal);
        if (s.parameters.temporal) s.flow->recordResults(commands);
        VkImageMemoryBarrier barriers[2]{};
        for (auto& barrier : barriers)
        {
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        barriers[0].image = s.input.at(imageIndex);
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].image = s.output.at(imageIndex);
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vk.CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, barriers);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {s.extent.width, s.extent.height, 1};
        vk.CmdCopyImageToBuffer(commands, barriers[0].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.packedInput.buffer, 1, &copy);
        memoryBarrier(s.device, commands);
        if (s.parameters.temporal)
        {
            s.context->dispatch().vkCmdFillBuffer(commands, s.reprojection.buffer, 0, 16, 0);
            memoryBarrier(s.device, commands);
            vk.CmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, s.reproject.pipeline);
            vk.CmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, s.context->pipelineLayout(), 0, 1,
                &s.reprojectSets[imageIndex], 0, nullptr);
            vk.CmdDispatch(commands, (s.extent.width + 7) / 8, (s.extent.height + 7) / 8, 1);
            memoryBarrier(s.device, commands);
        }
        vk.CmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, s.preprocess.pipeline);
        vk.CmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, s.context->pipelineLayout(), 0, 1,
            &s.preprocessSets[imageIndex], 0, nullptr);
        vk.CmdDispatch(commands, (s.parameters.fullWidth + 7) / 8, (s.parameters.fullHeight + 7) / 8, 1);
        memoryBarrier(s.device, commands);
        vk.CmdExecuteCommands(commands, 1, &s.network);
        memoryBarrier(s.device, commands);
        vk.CmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, s.composite.pipeline);
        vk.CmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, s.context->pipelineLayout(), 0, 1,
            &s.compositeSets[imageIndex], 0, nullptr);
        vk.CmdDispatch(commands, (s.extent.width + 7) / 8, (s.extent.height + 7) / 8, 1);
        memoryBarrier(s.device, commands);
        vk.CmdCopyBufferToImage(commands, s.packedOutput.buffer, barriers[1].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        if (s.parameters.temporal)
        {
            VkBufferCopy previousCopy{0, 0, s.packedInput.size};
            s.context->dispatch().vkCmdCopyBuffer(commands, s.packedInput.buffer, s.previousInput.buffer, 1, &previousCopy);
        }
        for (auto& barrier : barriers)
        {
            barrier.srcAccessMask = barrier.dstAccessMask;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            barrier.oldLayout = barrier.newLayout;
            barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        }
        vk.CmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 2, barriers);
    }
}
