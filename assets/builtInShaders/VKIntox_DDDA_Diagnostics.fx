// Original VKIntox diagnostic effect. Enable LAST, after DDDA Materials and lighting.
#include "VKIntox_DDDA.fxh"

uniform int DDDA_Debug < ui_type = "combo"; ui_label = "DDDA diagnostic view (effect must run LAST)";
    ui_items = "Native validity / source status\0Native view normals + depth fallback\0Native log exponent (invalid: magenta)\0Native light-group palette (NOT classes)\0ESTIMATED perceptual roughness\0HEURISTIC metallic confidence\0HEURISTIC skin confidence\0HEURISTIC hair confidence\0HEURISTIC metallic / skin / hair RGB\0Native versus depth normal agreement\0Native light-group numeric / 255\0Linearized depth\0";
> = 0;
float4 DDDA_Diagnostics(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 status = tex2D(VKIntox_DDDA_StatusSampler, float2(0.5, 0.5));
    if (!VKIntox_DDDA_Available) return float4(0.8, 0.02, 0.02, 1);
    if (status.z < 0.5 || status.w < 0.5 || any(abs(status.xy - VKIntox_DDDA_FrameStamp()) > 0.25)) return float4(1, 0.5, 0, 1);
    float4 n = tex2D(VKIntox_DDDA_NormalSampler, uv);
    float4 m = tex2D(VKIntox_DDDA_MaterialSampler, uv);
    float4 data = tex2D(VKIntox_DDDA_NativeSampler, uv);
    if (DDDA_Debug == 0) return float4(n.a > 0.5 ? float3(0.1, 0.8, 0.2) : float3(1, 0, 1), 1);
    if (DDDA_Debug == 1) return float4(n.xyz * 0.5 + 0.5, 1);
    if (DDDA_Debug == 4) return float4(m.rrr, 1);
    if (DDDA_Debug == 11) return float4(data.bbb, 1);
    if (n.a < 0.5) return float4(1, 0, 1, 1);
    if (DDDA_Debug == 2) return float4((log(max(data.r, 1.0)) / 8.0).xxx, 1);
    if (DDDA_Debug == 3) return float4(frac(data.g * float3(0.618034, 0.381966, 0.754878)), 1);
    if (DDDA_Debug == 5) return float4(m.ggg, 1);
    if (DDDA_Debug == 6) return float4(m.bbb, 1);
    if (DDDA_Debug == 7) return float4(m.aaa, 1);
    if (DDDA_Debug == 8) return float4(m.gba, 1);
    if (DDDA_Debug == 9) return float4((dot(n.xyz, DDDA_DepthNormal(uv)) * 0.5 + 0.5).xxx, 1);
    return float4((data.g / 255.0).xxx, 1);
}
technique VKIntox_DDDA_Diagnostics < ui_label = "DDDA Diagnostics (optional, run LAST)";
    ui_tooltip = "Red: source unavailable. Orange: producer missing/out of order. Magenta: invalid/uncovered native pixel. Green: native valid. Disable this technique for normal gameplay."; >
{
    pass { VertexShader = PostProcessVS; PixelShader = DDDA_Diagnostics; }
}
