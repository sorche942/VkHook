#include "dlss5_optical_flow.hh"

#include <algorithm>
#include <array>
#include <limits>

namespace VKIntox
{
    namespace
    {
        constexpr VkImageSubresourceRange colorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkBufferImageCopy imageCopy(VkExtent2D extent)
        {
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {extent.width, extent.height, 1};
            return copy;
        }

        template<typename Function>
        Function requireProc(PFN_vkVoidFunction address, const char* name)
        {
            if (!address) throw std::runtime_error(std::string("DLSS5 optical flow: missing downstream ") + name);
            return reinterpret_cast<Function>(address);
        }
    }

    struct Dlss5OpticalFlow::State
    {
        vk::Context& context;
        const vk::Dispatch& dispatch;
        VkDevice device;
        VkQueue opticalQueue = VK_NULL_HANDLE;
        uint32_t opticalFamily = 0;
        VkExtent2D extent{}, flowExtent{};
        uint32_t lumaGroupsX = 0, lumaGroupsY = 0;
        VkPhysicalDeviceMemoryProperties memoryProperties{};

        PFN_vkGetPhysicalDeviceOpticalFlowImageFormatsNV getOpticalFormats = nullptr;
        PFN_vkGetPhysicalDeviceFormatProperties2 getFormatProperties = nullptr;
        PFN_vkGetPhysicalDeviceImageFormatProperties2 getImageProperties = nullptr;
        PFN_vkCreateOpticalFlowSessionNV createSession = nullptr;
        PFN_vkDestroyOpticalFlowSessionNV destroySession = nullptr;
        PFN_vkBindOpticalFlowSessionImageNV bindSessionImage = nullptr;
        PFN_vkCmdOpticalFlowExecuteNV execute = nullptr;
        PFN_vkCreateImage createImage = nullptr;
        PFN_vkDestroyImage destroyImage = nullptr;
        PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
        PFN_vkBindImageMemory bindImageMemory = nullptr;
        PFN_vkCreateImageView createImageView = nullptr;
        PFN_vkDestroyImageView destroyImageView = nullptr;
        PFN_vkCreateSemaphore createSemaphore = nullptr;
        PFN_vkDestroySemaphore destroySemaphore = nullptr;
        PFN_vkCmdCopyImageToBuffer copyImageToBuffer = nullptr;
        PFN_vkCmdCopyBufferToImage copyBufferToImage = nullptr;
        PFN_vkCmdCopyImage copyImage = nullptr;
        PFN_vkCmdClearColorImage clearColorImage = nullptr;

        struct Image
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
        } current, previous, forwardImage, backwardImage, costImage;
        VkOpticalFlowSessionNV forwardSession = VK_NULL_HANDLE, backwardSession = VK_NULL_HANDLE;
        vk::Buffer packedSource{}, packedLuma{}, parameters{}, forward{}, backward{}, cost{};
        vk::Pipeline luma{};
        VkDescriptorSet lumaSet = VK_NULL_HANDLE;
        VkCommandPool graphicsPool = VK_NULL_HANDLE, opticalPool = VK_NULL_HANDLE;
        VkCommandBuffer initialize = VK_NULL_HANDLE;
        struct Frame
        {
            VkCommandBuffer prepass = VK_NULL_HANDLE, optical = VK_NULL_HANDLE;
            VkSemaphore lumaReady = VK_NULL_HANDLE, flowReady = VK_NULL_HANDLE;
        };
        std::vector<Frame> frames;
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        bool initialized = false;
        VkResult failure = VK_SUCCESS;

        explicit State(vk::Context& ctx) : context(ctx), dispatch(ctx.dispatch()), device(ctx.device()) {}

        ~State()
        {
            // No queue/device ownership and no CPU wait here. The host's retirement
            // gate also covers a prepass submitted before a later submit failed.
            if (graphicsPool) dispatch.vkDestroyCommandPool(device, graphicsPool, nullptr);
            if (opticalPool) dispatch.vkDestroyCommandPool(device, opticalPool, nullptr);
            for (const auto& frame : frames)
            {
                if (frame.lumaReady) destroySemaphore(device, frame.lumaReady, nullptr);
                if (frame.flowReady) destroySemaphore(device, frame.flowReady, nullptr);
            }
            if (forwardSession) destroySession(device, forwardSession, nullptr);
            if (backwardSession) destroySession(device, backwardSession, nullptr);
            for (Image* image : {&current, &previous, &forwardImage, &backwardImage, &costImage})
            {
                if (image->view) destroyImageView(device, image->view, nullptr);
                if (image->image) destroyImage(device, image->image, nullptr);
                if (image->memory) dispatch.vkFreeMemory(device, image->memory, nullptr);
            }
            context.destroyPipeline(luma);
            for (vk::Buffer* buffer : {&packedSource, &packedLuma, &parameters, &forward, &backward, &cost})
                context.destroyBuffer(*buffer);
            // The context owns its shader module and descriptor-pool allocations.
        }

        void load(PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa)
        {
            if (!gipa || !gdpa) throw std::runtime_error("DLSS5 optical flow requires downstream Vulkan loaders");
#define OF_INSTANCE(member, name) member = requireProc<PFN_##name>(gipa(context.instance(), #name), #name)
#define OF_DEVICE(member, name) member = requireProc<PFN_##name>(gdpa(device, #name), #name)
            OF_INSTANCE(getOpticalFormats, vkGetPhysicalDeviceOpticalFlowImageFormatsNV);
            OF_INSTANCE(getFormatProperties, vkGetPhysicalDeviceFormatProperties2);
            OF_INSTANCE(getImageProperties, vkGetPhysicalDeviceImageFormatProperties2);
            OF_DEVICE(createSession, vkCreateOpticalFlowSessionNV);
            OF_DEVICE(destroySession, vkDestroyOpticalFlowSessionNV);
            OF_DEVICE(bindSessionImage, vkBindOpticalFlowSessionImageNV);
            OF_DEVICE(execute, vkCmdOpticalFlowExecuteNV);
            OF_DEVICE(createImage, vkCreateImage);
            OF_DEVICE(destroyImage, vkDestroyImage);
            OF_DEVICE(getImageMemoryRequirements, vkGetImageMemoryRequirements);
            OF_DEVICE(bindImageMemory, vkBindImageMemory);
            OF_DEVICE(createImageView, vkCreateImageView);
            OF_DEVICE(destroyImageView, vkDestroyImageView);
            OF_DEVICE(createSemaphore, vkCreateSemaphore);
            OF_DEVICE(destroySemaphore, vkDestroySemaphore);
            OF_DEVICE(copyImageToBuffer, vkCmdCopyImageToBuffer);
            OF_DEVICE(copyBufferToImage, vkCmdCopyBufferToImage);
            OF_DEVICE(copyImage, vkCmdCopyImage);
            OF_DEVICE(clearColorImage, vkCmdClearColorImage);
#undef OF_INSTANCE
#undef OF_DEVICE
        }

        void requireFormat(VkFormat format, VkOpticalFlowUsageFlagsNV usage, const char* name)
        {
            VkOpticalFlowImageFormatInfoNV info{VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV};
            info.usage = usage;
            uint32_t count = 0;
            VK_CHECK(getOpticalFormats(context.physical(), &info, &count, nullptr));
            std::vector<VkOpticalFlowImageFormatPropertiesNV> formats(count,
                {VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_PROPERTIES_NV});
            VK_CHECK(getOpticalFormats(context.physical(), &info, &count, formats.data()));
            if (std::none_of(formats.begin(), formats.begin() + count,
                [format](const auto& candidate) { return candidate.format == format; }))
                throw std::runtime_error(std::string("DLSS5 optical flow requires ") + name);
        }

        void makeImage(Image& image, VkExtent2D size, VkFormat format,
                       VkOpticalFlowUsageFlagsNV opticalUsage, VkImageUsageFlags transferUsage,
                       VkFormatFeatureFlags2 opticalFeature, const char* name)
        {
            VkFormatProperties3 features{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
            VkFormatProperties2 properties{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &features};
            getFormatProperties(context.physical(), format, &properties);
            VkFormatFeatureFlags2 required = opticalFeature;
            if (transferUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) required |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
            if (transferUsage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) required |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
            if ((features.optimalTilingFeatures & required) != required)
                throw std::runtime_error(std::string("DLSS5 optical flow: unsupported optimal-tiling/transfer features for ") + name);
            // Optical-flow usage is a pNext property, not a VkImageUsage bit.
            // A view still needs a sampled/storage usage supported by its format.
            VkImageUsageFlags usage = transferUsage;
            if (features.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT)
                usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
            else if (features.optimalTilingFeatures & VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT)
                usage |= VK_IMAGE_USAGE_STORAGE_BIT;
            else
                throw std::runtime_error(std::string("DLSS5 optical flow: no image-view usage for ") + name);
            VkOpticalFlowImageFormatInfoNV optical{VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV};
            optical.usage = opticalUsage;
            VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &optical};
            query.format = format;
            query.type = VK_IMAGE_TYPE_2D;
            query.tiling = VK_IMAGE_TILING_OPTIMAL;
            query.usage = usage;
            VkImageFormatProperties2 supported{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
            const VkResult result = getImageProperties(context.physical(), &query, &supported);
            if (result != VK_SUCCESS)
                throw std::runtime_error(std::string("DLSS5 optical flow: unsupported image parameters for ")
                    + name + " (VkResult " + std::to_string(result) + ")");
            if (size.width > supported.imageFormatProperties.maxExtent.width
                || size.height > supported.imageFormatProperties.maxExtent.height)
                throw std::runtime_error(std::string("DLSS5 optical flow: image extent exceeds support for ") + name);
            const uint32_t families[]{context.queueFamily(), opticalFamily};
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &optical};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = format;
            info.extent = {size.width, size.height, 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = usage;
            info.sharingMode = families[0] == families[1] ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT;
            info.queueFamilyIndexCount = info.sharingMode == VK_SHARING_MODE_CONCURRENT ? 2 : 0;
            info.pQueueFamilyIndices = info.queueFamilyIndexCount ? families : nullptr;
            VK_CHECK(createImage(device, &info, nullptr, &image.image));
            VkMemoryRequirements requirements{};
            getImageMemoryRequirements(device, image.image, &requirements);
            uint32_t type = 0;
            for (; type < memoryProperties.memoryTypeCount; ++type)
                if ((requirements.memoryTypeBits & (1u << type))
                    && (memoryProperties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) break;
            if (type == memoryProperties.memoryTypeCount)
                throw std::runtime_error("DLSS5 optical flow: no device-local image memory type");
            // Dedicated allocations also satisfy implementations that require one.
            VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
            dedicated.image = image.image;
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = type;
            VK_CHECK(dispatch.vkAllocateMemory(device, &allocation, nullptr, &image.memory));
            VK_CHECK(bindImageMemory(device, image.image, image.memory, 0));
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format;
            view.subresourceRange = colorRange;
            VK_CHECK(createImageView(device, &view, nullptr, &image.view));
        }

        void barrier(VkCommandBuffer commands, VkPipelineStageFlags2 sourceStage,
                     VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 destinationStage,
                     VkAccessFlags2 destinationAccess) const noexcept
        {
            VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            memory.srcStageMask = sourceStage;
            memory.srcAccessMask = sourceAccess;
            memory.dstStageMask = destinationStage;
            memory.dstAccessMask = destinationAccess;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1;
            dependency.pMemoryBarriers = &memory;
            dispatch.vkCmdPipelineBarrier2(commands, &dependency);
        }

        void sourceLayout(VkCommandBuffer commands, VkImage source, bool reading) const noexcept
        {
            VkImageMemoryBarrier2 image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            image.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            image.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            image.srcAccessMask = reading ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT;
            image.dstAccessMask = reading ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_MEMORY_READ_BIT;
            image.oldLayout = reading ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            image.newLayout = reading ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            image.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            image.image = source;
            image.subresourceRange = colorRange;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &image;
            dispatch.vkCmdPipelineBarrier2(commands, &dependency);
        }

        void allocateCommands(VkCommandPool pool, VkCommandBuffer& commands)
        {
            VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            info.commandPool = pool;
            info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            info.commandBufferCount = 1;
            VK_CHECK(dispatch.vkAllocateCommandBuffers(device, &info, &commands));
            // Match Context's layer-owned dispatchable object initialization.
            *reinterpret_cast<void**>(commands) = *reinterpret_cast<void**>(device);
        }

        void recordInitialize()
        {
            allocateCommands(graphicsPool, initialize);
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(dispatch.vkBeginCommandBuffer(initialize, &begin));
            std::array<VkImageMemoryBarrier2, 5> images{};
            const Image* resources[]{&current, &previous, &forwardImage, &backwardImage, &costImage};
            for (size_t i = 0; i < images.size(); ++i)
            {
                auto& image = images[i];
                image.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                image.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                image.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                image.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                image.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                image.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                image.image = resources[i]->image;
                image.subresourceRange = colorRange;
            }
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = static_cast<uint32_t>(images.size());
            dependency.pImageMemoryBarriers = images.data();
            dispatch.vkCmdPipelineBarrier2(initialize, &dependency);
            const VkClearColorValue black{};
            clearColorImage(initialize, previous.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &colorRange);
            // A last partial R8 word is still read as uint by the reprojection shader.
            dispatch.vkCmdFillBuffer(initialize, cost.buffer, 0, VK_WHOLE_SIZE, 0);
            VK_CHECK(dispatch.vkEndCommandBuffer(initialize));
        }

        void recordFrame(Frame& frame, VkImage source)
        {
            allocateCommands(graphicsPool, frame.prepass);
            allocateCommands(opticalPool, frame.optical);
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            VK_CHECK(dispatch.vkBeginCommandBuffer(frame.prepass, &begin));
            // Shared scratch must not be overwritten before the previous final
            // graphics submission (including its previous-luma copy) completes.
            barrier(frame.prepass, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
            sourceLayout(frame.prepass, source, true);
            const auto copy = imageCopy(extent);
            copyImageToBuffer(frame.prepass, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, packedSource.buffer, 1, &copy);
            sourceLayout(frame.prepass, source, false);
            barrier(frame.prepass, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
            dispatch.vkCmdBindPipeline(frame.prepass, VK_PIPELINE_BIND_POINT_COMPUTE, luma.pipeline);
            dispatch.vkCmdBindDescriptorSets(frame.prepass, VK_PIPELINE_BIND_POINT_COMPUTE,
                context.pipelineLayout(), 0, 1, &lumaSet, 0, nullptr);
            dispatch.vkCmdDispatch(frame.prepass, lumaGroupsX, lumaGroupsY, 1);
            barrier(frame.prepass, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            copyBufferToImage(frame.prepass, packedLuma.buffer, current.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
            VK_CHECK(dispatch.vkEndCommandBuffer(frame.prepass));

            VK_CHECK(dispatch.vkBeginCommandBuffer(frame.optical, &begin));
            VkOpticalFlowExecuteInfoNV info{VK_STRUCTURE_TYPE_OPTICAL_FLOW_EXECUTE_INFO_NV};
            info.flags = VK_OPTICAL_FLOW_EXECUTE_DISABLE_TEMPORAL_HINTS_BIT_NV;
            // Semaphore dependencies make graphics writes visible to both reads.
            // Sessions have disjoint outputs and immutable reversed input bindings.
            execute(frame.optical, forwardSession, &info);
            execute(frame.optical, backwardSession, &info);
            VK_CHECK(dispatch.vkEndCommandBuffer(frame.optical));
        }
    };

    Dlss5OpticalFlow::Dlss5OpticalFlow(vk::Context& context, uint32_t opticalQueueFamily, VkQueue opticalQueue,
        VkExtent2D extent, VkFormat sourceFormat, const std::vector<VkImage>& sources,
        const std::string& shaderPath, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa)
        : state(std::make_unique<State>(context))
    {
        auto& s = *state;
        s.load(gipa, gdpa);
        if (!opticalQueue || sources.empty() || !extent.width || !extent.height)
            throw std::runtime_error("DLSS5 optical flow requires a queue and nonempty source images");
        const bool bgra = sourceFormat == VK_FORMAT_B8G8R8A8_UNORM || sourceFormat == VK_FORMAT_B8G8R8A8_SRGB;
        if (!bgra && sourceFormat != VK_FORMAT_R8G8B8A8_UNORM && sourceFormat != VK_FORMAT_R8G8B8A8_SRGB)
            throw std::runtime_error("DLSS5 optical flow requires RGBA8/BGRA8 source images");
        s.opticalQueue = opticalQueue;
        s.opticalFamily = opticalQueueFamily;
        s.extent = extent;
        VkPhysicalDeviceOpticalFlowPropertiesNV opticalProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_PROPERTIES_NV};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &opticalProperties};
        s.dispatch.vkGetPhysicalDeviceProperties2(context.physical(), &properties);
        if (!(opticalProperties.supportedOutputGridSizes & VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV))
            throw std::runtime_error("DLSS5 optical flow requires a supported 4x4 output grid");
        if (!opticalProperties.costSupported)
            throw std::runtime_error("DLSS5 optical flow requires hardware confidence/cost output");
        if (extent.width < opticalProperties.minWidth || extent.width > opticalProperties.maxWidth
            || extent.height < opticalProperties.minHeight || extent.height > opticalProperties.maxHeight)
            throw std::runtime_error("DLSS5 optical flow source extent is outside the device's supported range");
        const VkDeviceSize pixels = VkDeviceSize(extent.width) * extent.height;
        if (pixels > std::numeric_limits<uint32_t>::max() - 3u)
            throw std::runtime_error("DLSS5 optical flow source exceeds shader addressing range");
        s.flowExtent = {(extent.width + 3) / 4, (extent.height + 3) / 4};
        s.requireFormat(VK_FORMAT_R8_UNORM, VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV, "R8_UNORM luma input support");
        s.requireFormat(VK_FORMAT_R16G16_S10_5_NV, VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV, "R16G16_S10_5_NV flow output support");
        s.requireFormat(VK_FORMAT_R8_UINT, VK_OPTICAL_FLOW_USAGE_COST_BIT_NV, "R8_UINT cost output support");
        s.dispatch.vkGetPhysicalDeviceMemoryProperties(context.physical(), &s.memoryProperties);
        const VkImageUsageFlags lumaUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        s.makeImage(s.current, extent, VK_FORMAT_R8_UNORM, VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV,
            lumaUsage, VK_FORMAT_FEATURE_2_OPTICAL_FLOW_IMAGE_BIT_NV, "current R8 luma");
        s.makeImage(s.previous, extent, VK_FORMAT_R8_UNORM, VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV,
            lumaUsage, VK_FORMAT_FEATURE_2_OPTICAL_FLOW_IMAGE_BIT_NV, "previous R8 luma");
        s.makeImage(s.forwardImage, s.flowExtent, VK_FORMAT_R16G16_S10_5_NV, VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_FORMAT_FEATURE_2_OPTICAL_FLOW_VECTOR_BIT_NV, "forward S10.5 flow");
        s.makeImage(s.backwardImage, s.flowExtent, VK_FORMAT_R16G16_S10_5_NV, VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_FORMAT_FEATURE_2_OPTICAL_FLOW_VECTOR_BIT_NV, "backward S10.5 flow");
        s.makeImage(s.costImage, s.flowExtent, VK_FORMAT_R8_UINT, VK_OPTICAL_FLOW_USAGE_COST_BIT_NV,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_FORMAT_FEATURE_2_OPTICAL_FLOW_COST_BIT_NV, "R8 cost");

        VkOpticalFlowSessionCreateInfoNV session{VK_STRUCTURE_TYPE_OPTICAL_FLOW_SESSION_CREATE_INFO_NV};
        session.width = extent.width;
        session.height = extent.height;
        session.imageFormat = VK_FORMAT_R8_UNORM;
        session.flowVectorFormat = VK_FORMAT_R16G16_S10_5_NV;
        session.costFormat = VK_FORMAT_R8_UINT;
        session.outputGridSize = VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV;
        session.performanceLevel = VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_MEDIUM_NV;
        session.flags = VK_OPTICAL_FLOW_SESSION_CREATE_ENABLE_COST_BIT_NV;
        VK_CHECK(s.createSession(s.device, &session, nullptr, &s.forwardSession));
        // Only forward cost is consumed; a second session does not require the
        // optional bidirectional-flow capability and avoids mutable bindings.
        session.flags = 0;
        session.costFormat = VK_FORMAT_UNDEFINED;
        VK_CHECK(s.createSession(s.device, &session, nullptr, &s.backwardSession));
        auto bind = [&](VkOpticalFlowSessionNV target, VkOpticalFlowSessionBindingPointNV point, const State::Image& image) {
            VK_CHECK(s.bindSessionImage(s.device, target, point, image.view, VK_IMAGE_LAYOUT_GENERAL));
        };
        bind(s.forwardSession, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_INPUT_NV, s.current);
        bind(s.forwardSession, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_REFERENCE_NV, s.previous);
        bind(s.forwardSession, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_FLOW_VECTOR_NV, s.forwardImage);
        bind(s.forwardSession, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_COST_NV, s.costImage);
        bind(s.backwardSession, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_INPUT_NV, s.previous);
        bind(s.backwardSession, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_REFERENCE_NV, s.current);
        bind(s.backwardSession, VK_OPTICAL_FLOW_SESSION_BINDING_POINT_FLOW_VECTOR_NV, s.backwardImage);

        s.packedSource = context.createBuffer(pixels * 4, false, "DLSS5 optical raw input");
        s.packedLuma = context.createBuffer((pixels + 3) & ~VkDeviceSize(3), false, "DLSS5 optical packed luma");
        s.parameters = context.createBuffer(16, true, "DLSS5 optical luma parameters");
        const uint32_t parameters[]{extent.width, extent.height, bgra ? 1u : 0u, 0u};
        std::memcpy(s.parameters.mapped, parameters, sizeof(parameters));
        const VkDeviceSize cells = VkDeviceSize(s.flowExtent.width) * s.flowExtent.height;
        s.forward = context.createBuffer(cells * 4, false, "DLSS5 forward optical flow");
        s.backward = context.createBuffer(cells * 4, false, "DLSS5 backward optical flow");
        s.cost = context.createBuffer((cells + 3) & ~VkDeviceSize(3), false, "DLSS5 optical flow cost");
        const vk::Buffer* bindings[vk::kGenericBindings]{};
        bindings[0] = &s.packedSource;
        bindings[1] = &s.parameters;
        bindings[7] = &s.packedLuma;
        s.lumaSet = context.allocateSet(bindings);
        vk::SpecConstants constants;
        s.luma = context.createComputePipeline(context.loadShaderModule(shaderPath + "/dlss5_luma.spv"),
            constants, "DLSS5 optical luma");
        const uint32_t groups = static_cast<uint32_t>((pixels + 255) / 256);
        s.lumaGroupsX = std::min(groups, properties.properties.limits.maxComputeWorkGroupCount[0]);
        s.lumaGroupsY = (groups + s.lumaGroupsX - 1) / s.lumaGroupsX;
        if (s.lumaGroupsY > properties.properties.limits.maxComputeWorkGroupCount[1])
            throw std::runtime_error("DLSS5 optical flow luma dispatch exceeds device limits");

        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.queueFamilyIndex = context.queueFamily();
        VK_CHECK(s.dispatch.vkCreateCommandPool(s.device, &pool, nullptr, &s.graphicsPool));
        pool.queueFamilyIndex = opticalQueueFamily;
        VK_CHECK(s.dispatch.vkCreateCommandPool(s.device, &pool, nullptr, &s.opticalPool));
        s.recordInitialize();
        s.frames.resize(sources.size());
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (size_t i = 0; i < sources.size(); ++i)
        {
            auto& frame = s.frames[i];
            VK_CHECK(s.createSemaphore(s.device, &semaphore, nullptr, &frame.lumaReady));
            VK_CHECK(s.createSemaphore(s.device, &semaphore, nullptr, &frame.flowReady));
            s.recordFrame(frame, sources[i]);
        }
    }

    Dlss5OpticalFlow::~Dlss5OpticalFlow() = default;

    VkResult Dlss5OpticalFlow::prepare(uint32_t imageIndex, VkSubmitInfo& nextSubmit) noexcept
    {
        auto& s = *state;
        if (s.failure != VK_SUCCESS) return s.failure;
        if (imageIndex >= s.frames.size()) return VK_ERROR_INITIALIZATION_FAILED;
        auto& frame = s.frames[imageIndex];
        const VkCommandBuffer commands[]{s.initialize, frame.prepass};
        VkSubmitInfo prepass{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        prepass.waitSemaphoreCount = nextSubmit.waitSemaphoreCount;
        prepass.pWaitSemaphores = nextSubmit.pWaitSemaphores;
        prepass.pWaitDstStageMask = nextSubmit.pWaitDstStageMask;
        prepass.commandBufferCount = s.initialized ? 1 : 2;
        prepass.pCommandBuffers = commands + (s.initialized ? 1 : 0);
        prepass.signalSemaphoreCount = 1;
        prepass.pSignalSemaphores = &frame.lumaReady;
        VkResult result = s.dispatch.vkQueueSubmit(s.context.queue(), 1, &prepass, VK_NULL_HANDLE);
        if (result != VK_SUCCESS) return s.failure = result;
        s.initialized = true;
        VkSubmitInfo optical{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        optical.waitSemaphoreCount = 1;
        optical.pWaitSemaphores = &frame.lumaReady;
        optical.pWaitDstStageMask = &s.waitStage;
        optical.commandBufferCount = 1;
        optical.pCommandBuffers = &frame.optical;
        optical.signalSemaphoreCount = 1;
        optical.pSignalSemaphores = &frame.flowReady;
        result = s.dispatch.vkQueueSubmit(s.opticalQueue, 1, &optical, VK_NULL_HANDLE);
        if (result != VK_SUCCESS) return s.failure = result;
        nextSubmit.waitSemaphoreCount = 1;
        nextSubmit.pWaitSemaphores = &frame.flowReady;
        nextSubmit.pWaitDstStageMask = &s.waitStage;
        return VK_SUCCESS;
    }

    void Dlss5OpticalFlow::recordResults(VkCommandBuffer commands) const noexcept
    {
        const auto& s = *state;
        // The final graphics submission waits on flowReady at ALL_COMMANDS.
        // No optical-flow stage bits are used on a graphics-only queue.
        s.barrier(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const auto flowCopy = imageCopy(s.flowExtent);
        s.copyImageToBuffer(commands, s.forwardImage.image, VK_IMAGE_LAYOUT_GENERAL, s.forward.buffer, 1, &flowCopy);
        s.copyImageToBuffer(commands, s.backwardImage.image, VK_IMAGE_LAYOUT_GENERAL, s.backward.buffer, 1, &flowCopy);
        s.copyImageToBuffer(commands, s.costImage.image, VK_IMAGE_LAYOUT_GENERAL, s.cost.buffer, 1, &flowCopy);
        VkImageCopy historyCopy{};
        historyCopy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        historyCopy.dstSubresource = historyCopy.srcSubresource;
        historyCopy.extent = {s.extent.width, s.extent.height, 1};
        s.copyImage(commands, s.current.image, VK_IMAGE_LAYOUT_GENERAL,
            s.previous.image, VK_IMAGE_LAYOUT_GENERAL, 1, &historyCopy);
        s.barrier(commands, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }

    uint32_t Dlss5OpticalFlow::flowWidth() const noexcept { return state->flowExtent.width; }
    uint32_t Dlss5OpticalFlow::flowHeight() const noexcept { return state->flowExtent.height; }
    const vk::Buffer& Dlss5OpticalFlow::forward() const noexcept { return state->forward; }
    const vk::Buffer& Dlss5OpticalFlow::backward() const noexcept { return state->backward; }
    const vk::Buffer& Dlss5OpticalFlow::cost() const noexcept { return state->cost; }
}
