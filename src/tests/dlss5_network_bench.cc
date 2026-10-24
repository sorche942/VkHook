// DLSS5 network benchmark: the production PTX graph on a private device, timed with GPU timestamps.
// No layer, no swapchain: isolates the 71-block network that dominates the effect's frame cost.
//
//   vkintox-dlss5-bench --model DIR --shaders DIR --ptx DIR [--width W --height H --frames N]
//                       [--chain 0|1] [--profile] [--dump head.f32] [--compare head.f32]
//
// The recording is made once and resubmitted, as the effect does with its secondary command buffer.
// --dump/--compare write or check the f32 head bit for bit against an earlier run.
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
}

int main(int argc, char** argv)
{
    try {
        const std::string model = argument(argc, argv, "--model");
        const std::string shaders = argument(argc, argv, "--shaders");
        const std::string ptx = argument(argc, argv, "--ptx", shaders);
        require(!model.empty() && !shaders.empty(),
            "usage: vkintox-dlss5-bench --model DIR --shaders DIR [--ptx DIR] [--width W --height H --frames N] "
            "[--chain 0|1] [--profile] [--dump FILE] [--compare FILE]");
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
        std::vector<float> synthetic(static_cast<size_t>(fullRows) * 16);
        for (size_t index = 0; index < synthetic.size(); ++index)
            synthetic[index] = num::roundF16(std::sin(index * 0.0017f) * 0.125f);
        context.upload(features->buffer, synthetic.data(), synthetic.size() * sizeof(float));

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
        std::printf("%ux%u (field %ux%u), %u launches, %s\n", width, height, geometry.fullWidth, geometry.fullHeight,
            kernels.dispatchCount(), graph.chained() ? "counter chaining" : "barriers");

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
        vkd.vkDestroyQueryPool(device.device, queries, nullptr);
        vkd.vkDestroyCommandPool(device.device, pool, nullptr);
        return status;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "vkintox-dlss5-bench: %s\n", error.what());
        return 1;
    }
}
