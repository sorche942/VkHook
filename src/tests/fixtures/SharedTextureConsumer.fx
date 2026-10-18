// Must read the producer's allocation, not a separately cleared black texture.
texture SharedTexture
{
    Width = BUFFER_WIDTH;
    Height = BUFFER_HEIGHT;
    Format = RGBA8;
    MipLevels = 1;
};
sampler SharedTextureSampler
{
    Texture = SharedTexture;
};

void SharedTextureVS(uint id : SV_VertexID, out float4 position : SV_Position,
                     out float2 texcoord : TEXCOORD)
{
    texcoord = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
    position = float4(texcoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 ConsumeSharedTexture(float4 position : SV_Position,
                            float2 texcoord : TEXCOORD) : SV_Target
{
    return tex2D(SharedTextureSampler, texcoord);
}

technique SharedTextureConsumer
{
    pass
    {
        VertexShader = SharedTextureVS;
        PixelShader = ConsumeSharedTexture;
    }
}
