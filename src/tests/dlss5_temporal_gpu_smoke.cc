// Opt-in NVIDIA GPU regression: production optical flow + temporal shaders.
// No model weights required; the neural head is controlled to expose blending.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "dlss5_device.hh"
#include "gpu/queue.hh"
#include "dlss5_optical_flow.hh"
#include "dlss5_parameters.hh"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <memory>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace VkHook;
namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
// Resolve the host loader at runtime, rather than linking a cross-built test
// against a loader that may require newer glibc symbols than the target ABI.
#define VULKAN_ENTRYPOINTS(X) \
    X(vkGetInstanceProcAddr) X(vkGetDeviceProcAddr) \
    X(vkCreateInstance) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceFeatures2) \
    X(vkCreateDevice) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkBindImageMemory) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdDispatch) X(vkCmdPipelineBarrier2) X(vkCmdCopyBufferToImage) \
    X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdFillBuffer) X(vkCmdCopyBuffer) X(vkQueueSubmit)
#define DECLARE_ENTRYPOINT(name) PFN_##name name{};
VULKAN_ENTRYPOINTS(DECLARE_ENTRYPOINT)
#undef DECLARE_ENTRYPOINT
struct Loader {
    std::unique_ptr<void, int (*)(void*)> library{dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL), dlclose};
    Loader()
    {
        require(bool(library), "Vulkan loader unavailable");
#define LOAD_ENTRYPOINT(name) \
        name = reinterpret_cast<PFN_##name>(dlsym(library.get(), #name)); \
        require(name != nullptr, "Vulkan loader missing " #name);
        VULKAN_ENTRYPOINTS(LOAD_ENTRYPOINT)
#undef LOAD_ENTRYPOINT
    }
};
#undef VULKAN_ENTRYPOINTS
constexpr uint32_t width = 321, height = 241, pixels = width * height;
struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
};
struct Runtime {
    VkInstance instance{};
    VkDevice device{};
    VkCommandPool pool{};
    VkFence fence{};
    Image source;
    ~Runtime()
    {
        if (device) {
            vkDeviceWaitIdle(device);
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            if (source.image) vkDestroyImage(device, source.image, nullptr);
            if (source.memory) vkFreeMemory(device, source.memory, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};
uint32_t pixel(uint32_t x, uint32_t y)
{
    // Smooth, nonrepeating texture within a limited gamut. The scene cut's
    // blue/cyan values are deliberately outside this gamut.
    const float value = .5f + .18f * std::sin(x * .12f + y * .03f)
        + .16f * std::cos(y * .17f - x * .04f) + .1f * std::sin(x * .31f + y * .23f);
    const uint32_t r = static_cast<uint32_t>(70 + value * 110);
    const uint32_t g = static_cast<uint32_t>(35 + value * 100);
    const uint32_t b = static_cast<uint32_t>(20 + value * 65);
    return r | (g << 8) | (b << 16) | ((64 + x % 129) << 24);
}
template<typename T> std::vector<T> download(vk::Context& context, const vk::Buffer& buffer)
{
    const auto bytes = context.download(buffer, buffer.size);
    std::vector<T> result(bytes.size() / sizeof(T));
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
}
}

int main(int argc, char** argv)
{
    if (!std::getenv("VKHOOK_TEST_DLSS5_GPU")) return 77;
    try {
        require(argc == 2, "pass the compiled DLSS5 shader directory");
        Loader loader;
        Runtime runtime;
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &application;
        VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &runtime.instance));
        uint32_t count = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(runtime.instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        VK_CHECK(vkEnumeratePhysicalDevices(runtime.instance, &count, devices.data()));
        VkPhysicalDevice physical{};
        VkPhysicalDeviceProperties properties{};
        for (const auto candidate : devices) {
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.vendorID == 0x10de) { physical = candidate; break; }
        }
        require(physical != VK_NULL_HANDLE, "NVIDIA GPU required");
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        uint32_t graphics = UINT32_MAX, optical = UINT32_MAX;
        for (uint32_t i = 0; i < count; ++i) {
            if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) graphics = i;
            if (families[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV) optical = i;
        }
        require(graphics != UINT32_MAX && optical != UINT32_MAX && optical != graphics, "separate graphics/optical queues required");
        VK_CHECK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr));
        std::vector<VkExtensionProperties> extensions(count);
        VK_CHECK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()));
        const float priority = 1;
        VkDeviceQueueCreateInfo queues[2]{};
        for (auto& queue : queues) {
            queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue.queueCount = 1; queue.pQueuePriorities = &priority;
        }
        queues[0].queueFamilyIndex = graphics; queues[1].queueFamilyIndex = optical;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 2; deviceInfo.pQueueCreateInfos = queues;
        Dlss5DeviceFeatures features;
        std::vector<const char*> enabled;
        require(features.prepare(physical, VK_API_VERSION_1_3, vkGetPhysicalDeviceFeatures2,
            properties, extensions, deviceInfo, enabled, true), features.error().c_str());
        require(features.opticalFlowEnabled(), "opticalFlow feature unavailable");
        VK_CHECK(vkCreateDevice(physical, &deviceInfo, nullptr, &runtime.device));
        VkQueue opticalQueue{};
        vkGetDeviceQueue(runtime.device, optical, 0, &opticalQueue);
        vk::Context context(runtime.instance, physical, runtime.device, graphics, 0,
                            vkGetInstanceProcAddr, vkGetDeviceProcAddr);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = graphics;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(runtime.device, &poolInfo, nullptr, &runtime.pool));
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = runtime.pool; commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        VkCommandBuffer commands{};
        VK_CHECK(vkAllocateCommandBuffers(runtime.device, &commandInfo, &commands));
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(runtime.device, &fenceInfo, nullptr, &runtime.fence));
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D; imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {width, height, 1}; imageInfo.mipLevels = imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT; imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VK_CHECK(vkCreateImage(runtime.device, &imageInfo, nullptr, &runtime.source.image));
        VkMemoryRequirements memory{};
        vkGetImageMemoryRequirements(runtime.device, runtime.source.image, &memory);
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = memory.size; allocation.memoryTypeIndex = UINT32_MAX;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
            if ((memory.memoryTypeBits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                allocation.memoryTypeIndex = i; break;
            }
        require(allocation.memoryTypeIndex != UINT32_MAX, "image memory unavailable");
        VK_CHECK(vkAllocateMemory(runtime.device, &allocation, nullptr, &runtime.source.memory));
        VK_CHECK(vkBindImageMemory(runtime.device, runtime.source.image, runtime.source.memory, 0));
        Timeline timeline;
        QueueDispatch queueDispatch;
        fillQueueDispatch(runtime.device, vkGetDeviceProcAddr, &queueDispatch);
        QueueTable queueTable;
        queueTable.init(runtime.device, queueDispatch, timeline);
        queueTable.add(context.queue(), graphics, 0);
        queueTable.add(opticalQueue, optical, 0);
        Dlss5OpticalFlow flow(context, queueTable, optical, opticalQueue, {width, height}, imageInfo.format,
                             {runtime.source.image}, argv[1], vkGetInstanceProcAddr, vkGetDeviceProcAddr);
        auto input = context.createBuffer(pixels * 4, false, "test input");
        auto previous = context.createBuffer(pixels * 4, false, "test previous input");
        auto history = context.createBuffer(pixels * 16, false, "test neural history");
        auto reprojection = context.createBuffer(16 + pixels * 16, false, "test reprojection");
        auto packedOutput = context.createBuffer(pixels * 4, false, "test output");
        auto tensor = context.createBuffer(pixels * 64, false, "test features");
        auto head = context.createBuffer(pixels * 16, false, "test neural head");
        auto parameters = context.createBuffer(sizeof(Dlss5Parameters), true, "test parameters");
        Dlss5Parameters params{};
        params.width = params.fullWidth = width; params.height = params.fullHeight = height;
        params.intensity = params.localTone = params.localStructure = params.historyStrength = 1;
        params.skinStructure = -1; params.temporal = 1;
        params.flowWidth = flow.flowWidth(); params.flowHeight = flow.flowHeight();
        auto pipeline = [&](const char* name) {
            return context.createComputePipeline(context.loadShaderModule(std::string(argv[1]) + "/" + name + ".spv"), {}, name);
        };
        auto reprojectPipeline = pipeline("dlss5_reproject");
        auto preprocessPipeline = pipeline("dlss5_preprocess");
        auto compositePipeline = pipeline("dlss5_composite");
        const vk::Buffer* bindings[vk::kGenericBindings]{};
        bindings[0] = &input; bindings[1] = &parameters; bindings[2] = &previous;
        bindings[3] = &history; bindings[4] = &flow.forward(); bindings[5] = &flow.backward();
        bindings[6] = &flow.cost(); bindings[7] = &reprojection;
        const auto reprojectSet = context.allocateSet(bindings);
        std::fill(std::begin(bindings), std::end(bindings), nullptr);
        bindings[0] = &input; bindings[1] = &parameters; bindings[2] = &reprojection; bindings[7] = &tensor;
        const auto preprocessSet = context.allocateSet(bindings);
        bindings[2] = &head; bindings[3] = &tensor; bindings[4] = &reprojection;
        bindings[5] = &history; bindings[6] = &flow.forward(); bindings[7] = &packedOutput;
        const auto compositeSet = context.allocateSet(bindings);
        std::vector<uint32_t> source(pixels);
        std::vector<float> neuralHead(pixels * 4, 0);
        bool initialized = false;
        struct Drain {
            VkDevice device;
            ~Drain() { vkDeviceWaitIdle(device); }
        } drain{runtime.device};
        auto dispatch = [&](const vk::Pipeline& p, VkDescriptorSet set) {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, context.pipelineLayout(), 0, 1, &set, 0, nullptr);
            vkCmdDispatch(commands, (width + 7) / 8, (height + 7) / 8, 1);
            context.transferBarrier(commands);
        };
        auto frame = [&]() {
            context.upload(input, source.data(), pixels * 4);
            context.upload(head, neuralHead.data(), pixels * 16);
            std::memcpy(parameters.mapped, &params, sizeof(params));
            auto upload = context.beginCommands();
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.oldLayout = initialized ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = runtime.source.image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = 1; dependency.pImageMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(upload, &dependency);
            context.transferBarrier(upload);
            VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {width, height, 1};
            vkCmdCopyBufferToImage(upload, input.buffer, runtime.source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier.oldLayout = barrier.newLayout; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
            vkCmdPipelineBarrier2(upload, &dependency);
            context.endAndSubmit(upload); initialized = true;
            VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            if (params.temporal) VK_CHECK(flow.prepare(0, submission));
            VK_CHECK(vkResetCommandBuffer(commands, 0));
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            VK_CHECK(vkBeginCommandBuffer(commands, &begin));
            if (params.temporal) {
                flow.recordResults(commands);
                vkCmdFillBuffer(commands, reprojection.buffer, 0, 16, 0);
                context.transferBarrier(commands);
                dispatch(reprojectPipeline, reprojectSet);
            }
            dispatch(preprocessPipeline, preprocessSet);
            dispatch(compositePipeline, compositeSet);
            VkBufferCopy previousCopy{0, 0, pixels * 4};
            vkCmdCopyBuffer(commands, input.buffer, previous.buffer, 1, &previousCopy);
            context.transferBarrier(commands);
            VK_CHECK(vkEndCommandBuffer(commands));
            submission.commandBufferCount = 1; submission.pCommandBuffers = &commands;
            VK_CHECK(vkQueueSubmit(context.queue(), 1, &submission, runtime.fence));
            VK_CHECK(vkWaitForFences(runtime.device, 1, &runtime.fence, VK_TRUE, 30'000'000'000ull));
            VK_CHECK(vkResetFences(runtime.device, 1, &runtime.fence));
            const auto output = download<uint32_t>(context, packedOutput);
            for (uint32_t i = 0; i < pixels; ++i) require((output[i] >> 24) == (source[i] >> 24), "alpha changed");
            return output;
        };
        auto requireNoHistory = [&]() {
            const auto features = download<float>(context, tensor);
            for (uint32_t i = 0; i < pixels; ++i)
                for (uint32_t c = 0; c < 3; ++c)
                    require(features[i * 16 + 4 + c] == features[i * 16 + 7 + c], "invalid history reached model input");
        };
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) source[y * width + x] = pixel(x, y);
        for (uint32_t i = 0; i < pixels; ++i) neuralHead[i * 4] = .5f;
        frame(); requireNoHistory();
        std::cout << "PASS first frame uses no history\n";
        params.historyValid = 1;
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                source[y * width + x] = x >= 8 && y >= 4 ? pixel(x - 8, y - 4) : 0xffeeee05u;
        for (uint32_t i = 0; i < pixels; ++i) neuralHead[i * 4] = -.5f;
        const auto translated = frame();
        const auto forward = download<uint32_t>(context, flow.forward());
        std::vector<float> dx, dy;
        for (uint32_t y = 8; y + 8 < params.flowHeight; ++y)
            for (uint32_t x = 8; x + 8 < params.flowWidth; ++x) {
                const auto packed = forward[y * params.flowWidth + x];
                dx.push_back(static_cast<int16_t>(packed & 0xffffu) / 32.f);
                dy.push_back(static_cast<int16_t>(packed >> 16) / 32.f);
            }
        std::sort(dx.begin(), dx.end()); std::sort(dy.begin(), dy.end());
        std::cout << "Motion median: " << dx[dx.size()/2] << ", " << dy[dy.size()/2] << '\n';
        require(std::abs(dx[dx.size()/2] + 8) < 1.5f && std::abs(dy[dy.size()/2] + 4) < 1.5f, "optical motion direction/scale wrong");
        const auto repro = download<float>(context, reprojection);
        uint32_t accepted = 0; std::memcpy(&accepted, repro.data(), sizeof(accepted));
        std::cout << "Accepted history pixels: " << accepted << '/' << pixels << '\n';
        require(accepted > pixels / 2, "textured translation rejected globally");
        for (uint32_t y = 16; y + 16 < height; ++y)
            require(repro[4 + (y * width + 2) * 4 + 3] == 0, "newly uncovered region reused history");
        // Re-render the same temporal data: diagnostics must not poison feedback
        // or reset history, even at zero display intensity.
        const auto normalHistory = download<float>(context, history);
        auto debugComposite = [&](uint32_t mode) {
            params.debugView = mode;
            std::memcpy(parameters.mapped, &params, sizeof(params));
            auto cb = context.beginCommands();
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, compositePipeline.pipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, context.pipelineLayout(), 0, 1, &compositeSet, 0, nullptr);
            vkCmdDispatch(cb, (width + 7) / 8, (height + 7) / 8, 1);
            context.transferBarrier(cb);
            context.endAndSubmit(cb);
            return download<uint32_t>(context, packedOutput);
        };
        const uint32_t center = (height / 2) * width + width / 2;
        require(repro[4 + center * 4 + 3] > .5f, "debug fixture center lacks reliable history");
        params.intensity = 0;
        for (uint32_t mode = 1; mode <= 4; ++mode) {
            const auto image = debugComposite(mode);
            require(download<float>(context, history) == normalHistory, "debug visualization poisoned neural feedback");
            for (uint32_t i = 0; i < pixels; ++i)
                require((image[i] >> 24) == (source[i] >> 24), "debug view changed alpha");
            require((image[0] & 0xffffffu) == 0x00ff00u, "accepted-frame status strip incorrect");
            const uint32_t color = image[center];
            if (mode == 1) {
                const auto r = color & 255u, g = (color >> 8) & 255u, b = (color >> 16) & 255u;
                require(r == 0 && g > 20 && b > g && b > 100, "debug flow hue/magnitude does not show current-to-previous motion");
            } else {
                for (uint32_t c = 0; c < 3; ++c) {
                    const float expected = mode == 2 ? (c == 1 ? repro[4 + center * 4 + 3] : 0.f)
                        : mode == 3 ? repro[4 + center * 4 + c] : .5f * repro[4 + center * 4 + 3];
                    require(std::abs(float((color >> (c * 8)) & 255u) - expected * 255.f) <= 1.f,
                        "debug view does not match accepted history/confidence/blend");
                }
                require((image[32 * width + 2] & 0xffffffu) == 0, "debug view displays rejected disocclusion as valid history");
            }
        }
        params.historyValid = 0;
        require((debugComposite(1)[0] & 0xffffffu) == 0x00a6ffu, "debug reset status incorrect");
        params.temporal = 0;
        require((debugComposite(1)[0] & 0xffffffu) == 0xff00ffu, "debug temporal-off status incorrect");
        params.historyValid = 1; params.intensity = 1; params.debugView = 0;
        std::cout << "PASS live debug motion/confidence/history/weight, reset/off status, alpha, and unmodified feedback\n";
        params.temporal = 0;
        const auto unblended = frame(); requireNoHistory();
        uint32_t blended = 0;
        for (uint32_t y = 32; y + 32 < height; ++y)
            for (uint32_t x = 32; x + 32 < width; ++x) {
                const auto i = y * width + x;
                if ((translated[i] & 255) > (unblended[i] & 255) + 5) ++blended;
            }
        require(blended > pixels / 4, "valid reprojected history did not affect neural composite");
        std::cout << "PASS translated history affects composite; disocclusion rejected; temporal off bypasses history\n";
        // A large untextured background must not invalidate a moving object's
        // reliable history simply because fewer than half the pixels have flow.
        auto objectScene = [&](uint32_t sx, uint32_t sy) {
            std::fill(source.begin(), source.end(), 0x80333333u);
            for (uint32_t y = height / 3; y < 2 * height / 3; ++y)
                for (uint32_t x = width / 4; x < 3 * width / 4; ++x)
                    source[(y + sy) * width + x + sx] = pixel(x, y);
        };
        params.temporal = 1; params.historyValid = 0;
        objectScene(0, 0);
        for (uint32_t i = 0; i < pixels; ++i) neuralHead[i * 4] = .5f;
        frame();
        params.historyValid = 1;
        objectScene(8, 4);
        for (uint32_t i = 0; i < pixels; ++i) neuralHead[i * 4] = -.5f;
        const auto objectBlended = frame();
        params.temporal = 0;
        const auto objectUnblended = frame();
        uint32_t objectChanges = 0;
        for (uint32_t y = height / 3 + 16; y < 2 * height / 3 - 16; ++y)
            for (uint32_t x = width / 4 + 24; x < 3 * width / 4 - 16; ++x) {
                const auto i = y * width + x;
                if ((objectBlended[i] & 255) > (objectUnblended[i] & 255) + 5) ++objectChanges;
            }
        require(objectChanges > pixels / 100, "textureless background invalidated reliable object history");
        std::cout << "PASS moving object retains history against untextured background\n";
        params.temporal = params.historyValid = 1;
        std::fill(source.begin(), source.end(), 0x7fffed05u);
        const auto cut = frame(); requireNoHistory();
        params.temporal = 0;
        require(cut == frame(), "scene cut differs from no-history output");
        std::cout << "PASS scene cut rejects history globally\n";
        params.temporal = 1; params.historyValid = 0; params.intensity = 0;
        require(frame() == source, "zero intensity not byte-exact"); requireNoHistory();
        std::cout << "PASS explicit reset, byte-exact zero intensity, preserved alpha, odd dimensions\n";
        for (auto* p : {&reprojectPipeline, &preprocessPipeline, &compositePipeline}) context.destroyPipeline(*p);
        for (auto* b : {&input, &previous, &history, &reprojection, &packedOutput, &tensor, &head, &parameters}) context.destroyBuffer(*b);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
