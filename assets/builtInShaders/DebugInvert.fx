// Color-only diagnostic: an obvious negative image without depth-buffer access.
#include "ReShade.fxh"

float3 PS_DebugInvert(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    const float3 color = tex2D(ReShade::BackBuffer, texcoord).rgb;
    return 1.0 - saturate(color);
}

technique DebugInvert <
    ui_label = "Debug: Invert";
    ui_tooltip = "Negative colors. Tests color processing; no depth buffer required.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_DebugInvert;
    }
}
