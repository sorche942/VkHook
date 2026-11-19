#ifndef MEMORY_HPP_INCLUDED
#define MEMORY_HPP_INCLUDED
#include <vector>
#include <fstream>
#include <string>
#include <iostream>
#include <vector>
#include <memory>

#include "vulkan_include.hh"

#include "logical_device.hh"

namespace VkHook
{
    constexpr uint32_t kInvalidMemoryTypeIndex = 0xFFFFFFFFu;

    // Returns kInvalidMemoryTypeIndex when no allowed type qualifies. Callers must
    // check it: it is not a valid index.
    uint32_t findMemoryTypeIndex(LogicalDevice* logicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);
}

#endif // MEMORY_HPP_INCLUDED
