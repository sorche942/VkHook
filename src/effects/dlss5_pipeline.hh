#pragma once

#include "dlss5_parameters.hh"
#include "gpu/queue.hh"
#include "vk_context.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nr
{
    class Model;
}

namespace VkHook
{
    // One swapchain's DLSS5 frame on the GPU: optical flow, history reprojection,
    // input features, the network and the composite.
    //
    // It knows nothing about the layer: every Vulkan call goes through the borrowed
    // vk::Context and every queue call through the QueueTable, so the effect and
    // vkhook-dlss5-frame-bench record and submit the same frame.
    class Dlss5Pipeline
    {
    public:
        struct CreateInfo
        {
            vk::Context* context = nullptr; // borrowed; outlives the pipeline
            QueueTable*  queues  = nullptr; // borrowed; knows the context's queue and the optical queue
            nr::Model*   model   = nullptr; // borrowed weights
            std::mutex*  modelUse = nullptr; // held while the graph builds: the model uploads weights lazily
            // No optical queue: the frame runs without temporal history.
            VkQueue  opticalQueue  = VK_NULL_HANDLE;
            uint32_t opticalFamily = VK_QUEUE_FAMILY_IGNORED;
            // Reduction of the optical-flow input per axis: 1, 2 or 4, or 0 to let
            // Dlss5OpticalFlow::automaticDownscale choose from the extent.
            uint32_t flowDownscale = 0;
            VkExtent2D extent{};
            VkFormat   format = VK_FORMAT_UNDEFINED; // RGBA8/BGRA8, UNORM or SRGB
            // One entry per swapchain image. `motionSources` are what the game rendered;
            // `input` is this effect's input (the same images when it is first in the chain).
            std::vector<VkImage> input, output, motionSources;
            std::string shaderPath, ptxPath;
            PFN_vkGetInstanceProcAddr gipa = nullptr;
            PFN_vkGetDeviceProcAddr   gdpa = nullptr;
        };

        // Throws std::runtime_error when the frame cannot be built.
        explicit Dlss5Pipeline(const CreateInfo& info);
        ~Dlss5Pipeline();
        Dlss5Pipeline(const Dlss5Pipeline&)            = delete;
        Dlss5Pipeline& operator=(const Dlss5Pipeline&) = delete;

        bool temporalAvailable() const noexcept;

        // The controls of the next frame. Geometry fields are owned by the pipeline;
        // `temporal` is forced off when no optical flow is available.
        Dlss5Parameters& parameters() noexcept;
        // Publishes parameters() for `imageIndex`, whose previous submission has
        // completed. True when that image's commands must be re-recorded.
        bool commitParameters(uint32_t imageIndex);

        // Under the submission lock: decides whether history is valid for this frame,
        // submits the optical-flow work and replaces `submission`'s waits with it.
        VkResult prepare(uint32_t imageIndex, uint64_t frameSerial, VkSubmitInfo& submission) noexcept;

        // GPU timestamps after each stage, for benchmarks: query `first + stage`.
        enum Stage : uint32_t { kBegin, kFlowResults, kInputCopy, kReproject, kFeatures, kNetwork, kComposite, kEnd, kStageCount };
        struct Stamps
        {
            VkQueryPool pool  = VK_NULL_HANDLE;
            uint32_t    first = 0;
        };
        // Records the frame of `imageIndex` into `commands` (a primary buffer of the context's queue family).
        void record(uint32_t imageIndex, VkCommandBuffer commands, const Stamps* stamps = nullptr);

        // The optical-flow input reduction in use; 0 without temporal history.
        uint32_t     flowDownscale() const noexcept;
        const char*  networkName() const noexcept;
        uint32_t     blockCount() const noexcept;
        VkDeviceSize pooledActivationBytes() const noexcept;

    private:
        struct State;
        std::unique_ptr<State> state;
    };
} // namespace VkHook
