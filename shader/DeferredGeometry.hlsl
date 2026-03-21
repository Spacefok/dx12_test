static const uint MATERIAL_FLAG_HAS_BASE_COLOR_TEXTURE = 1u << 0;
static const uint MATERIAL_FLAG_HAS_NORMAL_TEXTURE = 1u << 1;
static const uint MATERIAL_FLAG_HAS_DISPLACEMENT_TEXTURE = 1u << 2;
static const uint MATERIAL_FLAG_HAS_OPACITY_TEXTURE = 1u << 3;
static const uint MATERIAL_FLAG_DISPLACEMENT_FROM_NORMAL = 1u << 4;

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

    float4 gTessellationParams; // x=min dist, y=max dist, z=min factor, w=max factor
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

struct GeometryPSInput
{
    float4 PosH : SV_POSITION;
    float3 PosW : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float4 Color : COLOR0;
    float2 TexC : TEXCOORD2;
    float3 TangentW : TEXCOORD3;
    float DebugDisplacement : TEXCOORD4;
    float DebugTessFactor : TEXCOORD5;
};

struct HSConstantData
{
    float EdgeTess[3] : SV_TessFactor;
    float InsideTess : SV_InsideTessFactor;
};

struct GBufferOut
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 PositionSpec : SV_Target2;
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

float SampleDebugDisplacement(float2 uv)
{
    float displacement = 0.0f;

    if ((gMatFlags & MATERIAL_FLAG_HAS_DISPLACEMENT_TEXTURE) != 0u)
    {
        displacement = SampleDisplacement(uv);
    }
    else if ((gMatFlags & MATERIAL_FLAG_HAS_NORMAL_TEXTURE) != 0u)
    {
        // Fallback for debug only: derive a "relief" proxy from the normal map
        // so the DISP panel stays informative even on scenes without height maps.
        float3 normalSample = gNormalMap.SampleLevel(gSamLinearWrap, uv, 0.0f).xyz;
        displacement = saturate(1.0f - normalSample.z);
    }

    return displacement;
}

float NormalizeDebugTessFactor(float tessFactor)
{
    float tessRange = max(gTessellationParams.w - gTessellationParams.z, 1e-4f);
    return saturate((tessFactor - gTessellationParams.z) / tessRange);
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

GeometryPSInput MakeGeometryOutput(
    ControlPointData cp,
    float3 displacedPosW,
    float2 uv,
    float debugDisplacement,
    float debugTessFactor)
{
    GeometryPSInput vout;
    vout.PosW = displacedPosW;
    vout.NormalW = cp.NormalW;
    vout.Color = cp.Color;
    vout.TexC = uv;
    vout.TangentW = cp.TangentW;
    vout.DebugDisplacement = debugDisplacement;
    vout.DebugTessFactor = debugTessFactor;
    vout.PosH = mul(float4(displacedPosW, 1.0f), gViewProj);
    return vout;
}

GeometryPSInput VSBasic(VertexIn vin)
{
    ControlPointData cp = BuildControlPointData(vin);
    float2 uv = ComputeMaterialUv(cp.BaseTexC);
    float debugDisplacement = SampleDebugDisplacement(uv);
    // Basic pipeline does not run tessellation, so keep the debug factor disabled.
    return MakeGeometryOutput(cp, cp.PosW, uv, debugDisplacement, 0.0f);
}

ControlPointData VSControlPoint(VertexIn vin)
{
    return BuildControlPointData(vin);
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HSConstants")]
[maxtessfactor(16.0f)]
ControlPointData HSMain(
    InputPatch<ControlPointData, 3> inputPatch,
    uint controlPointId : SV_OutputControlPointID)
{
    return inputPatch[controlPointId];
}

HSConstantData HSConstants(
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
GeometryPSInput DSMain(
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

    cp.Color =
        barycentrics.x * patch[0].Color +
        barycentrics.y * patch[1].Color +
        barycentrics.z * patch[2].Color;

    cp.BaseTexC =
        barycentrics.x * patch[0].BaseTexC +
        barycentrics.y * patch[1].BaseTexC +
        barycentrics.z * patch[2].BaseTexC;

    cp.TangentW =
        barycentrics.x * patch[0].TangentW +
        barycentrics.y * patch[1].TangentW +
        barycentrics.z * patch[2].TangentW;

    cp.TangentW = cp.TangentW - cp.NormalW * dot(cp.TangentW, cp.NormalW);
    cp.TangentW = SafeNormalize(cp.TangentW, BuildFallbackTangent(cp.NormalW));

    float2 uv = ComputeMaterialUv(cp.BaseTexC);
    float rawDisplacement = SampleDisplacement(uv);
    float displacement = rawDisplacement * gMatDisplacementScale + gMatDisplacementBias;
    float3 displacedPosW = cp.PosW - cp.NormalW * displacement;

    float debugDisplacement = SampleDebugDisplacement(uv);
    float debugTessFactor = NormalizeDebugTessFactor(hsConstData.InsideTess);

    return MakeGeometryOutput(cp, displacedPosW, uv, debugDisplacement, debugTessFactor);
}

GBufferOut PSGBuffer(GeometryPSInput pin)
{
    GBufferOut gout;

    float4 baseColorSample = float4(1.0f, 1.0f, 1.0f, 1.0f);
    if ((gMatFlags & MATERIAL_FLAG_HAS_BASE_COLOR_TEXTURE) != 0u)
    {
        baseColorSample = gBaseColorMap.Sample(gSamLinearWrap, pin.TexC);
    }

    float alpha = pin.Color.a * gMatDiffuseAlbedo.a * baseColorSample.a;
    if ((gMatFlags & MATERIAL_FLAG_HAS_OPACITY_TEXTURE) != 0u)
    {
        alpha *= gOpacityMap.Sample(gSamLinearWrap, pin.TexC).r;
    }

    if (gMatAlphaCutoff > 0.0f)
    {
        clip(alpha - gMatAlphaCutoff);
    }

    float3 albedo = pin.Color.rgb * gMatDiffuseAlbedo.rgb * baseColorSample.rgb;

    float3 normalW = SafeNormalize(pin.NormalW, float3(0.0f, 1.0f, 0.0f));
    if ((gMatFlags & MATERIAL_FLAG_HAS_NORMAL_TEXTURE) != 0u)
    {
        float3 tangentW = pin.TangentW - normalW * dot(pin.TangentW, normalW);
        tangentW = SafeNormalize(tangentW, BuildFallbackTangent(normalW));

        float3 bitangentW = SafeNormalize(cross(normalW, tangentW), float3(0.0f, 0.0f, 1.0f));
        float3 normalTS = gNormalMap.Sample(gSamLinearWrap, pin.TexC).xyz * 2.0f - 1.0f;
        normalW = SafeNormalize(
            normalTS.x * tangentW +
            normalTS.y * bitangentW +
            normalTS.z * normalW,
            normalW);
    }

    gout.Albedo = float4(albedo, saturate(pin.DebugTessFactor));
    gout.Normal = float4(normalW, saturate(pin.DebugDisplacement));
    gout.PositionSpec = float4(pin.PosW, max(gSpecPower, 1.0f));
    return gout;
}
