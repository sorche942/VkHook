#include "vk_context.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace vk {

namespace {
constexpr VkDeviceSize kStagingBytes = 256ull << 20;  // 256 MiB staging window
}  // namespace

Dispatch::Dispatch(VkInstance instance, VkDevice device, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa) {
  if (!instance || !device || !gipa || !gdpa) throw std::runtime_error("NR requires downstream Vulkan handles and loaders");
#define NR_INSTANCE_PROC(name) \
  name = reinterpret_cast<PFN_##name>(gipa(instance, #name)); \
  if (!name) throw std::runtime_error("missing downstream Vulkan entry point " #name);
#define NR_DEVICE_PROC(name) \
  name = reinterpret_cast<PFN_##name>(gdpa(device, #name)); \
  if (!name) throw std::runtime_error("missing downstream Vulkan entry point " #name);
#include "vk_dispatch.inc"
#undef NR_INSTANCE_PROC
#undef NR_DEVICE_PROC
}

Context::Context(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily,
                 uint32_t queueIndex, PFN_vkGetInstanceProcAddr downstreamGIPA, PFN_vkGetDeviceProcAddr downstreamGDPA)
    : Dispatch(instance, device, downstreamGIPA, downstreamGDPA),
      instance_(instance), physical_(physical), device_(device), queueFamily_(queueFamily), queueIndex_(queueIndex) {
  VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
  VkPhysicalDeviceShaderSMBuiltinsPropertiesNV smBuiltins{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_PROPERTIES_NV};
  properties.pNext = &subgroup;
  subgroup.pNext = &smBuiltins;
  vkGetPhysicalDeviceProperties2(physical_, &properties);
  smCount_ = smBuiltins.shaderSMCount;
  deviceName_ = properties.properties.deviceName;
  timestampPeriod_ = properties.properties.limits.timestampPeriod;
  maxSharedMemory_ = properties.properties.limits.maxComputeSharedMemorySize;
  vkGetPhysicalDeviceMemoryProperties(physical_, &memoryProperties_);
  try {
    initCommon();
  } catch (...) {
    destroyResources();
    throw;
  }
}

DeviceRequirements::DeviceRequirements() {
  f11.storageBuffer16BitAccess = VK_TRUE;
  f11.uniformAndStorageBuffer16BitAccess = VK_TRUE;
  f12.pNext = &f11;
  f12.storageBuffer8BitAccess = VK_TRUE;
  f12.uniformAndStorageBuffer8BitAccess = VK_TRUE;
  f12.shaderFloat16 = VK_TRUE;
  f12.shaderInt8 = VK_TRUE;
  f12.vulkanMemoryModel = VK_TRUE;
  f12.vulkanMemoryModelDeviceScope = VK_TRUE;
  f12.hostQueryReset = VK_TRUE;
  f12.bufferDeviceAddress = VK_TRUE;
  f13.pNext = &f12;
  f13.subgroupSizeControl = VK_TRUE;
  f13.computeFullSubgroups = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  f13.maintenance4 = VK_TRUE;   // LocalSizeId: gemm_fp8.comp takes its workgroup size from a specialization constant
  coop.pNext = &f13;
  coop.cooperativeMatrix = VK_TRUE;
  coop2.pNext = &coop;
  coop2.cooperativeMatrixWorkgroupScope = VK_TRUE;
  coop2.cooperativeMatrixFlexibleDimensions = VK_TRUE;
  coop2.cooperativeMatrixReductions = VK_TRUE;
  coop2.cooperativeMatrixConversions = VK_TRUE;
  coop2.cooperativeMatrixPerElementOperations = VK_TRUE;
  coop2.cooperativeMatrixTensorAddressing = VK_TRUE;
  coop2.cooperativeMatrixBlockLoads = VK_TRUE;
  fp8.pNext = &coop2;
  fp8.shaderFloat8 = VK_TRUE;
  fp8.shaderFloat8CooperativeMatrix = VK_TRUE;
  features.pNext = &fp8;
  features.features.shaderInt16 = VK_TRUE;
  features.features.shaderInt64 = VK_TRUE;
  // robustBufferAccess stays off (15% cost, masks bugs); experiment shaders must check their own ranges.
  features.features.robustBufferAccess = VK_FALSE;

  VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR& executableFeatures = executable;
  executableFeatures.pipelineExecutableInfo = VK_TRUE;
  executableFeatures.pNext = features.pNext;
  features.pNext = &executableFeatures;
  // Shader clocks (clockARB / clockRealtimeEXT).
  VkPhysicalDeviceShaderClockFeaturesKHR& clockFeatures = clock;
  clockFeatures.shaderSubgroupClock = VK_TRUE;
  clockFeatures.shaderDeviceClock = VK_TRUE;
  clockFeatures.pNext = features.pNext;
  features.pNext = &clockFeatures;
  VkPhysicalDeviceShaderSMBuiltinsFeaturesNV& smFeatures = sm;
  smFeatures.shaderSMBuiltins = VK_TRUE;
  smFeatures.pNext = features.pNext;
  features.pNext = &smFeatures;
  cuda.cudaKernelLaunchFeatures = VK_TRUE;
  cuda.pNext = features.pNext;
  features.pNext = &cuda;
  extensions = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME,
                              VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME, VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME,
                              VK_NV_CUDA_KERNEL_LAUNCH_EXTENSION_NAME, VK_KHR_SHADER_CLOCK_EXTENSION_NAME,
                              VK_NV_SHADER_SM_BUILTINS_EXTENSION_NAME};
}

void Context::initCommon() {
  vkGetDeviceQueue(device_, queueFamily_, queueIndex_, &queue_);

  VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = queueFamily_;
  VK_CHECK(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_));

  VkDescriptorSetLayoutBinding bindings[kGenericBindings];
  for (uint32_t index = 0; index < kGenericBindings; ++index) {
    bindings[index] = {index, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  }
  VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layoutInfo.bindingCount = kGenericBindings;
  layoutInfo.pBindings = bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &setLayout_));

  VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, kPushConstantBytes};
  VkPipelineLayoutCreateInfo pipelineLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipelineLayoutInfo.setLayoutCount = 1;
  pipelineLayoutInfo.pSetLayouts = &setLayout_;
  pipelineLayoutInfo.pushConstantRangeCount = 1;
  pipelineLayoutInfo.pPushConstantRanges = &pushRange;
  VK_CHECK(vkCreatePipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipelineLayout_));

  descriptorPools_.reserve(1);
  descriptorPools_.push_back(createDescriptorPool());
  descriptorPool_ = descriptorPools_.back();

  dummy_ = createBuffer(256, false, "dummy binding");
  fillZero(dummy_);
  staging_ = createBuffer(kStagingBytes, true, "staging");
}

Context::~Context() { destroyResources(); }

void Context::destroyResources() noexcept {
  // Borrowed queues and devices are never waited on or destroyed here.
  for (VkShaderModule module : modules_) vkDestroyShaderModule(device_, module, nullptr);
  destroyBuffer(dummy_);
  destroyBuffer(staging_);
  for (VkDescriptorPool pool : descriptorPools_) vkDestroyDescriptorPool(device_, pool, nullptr);
  if (pipelineLayout_) vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
  if (setLayout_) vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
  if (commandPool_) vkDestroyCommandPool(device_, commandPool_, nullptr);
}

uint32_t Context::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required) {
  for (uint32_t index = 0; index < memoryProperties_.memoryTypeCount; ++index) {
    if ((typeBits & (1u << index)) &&
        (memoryProperties_.memoryTypes[index].propertyFlags & required) == required)
      return index;
  }
  throw std::runtime_error("no suitable memory type");
}

Buffer Context::createUnboundBuffer(VkDeviceSize size, const char* label, VkMemoryRequirements& requirements,
                                    VkBufferUsageFlags extra) {
  Buffer result;
  result.size = std::max<VkDeviceSize>(size, 16);
  result.label = label;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = result.size;
  info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
               VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | extra;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VK_CHECK(vkCreateBuffer(device_, &info, nullptr, &result.buffer));
  vkGetBufferMemoryRequirements(device_, result.buffer, &requirements);
  return result;
}

VkDeviceMemory Context::allocateMemory(VkDeviceSize size, uint32_t memoryType) {
  VkMemoryAllocateFlagsInfo allocateFlags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  allocateFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocateInfo.pNext = &allocateFlags;
  allocateInfo.allocationSize = size;
  allocateInfo.memoryTypeIndex = memoryType;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateMemory(device_, &allocateInfo, nullptr, &memory));
  return memory;
}

Buffer Context::createBuffer(VkDeviceSize size, bool hostVisible, const char* label, VkBufferUsageFlags extra) {
  VkMemoryRequirements requirements;
  Buffer result = createUnboundBuffer(size, label, requirements, extra);
  result.hostVisible = hostVisible;
  try {
    result.memory = allocateMemory(requirements.size, findMemoryType(
        requirements.memoryTypeBits,
        hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    VK_CHECK(vkBindBufferMemory(device_, result.buffer, result.memory, 0));
    if (hostVisible) VK_CHECK(vkMapMemory(device_, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped));
  } catch (...) {
    destroyBuffer(result);
    throw;
  }
  return result;
}

VkDeviceMemory Context::allocateDeviceMemory(VkDeviceSize size, uint32_t memoryTypeBits) {
  return allocateMemory(size, findMemoryType(memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
}

void Context::bindBuffer(const Buffer& buffer, VkDeviceMemory memory, VkDeviceSize offset) {
  VK_CHECK(vkBindBufferMemory(device_, buffer.buffer, memory, offset));
}

void Context::freeMemory(VkDeviceMemory memory) {
  if (memory) vkFreeMemory(device_, memory, nullptr);
}

void Context::destroyBuffer(Buffer& buffer) {
  if (buffer.mapped) vkUnmapMemory(device_, buffer.memory);
  if (buffer.buffer) vkDestroyBuffer(device_, buffer.buffer, nullptr);
  if (buffer.memory) vkFreeMemory(device_, buffer.memory, nullptr);
  buffer = Buffer{};
}

void Context::upload(const Buffer& target, const void* data, VkDeviceSize size, VkDeviceSize offset) {
  if (offset + size > target.size) throw std::runtime_error(std::string("upload overflows ") + target.label);
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  VkDeviceSize done = 0;
  while (done < size) {
    VkDeviceSize chunk = std::min(size - done, staging_.size);
    memcpy(staging_.mapped, bytes + done, chunk);
    VkCommandBuffer commands = beginCommands();
    VkBufferCopy region{0, offset + done, chunk};
    vkCmdCopyBuffer(commands, staging_.buffer, target.buffer, 1, &region);
    endAndSubmit(commands, true);
    done += chunk;
  }
}

void Context::fillZero(const Buffer& target) {
  VkCommandBuffer commands = beginCommands();
  vkCmdFillBuffer(commands, target.buffer, 0, VK_WHOLE_SIZE, 0);
  endAndSubmit(commands, true);
}

std::vector<uint8_t> Context::download(const Buffer& source, VkDeviceSize size, VkDeviceSize offset) {
  if (offset + size > source.size) throw std::runtime_error(std::string("download overflows ") + source.label);
  std::vector<uint8_t> result(size);
  VkDeviceSize done = 0;
  while (done < size) {
    VkDeviceSize chunk = std::min(size - done, staging_.size);
    VkCommandBuffer commands = beginCommands();
    VkBufferCopy region{offset + done, 0, chunk};
    vkCmdCopyBuffer(commands, source.buffer, staging_.buffer, 1, &region);
    endAndSubmit(commands, true);
    memcpy(result.data() + done, staging_.mapped, chunk);
    done += chunk;
  }
  return result;
}

VkShaderModule Context::loadShaderModule(const std::string& spvPath) {
  std::ifstream file(spvPath, std::ios::binary | std::ios::ate);
  if (!file) throw std::runtime_error("missing shader " + spvPath);
  std::streamsize size = file.tellg();
  file.seekg(0);
  std::vector<uint32_t> words((size + 3) / 4);
  file.read(reinterpret_cast<char*>(words.data()), size);
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = size;
  info.pCode = words.data();
  VkShaderModule module;
  modules_.reserve(modules_.size() + 1);
  VK_CHECK(vkCreateShaderModule(device_, &info, nullptr, &module));
  modules_.push_back(module);
  return module;
}

Pipeline Context::createComputePipeline(VkShaderModule module, const SpecConstants& constants,
                                        const char* label, uint32_t requiredSubgroupSize) {
  VkSpecializationInfo specialization{};
  specialization.mapEntryCount = (uint32_t)constants.entries.size();
  specialization.pMapEntries = constants.entries.data();
  specialization.dataSize = constants.data.size() * 4;
  specialization.pData = constants.data.data();
  VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroupInfo{
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
  subgroupInfo.requiredSubgroupSize = requiredSubgroupSize;
  VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage.pNext = requiredSubgroupSize ? &subgroupInfo : nullptr;
  stage.flags = requiredSubgroupSize ? VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT : 0;
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = module;
  stage.pName = "main";
  stage.pSpecializationInfo = constants.entries.empty() ? nullptr : &specialization;
  VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage = stage;
  info.layout = pipelineLayout_;
  if (captureStatistics_)
    info.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR;
  Pipeline result;
  result.label = label;
  VK_CHECK(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &result.pipeline));
  return result;
}

std::string Context::pipelineStatistics(const Pipeline& pipeline, bool includeInternal) {
  std::string report;
  VkPipelineInfoKHR pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
  pipelineInfo.pipeline = pipeline.pipeline;
  uint32_t executableCount = 0;
  if (vkGetPipelineExecutablePropertiesKHR(device_, &pipelineInfo, &executableCount, nullptr) != VK_SUCCESS) return "n/a";
  std::vector<VkPipelineExecutablePropertiesKHR> executables(executableCount,
                                                             {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
  vkGetPipelineExecutablePropertiesKHR(device_, &pipelineInfo, &executableCount, executables.data());
  for (uint32_t e = 0; e < executableCount; ++e) {
    report += std::string(executables[e].name) + " (" + executables[e].description + ") subgroup " +
              std::to_string(executables[e].subgroupSize) + "\n";
    VkPipelineExecutableInfoKHR executableInfo{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
    executableInfo.pipeline = pipeline.pipeline;
    executableInfo.executableIndex = e;
    uint32_t statisticCount = 0;
    vkGetPipelineExecutableStatisticsKHR(device_, &executableInfo, &statisticCount, nullptr);
    std::vector<VkPipelineExecutableStatisticKHR> statistics(statisticCount,
                                                             {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
    vkGetPipelineExecutableStatisticsKHR(device_, &executableInfo, &statisticCount, statistics.data());
    for (const auto& statistic : statistics) {
      report += "  " + std::string(statistic.name) + " = ";
      switch (statistic.format) {
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: report += statistic.value.b32 ? "true" : "false"; break;
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: report += std::to_string(statistic.value.i64); break;
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: report += std::to_string(statistic.value.u64); break;
        case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: report += std::to_string(statistic.value.f64); break;
        default: break;
      }
      report += "  (" + std::string(statistic.description) + ")\n";
    }
    if (includeInternal) {
      uint32_t irCount = 0;
      vkGetPipelineExecutableInternalRepresentationsKHR(device_, &executableInfo, &irCount, nullptr);
      std::vector<VkPipelineExecutableInternalRepresentationKHR> irs(
          irCount, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
      vkGetPipelineExecutableInternalRepresentationsKHR(device_, &executableInfo, &irCount, irs.data());
      std::vector<std::vector<char>> storage(irCount);
      for (uint32_t i = 0; i < irCount; ++i) { storage[i].resize(irs[i].dataSize + 1); irs[i].pData = storage[i].data(); }
      vkGetPipelineExecutableInternalRepresentationsKHR(device_, &executableInfo, &irCount, irs.data());
      for (uint32_t i = 0; i < irCount; ++i) {
        report += "  --- " + std::string(irs[i].name) + " (" + irs[i].description + ", " + std::to_string(irs[i].dataSize) +
                  " bytes, text=" + (irs[i].isText ? "yes" : "no") + ")\n";
        if (irs[i].isText) report += std::string(storage[i].data(), irs[i].dataSize) + "\n";
      }
    }
  }
  return report;
}

void Context::destroyPipeline(Pipeline& pipeline) {
  if (pipeline.pipeline) vkDestroyPipeline(device_, pipeline.pipeline, nullptr);
  pipeline = Pipeline{};
}

VkDescriptorSet Context::allocateSet(const Buffer* const bindings[kGenericBindings],
                                     const VkDeviceSize offsets[kGenericBindings],
                                     const VkDeviceSize ranges[kGenericBindings]) {
  VkDescriptorBufferInfo infos[kGenericBindings];
  for (uint32_t index = 0; index < kGenericBindings; ++index) {
    const Buffer* buffer = bindings[index] ? bindings[index] : &dummy_;
    if (bindings[index] && observer_) buffer = &observer_->use(*buffer);
    // A stand-in has its own extent; the requested window belongs to the buffer it replaces.
    const bool window = !bindings[index] || buffer == bindings[index];
    infos[index] = {buffer->buffer, window && offsets ? offsets[index] : 0,
                    window && ranges && ranges[index] ? ranges[index] : VK_WHOLE_SIZE};
  }
  VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocateInfo.descriptorPool = descriptorPool_;
  allocateInfo.descriptorSetCount = 1;
  allocateInfo.pSetLayouts = &setLayout_;
  VkDescriptorSet set;
  VkResult result = vkAllocateDescriptorSets(device_, &allocateInfo, &set);
  if (result == VK_ERROR_OUT_OF_POOL_MEMORY || result == VK_ERROR_FRAGMENTED_POOL) {
    const size_t next = descriptorPoolIndex_ + 1;
    if (next == descriptorPools_.size()) {
      descriptorPools_.reserve(descriptorPools_.size() + 1);
      descriptorPools_.push_back(createDescriptorPool());
    }
    descriptorPoolIndex_ = next;
    descriptorPool_ = descriptorPools_[descriptorPoolIndex_];
    allocateInfo.descriptorPool = descriptorPool_;
    result = vkAllocateDescriptorSets(device_, &allocateInfo, &set);
  }
  VK_CHECK(result);
  VkWriteDescriptorSet writes[kGenericBindings];
  for (uint32_t index = 0; index < kGenericBindings; ++index) {
    writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[index].dstSet = set;
    writes[index].dstBinding = index;
    writes[index].descriptorCount = 1;
    writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[index].pBufferInfo = &infos[index];
  }
  vkUpdateDescriptorSets(device_, kGenericBindings, writes, 0, nullptr);
  return set;
}

VkDescriptorPool Context::createDescriptorPool() {
  VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192 * kGenericBindings};
  VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  info.maxSets = 8192;
  info.poolSizeCount = 1;
  info.pPoolSizes = &poolSize;
  VkDescriptorPool pool;
  VK_CHECK(vkCreateDescriptorPool(device_, &info, nullptr, &pool));
  return pool;
}

void Context::resetDescriptorPool() {
  // Only legal after all command buffers referring to these sets have been retired.
  for (VkDescriptorPool pool : descriptorPools_) VK_CHECK(vkResetDescriptorPool(device_, pool, 0));
  descriptorPoolIndex_ = 0;
  descriptorPool_ = descriptorPools_.front();
}

VkCommandBuffer Context::beginCommands() {
  VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocateInfo.commandPool = commandPool_;
  allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocateInfo.commandBufferCount = 1;
  VkCommandBuffer commands;
  VK_CHECK(vkAllocateCommandBuffers(device_, &allocateInfo, &commands));
  // Layer-owned dispatchable objects must carry the borrowed device's loader
  // dispatch table, just like command buffers allocated by VKIntox itself.
  *reinterpret_cast<void**>(commands) = *reinterpret_cast<void**>(device_);
  VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(commands, &beginInfo));
  return commands;
}

void Context::endAndSubmit(VkCommandBuffer commands, bool wait) {
  VK_CHECK(vkEndCommandBuffer(commands));
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &commands;
  VK_CHECK(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE));
  if (wait) {
    VK_CHECK(vkQueueWaitIdle(queue_));
    vkFreeCommandBuffers(device_, commandPool_, 1, &commands);
  }
}

void Context::freeCommands(VkCommandBuffer commands) {
  vkFreeCommandBuffers(device_, commandPool_, 1, &commands);
}

void Context::computeBarrier(VkCommandBuffer commands) {
  // Compute -> compute only. Including the transfer stages here made NVIDIA flush
  // caches between every dispatch (tens of microseconds per barrier once L2 is dirty).
  VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(commands, &dependency);
  if (observer_) observer_->barrier();
}

// Captures and uploads only.
void Context::transferBarrier(VkCommandBuffer commands) {
  VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                          VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.memoryBarrierCount = 1;
  dependency.pMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(commands, &dependency);
  if (observer_) observer_->barrier();
}

VkQueryPool Context::createTimestampPool(uint32_t count) {
  VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  info.queryType = VK_QUERY_TYPE_TIMESTAMP;
  info.queryCount = count;
  VkQueryPool pool;
  VK_CHECK(vkCreateQueryPool(device_, &info, nullptr, &pool));
  vkResetQueryPool(device_, pool, 0, count);
  return pool;
}

std::vector<double> Context::readTimestampsMs(VkQueryPool pool, uint32_t count) {
  std::vector<uint64_t> ticks(count);
  VK_CHECK(vkGetQueryPoolResults(device_, pool, 0, count, ticks.size() * 8, ticks.data(), 8,
                                 VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
  std::vector<double> result(count);
  for (uint32_t index = 0; index < count; ++index) result[index] = ticks[index] * (double)timestampPeriod_ * 1e-6;
  return result;
}

}  // namespace vk

namespace vk {

VkDeviceAddress Context::deviceAddress(const Buffer& buffer) const {
  VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  info.buffer = (observer_ ? observer_->use(buffer) : buffer).buffer;
  return vkGetBufferDeviceAddress(device_, &info);
}

VkCudaModuleNV Context::createCudaModule(const std::string& ptx) {
  VkCudaModuleCreateInfoNV info{VK_STRUCTURE_TYPE_CUDA_MODULE_CREATE_INFO_NV};
  info.dataSize = ptx.size() + 1;
  info.pData = ptx.c_str();
  VkCudaModuleNV module = VK_NULL_HANDLE;
  VK_CHECK(vkCreateCudaModuleNV(device_, &info, nullptr, &module));
  return module;
}

VkCudaFunctionNV Context::createCudaFunction(VkCudaModuleNV module, const char* name) {
  VkCudaFunctionCreateInfoNV info{VK_STRUCTURE_TYPE_CUDA_FUNCTION_CREATE_INFO_NV};
  info.module = module;
  info.pName = name;
  VkCudaFunctionNV function = VK_NULL_HANDLE;
  VK_CHECK(vkCreateCudaFunctionNV(device_, &info, nullptr, &function));
  return function;
}

void Context::destroyCudaFunction(VkCudaFunctionNV function) {
  if (function) vkDestroyCudaFunctionNV(device_, function, nullptr);
}

void Context::destroyCudaModule(VkCudaModuleNV module) {
  if (module) vkDestroyCudaModuleNV(device_, module, nullptr);
}

void Context::cudaLaunch(VkCommandBuffer commands, VkCudaFunctionNV function, uint32_t gridX, uint32_t gridY, uint32_t gridZ,
                         uint32_t blockX, uint32_t sharedBytes, const void* const* params, size_t paramCount) {
  VkCudaLaunchInfoNV info{VK_STRUCTURE_TYPE_CUDA_LAUNCH_INFO_NV};
  info.function = function;
  info.gridDimX = gridX; info.gridDimY = gridY; info.gridDimZ = gridZ;
  info.blockDimX = blockX; info.blockDimY = 1; info.blockDimZ = 1;
  info.sharedMemBytes = sharedBytes;
  info.paramCount = paramCount;
  info.pParams = params;
  vkCmdCudaLaunchKernelNV(commands, &info);
}

}  // namespace vk
