#include "ReShade.fxh"

float4 ScalarTextureFetch(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    int x = int(uv.x * BUFFER_WIDTH);
    return 0.5 * (tex1Dfetch(ReShade::BackBuffer, x) + tex2Dfetch(ReShade::BackBuffer, x));
}

technique ScalarTextureFetch
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = ScalarTextureFetch;
    }
}
