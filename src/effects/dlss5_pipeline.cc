#include "dlss5_pipeline.hh"

#include "dlss5_optical_flow.hh"
#include "nr_graph.h"
#include "reference.h"

#include <chrono>
#include <cstring>
#include <stdexcept>

namespace VkHook
{
    namespace
    {
        template<typename Function>
        Function requireProc(PFN_vkVoidFunction address, const char* name)
        {
            if (!address)
                throw std::runtime_error(std::string("DLSS5: missing downstream ") + name);
            return reinterpret_cast<Function>(address);
        }

        // The network's kernels need full 32-wide subgroups, which is the context's
        // default. These shaders use no subgroup operation, and their 8x8 groups
        // cannot ask for it (VUID-VkPipelineShaderStageCreateInfo-pNext-02757).
        vk::Pipeline bridgePipeline(vk::Context& context, const std::string& shaderPath, const char* shader, const char* label,
                                    const vk::SpecConstants& constants = {})
        {
            return context.createComputePipeline(context.loadShaderModule(shaderPath + "/" + shader + ".spv"), constants, label, 0);
        }
    } // namespace

    struct Dlss5Pipeline::State
    {
        vk::Context&        context;
        const vk::Dispatch& dispatch;
        VkDevice            device;
        VkExtent2D          extent{};
        std::vector<VkImage> input, output;
        std::unique_ptr<nr::Kernels> kernels;
        std::unique_ptr<nr::Graph>   graph;
        nr::Model*                   model    = nullptr;
        nr::Activation*              features = nullptr;
        std::unique_ptr<Dlss5OpticalFlow> flow;
        // Frames with history read the pixels the optical-flow prepass already
        // copied, when this effect's input is the image the game rendered.
        bool       sharedSource = false;
        vk::Buffer packedInput{}, packedOutput{};
        vk::Buffer previousInput{}, history{}, reprojection{};
        // The engine's vectors matched again on full-resolution pixels (dlss5_flow_refine.comp):
        // searched around each vector, then exchanged between neighbouring cells.
        vk::Buffer searchedForward{}, searchedBackward{}, refinedForward{}, refinedBackward{};
        Dlss5Parameters parameters{}, submittedParameters{};
        uint64_t lastFrameSerial = 0;
        std::chrono::steady_clock::time_point lastSubmission{};
        std::vector<uint8_t>    recordedTemporal;
        std::vector<vk::Buffer> parameterBuffers;
        struct Sets
        {
            VkDescriptorSet preprocess = VK_NULL_HANDLE, composite = VK_NULL_HANDLE, reproject = VK_NULL_HANDLE;
            // Flow refinement, one direction per dispatch.
            VkDescriptorSet searchForward = VK_NULL_HANDLE, searchBackward = VK_NULL_HANDLE, propagateForward = VK_NULL_HANDLE,
                            propagateBackward = VK_NULL_HANDLE;
        };
        // Per image: `still` reads packedInput, `moving` the source of a frame with history.
        std::vector<Sets> still, moving;
        vk::Pipeline    preprocess{}, composite{}, reproject{}, search{}, propagate{}, propagateWithOpposite{};
        VkCommandPool   commandPool = VK_NULL_HANDLE;
        VkCommandBuffer network     = VK_NULL_HANDLE;

        PFN_vkCmdPipelineBarrier   pipelineBarrier   = nullptr;
        PFN_vkCmdCopyImageToBuffer copyImageToBuffer = nullptr;
        PFN_vkCmdCopyBufferToImage copyBufferToImage = nullptr;
        PFN_vkCmdExecuteCommands   executeCommands   = nullptr;

        explicit State(vk::Context& ctx) : context(ctx), dispatch(ctx.dispatch()), device(ctx.device()) {}

        ~State()
        {
            // The owner's submission fence/teardown gate owns synchronization.
            if (commandPool)
                dispatch.vkDestroyCommandPool(device, commandPool, nullptr);
            context.destroyPipeline(preprocess);
            context.destroyPipeline(composite);
            context.destroyPipeline(reproject);
            context.destroyPipeline(search);
            context.destroyPipeline(propagate);
            context.destroyPipeline(propagateWithOpposite);
            context.destroyBuffer(previousInput);
            context.destroyBuffer(history);
            context.destroyBuffer(reprojection);
            context.destroyBuffer(searchedForward);
            context.destroyBuffer(searchedBackward);
            context.destroyBuffer(refinedForward);
            context.destroyBuffer(refinedBackward);
            for (auto& buffer : parameterBuffers)
                context.destroyBuffer(buffer);
            context.destroyBuffer(packedInput);
            context.destroyBuffer(packedOutput);
        }

        void memoryBarrier(VkCommandBuffer commands) const
        {
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
            pipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier,
                            0, nullptr, 0, nullptr);
        }

        void stamp(VkCommandBuffer commands, const Stamps* stamps, Stage stage) const
        {
            if (stamps)
                dispatch.vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, stamps->pool, stamps->first + stage);
        }
    };

    Dlss5Pipeline::Dlss5Pipeline(const CreateInfo& info)
    {
        if (!info.context || !info.queues || !info.model || !info.modelUse || !info.gipa || !info.gdpa)
            throw std::runtime_error("DLSS5 pipeline: incomplete create info");
        const bool bgra = info.format == VK_FORMAT_B8G8R8A8_UNORM || info.format == VK_FORMAT_B8G8R8A8_SRGB;
        if (!bgra && info.format != VK_FORMAT_R8G8B8A8_UNORM && info.format != VK_FORMAT_R8G8B8A8_SRGB)
            throw std::runtime_error("DLSS5 requires RGBA8/BGRA8 images");
        if (info.input.empty() || info.input.size() != info.output.size())
            throw std::runtime_error("DLSS5 input/output image counts differ");
        state = std::make_unique<State>(*info.context);
        auto& s = *state;
        s.extent = info.extent;
        s.input  = info.input;
        s.output = info.output;
        s.model  = info.model;
#define DLSS5_DEVICE(member, name) s.member = requireProc<PFN_##name>(info.gdpa(s.device, #name), #name)
        DLSS5_DEVICE(pipelineBarrier, vkCmdPipelineBarrier);
        DLSS5_DEVICE(copyImageToBuffer, vkCmdCopyImageToBuffer);
        DLSS5_DEVICE(copyBufferToImage, vkCmdCopyBufferToImage);
        DLSS5_DEVICE(executeCommands, vkCmdExecuteCommands);
#undef DLSS5_DEVICE

        const auto geometry = nr::Geometry::fromValid(info.extent.width, info.extent.height);
        if (info.opticalQueue)
        {
            const uint32_t downscale = info.flowDownscale ? info.flowDownscale : Dlss5OpticalFlow::automaticDownscale(info.extent);
            s.flow = std::make_unique<Dlss5OpticalFlow>(s.context, *info.queues, info.opticalFamily, info.opticalQueue, info.extent,
                                                        info.format, info.motionSources, downscale, info.shaderPath, info.gipa,
                                                        info.gdpa);
            s.sharedSource = info.input == info.motionSources;
        }
        s.kernels = std::make_unique<nr::Kernels>(s.context, info.shaderPath, info.ptxPath);
        // A foreign renderer may overlap long graphics workloads with this pass.
        // Keep PTX fusions, but use Vulkan barriers instead of spinning counter
        // chains whose co-residency assumptions belong to the standalone demo.
        s.kernels->setChainEnabled(false);
        s.kernels->setSiluTable(ref::siluTable());
        {
            std::lock_guard<std::mutex> lock(*info.modelUse);
            s.graph = std::make_unique<nr::Graph>(s.context, *info.model, *s.kernels, geometry, nr::Graph::Options{});
        }
        s.features = s.graph->allocate("input features", geometry.fullWidth * geometry.fullHeight, 16, nr::Format::F16);
        const VkDeviceSize pixelBytes = VkDeviceSize(info.extent.width) * info.extent.height * sizeof(uint32_t);
        s.packedInput  = s.context.createBuffer(pixelBytes, false, "DLSS5 packed input");
        s.packedOutput = s.context.createBuffer(pixelBytes, false, "DLSS5 packed output");
        s.parameters.width      = info.extent.width;
        s.parameters.height     = info.extent.height;
        s.parameters.fullWidth  = geometry.fullWidth;
        s.parameters.fullHeight = geometry.fullHeight;
        s.parameters.bgra       = bgra ? 1u : 0u;
        if (s.flow)
        {
            s.parameters.flowWidth  = s.flow->flowWidth();
            s.parameters.flowHeight = s.flow->flowHeight();
            s.parameters.flowScale  = s.flow->downscale();
            s.previousInput = s.context.createBuffer(pixelBytes, false, "DLSS5 previous input");
            s.history       = s.context.createBuffer(pixelBytes * 4, false, "DLSS5 neural history");
            s.reprojection  = s.context.createBuffer(16 + pixelBytes * 4, false, "DLSS5 reprojected history");
            s.reproject = bridgePipeline(s.context, info.shaderPath, "dlss5_reproject", "DLSS5 reprojection");
            // At full resolution the engine already matches source pixels.
            if (info.refineFlow && s.flow->downscale() > 1)
            {
                const VkDeviceSize flowBytes = VkDeviceSize(s.flow->flowWidth()) * s.flow->flowHeight() * sizeof(uint32_t);
                s.searchedForward  = s.context.createBuffer(flowBytes, false, "DLSS5 searched forward flow");
                s.searchedBackward = s.context.createBuffer(flowBytes, false, "DLSS5 searched backward flow");
                s.refinedForward   = s.context.createBuffer(flowBytes, false, "DLSS5 refined forward flow");
                s.refinedBackward  = s.context.createBuffer(flowBytes, false, "DLSS5 refined backward flow");
                vk::SpecConstants search, propagate, propagateWithOpposite; // dlss5_flow_refine.comp kPass
                search.add(0, 0);
                propagate.add(0, 1);
                propagateWithOpposite.add(0, 2);
                s.search    = bridgePipeline(s.context, info.shaderPath, "dlss5_flow_refine", "DLSS5 flow search", search);
                s.propagate = bridgePipeline(s.context, info.shaderPath, "dlss5_flow_refine", "DLSS5 flow propagation", propagate);
                s.propagateWithOpposite = bridgePipeline(s.context, info.shaderPath, "dlss5_flow_refine",
                                                         "DLSS5 backward flow propagation", propagateWithOpposite);
            }
        }

        // Record the expensive graph once. The per-image primaries execute this
        // secondary; re-recording one never allocates another network.
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = s.context.queueFamily();
        VK_CHECK(s.dispatch.vkCreateCommandPool(s.device, &poolInfo, nullptr, &s.commandPool));
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool        = s.commandPool;
        allocation.level              = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        allocation.commandBufferCount = 1;
        VK_CHECK(s.dispatch.vkAllocateCommandBuffers(s.device, &allocation, &s.network));
        // Layer-owned dispatchable objects carry the borrowed device's loader dispatch table.
        *reinterpret_cast<void**>(s.network) = *reinterpret_cast<void**>(s.device);
        VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
        VkCommandBufferBeginInfo       begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags            = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        begin.pInheritanceInfo = &inheritance;
        VK_CHECK(s.dispatch.vkBeginCommandBuffer(s.network, &begin));
        s.graph->record(s.network, *s.features);
        VK_CHECK(s.dispatch.vkEndCommandBuffer(s.network));

        s.preprocess = bridgePipeline(s.context, info.shaderPath, "dlss5_preprocess", "DLSS5 input");
        s.composite  = bridgePipeline(s.context, info.shaderPath, "dlss5_composite", "DLSS5 output");
        const size_t count = info.input.size();
        s.parameterBuffers.resize(count);
        s.still.resize(count);
        s.moving.resize(count);
        s.recordedTemporal.resize(count, 2);
        const vk::Buffer& movingSource = s.sharedSource ? s.flow->source() : s.packedInput;
        // What the reprojection and the debug views read: the refined vectors when there are any.
        const bool        refined  = s.search.pipeline != VK_NULL_HANDLE;
        const vk::Buffer* forward  = !s.flow ? nullptr : refined ? &s.refinedForward : &s.flow->forward();
        const vk::Buffer* backward = !s.flow ? nullptr : refined ? &s.refinedBackward : &s.flow->backward();
        for (uint32_t index = 0; index < count; ++index)
        {
            s.parameterBuffers[index] = s.context.createBuffer(sizeof(Dlss5Parameters), true, "DLSS5 parameters");
            std::memcpy(s.parameterBuffers[index].mapped, &s.parameters, sizeof(s.parameters));
            auto allocate = [&](const vk::Buffer& source, bool history) {
                State::Sets sets;
                const vk::Buffer* bindings[vk::kGenericBindings]{};
                bindings[0] = &source;
                bindings[1] = &s.parameterBuffers[index];
                bindings[2] = s.flow ? &s.reprojection : nullptr;
                bindings[7] = &s.features->buffer;
                sets.preprocess = s.context.allocateSet(bindings);
                bindings[2] = &s.graph->head().buffer;
                bindings[4] = s.flow ? &s.reprojection : nullptr;
                bindings[5] = s.flow ? &s.history : nullptr;
                bindings[6] = forward;
                bindings[7] = &s.packedOutput;
                sets.composite = s.context.allocateSet(bindings);
                if (history)
                {
                    bindings[2] = &s.previousInput;
                    bindings[3] = &s.history;
                    bindings[4] = forward;
                    bindings[5] = backward;
                    bindings[6] = &s.flow->cost();
                    bindings[7] = &s.reprojection;
                    sets.reproject = s.context.allocateSet(bindings);
                    if (refined)
                    {
                        // One direction per set: the image whose cells are matched, the image they are
                        // looked for in, the vectors in, the other direction's vectors and the vectors out.
                        const auto refineSet = [&](const vk::Buffer& reference, const vk::Buffer& displaced, const vk::Buffer& given,
                                                   const vk::Buffer* opposite, const vk::Buffer& result) {
                            const vk::Buffer* refine[vk::kGenericBindings]{};
                            refine[0] = &reference;
                            refine[1] = &s.parameterBuffers[index];
                            refine[2] = &displaced;
                            refine[4] = &given;
                            refine[5] = opposite;
                            refine[6] = &result;
                            return s.context.allocateSet(refine);
                        };
                        sets.searchForward     = refineSet(source, s.previousInput, s.flow->forward(), nullptr, s.searchedForward);
                        sets.searchBackward    = refineSet(s.previousInput, source, s.flow->backward(), nullptr, s.searchedBackward);
                        sets.propagateForward  = refineSet(source, s.previousInput, s.searchedForward, nullptr, s.refinedForward);
                        sets.propagateBackward =
                            refineSet(s.previousInput, source, s.searchedBackward, &s.searchedForward, s.refinedBackward);
                    }
                }
                return sets;
            };
            s.still[index] = allocate(s.packedInput, false);
            if (s.flow)
                s.moving[index] = allocate(movingSource, true);
        }
    }

    Dlss5Pipeline::~Dlss5Pipeline() = default;

    bool Dlss5Pipeline::temporalAvailable() const noexcept
    {
        return state->flow != nullptr;
    }
    Dlss5Parameters& Dlss5Pipeline::parameters() noexcept
    {
        return state->parameters;
    }
    uint32_t Dlss5Pipeline::flowDownscale() const noexcept
    {
        return state->flow ? state->flow->downscale() : 0;
    }
    const char* Dlss5Pipeline::networkName() const noexcept
    {
        return state->graph->layout().name;
    }
    uint32_t Dlss5Pipeline::blockCount() const noexcept
    {
        return state->model->blockCount();
    }
    VkDeviceSize Dlss5Pipeline::pooledActivationBytes() const noexcept
    {
        return state->graph->pooledBytes();
    }

    bool Dlss5Pipeline::commitParameters(uint32_t imageIndex)
    {
        auto& s = *state;
        if (!s.flow)
            s.parameters.temporal = 0;
        std::memcpy(s.parameterBuffers.at(imageIndex).mapped, &s.parameters, sizeof(s.parameters));
        return s.recordedTemporal.at(imageIndex) != s.parameters.temporal;
    }

    VkResult Dlss5Pipeline::prepare(uint32_t imageIndex, uint64_t frameSerial, VkSubmitInfo& submission) noexcept
    {
        auto&      s   = *state;
        const auto now = std::chrono::steady_clock::now();
        const bool sameControls = std::memcmp(&s.parameters, &s.submittedParameters, offsetof(Dlss5Parameters, temporal)) == 0
            && s.parameters.temporal == s.submittedParameters.temporal
            && s.parameters.historyStrength == s.submittedParameters.historyStrength
            && s.parameters.autoMask == s.submittedParameters.autoMask && s.parameters.style == s.submittedParameters.style;
        s.parameters.historyValid = s.parameters.temporal && s.lastFrameSerial != 0 && frameSerial == s.lastFrameSerial + 1
            && sameControls && now - s.lastSubmission < std::chrono::milliseconds(250);
        std::memcpy(s.parameterBuffers[imageIndex].mapped, &s.parameters, sizeof(s.parameters));
        if (!s.parameters.temporal)
        {
            s.lastFrameSerial = 0;
            return VK_SUCCESS;
        }
        const VkResult result = s.flow->prepare(imageIndex, submission);
        if (result == VK_SUCCESS)
        {
            s.lastFrameSerial     = frameSerial;
            s.lastSubmission      = now;
            s.submittedParameters = s.parameters;
        }
        else
            s.lastFrameSerial = 0;
        return result;
    }

    void Dlss5Pipeline::record(uint32_t imageIndex, VkCommandBuffer commands, const Stamps* stamps)
    {
        auto& s = *state;
        // Scratch is shared across frames, but all effect submissions use this
        // device's single queue. Serialize before any frame overwrites it.
        s.memoryBarrier(commands);
        s.stamp(commands, stamps, kBegin);
        const bool         temporal = s.parameters.temporal;
        const State::Sets& sets     = temporal ? s.moving[imageIndex] : s.still[imageIndex];
        // The optical-flow prepass of this frame may already hold these pixels.
        const bool        copyInput = !(temporal && s.sharedSource);
        const vk::Buffer& source    = copyInput ? s.packedInput : s.flow->source();
        s.recordedTemporal.at(imageIndex) = static_cast<uint8_t>(temporal);
        if (temporal)
            s.flow->recordResults(commands);
        s.stamp(commands, stamps, kFlowResults);
        VkImageMemoryBarrier barriers[2]{};
        for (auto& barrier : barriers)
        {
            barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        barriers[0].image         = s.input.at(imageIndex);
        barriers[0].oldLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[0].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[1].image         = s.output.at(imageIndex);
        barriers[1].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        VkImageMemoryBarrier* const used      = copyInput ? barriers : barriers + 1;
        const uint32_t              usedCount = copyInput ? 2 : 1;
        s.pipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                          usedCount, used);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent      = {s.extent.width, s.extent.height, 1};
        if (copyInput)
        {
            s.copyImageToBuffer(commands, barriers[0].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.packedInput.buffer, 1, &copy);
            s.memoryBarrier(commands);
        }
        s.stamp(commands, stamps, kInputCopy);
        const VkPipelineLayout layout = s.context.pipelineLayout();
        if (temporal && s.search.pipeline)
        {
            const auto refine = [&](const vk::Pipeline& pipeline, VkDescriptorSet set) {
                s.dispatch.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
                s.dispatch.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
                s.dispatch.vkCmdDispatch(commands, (s.parameters.flowWidth + 7) / 8, (s.parameters.flowHeight + 7) / 8, 1);
            };
            refine(s.search, sets.searchForward);
            refine(s.search, sets.searchBackward);
            s.memoryBarrier(commands);
            refine(s.propagate, sets.propagateForward);
            refine(s.propagateWithOpposite, sets.propagateBackward);
            s.memoryBarrier(commands);
        }
        s.stamp(commands, stamps, kFlowRefine);
        if (temporal)
        {
            s.dispatch.vkCmdFillBuffer(commands, s.reprojection.buffer, 0, 16, 0);
            s.memoryBarrier(commands);
            s.dispatch.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, s.reproject.pipeline);
            s.dispatch.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets.reproject, 0, nullptr);
            s.dispatch.vkCmdDispatch(commands, (s.extent.width + 7) / 8, (s.extent.height + 7) / 8, 1);
            s.memoryBarrier(commands);
        }
        s.stamp(commands, stamps, kReproject);
        s.dispatch.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, s.preprocess.pipeline);
        s.dispatch.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets.preprocess, 0, nullptr);
        s.dispatch.vkCmdDispatch(commands, (s.parameters.fullWidth + 7) / 8, (s.parameters.fullHeight + 7) / 8, 1);
        s.memoryBarrier(commands);
        s.stamp(commands, stamps, kFeatures);
        s.executeCommands(commands, 1, &s.network);
        s.memoryBarrier(commands);
        s.stamp(commands, stamps, kNetwork);
        s.dispatch.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, s.composite.pipeline);
        s.dispatch.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets.composite, 0, nullptr);
        s.dispatch.vkCmdDispatch(commands, (s.extent.width + 7) / 8, (s.extent.height + 7) / 8, 1);
        s.memoryBarrier(commands);
        s.stamp(commands, stamps, kComposite);
        s.copyBufferToImage(commands, s.packedOutput.buffer, barriers[1].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        if (temporal)
        {
            const VkBufferCopy previousCopy{0, 0, VkDeviceSize(s.extent.width) * s.extent.height * sizeof(uint32_t)};
            s.dispatch.vkCmdCopyBuffer(commands, source.buffer, s.previousInput.buffer, 1, &previousCopy);
        }
        for (uint32_t i = 0; i < usedCount; ++i)
        {
            used[i].srcAccessMask = used[i].dstAccessMask;
            used[i].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            used[i].oldLayout     = used[i].newLayout;
            used[i].newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        }
        s.pipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                          usedCount, used);
        s.stamp(commands, stamps, kEnd);
    }
} // namespace VkHook
