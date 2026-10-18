#ifndef DLSS5_COMMON_GLSL
#define DLSS5_COMMON_GLSL

// Scalar std430 layout shared with VKIntox::Dlss5Parameters (64 bytes).
layout(std430, binding = 1) readonly buffer Parameters {
    uint width, height, fullWidth, fullHeight;
    uint bgra, seed;
    float intensity, localTone, localStructure, skinStructure;
    uint temporal, historyValid, flowWidth, flowHeight;
    float historyStrength;
    uint debugView;
} p;

bool dlss5Finite(vec3 value) {
    return !any(isnan(value)) && !any(isinf(value));
}

bool dlss5TemporalEnabled() {
    return p.temporal != 0u && p.historyValid != 0u && p.historyStrength > 0.0;
}

bool dlss5FrameAccepted(uint accepted, uint candidates) {
    // Measure disagreement among reliable motion candidates, not the entire
    // image: textureless sky/background often has no trustworthy optical flow.
    // A tiny accidental match must not keep a cut's history alive.
    uint count = p.width * p.height;
    return accepted >= max(64u, (count + 99u) / 100u)
        && accepted >= candidates / 2u + candidates % 2u;
}

bool dlss5SampleAccepted(vec4 sampleValue) {
    return sampleValue.a > 0.0 && sampleValue.a <= 1.0 && dlss5Finite(sampleValue.rgb);
}

#endif
