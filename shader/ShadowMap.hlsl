static const uint MATERIAL_FLAG_HAS_BASE_COLOR_TEXTURE = 1u << 0;
static const uint MATERIAL_FLAG_HAS_NORMAL_TEXTURE = 1u << 1;
static const uint MATERIAL_FLAG_HAS_DISPLACEMENT_TEXTURE = 1u << 2;
static const uint MATERIAL_FLAG_HAS_OPACITY_TEXTURE = 1u << 3;
static const uint MATERIAL_FLAG_DISPLACEMENT_FROM_NORMAL = 1u << 4;
static const uint MATERIAL_FLAG_USE_TESSELLATION = 1u << 5;
static const uint MATERIAL_FLAG_PROCEDURAL_WATER = 1u << 6;

cbuffer ObjectCB : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
};

cbuffer PassCB : register(b1)
{
    float4x4 gViewProj;

    float3 gEyePosW;
    float _pad0;

    float3 gLightDirW;
    float _pad1;

    float4 gAmbient;
    float4 gDiffuse;
    float4 gSpecular;

    float gSpecPower;
    float3 _pad2;

    float2 gUvScroll;
    float2 gUvTiling;

    float gTime;
    float3 _pad3;

    float4 gTessellationParams;
};

cbuffer MaterialCB : register(b2)
{
    float4 gMatDiffuseAlbedo;
    float4 gMatUvTilingOffset;
    uint gMatFlags;
    float gMatDisplacementScale;
    float gMatDisplacementBias;
    float gMatAlphaCutoff;
    float4 gMatWindParams;
    float4 gMatWaterParams;
};

Texture2D gBaseColorMap : register(t0);
Texture2D gNormalMap : register(t1);
Texture2D gDisplacementMap : register(t2);
Texture2D gOpacityMap : register(t3);
SamplerState gSamLinearWrap : register(s0);

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float4 Color : COLOR;
    float2 TexC : TEXCOORD;
    float3 TangentL : TANGENT;
};

struct ControlPointData
{
    float3 PosW : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float4 Color : COLOR0;
    float2 BaseTexC : TEXCOORD2;
    float3 TangentW : TEXCOORD3;
};

struct ShadowPSInput
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD0;
    float4 Color : COLOR0;
};

struct HSConstantData
{
    float EdgeTess[3] : SV_TessFactor;
    float InsideTess : SV_InsideTessFactor;
};

float3 SafeNormalize(float3 value, float3 fallback)
{
    float3 result = fallback;
    float lenSq = dot(value, value);
    if (lenSq > 1e-8f)
    {
        result = value * rsqrt(lenSq);
    }

    return result;
}

float3 BuildFallbackTangent(float3 normalW)
{
    float3 helper = (abs(normalW.y) < 0.999f) ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    return SafeNormalize(cross(helper, normalW), float3(1.0f, 0.0f, 0.0f));
}

float2 ComputeMaterialUv(float2 baseTexC)
{
    return baseTexC * gMatUvTilingOffset.xy * gUvTiling + gMatUvTilingOffset.zw + gUvScroll;
}

float4 ApplyWind(float4 posW, float2 baseTexC)
{
    if (gMatWindParams.x > 0.5f)
    {
        float2 windDir = normalize(float2(0.8f, 0.6f));
        float bendWeight = saturate(baseTexC.y);

        float phase = gTime * gMatWindParams.w + dot(posW.xyz, float3(3.1f, 1.7f, 2.3f)) * gMatWindParams.z;
        float gust = sin(phase) + 0.5f * sin(phase * 1.91f + 1.2f);
        float flutter = sin(phase * 2.7f + baseTexC.x * 6.2831853f) * 0.25f;
        float wind = (gust + flutter) * gMatWindParams.y * bendWeight;

        posW.x += windDir.x * wind;
        posW.z += windDir.y * wind;
        posW.y += abs(wind) * 0.2f;
    }

    return posW;
}

ControlPointData BuildControlPointData(VertexIn vin)
{
    ControlPointData vout;

    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    posW = ApplyWind(posW, vin.TexC);

    float3 normalW = mul(vin.NormalL, (float3x3)gWorldInvTranspose);
    normalW = SafeNormalize(normalW, float3(0.0f, 1.0f, 0.0f));

    float3 tangentW = mul(vin.TangentL, (float3x3)gWorld);
    tangentW = tangentW - normalW * dot(tangentW, normalW);
    tangentW = SafeNormalize(tangentW, BuildFallbackTangent(normalW));

    vout.PosW = posW.xyz;
    vout.NormalW = normalW;
    vout.Color = vin.Color;
    vout.BaseTexC = vin.TexC;
    vout.TangentW = tangentW;
    return vout;
}

float SampleDisplacement(float2 uv)
{
    float displacement = 0.0f;

    if ((gMatFlags & MATERIAL_FLAG_HAS_DISPLACEMENT_TEXTURE) != 0u)
    {
        float4 displacementSample = gDisplacementMap.SampleLevel(gSamLinearWrap, uv, 0.0f);

        if ((gMatFlags & MATERIAL_FLAG_DISPLACEMENT_FROM_NORMAL) != 0u)
        {
            displacement = saturate(1.0f - displacementSample.b);
        }
        else
        {
            displacement = displacementSample.r;
        }
    }

    return displacement;
}

bool MaterialUsesProceduralWater()
{
    return (gMatFlags & MATERIAL_FLAG_PROCEDURAL_WATER) != 0u && gMatWaterParams.x > 1e-5f;
}

float EvaluateWaterWave(float2 baseTexC)
{
    float waveHeight = 0.0f;

    if (MaterialUsesProceduralWater())
    {
        float amplitude = gMatWaterParams.x;
        float frequency = max(gMatWaterParams.y, 0.1f);
        float speed = gMatWaterParams.z;
        float secondaryRatio = saturate(gMatWaterParams.w);

        float2 uv = baseTexC * gMatUvTilingOffset.xy * frequency + gMatUvTilingOffset.zw;

        const float2 dir0 = normalize(float2(1.0f, 0.35f));
        const float2 dir1 = normalize(float2(-0.45f, 1.0f));
        const float2 dir2 = normalize(float2(0.7f, -0.55f));
        const float tau = 6.2831853f;

        float phase0 = dot(uv, dir0) * tau + gTime * speed * 1.15f;
        float phase1 = dot(uv, dir1) * tau * 1.7f - gTime * speed * 1.55f + 1.2f;
        float phase2 = dot(uv, dir2) * tau * 0.8f + gTime * speed * 0.6f - 0.75f;

        waveHeight += amplitude * sin(phase0);
        waveHeight += amplitude * secondaryRatio * sin(phase1);
        waveHeight += amplitude * 0.35f * sin(phase2);
    }

    return waveHeight;
}

float3 ApplyShadowDisplacement(ControlPointData cp, float2 uv)
{
    float3 posW = cp.PosW;

    if ((gMatFlags & MATERIAL_FLAG_HAS_DISPLACEMENT_TEXTURE) != 0u)
    {
        float displacement = SampleDisplacement(uv) * gMatDisplacementScale + gMatDisplacementBias;
        posW -= cp.NormalW * displacement;
    }

    if (MaterialUsesProceduralWater())
    {
        posW += cp.NormalW * EvaluateWaterWave(cp.BaseTexC);
    }

    return posW;
}

float ComputeTessellationFactor(float distanceToEye)
{
    float minDistance = gTessellationParams.x;
    float maxDistance = gTessellationParams.y;
    float minFactor = gTessellationParams.z;
    float maxFactor = gTessellationParams.w;

    float distRange = max(maxDistance - minDistance, 1e-4f);
    float t = saturate((distanceToEye - minDistance) / distRange);
    return max(1.0f, lerp(maxFactor, minFactor, t));
}

float EdgeTessellationFactor(float3 p0, float3 p1)
{
    float3 midPoint = 0.5f * (p0 + p1);
    return ComputeTessellationFactor(distance(midPoint, gEyePosW));
}

ShadowPSInput MakeShadowOutput(float3 posW, float2 uv, float4 color)
{
    ShadowPSInput vout;
    vout.PosH = mul(float4(posW, 1.0f), gViewProj);
    vout.TexC = uv;
    vout.Color = color;
    return vout;
}

ShadowPSInput VSShadowBasic(VertexIn vin)
{
    ControlPointData cp = BuildControlPointData(vin);
    float2 uv = ComputeMaterialUv(cp.BaseTexC);
    return MakeShadowOutput(cp.PosW, uv, vin.Color);
}

ControlPointData VSShadowControlPoint(VertexIn vin)
{
    return BuildControlPointData(vin);
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HSShadowConstants")]
[maxtessfactor(16.0f)]
ControlPointData HSShadow(
    InputPatch<ControlPointData, 3> inputPatch,
    uint controlPointId : SV_OutputControlPointID)
{
    return inputPatch[controlPointId];
}

HSConstantData HSShadowConstants(
    InputPatch<ControlPointData, 3> inputPatch,
    uint patchId : SV_PrimitiveID)
{
    patchId = patchId;

    HSConstantData output;

    float3 p0 = inputPatch[0].PosW;
    float3 p1 = inputPatch[1].PosW;
    float3 p2 = inputPatch[2].PosW;

    output.EdgeTess[0] = EdgeTessellationFactor(p1, p2);
    output.EdgeTess[1] = EdgeTessellationFactor(p2, p0);
    output.EdgeTess[2] = EdgeTessellationFactor(p0, p1);

    float3 patchCenter = (p0 + p1 + p2) / 3.0f;
    output.InsideTess = ComputeTessellationFactor(distance(patchCenter, gEyePosW));

    return output;
}

[domain("tri")]
ShadowPSInput DSShadow(
    HSConstantData hsConstData,
    float3 barycentrics : SV_DomainLocation,
    const OutputPatch<ControlPointData, 3> patch)
{
    hsConstData = hsConstData;

    ControlPointData cp;
    cp.PosW =
        barycentrics.x * patch[0].PosW +
        barycentrics.y * patch[1].PosW +
        barycentrics.z * patch[2].PosW;

    cp.NormalW = SafeNormalize(
        barycentrics.x * patch[0].NormalW +
        barycentrics.y * patch[1].NormalW +
        barycentrics.z * patch[2].NormalW,
        float3(0.0f, 1.0f, 0.0f));

    cp.BaseTexC =
        barycentrics.x * patch[0].BaseTexC +
        barycentrics.y * patch[1].BaseTexC +
        barycentrics.z * patch[2].BaseTexC;

    cp.Color =
        barycentrics.x * patch[0].Color +
        barycentrics.y * patch[1].Color +
        barycentrics.z * patch[2].Color;

    cp.TangentW =
        barycentrics.x * patch[0].TangentW +
        barycentrics.y * patch[1].TangentW +
        barycentrics.z * patch[2].TangentW;

    cp.TangentW = cp.TangentW - cp.NormalW * dot(cp.TangentW, cp.NormalW);
    cp.TangentW = SafeNormalize(cp.TangentW, BuildFallbackTangent(cp.NormalW));

    float2 uv = ComputeMaterialUv(cp.BaseTexC);
    float3 posW = ApplyShadowDisplacement(cp, uv);
    return MakeShadowOutput(posW, uv, cp.Color);
}

void PSShadowDepth(ShadowPSInput pin)
{
    float alpha = pin.Color.a * gMatDiffuseAlbedo.a;

    if ((gMatFlags & MATERIAL_FLAG_HAS_BASE_COLOR_TEXTURE) != 0u)
    {
        alpha *= gBaseColorMap.Sample(gSamLinearWrap, pin.TexC).a;
    }

    if ((gMatFlags & MATERIAL_FLAG_HAS_OPACITY_TEXTURE) != 0u)
    {
        alpha *= gOpacityMap.Sample(gSamLinearWrap, pin.TexC).r;
    }

    if (gMatAlphaCutoff > 0.0f)
    {
        clip(alpha - gMatAlphaCutoff);
    }
}
