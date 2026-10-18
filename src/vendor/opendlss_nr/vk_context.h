// Minimal Vulkan compute host for the DLSS-NR port: one device, one queue,
// storage buffers, compute pipelines with specialization constants, a generic
// eight-binding descriptor layout and GPU timestamps. No graphics, no windows.
#pragma once
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_beta.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#define VK_CHECK(expr)                                                                 \
  do {                                                                                 \
    VkResult vk_check_result_ = (expr);                                                \
    if (vk_check_result_ != VK_SUCCESS)                                                \
      throw std::runtime_error(std::string(#expr) + " failed: " +                      \
                               std::to_string((int)vk_check_result_));                 \
  } while (0)

namespace vk {

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  bool hostVisible = false;
  void* mapped = nullptr;
  const char* label = "";
};

constexpr uint32_t kGenericBindings = 12;
constexpr uint32_t kPushConstantBytes = 128;

struct SpecConstants {
  std::vector<VkSpecializationMapEntry> entries;
  std::vector<uint32_t> data;
  void add(uint32_t id, uint32_t value) {
    entries.push_back({id, (uint32_t)(data.size() * 4), 4});
    data.push_back(value);
  }
  void addFloat(uint32_t id, float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    add(id, bits);
  }
};

struct Pipeline {
  VkPipeline pipeline = VK_NULL_HANDLE;
  const char* label = "";
};

// Required Vulkan 1.3 features and extensions. Merge true fields into the application's
// existing feature chain (do not append duplicate sTypes). This self-linked object is not copyable.
struct DeviceRequirements {
  VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
  VkPhysicalDeviceCooperativeMatrix2FeaturesNV coop2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV};
  VkPhysicalDeviceShaderFloat8FeaturesEXT fp8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
  VkPhysicalDeviceCudaKernelLaunchFeaturesNV cuda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUDA_KERNEL_LAUNCH_FEATURES_NV};
  VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
  VkPhysicalDeviceShaderClockFeaturesKHR clock{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
  VkPhysicalDeviceShaderSMBuiltinsFeaturesNV sm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_FEATURES_NV};
  VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  std::vector<const char*> extensions;
  DeviceRequirements();                       // fills the chain (features.pNext -> ...) and the extension list
  DeviceRequirements(const DeviceRequirements&) = delete;
  DeviceRequirements& operator=(const DeviceRequirements&) = delete;
  static constexpr uint32_t apiVersion = VK_API_VERSION_1_3;
  static constexpr VkQueueFlags queueFlags = VK_QUEUE_COMPUTE_BIT;
  void* pNextChain() { return features.pNext; }   // for a VkDeviceCreateInfo that carries VkPhysicalDeviceFeatures itself
};

// Every entry is loaded from the supplied downstream loaders, never from the process loader.
struct Dispatch {
#define NR_INSTANCE_PROC(name) PFN_##name name = nullptr;
#define NR_DEVICE_PROC(name) PFN_##name name = nullptr;
#include "vk_dispatch.inc"
#undef NR_INSTANCE_PROC
#undef NR_DEVICE_PROC
  Dispatch(VkInstance instance, VkDevice device, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa);
};

class Context : private Dispatch {
 public:
  // Borrows handles enabled with DeviceRequirements. The caller externally synchronizes
  // queue access, recording, and destruction, and waits before destroying dependent resources.
  Context(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily,
          uint32_t queueIndex, PFN_vkGetInstanceProcAddr downstreamGIPA, PFN_vkGetDeviceProcAddr downstreamGDPA);
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  const Dispatch& dispatch() const { return *this; }
  ~Context();
  uint32_t queueFamily() const { return queueFamily_; }
  uint32_t queueIndex() const { return queueIndex_; }
  VkInstance instance() const { return instance_; }

  VkDevice device() const { return device_; }

  uint32_t smCount() const { return smCount_; }
  VkQueue queue() const { return queue_; }
  VkPhysicalDevice physical() const { return physical_; }
  float timestampPeriodNs() const { return timestampPeriod_; }
  VkPipelineLayout pipelineLayout() const { return pipelineLayout_; }
  VkDescriptorSetLayout setLayout() const { return setLayout_; }
  const std::string& deviceName() const { return deviceName_; }

  // Buffers -----------------------------------------------------------------
  Buffer createBuffer(VkDeviceSize size, bool hostVisible, const char* label,
                      VkBufferUsageFlags extra = 0);
  void destroyBuffer(Buffer& buffer);
  // Upload through a staging buffer and wait for completion.
  void upload(const Buffer& target, const void* data, VkDeviceSize size, VkDeviceSize offset = 0);
  void fillZero(const Buffer& target);
  // Download via staging buffer and wait for completion.
  std::vector<uint8_t> download(const Buffer& source, VkDeviceSize size, VkDeviceSize offset = 0);
  const Buffer& dummyBuffer() const { return dummy_; }

  // Pipelines -----------------------------------------------------------------
  VkShaderModule loadShaderModule(const std::string& spvPath);
  Pipeline createComputePipeline(VkShaderModule module, const SpecConstants& constants,
                                 const char* label, uint32_t requiredSubgroupSize = 32);
  void destroyPipeline(Pipeline& pipeline);
  // VK_KHR_pipeline_executable_properties: register/spill statistics (and SASS when available).
  void setCaptureStatistics(bool enabled) { captureStatistics_ = enabled; }
  std::string pipelineStatistics(const Pipeline& pipeline, bool includeInternal = false);

  // Descriptors: one generic layout with kGenericBindings storage buffers.
  VkDescriptorSet allocateSet(const Buffer* const bindings[kGenericBindings],
                              const VkDeviceSize offsets[kGenericBindings] = nullptr,
                              const VkDeviceSize ranges[kGenericBindings] = nullptr);
  // Explicit reset only: the caller must first retire every command buffer referring to these sets.
  void resetDescriptorPool();

  // Commands --------------------------------------------------------------------
  VkCommandBuffer beginCommands();
  void endAndSubmit(VkCommandBuffer commands, bool wait = true);
  void waitIdle() { VK_CHECK(vkQueueWaitIdle(queue_)); }
  void computeBarrier(VkCommandBuffer commands);   // compute -> compute
  // VK_NV_cuda_kernel_launch: PTX modules launched from the command buffer on buffer device addresses.
  VkDeviceAddress deviceAddress(const Buffer& buffer) const;
  VkCudaModuleNV createCudaModule(const std::string& ptx);
  VkCudaFunctionNV createCudaFunction(VkCudaModuleNV module, const char* name);
  void destroyCudaFunction(VkCudaFunctionNV function);
  void destroyCudaModule(VkCudaModuleNV module);
  void cudaLaunch(VkCommandBuffer commands, VkCudaFunctionNV function, uint32_t gridX, uint32_t gridY, uint32_t gridZ,
                  uint32_t blockX, uint32_t sharedBytes, const void* const* params, size_t paramCount);
  void transferBarrier(VkCommandBuffer commands);  // compute/transfer -> compute/transfer (captures, uploads)

  // Timestamps -----------------------------------------------------------------
  VkQueryPool createTimestampPool(uint32_t count);
  std::vector<double> readTimestampsMs(VkQueryPool pool, uint32_t count);

  uint32_t maxComputeSharedMemory() const { return maxSharedMemory_; }

 private:
  uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required);
  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  uint32_t queueIndex_ = 0;
  VkCommandPool commandPool_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
  std::vector<VkDescriptorPool> descriptorPools_;
  size_t descriptorPoolIndex_ = 0;
  VkPhysicalDeviceMemoryProperties memoryProperties_{};
  float timestampPeriod_ = 1.0f;
  uint32_t maxSharedMemory_ = 0;
  uint32_t smCount_ = 0;   // streaming multiprocessors (co-residency bound of spinning grids)
  bool captureStatistics_ = false;
  std::string deviceName_;
  void initCommon();   // properties, memory types, command pool, layouts, pools, staging
  void destroyResources() noexcept;
  VkDescriptorPool createDescriptorPool();
  Buffer dummy_;
  Buffer staging_;
  std::vector<VkShaderModule> modules_;
};

}  // namespace vk
