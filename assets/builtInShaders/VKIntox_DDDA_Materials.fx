// Original VKIntox DDDA material estimates. Native data and heuristics stay separate.
#include "VKIntox_DDDA.fxh"

uniform int DDDA_Help < ui_type = "radio"; ui_label = "Native data versus estimates";
    ui_items = "Read the tooltip\0";
    ui_tooltip = "Order: DH motion, DDDA Materials, CompleteRT, then optional DDDA Diagnostics. Native: view normal, specular exponent, light-group ID. Estimated: roughness, metallic, skin, hair. Scene color is lit, NOT albedo; confidence is heuristic, NOT a probability or semantic classification. Light-group IDs are NOT material labels. Red diagnostics means native input unavailable; magenta means uncovered/invalid pixels. No preset is enabled automatically.";
> = 0;
uniform float DDDA_ExponentScale < ui_type = "slider"; ui_label = "Roughness estimate: exponent calibration"; ui_min = 0.1; ui_max = 4.0;
    ui_tooltip = "Approximation r=(2/(calibrated exponent+2))^(1/4), treating the legacy exponent as a Blinn-Phong lobe. Not native PBR roughness."; > = 1.0;
uniform float DDDA_RoughnessGain < ui_type = "slider"; ui_label = "Roughness estimate gain"; ui_min = 0.1; ui_max = 3.0; > = 1.0;
uniform float DDDA_RoughnessBias < ui_type = "slider"; ui_label = "Roughness estimate bias"; ui_min = -0.5; ui_max = 0.5; > = 0.0;
uniform float DDDA_RoughnessPower < ui_type = "slider"; ui_label = "Roughness estimate curve"; ui_min = 0.25; ui_max = 4.0; > = 1.0;
uniform float DDDA_FallbackRoughness < ui_type = "slider"; ui_label = "Diagnostic fallback roughness (not native)"; ui_min = 0.02; ui_max = 1.0;
    ui_tooltip = "Written for uncovered pixels only. CompleteRT preserves its own original roughness on those pixels."; > = 0.6;

uniform float DDDA_MetalStrength < ui_type = "slider"; ui_label = "HEURISTIC metallic confidence ceiling"; ui_min = 0.0; ui_max = 1.0;
    ui_tooltip = "Sharp exponent plus bright/desaturated lit color, excluding skin/hair candidates. Plastics, water, stone and highlights can fool this. Zero disables."; > = 0.35;
uniform float DDDA_MetalExponent < ui_type = "slider"; ui_label = "HEURISTIC metallic minimum exponent"; ui_min = 8.0; ui_max = 1000.0; > = 96.0;
uniform int DDDA_MetalGroup < ui_type = "slider"; ui_label = "Metallic light-group gate (-1: any)"; ui_min = -1; ui_max = 255;
    ui_tooltip = "Optional user-selected filter, NOT a semantic mapping. Inspect group IDs first."; > = -1;
uniform float DDDA_SkinStrength < ui_type = "slider"; ui_label = "HEURISTIC skin confidence ceiling"; ui_min = 0.0; ui_max = 1.0;
    ui_tooltip = "Chroma similarity, moderate exponent and low local texture contrast. Wood, leather, warm lighting and makeup can fool this. Zero disables."; > = 0.5;
uniform float3 DDDA_SkinTint < ui_type = "color"; ui_label = "HEURISTIC skin chroma reference (lit RGB)";
    ui_tooltip = "A user-tunable color cue, not a biological or identity classifier. Lighting changes require retuning."; > = float3(0.65, 0.40, 0.30);
uniform float DDDA_SkinTolerance < ui_type = "slider"; ui_label = "HEURISTIC skin chroma tolerance"; ui_min = 0.02; ui_max = 0.4; > = 0.13;
uniform int DDDA_SkinGroup < ui_type = "slider"; ui_label = "Skin light-group gate (-1: any)"; ui_min = -1; ui_max = 255; > = -1;
uniform float DDDA_HairStrength < ui_type = "slider"; ui_label = "HEURISTIC hair confidence ceiling"; ui_min = 0.0; ui_max = 1.0;
    ui_tooltip = "Dark lit color with directional local texture and a specular lobe. Misses bright/smooth hair; foliage and fabric can fool it. Not native anisotropy. Zero disables."; > = 0.35;
uniform float DDDA_HairDarkness < ui_type = "slider"; ui_label = "HEURISTIC hair maximum luminance"; ui_min = 0.05; ui_max = 0.8; > = 0.3;
uniform float DDDA_HairTexture < ui_type = "slider"; ui_label = "HEURISTIC hair texture sensitivity"; ui_min = 1.0; ui_max = 40.0; > = 12.0;
uniform int DDDA_HairGroup < ui_type = "slider"; ui_label = "Hair light-group gate (-1: any)"; ui_min = -1; ui_max = 255; > = -1;
uniform float DDDA_SkinRoughness < ui_type = "slider"; ui_label = "HEURISTIC skin roughness floor"; ui_min = 0.0; ui_max = 1.0; > = 0.35;
uniform float DDDA_HairRoughness < ui_type = "slider"; ui_label = "HEURISTIC hair roughness floor"; ui_min = 0.0; ui_max = 1.0; > = 0.3;

float DDDA_Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float DDDA_Gate(float group, int selected) { return selected < 0 || abs(group - selected) < 0.5 ? 1.0 : 0.0; }


void DDDA_Produce(float4 pos : SV_Position, float2 uv : TEXCOORD,
    out float4 normals : SV_Target0, out float4 materials : SV_Target1, out float4 nativeData : SV_Target2)
{
    float3 n;
    float exponent, group;
    bool valid = VKIntox_DDDA_Decode(uv, n, exponent, group);
    float depth = ReShade::GetLinearizedDepth(uv);
    normals = float4(valid ? n : DDDA_DepthNormal(uv), valid ? 1.0 : 0.0);
    nativeData = float4(exponent, group, depth, valid ? 1.0 : 0.0);
    materials = float4(DDDA_FallbackRoughness, 0, 0, 0);
    if (!valid) return;

    float roughness = pow(2.0 / (exponent * DDDA_ExponentScale + 2.0), 0.25);
    roughness = clamp(pow(roughness, DDDA_RoughnessPower) * DDDA_RoughnessGain + DDDA_RoughnessBias, 0.02, 1.0);
    // These are deliberately weak, adjustable lit-color cues, never recovered albedo.
    float3 color = saturate(tex2D(ReShade::BackBuffer, uv).rgb);
    float luma = DDDA_Luma(color);
    float3 chroma = color / max(dot(color, float3(1, 1, 1)), 0.03);
    float3 reference = DDDA_SkinTint / max(dot(DDDA_SkinTint, float3(1, 1, 1)), 0.03);
    float2 px = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT) * 2.0;
    float dx = DDDA_Luma(tex2D(ReShade::BackBuffer, uv + float2(px.x, 0)).rgb) -
               DDDA_Luma(tex2D(ReShade::BackBuffer, uv - float2(px.x, 0)).rgb);
    float dy = DDDA_Luma(tex2D(ReShade::BackBuffer, uv + float2(0, px.y)).rgb) -
               DDDA_Luma(tex2D(ReShade::BackBuffer, uv - float2(0, px.y)).rgb);
    float contrast = length(float2(dx, dy));
    float exposureTrust = smoothstep(0.015, 0.08, luma) * (1.0 - smoothstep(0.85, 0.98, luma));
    float skin = (1.0 - smoothstep(DDDA_SkinTolerance * 0.5, DDDA_SkinTolerance, distance(chroma, reference)));
    skin *= smoothstep(2.0, 12.0, exponent) * (1.0 - smoothstep(150.0, 500.0, exponent));
    skin *= 1.0 - smoothstep(0.04, 0.25, contrast);
    skin *= exposureTrust * DDDA_SkinStrength * DDDA_Gate(group, DDDA_SkinGroup);

    float directional = abs(abs(dx) - abs(dy)) / (abs(dx) + abs(dy) + 0.005);
    float hair = (1.0 - smoothstep(DDDA_HairDarkness * 0.5, DDDA_HairDarkness, luma));
    hair *= saturate(contrast * DDDA_HairTexture) * directional * smoothstep(8.0, 48.0, exponent);
    hair *= exposureTrust * (1.0 - skin) * DDDA_HairStrength * DDDA_Gate(group, DDDA_HairGroup);

    float saturation = (max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b))) /
                       max(max(color.r, max(color.g, color.b)), 0.03);
    float metal = smoothstep(DDDA_MetalExponent, DDDA_MetalExponent * 3.0, exponent);
    metal *= smoothstep(0.12, 0.45, luma) * (1.0 - 0.7 * saturation);
    metal *= exposureTrust * (1.0 - skin) * (1.0 - hair) * DDDA_MetalStrength * DDDA_Gate(group, DDDA_MetalGroup);
    roughness = lerp(roughness, max(roughness, DDDA_SkinRoughness), skin);
    roughness = lerp(roughness, max(roughness, DDDA_HairRoughness), hair);
    materials = float4(roughness, metal, skin, hair);
}

float4 DDDA_Status(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return float4(VKIntox_DDDA_FrameStamp(), 1, VKIntox_DDDA_Available ? 1.0 : 0.0);
}


technique VKIntox_DDDA_Materials < ui_label = "DDDA Materials: native + estimates";
    ui_tooltip = "Run before CompleteRT. Native normals/exponent; roughness and metallic/skin/hair are estimates. Does not modify scene color."; >
{
    pass Produce
    {
        VertexShader = PostProcessVS; PixelShader = DDDA_Produce;
        RenderTarget0 = VKIntox_DDDA_NormalTex;
        RenderTarget1 = VKIntox_DDDA_MaterialTex;
        RenderTarget2 = VKIntox_DDDA_NativeTex;
    }
    pass Status
    {
        VertexShader = PostProcessVS; PixelShader = DDDA_Status;
        RenderTarget = VKIntox_DDDA_StatusTex;
    }
}

