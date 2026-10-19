#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace VKIntox
{
    // Scalar std430 layout shared with shaders/glsl/dlss5_common.glsl.
    struct Dlss5Parameters
    {
        uint32_t width, height, fullWidth, fullHeight;
        uint32_t bgra, seed;
        float intensity, localTone, localStructure, skinStructure;
        uint32_t temporal, historyValid, flowWidth, flowHeight;
        float historyStrength;
        uint32_t debugView;
        uint32_t autoMask = 1;
        float style = 0;
    };

    static_assert(std::is_standard_layout_v<Dlss5Parameters>);
    static_assert(sizeof(Dlss5Parameters) == 72);
    static_assert(offsetof(Dlss5Parameters, intensity) == 24);
    static_assert(offsetof(Dlss5Parameters, temporal) == 40);
    static_assert(offsetof(Dlss5Parameters, historyStrength) == 56);
    static_assert(offsetof(Dlss5Parameters, debugView) == 60);
    static_assert(offsetof(Dlss5Parameters, autoMask) == 64);
    static_assert(offsetof(Dlss5Parameters, style) == 68);
}
