#ifndef UNIFORM_RING_HPP_INCLUDED
#define UNIFORM_RING_HPP_INCLUDED

#include <cstddef>
#include <cstdint>
#include <vector>

namespace VkHook
{
    // Layout of one effect's uniform data across the swapchain images: a single
    // buffer holding one slice per image, so the CPU can rewrite the slice of the
    // image it is about to submit while the GPU still reads the slices of frames
    // in flight on the other images.
    //
    // Slice i is written only by updateFrame(i), which the present path calls
    // after image i's previous submission completed; that is the only ordering
    // the ring relies on.
    //
    // Slices start at multiples of the largest offset alignment Vulkan permits a
    // device to require for uniform buffers (minUniformBufferOffsetAlignment is
    // at most 256), so every slice is bindable without asking the device.
    class UniformRing
    {
    public:
        static constexpr uint64_t kOffsetAlignment = 256;
        static constexpr uint64_t kNoGeneration = ~0ull;

        UniformRing() = default;

        UniformRing(uint32_t slotCount, uint32_t slotBytes)
            : m_slotBytes(slotBytes)
            , m_stride((static_cast<uint64_t>(slotBytes) + kOffsetAlignment - 1) / kOffsetAlignment * kOffsetAlignment)
            , m_defaultsGeneration(slotBytes == 0 ? 0 : slotCount, kNoGeneration)
        {
        }

        uint32_t slotCount() const { return static_cast<uint32_t>(m_defaultsGeneration.size()); }
        uint32_t slotBytes() const { return m_slotBytes; }
        uint64_t stride() const { return m_stride; }
        uint64_t totalBytes() const { return m_stride * m_defaultsGeneration.size(); }
        bool     contains(uint32_t slot) const { return slot < m_defaultsGeneration.size(); }

        // Byte offset of slice `slot` from the start of the buffer.
        uint64_t offset(uint32_t slot) const { return m_stride * slot; }

        // Pointer to slice `slot` inside a mapping of the whole buffer.
        void* slice(void* mapped, uint32_t slot) const
        {
            return static_cast<uint8_t*>(mapped) + offset(slot);
        }

        // A slice holds the shader defaults of the uniforms that have no live
        // parameter, written once per bindings generation. Rebuilding the
        // bindings invalidates every slice, but a slice may be in flight, so it
        // is refreshed lazily by the frame that next owns it.
        bool needsDefaults(uint32_t slot, uint64_t generation) const
        {
            return m_defaultsGeneration[slot] != generation;
        }

        void markDefaults(uint32_t slot, uint64_t generation) { m_defaultsGeneration[slot] = generation; }

    private:
        uint32_t              m_slotBytes = 0;
        uint64_t              m_stride = 0;
        std::vector<uint64_t> m_defaultsGeneration;
    };
} // namespace VkHook

#endif // UNIFORM_RING_HPP_INCLUDED
