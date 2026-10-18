#include "effect_dlss5.hh"
#include "effect_registry.hh"
#include "logical_device.hh"
#include "config.hh"
#include "config_serializer.hh"
#include "logger.hh"
#include "dlss5_paths.hh"
#include "nr_graph.h"
#include "reference.h"
#include <algorithm>
#include <dlfcn.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace VKIntox
{
    namespace
    {
        std::string pathOption(Config* config, const char* key, const char* environment, const std::string& fallback)
        {
            const char* value = std::getenv(environment);
            return config->getOption<std::string>(key, value && *value ? value : fallback);
        }

        std::string installedAssets(const char* kind, const char* systemPath)
        {
            // User-local installs keep kernels next to each architecture's .so.
            // System/Flatpak installs use Meson's configured data directory.
            Dl_info module{};
            if (dladdr(reinterpret_cast<void*>(&installedAssets), &module) && module.dli_fname)
            {
                const auto path = std::filesystem::path(module.dli_fname).parent_path() / "dlss5" / kind;
                if (std::filesystem::is_directory(path)) return path.string();
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

    struct Dlss5Effect::State
    {
        LogicalDevice* device = nullptr;
        EffectRegistry* registry = nullptr;
        std::string name;
        VkExtent2D extent{};
        std::vector<VkImage> input, output;
        std::unique_ptr<vk::Context> context;
        std::unique_ptr<nr::Model> model;
        std::unique_ptr<nr::Kernels> kernels;
        std::unique_ptr<nr::Graph> graph;
        nr::Activation* features = nullptr;
        vk::Buffer packedInput{}, packedOutput{};
        struct Parameters
        {
            uint32_t width, height, fullWidth, fullHeight;
            uint32_t bgra, seed;
            float intensity, localTone, localStructure, skinStructure;
        } parameters{};
        static_assert(sizeof(Parameters) == 40);
        std::vector<vk::Buffer> parameterBuffers;
        std::vector<VkDescriptorSet> preprocessSets, compositeSets;
        vk::Pipeline preprocess{}, composite{};
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer network = VK_NULL_HANDLE;

        ~State()
        {
            // The swapchain's submission fence/teardown gate owns synchronization.
            if (commandPool) device->vkd.DestroyCommandPool(device->device, commandPool, nullptr);
            if (!context) return;
            context->destroyPipeline(preprocess);
            context->destroyPipeline(composite);
            for (auto& buffer : parameterBuffers) context->destroyBuffer(buffer);
            context->destroyBuffer(packedInput);
            context->destroyBuffer(packedOutput);
        }
    };

    Dlss5Effect::Dlss5Effect(LogicalDevice* device, VkFormat format, VkColorSpaceKHR colorSpace,
                             VkExtent2D extent, const std::vector<VkImage>& input,
                             const std::vector<VkImage>& output, EffectRegistry* registry,
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
        const std::string modelPath = pathOption(config, "dlss5ModelPath", "VKINTOX_DLSS5_MODEL",
            (std::filesystem::path(ConfigSerializer::getBaseConfigDir()) / "models/dlss5").string());
        if (!std::filesystem::is_regular_file(std::filesystem::path(modelPath) / "manifest.json"))
            throw std::runtime_error("DLSS5 model missing: extract your DLL with scripts/extract-dlss5-weights.py and set dlss5ModelPath or VKINTOX_DLSS5_MODEL");
        const std::string shaderPath = pathOption(config, "dlss5ShaderPath", "VKINTOX_DLSS5_SHADER_DIR", installedAssets("shaders", DLSS5_SHADER_DIR));
        const std::string ptxPath = pathOption(config, "dlss5PtxPath", "VKINTOX_DLSS5_PTX_DIR", installedAssets("ptx", DLSS5_PTX_DIR));
        auto& s = *state;
        s.device = device;
        s.registry = registry;
        s.name = name;
        s.extent = extent;
        s.input = input;
        s.output = output;
        s.context = std::make_unique<vk::Context>(device->instance, device->physicalDevice, device->device,
            device->queueFamilyIndex, 0, device->downstreamGIPA, device->downstreamGDPA);
        s.model = std::make_unique<nr::Model>(*s.context, modelPath);
        s.kernels = std::make_unique<nr::Kernels>(*s.context, shaderPath, ptxPath);
        // A foreign renderer may overlap long graphics workloads with this pass.
        // Keep PTX fusions, but use Vulkan barriers instead of spinning counter
        // chains whose co-residency assumptions belong to the standalone demo.
        s.kernels->setChainEnabled(false);
        s.kernels->setSiluTable(ref::siluTable());
        s.graph = std::make_unique<nr::Graph>(*s.context, *s.model, *s.kernels, geometry, nr::Graph::Options{});
        s.features = s.graph->allocate("input features", geometry.fullWidth * geometry.fullHeight, 16, nr::Format::F32);
        const VkDeviceSize pixelBytes = VkDeviceSize(extent.width) * extent.height * sizeof(uint32_t);
        s.packedInput = s.context->createBuffer(pixelBytes, false, "DLSS5 packed input");
        s.packedOutput = s.context->createBuffer(pixelBytes, false, "DLSS5 packed output");
        s.parameters = {extent.width, extent.height, geometry.fullWidth, geometry.fullHeight, bgra ? 1u : 0u, 0u,
                        1.0f, 1.0f, 1.0f, -1.0f};

        // Record the expensive graph once. Swapchain command buffers reuse this
        // secondary; depth descriptor rebuilds never allocate another network.
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = device->queueFamilyIndex;
        VK_CHECK(device->vkd.CreateCommandPool(device->device, &poolInfo, nullptr, &s.commandPool));
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = s.commandPool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        allocation.commandBufferCount = 1;
        VK_CHECK(device->vkd.AllocateCommandBuffers(device->device, &allocation, &s.network));
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
        for (uint32_t index = 0; index < input.size(); ++index)
        {
            s.parameterBuffers[index] = s.context->createBuffer(sizeof(State::Parameters), true, "DLSS5 parameters");
            const vk::Buffer* bindings[vk::kGenericBindings]{};
            bindings[0] = &s.packedInput;
            bindings[1] = &s.parameterBuffers[index];
            bindings[7] = &s.features->buffer;
            s.preprocessSets[index] = s.context->allocateSet(bindings);
            bindings[2] = &s.graph->head().buffer;
            bindings[3] = &s.features->buffer;
            bindings[7] = &s.packedOutput;
            s.compositeSets[index] = s.context->allocateSet(bindings);
            updateFrame(index);
        }
        Logger::info("DLSS5: loaded " + std::to_string(s.model->blockCount()) + " blocks, PTX neural rendering at "
                     + std::to_string(extent.width) + "x" + std::to_string(extent.height) + " (SDR, no temporal history)");
    }

    Dlss5Effect::~Dlss5Effect() = default;

    void Dlss5Effect::updateFrame(uint32_t imageIndex)
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
        std::memcpy(s.parameterBuffers.at(imageIndex).mapped, &s.parameters, sizeof(s.parameters));
    }

    void Dlss5Effect::applyEffect(uint32_t imageIndex, VkCommandBuffer commands)
    {
        auto& s = *state;
        auto& vk = s.device->vkd;
        // Scratch is shared across frames, but all effect submissions use this
        // device's single queue. Serialize before any frame overwrites it.
        memoryBarrier(s.device, commands);
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
