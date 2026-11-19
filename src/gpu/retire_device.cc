#include "gpu/retire_device.hh"

#include "logical_device.hh"
#include "logger.hh"

namespace VkHook
{
namespace
{
    void destroyRetired(void* ctx, const RetiredObject& object)
    {
        LogicalDevice& d   = *static_cast<LogicalDevice*>(ctx);
        const VkDevice dev = d.device;
        const auto&    vkd = d.vkd;
        switch (object.type)
        {
        case VK_OBJECT_TYPE_IMAGE:
            vkd.DestroyImage(dev, u64ToHandle<VkImage>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_IMAGE_VIEW:
            vkd.DestroyImageView(dev, u64ToHandle<VkImageView>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_BUFFER:
            vkd.DestroyBuffer(dev, u64ToHandle<VkBuffer>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_DEVICE_MEMORY:
            vkd.FreeMemory(dev, u64ToHandle<VkDeviceMemory>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_SAMPLER:
            vkd.DestroySampler(dev, u64ToHandle<VkSampler>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_SHADER_MODULE:
            vkd.DestroyShaderModule(dev, u64ToHandle<VkShaderModule>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_PIPELINE:
            vkd.DestroyPipeline(dev, u64ToHandle<VkPipeline>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_PIPELINE_LAYOUT:
            vkd.DestroyPipelineLayout(dev, u64ToHandle<VkPipelineLayout>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_PIPELINE_CACHE:
            vkd.DestroyPipelineCache(dev, u64ToHandle<VkPipelineCache>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT:
            vkd.DestroyDescriptorSetLayout(dev, u64ToHandle<VkDescriptorSetLayout>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_DESCRIPTOR_POOL:
            vkd.DestroyDescriptorPool(dev, u64ToHandle<VkDescriptorPool>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_DESCRIPTOR_SET:
        {
            const VkDescriptorSet set = u64ToHandle<VkDescriptorSet>(object.handle);
            vkd.FreeDescriptorSets(dev, u64ToHandle<VkDescriptorPool>(object.extra), 1, &set);
            break;
        }
        case VK_OBJECT_TYPE_RENDER_PASS:
            vkd.DestroyRenderPass(dev, u64ToHandle<VkRenderPass>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_FRAMEBUFFER:
            vkd.DestroyFramebuffer(dev, u64ToHandle<VkFramebuffer>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_COMMAND_POOL:
            vkd.DestroyCommandPool(dev, u64ToHandle<VkCommandPool>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_COMMAND_BUFFER:
        {
            const VkCommandBuffer cb = u64ToHandle<VkCommandBuffer>(object.handle);
            vkd.FreeCommandBuffers(dev, u64ToHandle<VkCommandPool>(object.extra), 1, &cb);
            break;
        }
        case VK_OBJECT_TYPE_FENCE:
            vkd.DestroyFence(dev, u64ToHandle<VkFence>(object.handle), nullptr);
            break;
        case VK_OBJECT_TYPE_SEMAPHORE:
            vkd.DestroySemaphore(dev, u64ToHandle<VkSemaphore>(object.handle), nullptr);
            break;
        default:
            Logger::err("RetireQueue: no destroyer for VkObjectType " + std::to_string(static_cast<int>(object.type)));
            break;
        }
    }
} // namespace

    void bindRetireDestroyer(LogicalDevice& device)
    {
        device.retireQueue.bind(&destroyRetired, &device);
    }
} // namespace VkHook
