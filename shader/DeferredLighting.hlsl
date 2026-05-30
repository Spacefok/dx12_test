struct DirectionalLight
{
    float4 DirectionIntensity; // xyz = direction, w = intensity
    float4 Color;
};

struct PointLight
{
    float4 PositionRange; // xyz = position, w = range
    float4 ColorIntensity; // xyz = color, w = intensity
};

struct SpotLight
{
    float4 PositionRange; // xyz = position, w = range
    float4 DirectionCosInner; // xyz = direction, w = cos(inner)
    float4 ColorIntensity; // xyz = color, w = intensity
    float4 Params; // x = cos(outer)
};

cbuffer DeferredPassCB : register(b0)
{
    float3 gEyePosW;
    float gAmbientIntensity;
    float4 gAmbientColor;
    float4x4 gInvViewProj;
    float4x4 gView;
    float4x4 gShadowViewProj[4];
    float4 gShadowCascadeSplits;
    float4 gShadowParams; // x=texel size, y=enabled, z=receiver depth bias, w=cascade count
    uint gDirectionalLightCount;
    uint gPointLightCount;
    uint gSpotLightCount;
    uint gDebugViewEnabled;
};

Texture2D gAlbedoTex : register(t0);
Texture2D gNormalTex : register(t1);
Texture2D gPositionSpecTex : register(t2);
Texture2D<float> gDepthTex : register(t3);
StructuredBuffer<DirectionalLight> gDirectionalLights : register(t4);
StructuredBuffer<PointLight> gPointLights : register(t5);
StructuredBuffer<SpotLight> gSpotLights : register(t6);
Texture2DArray<float> gShadowMap : register(t7);
SamplerState gSamPointClamp : register(s0);
SamplerComparisonState gSamShadowCmp : register(s1);

struct FullscreenOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD0;
};

FullscreenOut VSFullscreen(uint vertexID : SV_VertexID)
{
    const float2 positions[3] =
    {
        float2(-1.0f, -1.0f),
        float2(-1.0f, 3.0f),
        float2(3.0f, -1.0f)
    };

    const float2 texcoords[3] =
    {
        float2(0.0f, 1.0f),
        float2(0.0f, -1.0f),
        float2(2.0f, 1.0f)
    };

    FullscreenOut vout;
    vout.PosH = float4(positions[vertexID], 0.0f, 1.0f);
    vout.TexC = texcoords[vertexID];
    return vout;
}

float3 ShadeBlinnPhong(float3 albedo, float3 normalW, float3 viewDir, float3 lightDir, float3 radiance, float specPower)
{
    float3 result = 0.0f.xxx;
    float ndotl = saturate(dot(normalW, lightDir));
    if (ndotl > 0.0f)
    {
        float3 halfVector = viewDir + lightDir;
        float halfLen2 = dot(halfVector, halfVector);
        if (halfLen2 > 1e-6f)
        {
            halfVector *= rsqrt(halfLen2);
        }
        else
        {
            halfVector = lightDir;
        }

        float spec = pow(saturate(dot(normalW, halfVector)), specPower);
        result = radiance * (albedo * ndotl + spec.xxx);
    }
    return result;
}

float3 EvaluateDirectional(DirectionalLight light, float3 albedo, float3 normalW, float3 viewDir, float specPower)
{
    float3 result = 0.0f.xxx;

    float3 lightDirVec = -light.DirectionIntensity.xyz;
    float len2 = dot(lightDirVec, lightDirVec);
    if (len2 > 1e-6f)
    {
        float3 lightDir = lightDirVec * rsqrt(len2);
        float intensity = max(light.DirectionIntensity.w, 0.0f);
        float3 radiance = light.Color.rgb * intensity;
        result = ShadeBlinnPhong(albedo, normalW, viewDir, lightDir, radiance, specPower);
    }
    return result;
}

float3 EvaluatePoint(PointLight light, float3 posW, float3 albedo, float3 normalW, float3 viewDir, float specPower)
{
    float3 result = 0.0f.xxx;

    float3 toLight = light.PositionRange.xyz - posW;
    float dist = length(toLight);
    float range = max(light.PositionRange.w, 1e-3f);
    if (dist < range)
    {
        float3 lightDir = toLight / max(dist, 1e-4f);
        float attenuation = saturate(1.0f - dist / range);
        attenuation *= attenuation;

        float3 radiance = light.ColorIntensity.rgb * max(light.ColorIntensity.w, 0.0f) * attenuation;
        result = ShadeBlinnPhong(albedo, normalW, viewDir, lightDir, radiance, specPower);
    }
    return result;
}

float3 EvaluateSpot(SpotLight light, float3 posW, float3 albedo, float3 normalW, float3 viewDir, float specPower)
{
    float3 result = 0.0f.xxx;

    float3 toLight = light.PositionRange.xyz - posW;
    float dist = length(toLight);
    float range = max(light.PositionRange.w, 1e-3f);
    if (dist < range)
    {
        float3 lightDir = toLight / max(dist, 1e-4f);

        float3 spotDir = light.DirectionCosInner.xyz;
        float spotDirLen2 = dot(spotDir, spotDir);
        if (spotDirLen2 > 1e-6f)
        {
            spotDir *= rsqrt(spotDirLen2);
        }
        else
        {
            spotDir = float3(0.0f, -1.0f, 0.0f);
        }

        float cosInner = light.DirectionCosInner.w;
        float cosOuter = light.Params.x;
        float cosTheta = dot(-lightDir, spotDir);
        float spotFactor = saturate((cosTheta - cosOuter) / max(cosInner - cosOuter, 1e-4f));

        float attenuation = saturate(1.0f - dist / range);
        attenuation = attenuation * attenuation * spotFactor;

        float3 radiance = light.ColorIntensity.rgb * max(light.ColorIntensity.w, 0.0f) * attenuation;
        result = ShadeBlinnPhong(albedo, normalW, viewDir, lightDir, radiance, specPower);
    }

    return result;
}

float3 NormalizeNormalForLighting(float3 normalW)
{
    float3 normalized = float3(0.0f, 1.0f, 0.0f);
    float len2 = dot(normalW, normalW);
    if (len2 > 1e-6f)
    {
        normalized = normalW * rsqrt(len2);
    }
    return normalized;
}

uint SelectShadowCascade(float viewDepth)
{
    uint cascadeIndex = 0u;
    if (viewDepth > gShadowCascadeSplits.x) cascadeIndex = 1u;
    if (viewDepth > gShadowCascadeSplits.y) cascadeIndex = 2u;
    if (viewDepth > gShadowCascadeSplits.z) cascadeIndex = 3u;
    return cascadeIndex;
}

float SampleShadowPcf(float3 uvw, float receiverDepth)
{
    float visibility = 0.0f;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            visibility += gShadowMap.SampleCmpLevelZero(
                gSamShadowCmp,
                uvw,
                receiverDepth,
                int2(x, y));
        }
    }

    return visibility / 9.0f;
}

float ComputeDirectionalShadow(float3 posW)
{
    float visibility = 1.0f;

    if (gShadowParams.y >= 0.5f)
    {
        float viewDepth = mul(float4(posW, 1.0f), gView).z;
        if (viewDepth > 0.0f && viewDepth <= gShadowCascadeSplits.w)
        {
            uint cascadeIndex = SelectShadowCascade(viewDepth);
            float4 worldPos = float4(posW, 1.0f);
            float4 shadowPos = mul(worldPos, gShadowViewProj[0]);
            if (cascadeIndex == 1u)
            {
                shadowPos = mul(worldPos, gShadowViewProj[1]);
            }
            else if (cascadeIndex == 2u)
            {
                shadowPos = mul(worldPos, gShadowViewProj[2]);
            }
            else if (cascadeIndex == 3u)
            {
                shadowPos = mul(worldPos, gShadowViewProj[3]);
            }

            if (abs(shadowPos.w) > 1e-6f)
            {
                shadowPos.xyz /= shadowPos.w;
                float2 shadowUv = float2(shadowPos.x * 0.5f + 0.5f, 0.5f - shadowPos.y * 0.5f);

                if (shadowUv.x >= 0.0f && shadowUv.x <= 1.0f &&
                    shadowUv.y >= 0.0f && shadowUv.y <= 1.0f &&
                    shadowPos.z >= 0.0f && shadowPos.z <= 1.0f)
                {
                    float receiverDepth = saturate(shadowPos.z - gShadowParams.z);
                    visibility = SampleShadowPcf(float3(shadowUv, (float)cascadeIndex), receiverDepth);
                }
            }
        }
    }

    return visibility;
}

float3 EvaluateSceneLighting(float3 albedo, float3 normalW, float3 posW, float specPower)
{
    float3 viewVec = gEyePosW - posW;
    float viewLen2 = dot(viewVec, viewVec);
    float3 viewDir = (viewLen2 > 1e-6f) ? (viewVec * rsqrt(viewLen2)) : float3(0.0f, 0.0f, 1.0f);

    float3 color = albedo * gAmbientColor.rgb * gAmbientIntensity;

    [loop]
    for (uint dirIndex = 0; dirIndex < gDirectionalLightCount; ++dirIndex)
    {
        float shadowVisibility = (dirIndex == 0u) ? ComputeDirectionalShadow(posW) : 1.0f;
        color += shadowVisibility * EvaluateDirectional(gDirectionalLights[dirIndex], albedo, normalW, viewDir, specPower);
    }

    [loop]
    for (uint pointIndex = 0; pointIndex < gPointLightCount; ++pointIndex)
    {
        color += EvaluatePoint(gPointLights[pointIndex], posW, albedo, normalW, viewDir, specPower);
    }

    [loop]
    for (uint spotIndex = 0; spotIndex < gSpotLightCount; ++spotIndex)
    {
        color += EvaluateSpot(gSpotLights[spotIndex], posW, albedo, normalW, viewDir, specPower);
    }

    return color;
}

float3 ReconstructWorldPos(float2 texC, float depth)
{
    float2 ndc;
    ndc.x = texC.x * 2.0f - 1.0f;
    ndc.y = 1.0f - texC.y * 2.0f;

    float4 clipPos = float4(ndc, depth, 1.0f);
    float4 worldPos = mul(clipPos, gInvViewProj);
    float invW = (abs(worldPos.w) > 1e-6f) ? rcp(worldPos.w) : 0.0f;
    return worldPos.xyz * invW;
}

float3 VisualizeNormal(float3 normalW)
{
    float3 n = float3(0.0f, 0.0f, 1.0f);
    float len2 = dot(normalW, normalW);
    if (len2 > 1e-6f) {
        n = normalW * rsqrt(len2);
    }
    return n * 0.5f + 0.5f;
}

float3 VisualizePosition(float3 posW)
{
    float3 absPos = abs(posW);
    return absPos / (1.0f + absPos);
}

float3 HeatMap01(float t)
{
    float3 result = float3(0.05f, 0.08f, 0.25f);
    t = saturate(t);

    const float3 c0 = float3(0.05f, 0.08f, 0.25f);
    const float3 c1 = float3(0.08f, 0.55f, 0.95f);
    const float3 c2 = float3(0.95f, 0.83f, 0.18f);
    const float3 c3 = float3(0.92f, 0.20f, 0.12f);

    if (t < 0.33f)
    {
        result = lerp(c0, c1, t / 0.33f);
    }
    else if (t < 0.66f)
    {
        result = lerp(c1, c2, (t - 0.33f) / 0.33f);
    }
    else
    {
        result = lerp(c2, c3, (t - 0.66f) / 0.34f);
    }

    return result;
}

float3 VisualizeDisplacement(float displacement)
{
    displacement = saturate(displacement);
    return lerp(float3(0.04f, 0.04f, 0.04f), float3(1.00f, 0.74f, 0.18f), displacement);
}

float3 VisualizeTessFactor(float tessFactor)
{
    float3 color = float3(0.10f, 0.02f, 0.02f);
    tessFactor = saturate(tessFactor);
    if (tessFactor > 1e-4f)
    {
        color = HeatMap01(tessFactor);
    }
    return color;
}

static const uint GLYPH_A = 0u;
static const uint GLYPH_B = 1u;
static const uint GLYPH_C = 2u;
static const uint GLYPH_D = 3u;
static const uint GLYPH_E = 4u;
static const uint GLYPH_H = 5u;
static const uint GLYPH_I = 6u;
static const uint GLYPH_L = 7u;
static const uint GLYPH_M = 8u;
static const uint GLYPH_N = 9u;
static const uint GLYPH_O = 10u;
static const uint GLYPH_P = 11u;
static const uint GLYPH_R = 12u;
static const uint GLYPH_S = 13u;
static const uint GLYPH_T = 14u;

static const uint LABEL_ALBEDO = 0u;
static const uint LABEL_NORMAL = 1u;
static const uint LABEL_SPEC = 2u;
static const uint LABEL_DEPTH = 3u;
static const uint LABEL_POSITION = 4u;
static const uint LABEL_DISP = 5u;
static const uint LABEL_TESS = 6u;
static const uint LABEL_LIT = 7u;
static const uint LABEL_CSM = 8u;
static const uint LABEL_MAP = 9u;

uint GlyphRowBits(uint glyph, uint row)
{
    uint bits = 0u;
    switch (glyph)
    {
    case GLYPH_A:
        switch (row) { case 0u: bits = 14u; break; case 1u: bits = 17u; break; case 2u: bits = 17u; break; case 3u: bits = 31u; break; case 4u: bits = 17u; break; case 5u: bits = 17u; break; case 6u: bits = 17u; break; }
        break;
    case GLYPH_B:
        switch (row) { case 0u: bits = 30u; break; case 1u: bits = 17u; break; case 2u: bits = 17u; break; case 3u: bits = 30u; break; case 4u: bits = 17u; break; case 5u: bits = 17u; break; case 6u: bits = 30u; break; }
        break;
    case GLYPH_C:
        switch (row) { case 0u: bits = 15u; break; case 1u: bits = 16u; break; case 2u: bits = 16u; break; case 3u: bits = 16u; break; case 4u: bits = 16u; break; case 5u: bits = 16u; break; case 6u: bits = 15u; break; }
        break;
    case GLYPH_D:
        switch (row) { case 0u: bits = 30u; break; case 1u: bits = 17u; break; case 2u: bits = 17u; break; case 3u: bits = 17u; break; case 4u: bits = 17u; break; case 5u: bits = 17u; break; case 6u: bits = 30u; break; }
        break;
    case GLYPH_E:
        switch (row) { case 0u: bits = 31u; break; case 1u: bits = 16u; break; case 2u: bits = 16u; break; case 3u: bits = 30u; break; case 4u: bits = 16u; break; case 5u: bits = 16u; break; case 6u: bits = 31u; break; }
        break;
    case GLYPH_H:
        switch (row) { case 0u: bits = 17u; break; case 1u: bits = 17u; break; case 2u: bits = 17u; break; case 3u: bits = 31u; break; case 4u: bits = 17u; break; case 5u: bits = 17u; break; case 6u: bits = 17u; break; }
        break;
    case GLYPH_I:
        switch (row) { case 0u: bits = 31u; break; case 1u: bits = 4u; break; case 2u: bits = 4u; break; case 3u: bits = 4u; break; case 4u: bits = 4u; break; case 5u: bits = 4u; break; case 6u: bits = 31u; break; }
        break;
    case GLYPH_L:
        switch (row) { case 0u: bits = 16u; break; case 1u: bits = 16u; break; case 2u: bits = 16u; break; case 3u: bits = 16u; break; case 4u: bits = 16u; break; case 5u: bits = 16u; break; case 6u: bits = 31u; break; }
        break;
    case GLYPH_M:
        switch (row) { case 0u: bits = 17u; break; case 1u: bits = 27u; break; case 2u: bits = 21u; break; case 3u: bits = 21u; break; case 4u: bits = 17u; break; case 5u: bits = 17u; break; case 6u: bits = 17u; break; }
        break;
    case GLYPH_N:
        switch (row) { case 0u: bits = 17u; break; case 1u: bits = 25u; break; case 2u: bits = 21u; break; case 3u: bits = 19u; break; case 4u: bits = 17u; break; case 5u: bits = 17u; break; case 6u: bits = 17u; break; }
        break;
    case GLYPH_O:
        switch (row) { case 0u: bits = 14u; break; case 1u: bits = 17u; break; case 2u: bits = 17u; break; case 3u: bits = 17u; break; case 4u: bits = 17u; break; case 5u: bits = 17u; break; case 6u: bits = 14u; break; }
        break;
    case GLYPH_P:
        switch (row) { case 0u: bits = 30u; break; case 1u: bits = 17u; break; case 2u: bits = 17u; break; case 3u: bits = 30u; break; case 4u: bits = 16u; break; case 5u: bits = 16u; break; case 6u: bits = 16u; break; }
        break;
    case GLYPH_R:
        switch (row) { case 0u: bits = 30u; break; case 1u: bits = 17u; break; case 2u: bits = 17u; break; case 3u: bits = 30u; break; case 4u: bits = 20u; break; case 5u: bits = 18u; break; case 6u: bits = 17u; break; }
        break;
    case GLYPH_S:
        switch (row) { case 0u: bits = 15u; break; case 1u: bits = 16u; break; case 2u: bits = 16u; break; case 3u: bits = 14u; break; case 4u: bits = 1u; break; case 5u: bits = 1u; break; case 6u: bits = 30u; break; }
        break;
    case GLYPH_T:
        switch (row) { case 0u: bits = 31u; break; case 1u: bits = 4u; break; case 2u: bits = 4u; break; case 3u: bits = 4u; break; case 4u: bits = 4u; break; case 5u: bits = 4u; break; case 6u: bits = 4u; break; }
        break;
    default:
        bits = 0u;
        break;
    }
    return bits;
}

float GlyphMask(float2 uv, uint glyph)
{
    float mask = 0.0f;

    if (!(uv.x < 0.0f || uv.y < 0.0f || uv.x >= 1.0f || uv.y >= 1.0f))
    {
        uint px = (uint)floor(uv.x * 5.0f);
        uint py = (uint)floor(uv.y * 7.0f);
        uint rowBits = GlyphRowBits(glyph, py);
        uint bit = (rowBits >> (4u - px)) & 1u;
        mask = (bit != 0u) ? 1.0f : 0.0f;
    }

    return mask;
}

float GlyphAt(float2 uv, float2 origin, float2 charSize, uint glyph)
{
    float2 charUv = (uv - origin) / charSize;
    return GlyphMask(charUv, glyph);
}

float DrawWord(float2 uv, uint g0, uint g1, uint g2, uint g3, uint g4, uint g5, uint g6, uint g7, uint glyphCount)
{
    const float2 charSize = float2(0.034f, 0.060f);
    const float xStep = 0.040f;
    const float2 origin = float2(0.030f, 0.035f);

    float mask = 0.0f;
    if (glyphCount > 0u) mask = max(mask, GlyphAt(uv, origin + float2(xStep * 0.0f, 0.0f), charSize, g0));
    if (glyphCount > 1u) mask = max(mask, GlyphAt(uv, origin + float2(xStep * 1.0f, 0.0f), charSize, g1));
    if (glyphCount > 2u) mask = max(mask, GlyphAt(uv, origin + float2(xStep * 2.0f, 0.0f), charSize, g2));
    if (glyphCount > 3u) mask = max(mask, GlyphAt(uv, origin + float2(xStep * 3.0f, 0.0f), charSize, g3));
    if (glyphCount > 4u) mask = max(mask, GlyphAt(uv, origin + float2(xStep * 4.0f, 0.0f), charSize, g4));
    if (glyphCount > 5u) mask = max(mask, GlyphAt(uv, origin + float2(xStep * 5.0f, 0.0f), charSize, g5));
    if (glyphCount > 6u) mask = max(mask, GlyphAt(uv, origin + float2(xStep * 6.0f, 0.0f), charSize, g6));
    if (glyphCount > 7u) mask = max(mask, GlyphAt(uv, origin + float2(xStep * 7.0f, 0.0f), charSize, g7));

    return mask;
}

float PanelLabelMask(uint labelId, float2 panelUv)
{
    float mask = 0.0f;

    if (labelId == LABEL_ALBEDO) {
        mask = DrawWord(panelUv, GLYPH_A, GLYPH_L, GLYPH_B, GLYPH_E, GLYPH_D, GLYPH_O, GLYPH_A, GLYPH_A, 6u);
    }
    else if (labelId == LABEL_NORMAL) {
        mask = DrawWord(panelUv, GLYPH_N, GLYPH_O, GLYPH_R, GLYPH_M, GLYPH_A, GLYPH_L, GLYPH_A, GLYPH_A, 6u);
    }
    else if (labelId == LABEL_SPEC) {
        mask = DrawWord(panelUv, GLYPH_S, GLYPH_P, GLYPH_E, GLYPH_C, GLYPH_A, GLYPH_A, GLYPH_A, GLYPH_A, 4u);
    }
    else if (labelId == LABEL_DEPTH) {
        mask = DrawWord(panelUv, GLYPH_D, GLYPH_E, GLYPH_P, GLYPH_T, GLYPH_H, GLYPH_A, GLYPH_A, GLYPH_A, 5u);
    }
    else if (labelId == LABEL_POSITION) {
        mask = DrawWord(panelUv, GLYPH_P, GLYPH_O, GLYPH_S, GLYPH_I, GLYPH_T, GLYPH_I, GLYPH_O, GLYPH_N, 8u);
    }
    else if (labelId == LABEL_DISP) {
        mask = DrawWord(panelUv, GLYPH_D, GLYPH_I, GLYPH_S, GLYPH_P, GLYPH_A, GLYPH_A, GLYPH_A, GLYPH_A, 4u);
    }
    else if (labelId == LABEL_TESS) {
        mask = DrawWord(panelUv, GLYPH_T, GLYPH_E, GLYPH_S, GLYPH_S, GLYPH_A, GLYPH_A, GLYPH_A, GLYPH_A, 4u);
    }
    else if (labelId == LABEL_CSM) {
        mask = DrawWord(panelUv, GLYPH_C, GLYPH_S, GLYPH_M, GLYPH_A, GLYPH_A, GLYPH_A, GLYPH_A, GLYPH_A, 3u);
    }
    else if (labelId == LABEL_MAP) {
        mask = DrawWord(panelUv, GLYPH_M, GLYPH_A, GLYPH_P, GLYPH_A, GLYPH_A, GLYPH_A, GLYPH_A, GLYPH_A, 3u);
    }
    else {
        mask = DrawWord(panelUv, GLYPH_L, GLYPH_I, GLYPH_T, GLYPH_A, GLYPH_A, GLYPH_A, GLYPH_A, GLYPH_A, 3u);
    }

    return mask;
}

float3 ApplyLabelOverlay(float3 panelColor, uint labelId, float2 panelUv)
{
    float shadow = PanelLabelMask(labelId, panelUv + float2(0.006f, 0.008f));
    float text = PanelLabelMask(labelId, panelUv);
    panelColor = lerp(panelColor, float3(0.0f, 0.0f, 0.0f), saturate(shadow));
    panelColor = lerp(panelColor, float3(1.0f, 1.0f, 0.0f), saturate(text));
    return panelColor;
}

float3 CascadeDebugColor(uint cascadeIndex)
{
    float3 color = float3(1.00f, 0.26f, 0.22f);
    if (cascadeIndex == 0u)
    {
        color = float3(0.18f, 0.72f, 1.00f);
    }
    else if (cascadeIndex == 1u)
    {
        color = float3(0.22f, 1.00f, 0.44f);
    }
    else if (cascadeIndex == 2u)
    {
        color = float3(1.00f, 0.82f, 0.20f);
    }

    return color;
}

float3 VisualizeShadowStatus(float3 posW)
{
    float3 result = float3(0.10f, 0.10f, 0.10f);
    if (gShadowParams.y >= 0.5f)
    {
        float viewDepth = mul(float4(posW, 1.0f), gView).z;
        result = float3(0.03f, 0.03f, 0.03f);

        if (viewDepth > 0.0f && viewDepth <= gShadowCascadeSplits.w)
        {
            uint cascadeIndex = SelectShadowCascade(viewDepth);
            float visibility = ComputeDirectionalShadow(posW);
            float3 cascadeColor = CascadeDebugColor(cascadeIndex);

            result = lerp(float3(0.02f, 0.02f, 0.025f), cascadeColor, visibility);
        }
    }

    return result;
}

float3 VisualizeShadowMapArray(float2 panelUv)
{
    float3 result = float3(0.10f, 0.10f, 0.10f);
    if (gShadowParams.y >= 0.5f)
    {
        float2 quadrant = floor(saturate(panelUv) * 2.0f);
        uint cascadeIndex = (uint)quadrant.x + (uint)quadrant.y * 2u;
        float2 localUv = frac(panelUv * 2.0f);
        float shadowDepth = gShadowMap.SampleLevel(gSamPointClamp, float3(localUv, (float)cascadeIndex), 0.0f);
        float depthContrast = saturate((1.0f - shadowDepth) * 12.0f);
        float3 panel = lerp(float3(0.015f, 0.015f, 0.018f), CascadeDebugColor(cascadeIndex), depthContrast);

        float2 gridLine = abs(frac(panelUv * 2.0f) - 0.5f);
        float separator = step(0.492f, max(gridLine.x, gridLine.y));
        result = lerp(panel, float3(0.92f, 0.92f, 0.82f), separator * 0.45f);
    }

    return result;
}

float3 SampleDebugPanel(uint tileX, uint tileY, float2 panelUv, out uint labelId)
{
    float4 albedoSample = gAlbedoTex.Sample(gSamPointClamp, panelUv);
    float4 normalSample = gNormalTex.Sample(gSamPointClamp, panelUv);
    float4 positionSpec = gPositionSpecTex.Sample(gSamPointClamp, panelUv);
    float3 albedo = albedoSample.rgb;
    float3 normalW = NormalizeNormalForLighting(normalSample.xyz);
    float debugDisplacement = saturate(normalSample.w);
    float debugTessFactor = saturate(albedoSample.a);
    float specPower = max(positionSpec.w, 1.0f);
    float depth = saturate(gDepthTex.Sample(gSamPointClamp, panelUv));
    float3 posW = ReconstructWorldPos(panelUv, depth);
    float3 litColor = EvaluateSceneLighting(albedo, normalW, posW, specPower);

    float3 panel = depth.xxx;
    labelId = LABEL_DEPTH;

    if (tileY == 0u && tileX == 0u) { panel = albedo; labelId = LABEL_ALBEDO; }
    else if (tileY == 0u && tileX == 1u) { panel = VisualizeNormal(normalW); labelId = LABEL_NORMAL; }
    else if (tileY == 0u && tileX == 2u) { panel = saturate(specPower / 128.0f).xxx; labelId = LABEL_SPEC; }
    else if (tileY == 1u && tileX == 0u) { panel = depth.xxx; labelId = LABEL_DEPTH; }
    else if (tileY == 1u && tileX == 1u) { panel = VisualizeShadowStatus(posW); labelId = LABEL_CSM; }
    else if (tileY == 1u && tileX == 2u) { panel = VisualizeShadowMapArray(panelUv); labelId = LABEL_MAP; }
    else if (tileY == 2u && tileX == 0u) { panel = VisualizeDisplacement(debugDisplacement); labelId = LABEL_DISP; }
    else if (tileY == 2u && tileX == 1u) { panel = VisualizeTessFactor(debugTessFactor); labelId = LABEL_TESS; }
    else if (tileY == 2u && tileX == 2u) { panel = litColor; labelId = LABEL_LIT; }

    return panel;
}

float4 PSLighting(FullscreenOut pin) : SV_Target
{
    float3 albedo = gAlbedoTex.Sample(gSamPointClamp, pin.TexC).rgb;
    float3 normalW = gNormalTex.Sample(gSamPointClamp, pin.TexC).xyz;
    float specPower = max(gPositionSpecTex.Sample(gSamPointClamp, pin.TexC).w, 1.0f);
    float depth = gDepthTex.Sample(gSamPointClamp, pin.TexC);
    float3 posW = ReconstructWorldPos(pin.TexC, saturate(depth));

    normalW = NormalizeNormalForLighting(normalW);
    float3 color = EvaluateSceneLighting(albedo, normalW, posW, specPower);

    if (gDebugViewEnabled != 0u)
    {
        float2 debugUv = saturate(pin.TexC);
        float2 tileCoord = debugUv * 3.0f;
        uint tileX = (uint)min(floor(tileCoord.x), 2.0f);
        uint tileY = (uint)min(floor(tileCoord.y), 2.0f);

        float2 panelUv = frac(tileCoord);
        uint labelId = LABEL_DEPTH;
        float3 panelColor = SampleDebugPanel(tileX, tileY, panelUv, labelId);
        panelColor = ApplyLabelOverlay(panelColor, labelId, panelUv);
        return float4(panelColor, 1.0f);
    }

    return float4(color, 1.0f);
}
