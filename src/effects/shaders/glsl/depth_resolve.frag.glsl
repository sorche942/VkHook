#version 450

// Copies the application's depth into the layer's R32_SFLOAT capture target.
// Built twice from this file: plain (sampler2D) and with -DMSAA (sampler2DMS).
// Resolving a multisampled depth image by shader (not vkCmdResolveImage, which
// is colour-only) leaves the application's image untouched.

// RESOLVE_MODE: 0 sample zero, 1 average, 2 minimum, 3 maximum. Both constants
// only matter for the multisampled build.
layout(constant_id = 0) const int SAMPLES = 1;
layout(constant_id = 1) const int RESOLVE_MODE = 0;

#ifdef MSAA
layout(set = 0, binding = 0) uniform sampler2DMS depthTexture;
#else
layout(set = 0, binding = 0) uniform sampler2D depthTexture;
#endif

// three int32: a GLSL bool is 4 bytes, the C++ side pushes plain int32
layout(push_constant) uniform DepthParams {
    int invertDepth;  // depth = 1 - depth
    int flipDepth;    // mirror vertically
    int flopDepth;    // mirror horizontally
} params;

layout(location = 0) in vec2 textureCoord;
layout(location = 0) out float resolvedDepth;

void main()
{
    // branchless mirror: a flipped/flopped renderer reads upright depth without
    // rebuilding any pipeline
    vec2 uv = mix(textureCoord, vec2(1.0) - textureCoord,
                  vec2(float(params.flopDepth), float(params.flipDepth)));

#ifdef MSAA
    // the target and the source need not share a size: map by normalised position
    ivec2 size = textureSize(depthTexture);
    ivec2 texel = clamp(ivec2(uv * vec2(size)), ivec2(0), size - ivec2(1));
    float depth = texelFetch(depthTexture, texel, 0).r;
    if (RESOLVE_MODE != 0)
    {
        float sum = depth;
        for (int s = 1; s < SAMPLES; ++s)
        {
            float d = texelFetch(depthTexture, texel, s).r;
            sum += d;
            depth = RESOLVE_MODE == 2 ? min(depth, d) : max(depth, d);
        }
        if (RESOLVE_MODE == 1)
            depth = sum / float(SAMPLES);
    }
#else
    float depth = textureLod(depthTexture, uv, 0.0).r;
#endif

    resolvedDepth = params.invertDepth != 0 ? 1.0 - depth : depth;
}
