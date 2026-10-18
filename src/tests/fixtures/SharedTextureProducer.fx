// Standalone fixture: write only the named offscreen texture, never the screen.
texture SharedTexture
{
    Width = BUFFER_WIDTH;
    Height = BUFFER_HEIGHT;
    Format = RGBA8;
    MipLevels = 1;
};

// Exercise the same sampled-backbuffer path as DH's motion producer.
texture SceneTexture : COLOR;
sampler SceneSampler { Texture = SceneTexture; };

void SharedTextureVS(uint id : SV_VertexID, out float4 position : SV_Position,
                     out float2 texcoord : TEXCOORD)
{
    texcoord = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 ProduceSharedTexture(float4 position : SV_Position,
                            float2 texcoord : TEXCOORD) : SV_Target
{
    // The UNORM backbuffer is nonnegative, so this writes known green even
    // when the application's alpha channel is zero.
    return float4(0.0, saturate(tex2D(SceneSampler, texcoord).r + 1.0), 0.0, 1.0);
}

technique SharedTextureProducer
{
    pass
    {
        VertexShader = SharedTextureVS;
        PixelShader = ProduceSharedTexture;
        RenderTarget = SharedTexture;
    }
}
