#pragma once
#include "vulkan_include.hh"
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <string_view>

namespace VKIntox {
struct LogicalDevice;
struct NativeSnapshot {
    LogicalDevice* device = nullptr;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkExtent3D extent{};
    ~NativeSnapshot();
};
bool nativeCaptureEnabled();
bool nativeProducerShader(const uint32_t* words, size_t bytes);

// All tracking is protected by globalLock. Images retained here are layer-owned;
// application image/view handles are used only while recording an application CB.
struct NativeCapture {
    struct Image { VkExtent3D extent{}; uint64_t generation = 0; };
    struct View { VkImage image{}; uint64_t generation = 0; };
    struct Pass { uint32_t attachment = VK_ATTACHMENT_UNUSED; VkImageLayout finalLayout{}; bool usable = false; };
    struct Framebuffer { std::vector<VkImageView> attachments; };
    struct Pipeline {
        bool producer = false, output = false, discard = false;
        bool alphaPreserved = false, blendEnabled = false;
        uint32_t dynamic = 0;
    };
    struct Command {
        VkCommandPool pool{};
        uint32_t queueFamily = UINT32_MAX;
        VkCommandBufferLevel level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        bool inherited = false, inScope = false, producer = false, drew = false, rejected = false;
        bool alphaPreserved = false, staticBlending = false;
        uint32_t dynamicRequired = 0, dynamicKnown = 0, dynamicValid = 0;
        VkImageView attachment{};
        VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkExtent2D area{};
        // Keep every recorded copy alive, not merely the last candidate.
        std::vector<std::shared_ptr<NativeSnapshot>> copies;
        std::shared_ptr<NativeSnapshot> last;
    };
    uint64_t generation = 0;
    uint32_t queueFamily = UINT32_MAX;
    bool queueSafe = false;
    std::unordered_map<std::string_view, PFN_vkVoidFunction> driverFunctions;
    std::unordered_map<VkCommandPool, uint32_t> pools;
    std::unordered_map<VkImage, Image> images;
    std::unordered_map<VkImageView, View> views;
    std::unordered_set<VkShaderModule> shaders;
    std::unordered_map<VkPipeline, Pipeline> pipelines;
    std::unordered_set<std::string> shaderIdentifiers;
    std::unordered_map<VkRenderPass, Pass> passes;
    std::unordered_map<VkFramebuffer, Framebuffer> framebuffers;
    std::unordered_map<VkCommandBuffer, Command> commands;
    std::shared_ptr<NativeSnapshot> pending;
    VkQueue pendingQueue = VK_NULL_HANDLE;
    bool queueConflict = false;
    int availability = -1;

    void begin(VkCommandBuffer cb, const VkCommandBufferBeginInfo& info);
    std::vector<std::shared_ptr<NativeSnapshot>> recycled;
    void dynamicBegin(VkCommandBuffer cb, const VkRenderingInfo& info);
    void legacyBegin(VkCommandBuffer cb, const VkRenderPassBeginInfo& info);
    void draw(VkCommandBuffer cb, bool nonempty);
    void execute(VkCommandBuffer cb, uint32_t count, const VkCommandBuffer* children);
    void end(LogicalDevice* device, VkCommandBuffer cb);
    void submitted(VkQueue queue, VkCommandBuffer cb);
    std::shared_ptr<NativeSnapshot> takeFrame(VkQueue queue);
    void erasePool(VkCommandPool pool, bool destroy);
};
PFN_vkVoidFunction nativeIntercept(const char* name);
}
