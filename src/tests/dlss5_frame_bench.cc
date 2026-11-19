// DLSS5 frame benchmark: the effect's whole frame (optical flow, reprojection, input features, network, composite)
// through the production Dlss5Pipeline on a private device with a ring of swapchain-like images.
// No layer, no window.
//
//   vkhook-dlss5-frame-bench --model DIR --shaders DIR [--ptx DIR] [--width W --height H --frames N]
//                            [--images N] [--temporal 0|1] [--flow-scale 0|1|2|4] [--flow-refine 0|1] [--depth N]
//                            [--motion DX,DY] [--objects DX,DY] [--pattern smooth|detail] [--image in.rgba]
//                            [--debug-view N] [--output out.rgba]
//
// The source ring shows the same picture translated by --motion pixels per image (default 8,4), so optical flow
// and history reprojection do real work. The built-in patterns are evaluated at the translated coordinates, so a
// fractional motion is exact; an --image is resampled bilinearly. `detail` adds hard edges and 3-pixel texture;
// --objects adds foreground squares with a motion of their own.
// Each frame submits as a present does and is timed two ways: wall clock from the first
// queue submit until the frame's fence signals, and GPU timestamps after each stage of the final submission. Wall
// time minus the final submission's GPU span is what precedes it: the luma prepass, the optical-flow engine and
// the cross-queue semaphore round trips, during which the graphics queue runs nothing else.
// --depth N keeps N frames in flight (the fence wait trails the submit by N - 1 frames), which is how a game
// that does not wait for its presents drives the layer.
// --flow-refine 0 reprojects with the optical-flow engine's own vectors instead of the refined ones.
// --output writes the last frame's output image as raw RGBA8, to compare builds on pixels; with --debug-view
// (1 motion, 2 confidence, 3 reprojected history, 4 blend weight) it is the effect's diagnostic picture.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "dlss5_device.hh"
#include "dlss5_pipeline.hh"
#include "gpu/queue.hh"
#include "nr_model.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace VkHook;

namespace {
void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

#define VULKAN_ENTRYPOINTS(X) \
    X(vkGetInstanceProcAddr) X(vkGetDeviceProcAddr) X(vkCreateInstance) X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateDevice) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
    X(vkResetFences) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindImageMemory) X(vkCmdPipelineBarrier2) X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdResetQueryPool) X(vkDestroyQueryPool)
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

struct Image {
    VkImage image{};
    VkDeviceMemory memory{};
};

struct Runtime {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkCommandPool pool{};
    std::vector<VkFence> fences;
    std::vector<VkQueryPool> queries;
    std::vector<Image> images;
    ~Runtime()
    {
        if (device) {
            vkDeviceWaitIdle(device);
            for (VkFence fence : fences) vkDestroyFence(device, fence, nullptr);
            for (VkQueryPool pool_ : queries) vkDestroyQueryPool(device, pool_, nullptr);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            for (const Image& image : images) {
                vkDestroyImage(device, image.image, nullptr);
                vkFreeMemory(device, image.memory, nullptr);
            }
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};

std::string argument(int argc, char** argv, const char* name, const std::string& fallback = "")
{
    for (int index = 1; index + 1 < argc; ++index)
        if (!std::strcmp(argv[index], name)) return argv[index + 1];
    return fallback;
}

struct Color {
    float r, g, b;
};

// A smooth, non-repeating texture: reliable optical flow everywhere.
Color smoothPattern(float x, float y)
{
    const float value = .5f + .18f * std::sin(x * .12f + y * .03f) + .16f * std::cos(y * .17f - x * .04f)
        + .1f * std::sin(x * .31f + y * .23f);
    return {70 + value * 110, 35 + value * 100, 20 + value * 65};
}

float cellHash(int x, int y)
{
    uint32_t h = static_cast<uint32_t>(x) * 0x8da6b343u ^ static_cast<uint32_t>(y) * 0xd8163841u;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return static_cast<float>(h & 0xffffffu) / 16777216.f;
}

// The smooth texture under flat 24-pixel blocks with hard edges, some carrying 3-pixel stripes or fine
// two-dimensional detail: what a rendered frame asks of sub-pixel motion estimation.
Color detailPattern(float x, float y)
{
    const Color base = smoothPattern(x, y);
    const int cx = static_cast<int>(std::floor(x / 24)), cy = static_cast<int>(std::floor(y / 24));
    const float cell = cellHash(cx, cy), tint = cellHash(cy + 977, cx - 131);
    float value = cell - .5f;
    if (cell > .6f) value += .35f * std::sin(x * 1.9f + y * .7f) * std::sin(y * 1.3f - x * .4f);
    else if (cell < .25f) value += (static_cast<int>(std::floor((x + y * .5f) / 3)) & 1) ? .3f : -.3f;
    return {base.r + 90 * value, base.g + 90 * value * (.5f + tint), base.b + 90 * value * (1.5f - tint)};
}

uint32_t pack(Color c)
{
    const auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.f, 255.f)); };
    return byte(c.r) | (byte(c.g) << 8) | (byte(c.b) << 16) | 0xff000000u;
}

void imageLayout(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);
}

double median(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}
}

int main(int argc, char** argv)
{
    try {
        const std::string model = argument(argc, argv, "--model");
        const std::string shaders = argument(argc, argv, "--shaders");
        const std::string ptx = argument(argc, argv, "--ptx", shaders);
        require(!model.empty() && !shaders.empty(),
            "usage: vkhook-dlss5-frame-bench --model DIR --shaders DIR [--ptx DIR] [--width W --height H --frames N] "
            "[--images N] [--temporal 0|1] [--flow-scale 0|1|2|4] [--flow-refine 0|1] [--depth N] [--motion DX,DY] [--objects DX,DY] "
            "[--pattern smooth|detail] [--image FILE] [--debug-view N] [--output FILE]");
        const uint32_t width = std::stoul(argument(argc, argv, "--width", "1920"));
        const uint32_t height = std::stoul(argument(argc, argv, "--height", "1080"));
        const int frames = std::max(1, std::stoi(argument(argc, argv, "--frames", "60")));
        const uint32_t imageCount = std::max(2ul, std::stoul(argument(argc, argv, "--images", "3")));
        const bool temporal = argument(argc, argv, "--temporal", "1") != "0";
        const uint32_t depth = std::clamp<uint32_t>(std::stoul(argument(argc, argv, "--depth", "1")), 1, imageCount);
        const uint32_t flowScale = std::stoul(argument(argc, argv, "--flow-scale", "0")); // 0: as the effect chooses
        const uint32_t debugView = std::stoul(argument(argc, argv, "--debug-view", "0"));
        const bool detail = argument(argc, argv, "--pattern", "smooth") == "detail";
        const std::string motion = argument(argc, argv, "--motion", "8,4");
        const size_t comma = motion.find(',');
        require(comma != std::string::npos, "--motion takes DX,DY");
        const float motionX = std::stof(motion.substr(0, comma)), motionY = std::stof(motion.substr(comma + 1));
        // Optional foreground: 128-pixel squares on a 384-pixel lattice with a motion of their own,
        // for the motion boundaries and disocclusions a uniform translation never has.
        const std::string objects = argument(argc, argv, "--objects");
        const size_t objectComma = objects.find(',');
        require(objects.empty() || objectComma != std::string::npos, "--objects takes DX,DY");
        const float objectX = objects.empty() ? 0 : std::stof(objects.substr(0, objectComma));
        const float objectY = objects.empty() ? 0 : std::stof(objects.substr(objectComma + 1));

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
        VkPhysicalDeviceProperties properties{};
        for (const auto candidate : devices) {
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.vendorID == 0x10de) { runtime.physical = candidate; break; }
        }
        require(runtime.physical != VK_NULL_HANDLE, "NVIDIA GPU required");
        vkGetPhysicalDeviceQueueFamilyProperties(runtime.physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(runtime.physical, &count, families.data());
        uint32_t graphics = UINT32_MAX, optical = UINT32_MAX;
        for (uint32_t i = 0; i < count; ++i) {
            if (graphics == UINT32_MAX && (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
                == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) graphics = i;
            if (families[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV) optical = i;
        }
        require(graphics != UINT32_MAX, "graphics/compute queue required");
        require(!temporal || (optical != UINT32_MAX && optical != graphics), "--temporal 1 needs a separate optical-flow queue");
        VK_CHECK(vkEnumerateDeviceExtensionProperties(runtime.physical, nullptr, &count, nullptr));
        std::vector<VkExtensionProperties> extensions(count);
        VK_CHECK(vkEnumerateDeviceExtensionProperties(runtime.physical, nullptr, &count, extensions.data()));
        const float priority = 1;
        VkDeviceQueueCreateInfo queueInfos[2]{};
        for (auto& queue : queueInfos) {
            queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue.queueCount = 1;
            queue.pQueuePriorities = &priority;
        }
        queueInfos[0].queueFamilyIndex = graphics;
        queueInfos[1].queueFamilyIndex = optical;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = temporal ? 2 : 1;
        deviceInfo.pQueueCreateInfos = queueInfos;
        Dlss5DeviceFeatures features;
        std::vector<const char*> enabled;
        require(features.prepare(runtime.physical, VK_API_VERSION_1_3, vkGetPhysicalDeviceFeatures2, properties, extensions,
            deviceInfo, enabled, temporal), features.error());
        require(!temporal || features.opticalFlowEnabled(), "opticalFlow feature unavailable");
        VK_CHECK(vkCreateDevice(runtime.physical, &deviceInfo, nullptr, &runtime.device));

        vk::Context context(runtime.instance, runtime.physical, runtime.device, graphics, 0, vkGetInstanceProcAddr,
            vkGetDeviceProcAddr);
        std::printf("device: %s, %u SMs\n", context.deviceName().c_str(), context.smCount());
        Timeline timeline;
        QueueDispatch queueDispatch;
        fillQueueDispatch(runtime.device, vkGetDeviceProcAddr, &queueDispatch);
        QueueTable queues;
        queues.init(runtime.device, queueDispatch, timeline);
        queues.add(context.queue(), graphics, 0);
        VkQueue opticalQueue{};
        if (temporal) {
            vkGetDeviceQueue(runtime.device, optical, 0, &opticalQueue);
            queues.add(opticalQueue, optical, 0);
        }

        // The swapchain stand-ins: what the game rendered, and what is presented.
        const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(runtime.physical, &memoryProperties);
        std::vector<VkImage> sources, outputs;
        for (uint32_t i = 0; i < 2 * imageCount; ++i) {
            VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            imageInfo.imageType = VK_IMAGE_TYPE_2D;
            imageInfo.format = format;
            imageInfo.extent = {width, height, 1};
            imageInfo.mipLevels = imageInfo.arrayLayers = 1;
            imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
            imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
            imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            Image image;
            VK_CHECK(vkCreateImage(runtime.device, &imageInfo, nullptr, &image.image));
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(runtime.device, image.image, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = UINT32_MAX;
            for (uint32_t type = 0; type < memoryProperties.memoryTypeCount; ++type)
                if ((requirements.memoryTypeBits & (1u << type))
                    && (memoryProperties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    allocation.memoryTypeIndex = type;
                    break;
                }
            require(allocation.memoryTypeIndex != UINT32_MAX, "image memory unavailable");
            VK_CHECK(vkAllocateMemory(runtime.device, &allocation, nullptr, &image.memory));
            runtime.images.push_back(image);
            VK_CHECK(vkBindImageMemory(runtime.device, image.image, image.memory, 0));
            (i < imageCount ? sources : outputs).push_back(image.image);
        }
        const size_t pixels = static_cast<size_t>(width) * height;
        std::vector<uint32_t> base;
        const std::string picture = argument(argc, argv, "--image");
        if (!picture.empty()) {
            std::ifstream file(picture, std::ios::binary);
            require(bool(file), "cannot read " + picture);
            base.resize(pixels);
            file.read(reinterpret_cast<char*>(base.data()), pixels * 4);
            require(static_cast<size_t>(file.gcount()) == pixels * 4,
                picture + " is not a raw " + std::to_string(width) + "x" + std::to_string(height) + " RGBA8 frame");
        }
        vk::Buffer staging = context.createBuffer(pixels * 4, false, "frame bench staging");
        VkBufferImageCopy wholeImage{};
        wholeImage.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        wholeImage.imageExtent = {width, height, 1};
        std::vector<uint32_t> shifted(pixels);
        const float maxX = static_cast<float>(width - 1), maxY = static_cast<float>(height - 1);
        // The picture at a source position; positions outside it repeat its border.
        const auto picked = [&](float fx, float fy) -> Color {
            fx = std::clamp(fx, 0.f, maxX);
            fy = std::clamp(fy, 0.f, maxY);
            if (base.empty()) return detail ? detailPattern(fx, fy) : smoothPattern(fx, fy);
            const uint32_t x0 = static_cast<uint32_t>(fx), y0 = static_cast<uint32_t>(fy);
            const uint32_t x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
            const float tx = fx - x0, ty = fy - y0;
            Color c{};
            const auto add = [&](uint32_t x, uint32_t y, float weight) {
                const uint32_t p = base[static_cast<size_t>(y) * width + x];
                c.r += weight * (p & 255u); c.g += weight * ((p >> 8) & 255u); c.b += weight * ((p >> 16) & 255u);
            };
            add(x0, y0, (1 - tx) * (1 - ty)); add(x1, y0, tx * (1 - ty)); add(x0, y1, (1 - tx) * ty); add(x1, y1, tx * ty);
            return c;
        };
        for (uint32_t i = 0; i < imageCount; ++i) {
            const float dx = motionX * static_cast<float>(i), dy = motionY * static_cast<float>(i);
            const float ox = objectX * static_cast<float>(i), oy = objectY * static_cast<float>(i);
            const auto scene = [&](float x, float y) -> Color {
                if (!objects.empty()) {
                    const float u = x - ox, v = y - oy;
                    if (u - 384 * std::floor(u / 384) < 128 && v - 384 * std::floor(v / 384) < 128) {
                        // Another part of the picture, moving with the square. Wrapped: the repeated
                        // border beyond the picture would leave the square's vertical motion open.
                        return picked(std::fmod(u + 1000, static_cast<float>(width)), std::fmod(v + 700, static_cast<float>(height)));
                    }
                }
                return picked(x - dx, y - dy);
            };
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x) {
                    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
                    Color c = scene(fx, fy);
                    if (detail && base.empty()) {
                        // Hard edges are box-filtered over the pixel, as an antialiased frame has them.
                        c = {};
                        for (const float ty : {-.25f, .25f})
                            for (const float tx : {-.25f, .25f}) {
                                const Color tap = scene(fx + tx, fy + ty);
                                c.r += .25f * tap.r; c.g += .25f * tap.g; c.b += .25f * tap.b;
                            }
                    }
                    shifted[static_cast<size_t>(y) * width + x] = pack(c);
                }
            context.upload(staging, shifted.data(), pixels * 4);
            VkCommandBuffer upload = context.beginCommands();
            imageLayout(upload, sources[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdCopyBufferToImage(upload, staging.buffer, sources[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &wholeImage);
            imageLayout(upload, sources[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            context.endAndSubmit(upload, true);
        }

        nr::Model network(context, model, false);
        std::mutex modelUse;
        Dlss5Pipeline::CreateInfo info;
        info.context = &context;
        info.queues = &queues;
        info.model = &network;
        info.modelUse = &modelUse;
        info.opticalQueue = opticalQueue;
        info.opticalFamily = optical;
        info.flowDownscale = flowScale;
        info.refineFlow = argument(argc, argv, "--flow-refine", "1") != "0";
        info.extent = {width, height};
        info.format = format;
        info.input = sources;
        info.output = outputs;
        info.motionSources = sources;
        info.shaderPath = shaders;
        info.ptxPath = ptx;
        info.gipa = vkGetInstanceProcAddr;
        info.gdpa = vkGetDeviceProcAddr;
        Dlss5Pipeline pipeline(info);
        // The pipeline does not wait for its own work: drain before it is destroyed, on every path.
        struct Drain {
            VkDevice device;
            ~Drain() { vkDeviceWaitIdle(device); }
        } drain{runtime.device};
        Dlss5Parameters& parameters = pipeline.parameters();
        parameters.intensity = parameters.localTone = parameters.localStructure = parameters.historyStrength = 1;
        parameters.skinStructure = -1;
        parameters.autoMask = 1;
        parameters.temporal = temporal;
        parameters.debugView = debugView;
        const std::string flowNote = temporal ? "on, optical flow at 1/" + std::to_string(pipeline.flowDownscale()) : "off";
        std::printf("%s network (%u blocks), %ux%u, %u images, temporal %s, %u frame(s) in flight, %llu MiB of pooled activations\n",
            pipeline.networkName(), pipeline.blockCount(), width, height, imageCount, flowNote.c_str(), depth,
            static_cast<unsigned long long>(pipeline.pooledActivationBytes() >> 20));

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = graphics;
        VK_CHECK(vkCreateCommandPool(runtime.device, &poolInfo, nullptr, &runtime.pool));
        std::vector<VkCommandBuffer> primaries(imageCount);
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = runtime.pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = imageCount;
        VK_CHECK(vkAllocateCommandBuffers(runtime.device, &allocation, primaries.data()));
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (uint32_t i = 0; i < imageCount; ++i) {
            VkFence fence{};
            VK_CHECK(vkCreateFence(runtime.device, &fenceInfo, nullptr, &fence));
            runtime.fences.push_back(fence);
            runtime.queries.push_back(context.createTimestampPool(Dlss5Pipeline::kStageCount));
        }
        auto record = [&](uint32_t index) {
            VK_CHECK(vkResetCommandBuffer(primaries[index], 0));
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
            VK_CHECK(vkBeginCommandBuffer(primaries[index], &begin));
            vkCmdResetQueryPool(primaries[index], runtime.queries[index], 0, Dlss5Pipeline::kStageCount);
            const Dlss5Pipeline::Stamps stamps{runtime.queries[index], 0};
            pipeline.record(index, primaries[index], &stamps);
            VK_CHECK(vkEndCommandBuffer(primaries[index]));
        };

        static const char* const stageNames[Dlss5Pipeline::kStageCount] = {"", "flow results", "input copy", "flow refinement",
            "reprojection", "input features", "network", "composite", "output copy"};
        using Clock = std::chrono::steady_clock;
        struct Pending { uint32_t index; Clock::time_point start; };
        std::vector<Pending> inFlight;
        std::vector<double> wall, finalSpan, before, stages[Dlss5Pipeline::kStageCount];
        const int warmup = 5;
        auto retire = [&](bool measured) {
            const Pending frame = inFlight.front();
            inFlight.erase(inFlight.begin());
            VK_CHECK(vkWaitForFences(runtime.device, 1, &runtime.fences[frame.index], VK_TRUE, 30'000'000'000ull));
            const double elapsed = std::chrono::duration<double, std::milli>(Clock::now() - frame.start).count();
            VK_CHECK(vkResetFences(runtime.device, 1, &runtime.fences[frame.index]));
            const auto stamps = context.readTimestampsMs(runtime.queries[frame.index], Dlss5Pipeline::kStageCount);
            if (!measured) return;
            wall.push_back(elapsed);
            finalSpan.push_back(stamps[Dlss5Pipeline::kEnd] - stamps[Dlss5Pipeline::kBegin]);
            before.push_back(elapsed - finalSpan.back());
            for (uint32_t stage = 1; stage < Dlss5Pipeline::kStageCount; ++stage)
                stages[stage].push_back(stamps[stage] - stamps[stage - 1]);
        };
        Clock::time_point measureStart{};
        for (int frame = 0; frame < warmup + frames; ++frame) {
            const uint32_t index = static_cast<uint32_t>(frame) % imageCount;
            if (inFlight.size() == depth) retire(frame - static_cast<int>(depth) >= warmup);
            if (frame == warmup) measureStart = Clock::now();
            if (pipeline.commitParameters(index)) record(index);
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            const auto start = Clock::now();
            VK_CHECK(pipeline.prepare(index, static_cast<uint64_t>(frame) + 1, submit));
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &primaries[index];
            VK_CHECK(queues.submit(context.queue(), 1, &submit, runtime.fences[index]));
            inFlight.push_back({index, start});
        }
        const uint32_t lastIndex = inFlight.back().index;
        while (!inFlight.empty()) retire(true);
        const double measuredMs = std::chrono::duration<double, std::milli>(Clock::now() - measureStart).count();

        // Another process on the GPU stretches medians; minima still show what the frame costs.
        const auto least = [](const std::vector<double>& values) { return *std::min_element(values.begin(), values.end()); };
        std::printf("frame wall   min %.3f ms, median %.3f ms over %zu frames\n", least(wall), median(wall), wall.size());
        std::printf("throughput   %.3f ms per frame (%.1f fps)\n", measuredMs / frames, 1000.0 * frames / measuredMs);
        std::printf("final submission GPU span   min %.3f ms, median %.3f ms\n", least(finalSpan), median(finalSpan));
        if (depth == 1)
            std::printf("before the final submission (prepass, optical flow, queue hand-offs)   min %.3f ms, median %.3f ms\n",
                least(before), median(before));
        for (uint32_t stage = 1; stage < Dlss5Pipeline::kStageCount; ++stage)
            std::printf("  min %7.3f ms, median %7.3f ms  %s\n", least(stages[stage]), median(stages[stage]), stageNames[stage]);

        const std::string output = argument(argc, argv, "--output");
        if (!output.empty()) {
            VkCommandBuffer readback = context.beginCommands();
            imageLayout(readback, outputs[lastIndex], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vkCmdCopyImageToBuffer(readback, outputs[lastIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &wholeImage);
            imageLayout(readback, outputs[lastIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            context.endAndSubmit(readback, true);
            const auto bytes = context.download(staging, pixels * 4);
            std::ofstream(output, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            std::printf("output image %u written to %s\n", lastIndex, output.c_str());
        }
        VK_CHECK(vkDeviceWaitIdle(runtime.device));
        context.destroyBuffer(staging);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "vkhook-dlss5-frame-bench: %s\n", error.what());
        return 1;
    }
}
