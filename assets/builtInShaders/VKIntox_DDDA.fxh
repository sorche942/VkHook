// Original VKIntox DDDA material interface. No CompleteRT implementation is included.
#pragma once
#include "ReShade.fxh"
uniform float DDDA_FOV < ui_type = "slider"; ui_label = "Depth fallback vertical FOV"; ui_min = 20.0; ui_max = 120.0;
    ui_tooltip = "Only affects depth-reconstructed fallback normals. Match the camera; capture was approximately 55 degrees."; > = 55.0;

uniform bool VKIntox_DDDA_Available < source = "ddda_gbuffer_available"; > = false;
uniform uint VKIntox_DDDA_Frame < source = "framecount"; >;
texture VKIntox_DDDA_RawTex : DDDA_GBUFFER;
sampler VKIntox_DDDA_RawSampler
{
    Texture = VKIntox_DDDA_RawTex;
    MinFilter = POINT; MagFilter = POINT; MipFilter = POINT;
    AddressU = CLAMP; AddressV = CLAMP;
};

// Native RH view coordinates: +X right, +Y up, visible positions have -Z.
// RGB is SIGNED, not remapped to [0,1]. A is 1 only for valid native pixels.
texture VKIntox_DDDA_NormalTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
// R = estimated perceptual roughness; G/B/A = heuristic metallic/skin/hair confidence.
texture VKIntox_DDDA_MaterialTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
// R = native exponent; G = native light-group ID (not a semantic class);
// B = linearized depth; A = native validity.
texture VKIntox_DDDA_NativeTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
// Frame count split into exact 16-bit words, producer sentinel, source availability.
texture VKIntox_DDDA_StatusTex { Width = 1; Height = 1; Format = RGBA32F; };
sampler VKIntox_DDDA_StatusSampler { Texture = VKIntox_DDDA_StatusTex; };
sampler VKIntox_DDDA_NormalSampler
{
    Texture = VKIntox_DDDA_NormalTex;
    MinFilter = POINT; MagFilter = POINT; MipFilter = POINT;
    AddressU = CLAMP; AddressV = CLAMP;
};
sampler VKIntox_DDDA_MaterialSampler
{
    Texture = VKIntox_DDDA_MaterialTex;
    MinFilter = POINT; MagFilter = POINT; MipFilter = POINT;
    AddressU = CLAMP; AddressV = CLAMP;
};
sampler VKIntox_DDDA_NativeSampler
{
    Texture = VKIntox_DDDA_NativeTex;
    MinFilter = POINT; MagFilter = POINT; MipFilter = POINT;
    AddressU = CLAMP; AddressV = CLAMP;
};

float2 VKIntox_DDDA_FrameStamp()
{
    return float2(VKIntox_DDDA_Frame & 65535, (VKIntox_DDDA_Frame >> 16) & 65535);
}
bool VKIntox_DDDA_Decode(float2 uv, out float3 normal, out float exponent, out float group)
{
    normal = float3(0, 0, 1);
    exponent = 0;
    group = 0;
    // Never sample an engine fallback descriptor as if it were native data.
    if (!VKIntox_DDDA_Available) return false;
    float4 encoded = tex2Dlod(VKIntox_DDDA_RawSampler, float4(uv, 0, 0));
    float2 p = 4.0 * encoded.rg - 2.0;
    float f = dot(p, p);
    group = floor(encoded.a * 255.0 + 0.5);
    // Zero is the uncovered clear value, including foliage holes. Alpha is NOT opacity.
    // Allow half a UNORM quantization step at the sphere boundary.
    if (group < 1.0 || f > 4.045) { group = 0; return false; }
    normal = normalize(float3(p * sqrt(max(0.0, 1.0 - f * 0.25)), 1.0 - f * 0.5));
    exponent = exp(encoded.b * 8.0);
    return true;
}

bool VKIntox_DDDA_Read(float2 uv, out float4 normal, out float4 material, out float4 nativeData)
{
    normal = float4(0, 0, 1, 0);
    material = float4(0.6, 0, 0, 0);
    nativeData = 0;
    if (!VKIntox_DDDA_Available) return false;
    float4 status = tex2Dlod(VKIntox_DDDA_StatusSampler, float4(0.5, 0.5, 0, 0));
    if (status.z < 0.5 || status.w < 0.5 ||
        any(abs(status.xy - VKIntox_DDDA_FrameStamp()) > 0.25)) return false;
    nativeData = tex2Dlod(VKIntox_DDDA_NativeSampler, float4(uv, 0, 0));
    normal = tex2Dlod(VKIntox_DDDA_NormalSampler, float4(uv, 0, 0));
    if (normal.a < 0.5) return false;
    material = tex2Dlod(VKIntox_DDDA_MaterialSampler, float4(uv, 0, 0));
    return true;
}
float3 DDDA_Position(float2 uv)
{
    float depth = max(ReShade::GetLinearizedDepth(uv), 1e-6);
    float2 xy = (uv * 2.0 - 1.0) * float2(BUFFER_WIDTH * BUFFER_RCP_HEIGHT, -1.0);
    return float3(xy * tan(DDDA_FOV * 0.00872664626), -1.0) * depth;
}

float3 DDDA_DepthNormal(float2 uv)
{
    float2 px = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);
    float3 c = DDDA_Position(uv);
    float3 l = DDDA_Position(uv - float2(px.x, 0));
    float3 r = DDDA_Position(uv + float2(px.x, 0));
    float3 u = DDDA_Position(uv - float2(0, px.y));
    float3 d = DDDA_Position(uv + float2(0, px.y));
    float3 dx = abs(l.z - c.z) < abs(r.z - c.z) ? c - l : r - c;
    float3 dy = abs(u.z - c.z) < abs(d.z - c.z) ? c - u : d - c;
    float3 n = cross(dy, dx);
    return dot(n, n) > 1e-20 ? normalize(n) : float3(0, 0, 1);
}

// CompleteRT reconstructs positions right/down/forward, and its cross(down,left)
// normals point INTO the surface. Negate the RH->screen normal transform.
float3 VKIntox_DDDA_CompleteNormal(float3 nativeNormal)
{
    return nativeNormal * float3(-1, 1, 1);
}
