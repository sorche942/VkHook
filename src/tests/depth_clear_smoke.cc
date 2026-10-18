// Opt-in GPU regression for dynamic rendering and legacy render passes. Build with the generated shader
// directory on the include path; test_depth_clear_smoke.py owns isolation and
// checks the layer's selected-depth evidence and postprocessed window pixels.
#define VK_USE_PLATFORM_XLIB_KHR
#include <vulkan/vulkan.h>
#include <X11/Xlib.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <poll.h>
#include <unistd.h>
#include "native_material_smoke_spirv.hh"

static void check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error(std::string(operation) + ": VkResult=" + std::to_string(result));
}
#define VK_CHECK(call) check((call), #call)

static const uint32_t vertexCode[] = {
#include "full_screen_triangle.vert.h"
};
// SPIR-V 1.0: fragment shader, location 0 = vec4(1). The pipeline's
// CONSTANT_COLOR blend factor supplies the draw color without extra resources.
static const uint32_t fragmentCode[] = {
    0x07230203, 0x00010000, 0, 12, 0,
    0x00020011, 1,                         // OpCapability Shader
    0x0003000e, 0, 1,                      // Logical GLSL450
    0x0006000f, 4, 10, 0x6e69616d, 0, 6,   // Fragment main %6
    0x00030010, 10, 7,                     // OriginUpperLeft
    0x00040047, 6, 30, 0,                  // Location 0
    0x00020013, 1,                         // void
    0x00030021, 2, 1,                      // function type
    0x00030016, 3, 32,                     // float
    0x00040017, 4, 3, 4,                   // vec4
    0x00040020, 5, 3, 4,                   // Output vec4 pointer
    0x0004003b, 5, 6, 3,                   // output variable
    0x0004002b, 3, 7, 0x3f800000,           // 1.0
    0x0007002c, 4, 8, 7, 7, 7, 7,         // white
    0x00050036, 1, 10, 0, 2,               // main
    0x000200f8, 11,
    0x0003003e, 6, 8,
    0x000100fd,
    0x00010038
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

int main(int argc, char** argv)
{
    try {
        uint32_t width = 640, height = 480;
        bool dynamicRendering = true;
        bool nativeMaterial = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--native-material") == 0) {
                nativeMaterial = true;
                continue;
            }
            if (std::strcmp(argv[i], "--legacy") == 0) {
                dynamicRendering = false;
                continue;
            }
            if (i + 1 == argc) throw std::runtime_error("expected --width N or --height N");
            const std::string option = argv[i++];
            const unsigned long value = std::stoul(argv[i]);
            if (value < 64 || value > 8192) throw std::runtime_error("dimension outside 64..8192");
            if (option == "--width") width = value;
            else if (option == "--height") height = value;
            else throw std::runtime_error("unknown option: " + option);
        }
        Display* display = XOpenDisplay(nullptr);
        if (!display) throw std::runtime_error("XOpenDisplay failed");
        Window window = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                            0, 0, width, height, 0, 0, 0);
        XStoreName(display, window, "VKIntox depth clear regression");
        XMapRaised(display, window);
        XSync(display, False);
        std::printf("WINDOW=%lu\n", window);
        std::fflush(stdout);

        const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "VKIntox depth clear regression";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        instanceInfo.enabledExtensionCount = 2;
        instanceInfo.ppEnabledExtensionNames = instanceExtensions;
        VkInstance instance;
        VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &instance));
        VkXlibSurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.dpy = display;
        surfaceInfo.window = window;
        VkSurfaceKHR surface;
        VK_CHECK(vkCreateXlibSurfaceKHR(instance, &surfaceInfo, nullptr, &surface));
        uint32_t count = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> physicalDevices(count);
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, physicalDevices.data()));
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        for (auto candidate : physicalDevices) {
            uint32_t queueCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, nullptr);
            std::vector<VkQueueFamilyProperties> queues(queueCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueCount, queues.data());
            for (uint32_t i = 0; i < queueCount; ++i) {
                VkBool32 present = VK_FALSE;
                VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present));
                if (present && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                    physical = candidate;
                    queueFamily = i;
                    break;
                }
            }
            if (physical) break;
        }
        if (!physical) throw std::runtime_error("no graphics+present queue");
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical, &properties);
        std::printf("GPU=%s\n", properties.deviceName);
        VkFormatProperties depthProperties;
        vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_D24_UNORM_S8_UINT, &depthProperties);
        const auto requiredDepth = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((depthProperties.optimalTilingFeatures & requiredDepth) != requiredDepth)
            throw std::runtime_error("D24S8 sampled depth attachment unsupported");
        const float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        const char* deviceExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        VkPhysicalDeviceDynamicRenderingFeatures renderingFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
        renderingFeatures.dynamicRendering = VK_TRUE;
        deviceInfo.pNext = dynamicRendering ? &renderingFeatures : nullptr;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1;
        deviceInfo.ppEnabledExtensionNames = &deviceExtension;
        VkDevice device;
        VK_CHECK(vkCreateDevice(physical, &deviceInfo, nullptr, &device));
        VkQueue queue;
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        VkPhysicalDeviceMemoryProperties memoryProperties;
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        auto memoryType = [&](uint32_t bits, VkMemoryPropertyFlags flags) {
            for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
                if ((bits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags)
                    return i;
            throw std::runtime_error("required memory type unavailable");
        };

        VkSurfaceCapabilitiesKHR capabilities;
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities));
        if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            throw std::runtime_error("swapchain cannot receive transfer");
        VkExtent2D extent = capabilities.currentExtent;
        if (extent.width == UINT32_MAX) extent = {width, height};
        if (extent.width != width || extent.height != height)
            throw std::runtime_error("window manager changed requested extent");
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr));
        std::vector<VkSurfaceFormatKHR> formats(count);
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()));
        VkSurfaceFormatKHR format{};
        for (const auto& candidate : formats)
            if ((candidate.format == VK_FORMAT_B8G8R8A8_UNORM
                 || (!nativeMaterial && candidate.format == VK_FORMAT_R8G8B8A8_UNORM))
                && candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                format = candidate;
                break;
            }
        if (format.format == VK_FORMAT_UNDEFINED) throw std::runtime_error("8-bit UNORM surface unavailable");
        uint32_t imageCount = std::max(2u, capabilities.minImageCount);
        if (capabilities.maxImageCount) imageCount = std::min(imageCount, capabilities.maxImageCount);
        VkSwapchainCreateInfoKHR swapchainInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        swapchainInfo.surface = surface;
        swapchainInfo.minImageCount = imageCount;
        swapchainInfo.imageFormat = format.format;
        swapchainInfo.imageColorSpace = format.colorSpace;
        swapchainInfo.imageExtent = extent;
        swapchainInfo.imageArrayLayers = 1;
        swapchainInfo.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchainInfo.preTransform = capabilities.currentTransform;
        for (auto alpha : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                           VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (capabilities.supportedCompositeAlpha & alpha) { swapchainInfo.compositeAlpha = alpha; break; }
        swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, nullptr));
        std::vector<VkPresentModeKHR> modes(count);
        VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count, modes.data()));
        if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end())
            swapchainInfo.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        swapchainInfo.clipped = VK_TRUE;
        VkSwapchainKHR swapchain;
        VK_CHECK(vkCreateSwapchainKHR(device, &swapchainInfo, nullptr, &swapchain));
        VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr));
        std::vector<VkImage> swapImages(count);
        VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &count, swapImages.data()));

        auto makeImage = [&](VkFormat imageFormat, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
            Image image;
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = imageFormat;
            info.extent = {width, height, 1};
            info.mipLevels = 1;
            info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = usage;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VK_CHECK(vkCreateImage(device, &info, nullptr, &image.image));
            VkMemoryRequirements requirements;
            vkGetImageMemoryRequirements(device, image.image, &requirements);
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VK_CHECK(vkAllocateMemory(device, &allocation, nullptr, &image.memory));
            VK_CHECK(vkBindImageMemory(device, image.image, image.memory, 0));
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = image.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = imageFormat;
            view.subresourceRange = {aspect, 0, 1, 0, 1};
            VK_CHECK(vkCreateImageView(device, &view, nullptr, &image.view));
            return image;
        };
        Image color = makeImage(format.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
        Image depth = makeImage(VK_FORMAT_D24_UNORM_S8_UINT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        const VkImageLayout depthLayout = dynamicRendering ? VK_IMAGE_LAYOUT_GENERAL
                                                          : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        std::printf("DEPTH format=D24_UNORM_S8_UINT samples=1 layout=%d extent=%ux%u offscreenColor=true\n",
                    int(depthLayout), width, height);
        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        if (!dynamicRendering) {
            std::array<VkAttachmentDescription, 2> attachments{};
            attachments[0].format = format.format;
            attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            attachments[1].format = VK_FORMAT_D24_UNORM_S8_UINT;
            attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachments[1].finalLayout = depthLayout;
            VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
            VkAttachmentReference depthReference{1, depthLayout};
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorReference;
            subpass.pDepthStencilAttachment = &depthReference;
            std::array<VkSubpassDependency, 2> dependencies{};
            dependencies[0] = {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0};
            dependencies[1] = {0, VK_SUBPASS_EXTERNAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT, 0};
            VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
            passInfo.attachmentCount = attachments.size();
            passInfo.pAttachments = attachments.data();
            passInfo.subpassCount = 1;
            passInfo.pSubpasses = &subpass;
            passInfo.dependencyCount = dependencies.size();
            passInfo.pDependencies = dependencies.data();
            VK_CHECK(vkCreateRenderPass(device, &passInfo, nullptr, &renderPass));
            const VkImageView views[] = {color.view, depth.view};
            VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            framebufferInfo.renderPass = renderPass;
            framebufferInfo.attachmentCount = 2;
            framebufferInfo.pAttachments = views;
            framebufferInfo.width = width;
            framebufferInfo.height = height;
            framebufferInfo.layers = 1;
            VK_CHECK(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer));
        }
        auto shader = [&](const uint32_t* code, size_t size) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = size;
            info.pCode = code;
            VkShaderModule module;
            VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
            return module;
        };
        VkShaderModule vertex = shader(vertexCode, sizeof(vertexCode));
        const auto nativeCode = nativeMaterialSmokeSpirv();
        VkShaderModule fragment = nativeMaterial
            ? shader(nativeCode.data(), nativeCode.size() * sizeof(uint32_t))
            : shader(fragmentCode, sizeof(fragmentCode));
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        VkPipelineLayout layout;
        VK_CHECK(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout));
        VkPipelineShaderStageCreateInfo stages[2]{};
        for (auto& stage : stages) { stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main"; }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depthState{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depthState.depthTestEnable = VK_TRUE;
        depthState.depthWriteEnable = VK_TRUE;
        depthState.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = nativeMaterial ? VK_FALSE : VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;
        VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 3;
        dynamic.pDynamicStates = dynamics;
        VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        VkPipelineRenderingCreateInfo renderingPipeline{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        renderingPipeline.colorAttachmentCount = 1;
        renderingPipeline.pColorAttachmentFormats = &format.format;
        renderingPipeline.depthAttachmentFormat = renderingPipeline.stencilAttachmentFormat = VK_FORMAT_D24_UNORM_S8_UINT;
        pipelineInfo.pNext = dynamicRendering ? &renderingPipeline : nullptr;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &assembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthState;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = layout;
        pipelineInfo.renderPass = renderPass;
        VkPipeline pipeline;
        VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline));
        VkPipeline rgbBlendPipeline = VK_NULL_HANDLE;
        if (nativeMaterial) {
            blendAttachment.blendEnable = VK_TRUE;
            blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachment.colorWriteMask = 7; // DDDA alpha-tested producers preserve group alpha.
            VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &rgbBlendPipeline));
        }

        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = 8;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkBuffer readback;
        VK_CHECK(vkCreateBuffer(device, &bufferInfo, nullptr, &readback));
        VkMemoryRequirements bufferRequirements;
        vkGetBufferMemoryRequirements(device, readback, &bufferRequirements);
        VkMemoryAllocateInfo bufferAllocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        bufferAllocation.allocationSize = bufferRequirements.size;
        bufferAllocation.memoryTypeIndex = memoryType(bufferRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkDeviceMemory readbackMemory;
        VK_CHECK(vkAllocateMemory(device, &bufferAllocation, nullptr, &readbackMemory));
        VK_CHECK(vkBindBufferMemory(device, readback, readbackMemory, 0));
        void* mapped;
        VK_CHECK(vkMapMemory(device, readbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped));
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = queueFamily;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCommandPool pool;
        VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = pool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        VkCommandBuffer command;
        VK_CHECK(vkAllocateCommandBuffers(device, &commandInfo, &command));
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore acquired;
        std::vector<VkSemaphore> finished(swapImages.size());
        VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &acquired));
        for (auto& semaphore : finished)
            VK_CHECK(vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore));
        for (unsigned phase = 0; phase != (nativeMaterial ? 5u : 2u); ++phase) {
            if (nativeMaterial && (phase == 2 || phase == 3)) {
                // Prior phase waited for the queue. Reset the CB before
                // destroying its recorded application attachment references.
                VK_CHECK(vkResetCommandBuffer(command, 0));
                if (framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
                vkDestroyImageView(device, color.view, nullptr);
                vkDestroyImage(device, color.image, nullptr);
                vkFreeMemory(device, color.memory, nullptr);
                width = phase == 2 ? extent.width / 2 : extent.width;
                height = phase == 2 ? extent.height / 2 : extent.height;
                color = makeImage(format.format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
                if (!dynamicRendering) {
                    const VkImageView views[] = {color.view, depth.view};
                    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
                    info.renderPass = renderPass;
                    info.attachmentCount = 2;
                    info.pAttachments = views;
                    info.width = width;
                    info.height = height;
                    info.layers = 1;
                    VK_CHECK(vkCreateFramebuffer(device, &info, nullptr, &framebuffer));
                }
            }
            bool readyAnnounced = false;
            for (unsigned frame = 0;; ++frame) {
                uint32_t index;
                VK_CHECK(vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index));
                VK_CHECK(vkResetCommandBuffer(command, 0));
                VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                VK_CHECK(vkBeginCommandBuffer(command, &begin));
                if (dynamicRendering) {
                    std::array<VkImageMemoryBarrier, 2> transitions{};
                    for (auto& barrier : transitions) {
                        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    }
                    transitions[0].image = color.image;
                    transitions[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    transitions[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    transitions[1].image = depth.image;
                    transitions[1].newLayout = depthLayout;
                    transitions[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                    transitions[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                            | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        0, 0, nullptr, 0, nullptr, transitions.size(), transitions.data());
                    VkRenderingAttachmentInfo colorAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    colorAttachment.imageView = color.view;
                    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    if (nativeMaterial && phase == 3) colorAttachment.clearValue.color.float32[3] = 1.0f / 255.0f;
                    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    depthAttachment.imageView = depth.view;
                    depthAttachment.imageLayout = depthLayout;
                    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    depthAttachment.clearValue.depthStencil = {1.0f, 0};
                    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
                    rendering.renderArea = {{0, 0}, {width, height}};
                    rendering.layerCount = 1;
                    rendering.colorAttachmentCount = 1;
                    rendering.pColorAttachments = &colorAttachment;
                    rendering.pDepthAttachment = rendering.pStencilAttachment = &depthAttachment;
                    vkCmdBeginRendering(command, &rendering);
                } else {
                    VkClearValue clears[2]{};
                    if (nativeMaterial && phase == 3) clears[0].color.float32[3] = 1.0f / 255.0f;
                    clears[1].depthStencil = {1.0f, 0};
                    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                    pass.renderPass = renderPass;
                    pass.framebuffer = framebuffer;
                    pass.renderArea = {{0, 0}, {width, height}};
                    pass.clearValueCount = 2;
                    pass.pClearValues = clears;
                    vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
                }
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  nativeMaterial && phase == 3 ? rgbBlendPipeline : pipeline);
                VkRect2D scissor{{0, 0}, {width, height}};
                vkCmdSetScissor(command, 0, 1, &scissor);
                auto draw = [&](float z, float r, float g, float b) {
                    VkViewport viewport{0, 0, float(width), float(height), z, z};
                    const float constants[] = {r, g, b, 1};
                    vkCmdSetViewport(command, 0, 1, &viewport);
                    vkCmdSetBlendConstants(command, constants);
                    vkCmdDraw(command, 3, 1, 0, 0);
                };
                if (!nativeMaterial || (phase != 1 && phase != 4))
                    draw(0.25f, 1, 0, 0);
                if (!nativeMaterial && phase) {
                    VkClearAttachment clear{};
                    clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                    clear.clearValue.depthStencil.depth = 0.9f;
                    VkClearRect rectangle{{{0, 0}, {width / 2, height}}, 0, 1};
                    // No pipeline rebind after the clear: catch layer state clobber.
                    vkCmdClearAttachments(command, 1, &clear, 1, &rectangle);
                    draw(0.75f, 0, 1, 0);
                    draw(0.85f, 0, 0, 1); // Must fail depth everywhere.
                }
                if (dynamicRendering) {
                    vkCmdEndRendering(command);
                    VkImageMemoryBarrier colorRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                    colorRead.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    colorRead.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    colorRead.srcQueueFamilyIndex = colorRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    colorRead.image = color.image;
                    colorRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    colorRead.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    colorRead.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &colorRead);
                } else {
                    vkCmdEndRenderPass(command);
                }
                std::array<VkBufferImageCopy, 2> samples{};
                for (unsigned i = 0; i != samples.size(); ++i) {
                    samples[i].bufferOffset = i * 4;
                    samples[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    samples[i].imageOffset = {int32_t(width * (i ? 3 : 1) / 4), int32_t(height / 4), 0};
                    samples[i].imageExtent = {1, 1, 1};
                }
                vkCmdCopyImageToBuffer(command, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       readback, samples.size(), samples.data());
                VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = swapImages[index];
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     0, 0, nullptr, 0, nullptr, 1, &barrier);
                VkImageCopy copy{};
                copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.extent = {width, height, 1};
                vkCmdCopyImage(command, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = 0;
                VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
                hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                hostBarrier.buffer = readback;
                hostBarrier.size = VK_WHOLE_SIZE;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                    0, 0, nullptr, 1, &hostBarrier, 1, &barrier);
                VK_CHECK(vkEndCommandBuffer(command));
                const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.waitSemaphoreCount = 1;
                submit.pWaitSemaphores = &acquired;
                submit.pWaitDstStageMask = &waitStage;
                submit.commandBufferCount = 1;
                submit.pCommandBuffers = &command;
                submit.signalSemaphoreCount = 1;
                submit.pSignalSemaphores = &finished[index];
                VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
                VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                present.waitSemaphoreCount = 1;
                present.pWaitSemaphores = &finished[index];
                present.swapchainCount = 1;
                present.pSwapchains = &swapchain;
                present.pImageIndices = &index;
                VK_CHECK(vkQueuePresentKHR(queue, &present));
                VK_CHECK(vkQueueWaitIdle(queue));
                const auto* pixels = static_cast<const uint8_t*>(mapped);
                for (unsigned i = 0; i != 2; ++i) {
                    if (nativeMaterial) {
                        const bool missing = phase == 1 || phase == 4;
                        const uint8_t expected[] = {
                            uint8_t(missing ? 0 : 110), uint8_t(missing ? 0 : 128),
                            uint8_t(missing ? 0 : 128), uint8_t(missing ? 0 : 1)};
                        for (unsigned channel = 0; channel < 4; ++channel)
                            if (std::abs(int(pixels[i * 4 + channel]) - int(expected[channel])) > (channel == 3 ? 0 : 1))
                                throw std::runtime_error("native BGRA8 producer readback mismatch phase=" + std::to_string(phase)
                                    + " frame=" + std::to_string(frame) + " channel=" + std::to_string(channel)
                                    + " actual=" + std::to_string(pixels[i * 4 + channel])
                                    + " expected=" + std::to_string(expected[channel]));
                        continue;
                    }
                    const unsigned red = format.format == VK_FORMAT_B8G8R8A8_UNORM ? 2 : 0;
                    const unsigned blue = 2 - red;
                    const bool green = phase && i == 0;
                    if (pixels[i * 4 + red] != (green ? 0 : 255)
                        || pixels[i * 4 + 1] != (green ? 255 : 0) || pixels[i * 4 + blue] != 0)
                        throw std::runtime_error("GPU color readback mismatch phase=" + std::to_string(phase)
                            + " frame=" + std::to_string(frame) + " pixel=" + std::to_string(i));
                }
                if (frame + 1 >= 120) {
                    if (!readyAnnounced) {
                        const char* nativePhases[] = {"native", "missing", "resized", "restored", "missing-again"};
                        std::printf("READY phase=%s color_readback=PASS frames=%u\n",
                                    nativeMaterial ? nativePhases[phase] : (phase ? "clear" : "warmup"), frame + 1);
                        std::fflush(stdout);
                        readyAnnounced = true;
                    }
                    // Keep presenting until the wrapper sees the postprocessed
                    // image. Stopping at frame 120 can precede deferred startup,
                    // especially with an immediate-mode offscreen compositor.
                    pollfd input{STDIN_FILENO, POLLIN, 0};
                    if (poll(&input, 1, 0) > 0) {
                        char response[32];
                        if (!std::fgets(response, sizeof(response), stdin) || std::strcmp(response, "continue\n") != 0)
                            throw std::runtime_error("wrapper handshake missing");
                        break;
                    }
                }
            }
        }
        VK_CHECK(vkDeviceWaitIdle(device));
        for (auto semaphore : finished)
            vkDestroySemaphore(device, semaphore, nullptr);
        vkDestroySemaphore(device, acquired, nullptr);
        vkDestroyCommandPool(device, pool, nullptr);
        vkUnmapMemory(device, readbackMemory);
        vkDestroyBuffer(device, readback, nullptr);
        vkFreeMemory(device, readbackMemory, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        if (rgbBlendPipeline) vkDestroyPipeline(device, rgbBlendPipeline, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyShaderModule(device, fragment, nullptr);
        vkDestroyShaderModule(device, vertex, nullptr);
        vkDestroyFramebuffer(device, framebuffer, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);
        for (const Image& image : {depth, color}) {
            vkDestroyImageView(device, image.view, nullptr);
            vkDestroyImage(device, image.image, nullptr);
            vkFreeMemory(device, image.memory, nullptr);
        }
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        std::puts("PASS clean_shutdown=true");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
