cbuffer PostProcessCB : register(b0)
{
    float2 gInvSceneSize;
    float2 gInvPostSize;
    float gExposure;
    float gBloomStrength;
    float gBloomThreshold;
    float gBloomKnee;
    float gFocusDistance;
    float gFocusRange;
    float gMaxDofBlur;
    float gTime;
    float gVignetteStrength;
    float gChromaticAberration;
    float gGrainStrength;
    float gGamma;
    float2 gCameraNearFar;
    float gFisheyeStrength;
    float gFisheyeZoom;
};

Texture2D gSourceTex : register(t0);
Texture2D<float> gDepthTex : register(t1);
Texture2D gBloomTex : register(t2);
Texture2D gSceneBlurTex : register(t3);

SamplerState gSamPointClamp : register(s0);
SamplerState gSamLinearClamp : register(s1);

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

float Luminance(float3 color)
{
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 SampleSceneBox(float2 uv, float2 texelSize)
{
    float3 color = 0.0f.xxx;
    color += gSourceTex.SampleLevel(gSamLinearClamp, uv + texelSize * float2(-0.5f, -0.5f), 0.0f).rgb;
    color += gSourceTex.SampleLevel(gSamLinearClamp, uv + texelSize * float2(0.5f, -0.5f), 0.0f).rgb;
    color += gSourceTex.SampleLevel(gSamLinearClamp, uv + texelSize * float2(-0.5f, 0.5f), 0.0f).rgb;
    color += gSourceTex.SampleLevel(gSamLinearClamp, uv + texelSize * float2(0.5f, 0.5f), 0.0f).rgb;
    return color * 0.25f;
}

float4 PSDownsample(FullscreenOut pin) : SV_Target
{
    return float4(SampleSceneBox(pin.TexC, gInvSceneSize), 1.0f);
}

float3 SoftKneeBrightPass(float3 color)
{
    float brightness = max(max(color.r, color.g), color.b);
    float knee = max(gBloomKnee, 1e-4f);
    float soft = brightness - gBloomThreshold + knee;
    soft = saturate(soft / (2.0f * knee)) * soft;
    float contribution = max(brightness - gBloomThreshold, soft);
    contribution /= max(brightness, 1e-4f);
    return color * saturate(contribution);
}

float4 PSBrightPass(FullscreenOut pin) : SV_Target
{
    float3 color = SampleSceneBox(pin.TexC, gInvSceneSize);
    return float4(SoftKneeBrightPass(color), 1.0f);
}

float3 GaussianBlur(float2 uv, float2 texelStep)
{
    static const float weights[7] =
    {
        0.214607f,
        0.189879f,
        0.131514f,
        0.071303f,
        0.030173f,
        0.009976f,
        0.002583f
    };

    float3 color = gSourceTex.SampleLevel(gSamLinearClamp, uv, 0.0f).rgb * weights[0];

    [unroll]
    for (int i = 1; i < 7; ++i)
    {
        float2 offset = texelStep * (float)i;
        color += gSourceTex.SampleLevel(gSamLinearClamp, uv + offset, 0.0f).rgb * weights[i];
        color += gSourceTex.SampleLevel(gSamLinearClamp, uv - offset, 0.0f).rgb * weights[i];
    }

    return color;
}

float4 PSBlurHorizontal(FullscreenOut pin) : SV_Target
{
    return float4(GaussianBlur(pin.TexC, float2(gInvPostSize.x, 0.0f)), 1.0f);
}

float4 PSBlurVertical(FullscreenOut pin) : SV_Target
{
    return float4(GaussianBlur(pin.TexC, float2(0.0f, gInvPostSize.y)), 1.0f);
}

float LinearizeDepth(float depth)
{
    float nearZ = gCameraNearFar.x;
    float farZ = gCameraNearFar.y;
    return (nearZ * farZ) / max(farZ - depth * (farZ - nearZ), 1e-4f);
}

float DofFactor(float2 uv)
{
    float depth = saturate(gDepthTex.SampleLevel(gSamPointClamp, uv, 0.0f));
    float viewDepth = LinearizeDepth(depth);
    float coc = abs(viewDepth - gFocusDistance) / max(gFocusRange, 1e-3f);
    float validDepth = (depth < 0.99999f) ? 1.0f : 0.0f;
    return smoothstep(0.10f, 1.0f, saturate(coc)) * saturate(gMaxDofBlur) * validDepth;
}

float3 SampleSceneChromatic(float2 uv)
{
    float2 fromCenter = uv - 0.5f.xx;
    float dist2 = dot(fromCenter, fromCenter);
    float2 offset = normalize(fromCenter + 1e-5f.xx) * dist2 * gChromaticAberration * 0.0035f;

    float r = gSourceTex.SampleLevel(gSamLinearClamp, uv + offset, 0.0f).r;
    float g = gSourceTex.SampleLevel(gSamLinearClamp, uv, 0.0f).g;
    float b = gSourceTex.SampleLevel(gSamLinearClamp, uv - offset, 0.0f).b;
    return float3(r, g, b);
}

float2 ApplyFisheye(float2 uv, out float inBounds)
{
    float2 centered = uv * 2.0f - 1.0f;
    float radiusSq = dot(centered, centered);
    float warp = 1.0f + gFisheyeStrength * radiusSq;
    float2 warped = centered * warp / max(gFisheyeZoom, 1e-3f);
    float2 warpedUv = warped * 0.5f + 0.5f;

    inBounds =
        (warpedUv.x >= 0.0f && warpedUv.x <= 1.0f &&
         warpedUv.y >= 0.0f && warpedUv.y <= 1.0f) ? 1.0f : 0.0f;

    return saturate(warpedUv);
}

float3 AcesFitted(float3 color)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

float4 PSFinalComposite(FullscreenOut pin) : SV_Target
{
    float edgeMask = 1.0f;
    float2 uv = ApplyFisheye(pin.TexC, edgeMask);

    float3 scene = SampleSceneChromatic(uv);
    float3 blurredScene = gSceneBlurTex.SampleLevel(gSamLinearClamp, uv, 0.0f).rgb;
    scene = lerp(scene, blurredScene, DofFactor(uv));

    float3 bloom = gBloomTex.SampleLevel(gSamLinearClamp, uv, 0.0f).rgb * gBloomStrength;
    float3 hdr = scene + bloom;

    float3 ldr = AcesFitted(hdr * gExposure);

    float2 centered = pin.TexC * 2.0f - 1.0f;
    float vignette = saturate(1.0f - dot(centered, centered) * gVignetteStrength);
    ldr *= vignette * edgeMask;

    float noise = InterleavedGradientNoise(pin.PosH.xy + gTime * float2(37.0f, 17.0f)) - 0.5f;
    ldr += noise * gGrainStrength;

    float dither = InterleavedGradientNoise(pin.PosH.xy + gTime * float2(19.0f, 53.0f)) - 0.5f;
    ldr += dither / 255.0f;

    float invGamma = rcp(max(gGamma, 1e-3f));
    float3 gammaEncoded = pow(saturate(ldr), float3(invGamma, invGamma, invGamma));
    return float4(gammaEncoded, 1.0f);
}
