#ifndef DLSS5_DEBUG_GLSL
#define DLSS5_DEBUG_GLSL

layout(std430, binding = 6) readonly buffer DebugForwardFlow { uint vectors[]; } debugFlow;

// Display only: never publish diagnostic colors into neural history.
vec3 dlss5DebugColor(uvec2 id, bool frameAccepted, vec4 previous, float weight) {
    bool available = p.temporal != 0u && p.flowWidth != 0u && p.flowHeight != 0u;
    bool valid = available && p.historyValid != 0u;
    vec3 status = !available ? vec3(1.0, 0.0, 1.0)
        : !valid ? vec3(1.0, 0.65, 0.0)
        : frameAccepted ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    // Top strip: magenta unavailable/off, amber reset, red rejected, green accepted.
    if (id.y < 8u) return status;
    if (!valid) return status * ((((id.x / 16u) ^ (id.y / 16u)) & 1u) != 0u ? 0.25 : 0.5);
    if (p.debugView == 1u) {
        uvec2 cell = min(id / (4u * p.flowScale), uvec2(p.flowWidth - 1u, p.flowHeight - 1u));
        uint packed = debugFlow.vectors[cell.y * p.flowWidth + cell.x];
        vec2 motion = vec2(int(packed << 16u) >> 16, int(packed) >> 16) * (float(p.flowScale) / 32.0);
        float magnitude = length(motion);
        if (magnitude == 0.0) return vec3(0.0);
        // Current -> previous displacement: right red, down yellow-green,
        // left cyan, up violet. Half brightness at eight pixels of displacement.
        float hue = fract(atan(motion.y, motion.x) / 6.28318530718 + 1.0);
        vec3 wheel = clamp(abs(fract(vec3(hue) + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
        return wheel * (1.0 - exp2(-magnitude / 8.0));
    }
    bool accepted = frameAccepted && dlss5SampleAccepted(previous);
    if (p.debugView == 2u) return accepted ? vec3(0.0, previous.a, 0.0) : vec3(0.0);
    if (p.debugView == 3u) return accepted ? clamp(previous.rgb, 0.0, 1.0) : vec3(0.0);
    return vec3(weight); // Actual sigmoid * history strength * confidence.
}

#endif
