#ifndef DLSS5_FEATURES_GLSL
#define DLSS5_FEATURES_GLSL

#extension GL_EXT_shader_explicit_arithmetic_types : require
#include "common.glsl"

// The network's encoding of display code values, from OpenDLSS-NR (MIT). The
// composite adds the head to exactly these values, so both shaders share them.
vec3 dlss5Centred(vec3 color) {
    vec3 centred;
    for (uint c = 0u; c < 3u; ++c)
        centred[c] = roundF16(roundF16(roundF16(color[c]) - 0.5) * 0.125);
    return centred;
}

#endif
