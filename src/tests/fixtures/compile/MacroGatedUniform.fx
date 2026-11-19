// Parameter discovery fixture: one uniform exists only when ENABLE_EXTRA is
// non-zero, and two take their default from the buffer extent macros.
#ifndef ENABLE_EXTRA
    #define ENABLE_EXTRA 0
#endif

#if ENABLE_EXTRA
uniform float ExtraStrength < ui_min = 0.0; ui_max = 1.0; > = 0.25;
#endif

uniform float AlwaysStrength < ui_min = 0.0; ui_max = 1.0; > = 0.5;
uniform float WidthProbe < ui_min = 0.0; ui_max = 16384.0; > = BUFFER_WIDTH;
uniform float HeightProbe < ui_min = 0.0; ui_max = 16384.0; > = BUFFER_HEIGHT;

void ProbeVS(uint id : SV_VertexID, out float4 position : SV_Position)
{
    position = float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0);
}

float4 ProbePS(float4 position : SV_Position) : SV_Target
{
    return float4(AlwaysStrength, WidthProbe * 0.0, HeightProbe * 0.0, 1.0);
}

technique MacroGatedUniform
{
    pass
    {
        VertexShader = ProbeVS;
        PixelShader = ProbePS;
    }
}
