// Color-only diagnostic: a visible sepia tint without depth-buffer access.
#include "ReShade.fxh"

float3 PS_DebugSepia(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    const float3 color = tex2D(ReShade::BackBuffer, texcoord).rgb;
    return saturate(float3(
        dot(color, float3(0.393, 0.769, 0.189)),
        dot(color, float3(0.349, 0.686, 0.168)),
        dot(color, float3(0.272, 0.534, 0.131))));
}

technique DebugSepia <
    ui_label = "Debug: Sepia";
    ui_tooltip = "Warm sepia tint. Tests color processing; no depth buffer required.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_DebugSepia;
    }
}
