// Multi-technique fixture: three techniques with 1, 2 and 3 passes. A user
// macro (MULTI_EXTRA) gates one uniform, so the file reports a user macro and
// its uniform list follows that macro. Used by compiler_test and the registry
// tests.
#ifndef MULTI_EXTRA
    #define MULTI_EXTRA 0
#endif

#if MULTI_EXTRA
uniform float ExtraGain < ui_min = 0.0; ui_max = 1.0; > = 0.25;
#endif

uniform float BaseGain < ui_min = 0.0; ui_max = 1.0; > = 0.5;

texture2D texTemp { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA8; };
sampler2D samplerTemp { Texture = texTemp; };

void FullscreenVS(uint id : SV_VertexID, out float4 position : SV_Position)
{
    position = float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0);
}

float4 SolidPS(float4 position : SV_Position) : SV_Target
{
#if MULTI_EXTRA
    return float4(BaseGain, ExtraGain, 0.0, 1.0);
#else
    return float4(BaseGain, 0.0, 0.0, 1.0);
#endif
}

float4 ReadPS(float4 position : SV_Position) : SV_Target
{
    return tex2Dfetch(samplerTemp, int2(position.xy)) * BaseGain;
}

technique SinglePass
{
    pass
    {
        VertexShader = FullscreenVS;
        PixelShader = SolidPS;
    }
}

technique DoublePass
{
    pass
    {
        VertexShader = FullscreenVS;
        PixelShader = SolidPS;
        RenderTarget = texTemp;
    }
    pass
    {
        VertexShader = FullscreenVS;
        PixelShader = ReadPS;
    }
}

technique TriplePass
{
    pass
    {
        VertexShader = FullscreenVS;
        PixelShader = SolidPS;
        RenderTarget = texTemp;
    }
    pass
    {
        VertexShader = FullscreenVS;
        PixelShader = ReadPS;
        RenderTarget = texTemp;
    }
    pass
    {
        VertexShader = FullscreenVS;
        PixelShader = ReadPS;
    }
}
