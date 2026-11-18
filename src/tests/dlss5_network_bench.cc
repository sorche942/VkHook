// DLSS5 network benchmark: the production PTX graph on a private device, timed with GPU timestamps.
// No layer, no swapchain: isolates the network that dominates the effect's frame cost.
//
//   vkhook-dlss5-bench --model DIR --shaders DIR --ptx DIR [--width W --height H --frames N]
//                       [--chain 0|1] [--profile] [--dump head.f32] [--compare head.f32]
//                       [--image in.rgba [--seed S] [--output out.rgba] [--expect reference.rgba]]
//
// The recording is made once and resubmitted, as the effect does with its secondary command buffer.
// --dump/--compare write or check the f32 head bit for bit against an earlier run.
// --image runs a raw RGBA8 frame of --width x --height through the proxy preprocess with NGX's default conditioning
// (Intensity 1, Style 0, LocalTone 1, LocalStructure 1, SkinStructure -1, AutoMask on) instead of synthetic features,
// and composes the output as the effect does for a frame without history. --output writes that RGBA8 frame;
// --expect compares it with another implementation's RGBA8 output (e.g. NVIDIA's DLL).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "kernels.h"
#include "nr_graph.h"
#include "numeric.h"
#include "reference.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

#define VULKAN_ENTRYPOINTS(X) \
    X(vkGetInstanceProcAddr) X(vkGetDeviceProcAddr) X(vkCreateInstance) X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkCreateDevice) X(vkDestroyDevice) X(vkDeviceWaitIdle)
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

struct Device {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    uint32_t family = UINT32_MAX;
    ~Device()
    {
        if (device) { vkDeviceWaitIdle(device); vkDestroyDevice(device, nullptr); }
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};

std::string argument(int argc, char** argv, const char* name, const std::string& fallback = "")
{
    for (int index = 1; index + 1 < argc; ++index)
        if (!std::strcmp(argv[index], name)) return argv[index + 1];
    return fallback;
}

bool flag(int argc, char** argv, const char* name)
{
    for (int index = 1; index < argc; ++index)
        if (!std::strcmp(argv[index], name)) return true;
    return false;
}

void createDevice(Device& device, const vk::DeviceRequirements& requirements)
{
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.apiVersion = vk::DeviceRequirements::apiVersion;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &device.instance));
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(device.instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    VK_CHECK(vkEnumeratePhysicalDevices(device.instance, &count, devices.data()));
    for (const auto candidate : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.vendorID == 0x10de) { device.physical = candidate; break; }
    }
    require(device.physical != VK_NULL_HANDLE, "NVIDIA GPU required");
    vkGetPhysicalDeviceQueueFamilyProperties(device.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device.physical, &count, families.data());
    for (uint32_t index = 0; index < count && device.family == UINT32_MAX; ++index)
        if ((families[index].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) device.family = index;
    require(device.family != UINT32_MAX, "graphics/compute queue required");
    const float priority = 1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = device.family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.pNext = &requirements.features;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queue;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(requirements.extensions.size());
    deviceInfo.ppEnabledExtensionNames = requirements.extensions.data();
    VK_CHECK(vkCreateDevice(device.physical, &deviceInfo, nullptr, &device.device));
}

std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    require(bool(file), "cannot read " + path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// The effect's composite for a frame without history at intensity 1 (dlss5_composite.comp).
uint8_t composeChannel(float head, float centred)
{
    const float inner = std::fmaf(head, 0.03125f, centred);
    const float neural = std::fmin(std::fmax(inner * 8.0f + 0.5f, 0.0f), 1.0f);
    uint16_t bits = num::f16Bits(neural);
    if (num::f16ToF32(bits) > neural) --bits;   // truncate to the half grid
    return static_cast<uint8_t>(std::lround(num::f16ToF32(bits) * 255.0f));
}

void compareImages(const std::vector<uint8_t>& actual, const std::vector<uint8_t>& expected, const std::string& name)
{
    require(actual.size() == expected.size(), name + " is not the output size");
    const size_t pixels = actual.size() / 4;
    size_t exact = 0, withinOne = 0;
    int largest = 0;
    double squared = 0, signedSum[3] = {};
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
            const int difference = int(actual[pixel * 4 + c]) - int(expected[pixel * 4 + c]);
            worst = std::max(worst, std::abs(difference));
            squared += double(difference) * difference;
            signedSum[c] += difference;
        }
        exact += worst == 0;
        withinOne += worst <= 1;
        largest = std::max(largest, worst);
    }
    const double mse = squared / (pixels * 3.0);
    std::printf("output vs %s: PSNR %.2f dB, mean difference %+.3f %+.3f %+.3f /255, %.2f%% pixels identical, "
        "%.2f%% within one step, largest %d\n", name.c_str(), mse ? 10 * std::log10(255.0 * 255.0 / mse) : INFINITY,
        signedSum[0] / pixels, signedSum[1] / pixels, signedSum[2] / pixels, 100.0 * exact / pixels,
        100.0 * withinOne / pixels, largest);
}
}

int main(int argc, char** argv)
{
    try {
        const std::string model = argument(argc, argv, "--model");
        const std::string shaders = argument(argc, argv, "--shaders");
        const std::string ptx = argument(argc, argv, "--ptx", shaders);
        require(!model.empty() && !shaders.empty(),
            "usage: vkhook-dlss5-bench --model DIR --shaders DIR [--ptx DIR] [--width W --height H --frames N] "
            "[--chain 0|1] [--profile] [--dump FILE] [--compare FILE] "
            "[--image FILE [--seed S] [--output FILE] [--expect FILE]]");
        const uint32_t width = std::stoul(argument(argc, argv, "--width", "1920"));
        const uint32_t height = std::stoul(argument(argc, argv, "--height", "1080"));
        const int frames = std::max(1, std::stoi(argument(argc, argv, "--frames", "40")));
        const bool profile = flag(argc, argv, "--profile");

        Loader loader;
        Device device;
        vk::DeviceRequirements requirements;
        createDevice(device, requirements);
        vk::Context context(device.instance, device.physical, device.device, device.family, 0,
            vkGetInstanceProcAddr, vkGetDeviceProcAddr);
        const auto& vkd = context.dispatch();
        std::printf("device: %s, %u SMs\n", context.deviceName().c_str(), context.smCount());

        nr::Model network(context, model, false);
        nr::Kernels kernels(context, shaders, ptx);
        // The effect's barrier schedule unless --chain 1 asks for upstream's counter chains; profile timings need
        // barriers between launches.
        kernels.setChainEnabled(argument(argc, argv, "--chain", "0") != "0" && !profile);
        kernels.setSiluTable(ref::siluTable());
        const nr::Geometry geometry = nr::Geometry::fromValid(width, height);
        nr::Graph graph(context, network, kernels, geometry, nr::Graph::Options{});
        const uint32_t fullRows = geometry.fullWidth * geometry.fullHeight;
        nr::Activation* features = graph.allocate("input features", fullRows, 16, nr::Format::F32);
        const std::string image = argument(argc, argv, "--image");
        std::vector<uint8_t> source;
        if (image.empty()) {
            std::vector<float> synthetic(static_cast<size_t>(fullRows) * 16);
            for (size_t index = 0; index < synthetic.size(); ++index)
                synthetic[index] = num::roundF16(std::sin(index * 0.0017f) * 0.125f);
            context.upload(features->buffer, synthetic.data(), synthetic.size() * sizeof(float));
        } else {
            source = readFile(image);
            require(source.size() == static_cast<size_t>(width) * height * 4,
                image + " is not a raw " + std::to_string(width) + "x" + std::to_string(height) + " RGBA8 frame");
            std::vector<float> proxy(source.size());
            for (size_t index = 0; index < source.size(); ++index) proxy[index] = source[index] / 255.0f;
            vk::Buffer proxyBuffer = context.createBuffer(proxy.size() * sizeof(float), false, "proxy");
            context.upload(proxyBuffer, proxy.data(), proxy.size() * sizeof(float));
            const nr::Kernels::PreprocessArgs conditioning{geometry.fullWidth, geometry.fullHeight, width, height, width,
                height, static_cast<uint32_t>(std::stoul(argument(argc, argv, "--seed", "0"))), true, 1.0f, 1.0f,
                -1.0f, 0.0f};
            VkCommandBuffer preprocess = context.beginCommands();
            kernels.preprocessFromProxy(preprocess, proxyBuffer, *features, conditioning);
            context.endAndSubmit(preprocess, true);
            context.destroyBuffer(proxyBuffer);
        }

        VkQueryPool queries = context.createTimestampPool(2);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = device.family;
        VkCommandPool pool{};
        VK_CHECK(vkd.vkCreateCommandPool(device.device, &poolInfo, nullptr, &pool));
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        VkCommandBuffer commands{};
        VK_CHECK(vkd.vkAllocateCommandBuffers(device.device, &allocation, &commands));
        *reinterpret_cast<void**>(commands) = *reinterpret_cast<void**>(device.device);
        // Profiling pools are consumed per read, so profiled frames are re-recorded; timed frames are not.
        auto record = [&] {
            if (profile) context.resetDescriptorPool();
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            VK_CHECK(vkd.vkBeginCommandBuffer(commands, &begin));
            vkd.vkCmdResetQueryPool(commands, queries, 0, 2);
            vkd.vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
            if (profile) kernels.beginProfile(commands, 4096);
            graph.record(commands, *features);
            vkd.vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
            VK_CHECK(vkd.vkEndCommandBuffer(commands));
        };
        record();
        std::printf("%s network (%u blocks), %ux%u (field %ux%u), %u launches, %s, %llu MiB of pooled activations\n",
            graph.layout().name, graph.layout().blockCount(), width, height, geometry.fullWidth, geometry.fullHeight,
            kernels.dispatchCount(), graph.chained() ? "counter chaining" : "barriers",
            (unsigned long long)(graph.pooledBytes() >> 20));

        VkQueue queue = context.queue();
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        auto run = [&] {
            VK_CHECK(vkd.vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
            VK_CHECK(vkd.vkQueueWaitIdle(queue));
            const auto timeouts = kernels.chainTimeouts();
            require(timeouts.waits == 0, "chained wait timed out on " + timeouts.counter);
            const auto stamps = context.readTimestampsMs(queries, 2);
            return stamps[1] - stamps[0];
        };
        for (int warm = 0; warm < 3; ++warm) run();   // JIT, clocks
        std::vector<double> samples;
        std::vector<nr::Kernels::ProfileEntry> best;
        for (int frame = 0; frame < frames; ++frame) {
            if (profile && frame) record();
            samples.push_back(run());
            if (!profile) continue;
            auto entries = kernels.endProfile();
            if (best.empty()) best = entries;
            for (size_t index = 0; index < entries.size() && index < best.size(); ++index)
                best[index].milliseconds = std::min(best[index].milliseconds, entries[index].milliseconds);
        }
        std::vector<double> sorted = samples;
        std::sort(sorted.begin(), sorted.end());
        std::printf("GPU min %.3f ms, median %.3f ms over %zu frames\n", sorted.front(), sorted[sorted.size() / 2],
            sorted.size());

        if (profile) {
            std::map<std::string, double> byKernel, byStage;
            double total = 0;
            for (const auto& entry : best) {
                const size_t bar = entry.label.find(" | ");
                byStage[entry.label.substr(0, bar)] += entry.milliseconds;
                byKernel[entry.label.substr(bar + 3)] += entry.milliseconds;
                total += entry.milliseconds;
            }
            auto print = [&](const char* title, const std::map<std::string, double>& totals) {
                std::vector<std::pair<std::string, double>> rows(totals.begin(), totals.end());
                std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                std::printf("\n%s:\n", title);
                for (const auto& [label, ms] : rows) std::printf("  %8.3f ms %5.1f%%  %s\n", ms, 100 * ms / total, label.c_str());
            };
            std::printf("sum of launch spans %.3f ms\n", total);
            print("by kernel", byKernel);
            print("by stage", byStage);
        }

        const nr::Activation& head = graph.head();
        const auto bytes = context.download(head.buffer, head.validBytes());
        const std::string dump = argument(argc, argv, "--dump");
        if (!dump.empty()) {
            std::ofstream(dump, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            std::printf("head written to %s\n", dump.c_str());
        }
        const std::string compare = argument(argc, argv, "--compare");
        int status = 0;
        if (!compare.empty()) {
            const auto expected = readFile(compare);
            require(expected.size() == bytes.size(), "head size differs from " + compare);
            size_t differing = 0;
            for (size_t index = 0; index < bytes.size(); index += 4)
                differing += std::memcmp(&bytes[index], &expected[index], 4) != 0;
            std::printf("head vs %s: %s (%zu of %zu values differ)\n", compare.c_str(),
                differing ? "MISMATCH" : "bit-exact", differing, bytes.size() / 4);
            status = differing ? 1 : 0;
        }
        if (!image.empty()) {
            const auto lanes = context.download(features->buffer, features->validBytes());
            const float* centred = reinterpret_cast<const float*>(lanes.data());
            const float* rgbw = reinterpret_cast<const float*>(bytes.data());
            std::vector<uint8_t> composed(source.size());
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x) {
                    const size_t pixel = static_cast<size_t>(y) * width + x;
                    const size_t padded = static_cast<size_t>(y) * geometry.fullWidth + x;
                    for (int c = 0; c < 3; ++c)
                        composed[pixel * 4 + c] = composeChannel(rgbw[padded * 4 + c], centred[padded * 16 + 4 + c]);
                    composed[pixel * 4 + 3] = source[pixel * 4 + 3];
                }
            const std::string output = argument(argc, argv, "--output");
            if (!output.empty()) {
                std::ofstream(output, std::ios::binary).write(reinterpret_cast<const char*>(composed.data()), composed.size());
                std::printf("output written to %s\n", output.c_str());
            }
            const std::string expect = argument(argc, argv, "--expect");
            if (!expect.empty()) compareImages(composed, readFile(expect), expect);
        }
        vkd.vkDestroyQueryPool(device.device, queries, nullptr);
        vkd.vkDestroyCommandPool(device.device, pool, nullptr);
        return status;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "vkhook-dlss5-bench: %s\n", error.what());
        return 1;
    }
}
