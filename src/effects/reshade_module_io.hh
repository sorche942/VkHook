#ifndef RESHADE_MODULE_IO_HPP_INCLUDED
#define RESHADE_MODULE_IO_HPP_INCLUDED

#include <cstddef>
#include <cstdint>
#include <vector>

#include "reshade/effect_module.hh"

namespace VkHook
{
    // Flat, field-by-field encoding of a compiled ReShade module, independent of
    // the compiler's struct layout and of the host word size (every field has a
    // fixed width). This is what the on-disk shader cache stores.
    std::vector<uint8_t> serializeReshadeModule(const reshadefx::module& module);

    // Decodes what serializeReshadeModule wrote. Returns false, leaving `module`
    // unspecified, on any truncation, oversized length, trailing bytes or a
    // SPIR-V blob that does not start with the SPIR-V magic. Never reads past
    // `size`, never throws on malformed input.
    bool deserializeReshadeModule(const uint8_t* data, size_t size, reshadefx::module& module);
}

#endif // RESHADE_MODULE_IO_HPP_INCLUDED
