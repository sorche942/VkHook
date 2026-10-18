// Test-only raw binding probe. Left half: logical RG/specular-B; right half:
// availability (green) and light-group alpha * 255 (blue). Missing/resize
// mismatch must be magenta, distinct from the application's scene color.
// Native phases expect left RGB=(128,128,110)/255 and right RGB=(0,1,1).
uniform bool NativeAvailable < source = "ddda_gbuffer_available"; >;
void PostProcessVS(uint id : SV_VertexID, out float4 position : SV_Position,
                   out float2 texcoord : TEXCOORD)
{
    texcoord = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
texture2D NativeRaw : DDDA_GBUFFER;
sampler2D NativeSampler { Texture = NativeRaw; MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; };
float4 Probe(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    if (!NativeAvailable) return float4(1, 0, 1, 1);
    float4 raw = tex2D(NativeSampler, uv);
    return uv.x < 0.5 ? float4(raw.rgb, 1) : float4(0, 1, raw.a * 255.0, 1);
}
technique NativeMaterialProbe
{
    pass { VertexShader = PostProcessVS; PixelShader = Probe; }
}
