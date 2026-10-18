// Color-only diagnostic: remove color without depth-buffer access.
#include "ReShade.fxh"

float3 PS_DebugGrayscale(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
    const float3 color = tex2D(ReShade::BackBuffer, texcoord).rgb;
    const float gray = dot(color, float3(0.2126, 0.7152, 0.0722));
    return float3(gray, gray, gray);
}

technique DebugGrayscale <
    ui_label = "Debug: Grayscale";
    ui_tooltip = "Black and white image. Tests color processing; no depth buffer required.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = PS_DebugGrayscale;
    }
}
