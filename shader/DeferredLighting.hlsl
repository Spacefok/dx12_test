#define MAX_DIRECTIONAL_LIGHTS 4
#define MAX_POINT_LIGHTS 32
#define MAX_SPOT_LIGHTS 16

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
    uint gDirectionalLightCount;
    uint gPointLightCount;
    uint gSpotLightCount;
    uint gPad0;
    DirectionalLight gDirectionalLights[MAX_DIRECTIONAL_LIGHTS];
    PointLight gPointLights[MAX_POINT_LIGHTS];
    SpotLight gSpotLights[MAX_SPOT_LIGHTS];
};

Texture2D gAlbedoTex : register(t0);
Texture2D gNormalTex : register(t1);
Texture2D gPositionSpecTex : register(t2);
SamplerState gSamPointClamp : register(s0);

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

float4 PSLighting(FullscreenOut pin) : SV_Target
{
    float3 albedo = gAlbedoTex.Sample(gSamPointClamp, pin.TexC).rgb;
    float3 normalW = gNormalTex.Sample(gSamPointClamp, pin.TexC).xyz;
    float4 posSpec = gPositionSpecTex.Sample(gSamPointClamp, pin.TexC);
    float3 posW = posSpec.xyz;
    float specPower = max(posSpec.w, 1.0f);

    if (dot(normalW, normalW) < 1e-6f)
    {
        normalW = float3(0.0f, 1.0f, 0.0f);
    }
    else
    {
        normalW = normalize(normalW);
    }

    float3 viewVec = gEyePosW - posW;
    float viewLen2 = dot(viewVec, viewVec);
    float3 viewDir = (viewLen2 > 1e-6f) ? (viewVec * rsqrt(viewLen2)) : float3(0.0f, 0.0f, 1.0f);

    float3 color = albedo * gAmbientColor.rgb * gAmbientIntensity;

    [loop]
    for (uint dirIndex = 0; dirIndex < gDirectionalLightCount && dirIndex < MAX_DIRECTIONAL_LIGHTS; ++dirIndex)
    {
        color += EvaluateDirectional(gDirectionalLights[dirIndex], albedo, normalW, viewDir, specPower);
    }

    [loop]
    for (uint pointIndex = 0; pointIndex < gPointLightCount && pointIndex < MAX_POINT_LIGHTS; ++pointIndex)
    {
        color += EvaluatePoint(gPointLights[pointIndex], posW, albedo, normalW, viewDir, specPower);
    }

    [loop]
    for (uint spotIndex = 0; spotIndex < gSpotLightCount && spotIndex < MAX_SPOT_LIGHTS; ++spotIndex)
    {
        color += EvaluateSpot(gSpotLights[spotIndex], posW, albedo, normalW, viewDir, specPower);
    }

    return float4(color, 1.0f);
}
