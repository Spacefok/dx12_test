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

struct SurfaceDisplacementData
{
    float3 PosW;
    float3 NormalW;
    float3 TangentW;
    float DebugDisplacement;
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

float3 GammaToLinear(float3 color)
{
    return pow(saturate(color), float3(2.2f, 2.2f, 2.2f));
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

bool MaterialUsesProceduralWater()
{
    return (gMatFlags & MATERIAL_FLAG_PROCEDURAL_WATER) != 0u && gMatWaterParams.x > 1e-5f;
}

float EvaluateWaterWave(float2 baseTexC, out float2 displacementGradient)
{
    displacementGradient = float2(0.0f, 0.0f);
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

        float s0;
        float c0;
        float s1;
        float c1;
        float s2;
        float c2;
        sincos(phase0, s0, c0);
        sincos(phase1, s1, c1);
        sincos(phase2, s2, c2);
        
        waveHeight += amplitude * s0;
        waveHeight += amplitude * secondaryRatio * s1;
        waveHeight += amplitude * 0.35f * s2;

        displacementGradient += dir0 * (amplitude * tau * frequency * c0);
        displacementGradient += dir1 * (amplitude * secondaryRatio * tau * frequency * 1.7f * c1);
        displacementGradient += dir2 * (amplitude * 0.35f * tau * frequency * 0.8f * c2);
    }

    return waveHeight;
}

SurfaceDisplacementData ApplySurfaceDisplacement(ControlPointData cp, float2 uv)
{
    SurfaceDisplacementData output;
    output.PosW = cp.PosW;
    output.NormalW = cp.NormalW;
    output.TangentW = cp.TangentW;
    output.DebugDisplacement = SampleDebugDisplacement(uv);

    if ((gMatFlags & MATERIAL_FLAG_HAS_DISPLACEMENT_TEXTURE) != 0u)
    {
        float rawDisplacement = SampleDisplacement(uv);
        float displacement = rawDisplacement * gMatDisplacementScale + gMatDisplacementBias;
        output.PosW -= cp.NormalW * displacement;
        output.DebugDisplacement = max(output.DebugDisplacement, saturate(abs(displacement) * 10.0f));
    }

    if (MaterialUsesProceduralWater())
    {
        float2 displacementGradient = float2(0.0f, 0.0f);
        float waveHeight = EvaluateWaterWave(cp.BaseTexC, displacementGradient);

        float3 tangentW = output.TangentW - output.NormalW * dot(output.TangentW, output.NormalW);
        tangentW = SafeNormalize(tangentW, BuildFallbackTangent(output.NormalW));
        float3 bitangentW = SafeNormalize(cross(output.NormalW, tangentW), float3(0.0f, 0.0f, 1.0f));

        float3 dpdu = tangentW + output.NormalW * displacementGradient.x;
        float3 dpdv = bitangentW + output.NormalW * displacementGradient.y;
        float3 waveNormalW = SafeNormalize(cross(dpdv, dpdu), output.NormalW);

        output.PosW += cp.NormalW * waveHeight;
        output.NormalW = waveNormalW;
        output.TangentW = SafeNormalize(dpdu - waveNormalW * dot(dpdu, waveNormalW), tangentW);
        output.DebugDisplacement = max(output.DebugDisplacement, saturate(abs(waveHeight) / max(gMatWaterParams.x, 1e-4f)));
    }

    return output;
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
    SurfaceDisplacementData surface = ApplySurfaceDisplacement(cp, uv);
    float debugTessFactor = NormalizeDebugTessFactor(hsConstData.InsideTess);

    cp.NormalW = surface.NormalW;
    cp.TangentW = surface.TangentW;
    return MakeGeometryOutput(cp, surface.PosW, uv, surface.DebugDisplacement, debugTessFactor);
}

GBufferOut PSGBuffer(GeometryPSInput pin)
{
    GBufferOut gout;

    float4 baseColorSample = float4(1.0f, 1.0f, 1.0f, 1.0f);
    if ((gMatFlags & MATERIAL_FLAG_HAS_BASE_COLOR_TEXTURE) != 0u)
    {
        baseColorSample = gBaseColorMap.Sample(gSamLinearWrap, pin.TexC);
        baseColorSample.rgb = GammaToLinear(baseColorSample.rgb);
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

float4 PSTransparent(GeometryPSInput pin) : SV_Target
{
    float4 baseColorSample = float4(1.0f, 1.0f, 1.0f, 1.0f);
    if ((gMatFlags & MATERIAL_FLAG_HAS_BASE_COLOR_TEXTURE) != 0u)
    {
        baseColorSample = gBaseColorMap.Sample(gSamLinearWrap, pin.TexC);
        baseColorSample.rgb = GammaToLinear(baseColorSample.rgb);
    }

    float alpha = pin.Color.a * gMatDiffuseAlbedo.a * baseColorSample.a;
    if ((gMatFlags & MATERIAL_FLAG_HAS_OPACITY_TEXTURE) != 0u)
    {
        alpha *= gOpacityMap.Sample(gSamLinearWrap, pin.TexC).r;
    }

    alpha = saturate(alpha);
    if (alpha <= 1e-3f)
    {
        discard;
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

    float3 viewDir = SafeNormalize(gEyePosW - pin.PosW, float3(0.0f, 0.0f, 1.0f));
    float3 lightDir = SafeNormalize(-gLightDirW, float3(0.0f, -1.0f, 0.0f));
    float ndotl = saturate(dot(normalW, lightDir));
    float3 color = gAmbient.rgb * albedo + gDiffuse.rgb * albedo * ndotl;

    float3 halfVector = SafeNormalize(viewDir + lightDir, lightDir);
    float spec = pow(saturate(dot(normalW, halfVector)), max(gSpecPower, 1.0f));
    color += gSpecular.rgb * spec;

    return float4(color, alpha);
}
