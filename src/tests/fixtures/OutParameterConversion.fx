#include "ReShade.fxh"

void WriteHit(out bool hit) { hit = true; }
void Increment(inout int value) { value += 1; }
void WritePair(out bool2 flags) { flags = bool2(true, false); }

float4 OutParameterConversion(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float hit;
    WriteHit(hit);
    float count = 2.75;
    Increment(count);
    float4 channels = 0;
    WritePair(channels.yx);
    uint values[2];
    WriteHit(values[int(uv.x > 0.5)]);
    return float4(hit, count / 3.0, channels.y, 1.0);
}

technique OutParameterConversion
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = OutParameterConversion;
    }
}
