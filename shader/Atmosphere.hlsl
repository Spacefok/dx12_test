// Single scattering: camera transmittance * local scattering * sun transmittance.
// Model and HDR composition: GPU Gems 2, chapter 16 (Sean O'Neil).
// Unlike the outgoing/outgoing convention in PBRT, mu below is positive when
// looking toward the sun. Henyey-Greenstein therefore uses 1+g*g-2*g*mu.
cbuffer AtmosphereCB : register(b0)
{
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float gWorldToKm;
    float3 gSunDirection;
    float gSunIntensity;
    float3 gRayleighScattering;
    float gRayleighScale;
    float gMieScattering;
    float gMieExtinction;
    float gMieScale;
    float gMieG;
    float gPlanetRadiusKm;
    float gAtmosphereRadiusKm;
    float gRayleighHeightKm;
    float gMieHeightKm;
    uint gObjectsEnabled;
    uint gViewSamples;
    uint gLightSamples;
    uint gAtmospherePadding;
};

Texture2D<float4> gSceneColor : register(t0);
Texture2D<float> gSceneDepth : register(t1);
SamplerState gPointClamp : register(s0);

static const float PI = 3.14159265358979323846f;

struct FullscreenOut
{
    float4 Position : SV_POSITION;
    float2 UV : TEXCOORD0;
};

FullscreenOut VSFullscreen(uint vertexId : SV_VertexID)
{
    FullscreenOut output;
    output.UV = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(output.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float2 RaySphere(float3 origin, float3 direction, float radius)
{
    float b = dot(origin, direction);
    // Factoring the radius difference avoids subtracting two ~40,000,000 values.
    float originRadius = length(origin);
    float c = (originRadius - radius) * (originRadius + radius);
    float discriminant = b * b - c;
    float2 result = float2(-1.0f, -1.0f);
    if (discriminant >= 0.0f)
    {
        float root = sqrt(discriminant);
        float q = -b - (b >= 0.0f ? root : -root);
        result = float2(-b, -b);
        if (abs(q) >= 1e-6f)
        {
            float other = c / q;
            result = float2(min(q, other), max(q, other));
        }
    }
    return result;
}

float2 Density(float3 positionKm)
{
    float altitude = max(length(positionKm) - gPlanetRadiusKm, 0.0f);
    return exp(-altitude / float2(gRayleighHeightKm, gMieHeightKm));
}

float RayleighPhase(float cosine)
{
    return 3.0f * (1.0f + cosine * cosine) / (16.0f * PI);
}

float MiePhase(float cosine)
{
    float g = clamp(gMieG, -0.95f, 0.95f);
    float denominator = max(1.0f + g * g - 2.0f * g * cosine, 1e-4f);
    return (1.0f - g * g) / (4.0f * PI * denominator * sqrt(denominator));
}

float3 SunTransmittance(float3 positionKm, float3 toSun, float3 betaR, float betaMExtinction)
{
    // The planet blocks direct sunlight on the night side.
    float2 ground = RaySphere(positionKm, toSun, gPlanetRadiusKm);
    float3 result = 0.0f.xxx;
    if (ground.x <= 0.0f)
    {
        float distance = max(RaySphere(positionKm, toSun, gAtmosphereRadiusKm).y, 0.0f);
        uint samples = clamp(gLightSamples, 1u, 16u);
        float2 opticalLength = 0.0f.xx;
        float previousT = 0.0f;
        [loop]
        for (uint sampleIndex = 0; sampleIndex < samples; ++sampleIndex)
        {
            // Denser samples near the scattering point resolve the low Mie layer.
            float fraction = (float)(sampleIndex + 1u) / (float)samples;
            float nextT = distance * fraction * fraction;
            float midpoint = 0.5f * (previousT + nextT);
            opticalLength += Density(positionKm + toSun * midpoint) * (nextT - previousT);
            previousT = nextT;
        }
        result = exp(-(betaR * opticalLength.x + betaMExtinction * opticalLength.y));
    }
    return result;
}

float3 SegmentIntegral(float3 extinction, float lengthKm)
{
    // Integral exp(-extinction*t) dt for a locally constant medium.
    // The series keeps zero/tiny coefficients continuous instead of dividing by zero.
    float3 tau = extinction * lengthKm;
    float3 ratio = (1.0f.xxx - exp(-tau)) / max(tau, 1e-6f.xxx);
    float3 series = 1.0f.xxx - 0.5f * tau + tau * tau / 6.0f;
    return lengthKm * float3(tau.x < 0.001f ? series.x : ratio.x,
        tau.y < 0.001f ? series.y : ratio.y, tau.z < 0.001f ? series.z : ratio.z);
}

float4 PSAtmosphere(FullscreenOut input) : SV_Target
{
    float depth = gSceneDepth.SampleLevel(gPointClamp, input.UV, 0.0f);
    float3 scene = gSceneColor.SampleLevel(gPointClamp, input.UV, 0.0f).rgb;
    bool surface = depth < 1.0f;
    if (surface && gObjectsEnabled == 0u) return float4(scene, 1.0f);

    float4 projected = mul(float4(input.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), depth, 1.0f), gInvViewProj);
    float3 pointW = projected.xyz / projected.w;
    float3 rayW = pointW - gEyePosW;
    float3 direction = normalize(rayW);
    float3 origin = gEyePosW * gWorldToKm;
    origin.y = gPlanetRadiusKm + max(origin.y, 0.001f);
    float2 atmosphereHit = RaySphere(origin, direction, gAtmosphereRadiusKm);
    if (atmosphereHit.y <= 0.0f) return float4(surface ? scene : 0.0f.xxx, 1.0f);
    float startT = max(atmosphereHit.x, 0.0f);
    float endT = atmosphereHit.y;
    if (surface) endT = min(endT, length(rayW) * gWorldToKm);
    float2 groundHit = RaySphere(origin, direction, gPlanetRadiusKm);
    if (groundHit.x > 0.0f) endT = min(endT, groundHit.x);
    if (endT <= startT) return float4(surface ? scene : 0.0f.xxx, 1.0f);

    float3 toSun = normalize(gSunDirection);
    float cosine = clamp(dot(direction, toSun), -1.0f, 1.0f);
    float phaseR = RayleighPhase(cosine);
    float phaseM = MiePhase(cosine);
    float3 betaR = gRayleighScattering * max(gRayleighScale, 0.0f);
    float betaM = gMieScattering * max(gMieScale, 0.0f);
    float betaMExtinction = gMieExtinction * max(gMieScale, 0.0f);
    float3 transmittance = 1.0f.xxx;
    float3 scattering = 0.0f.xxx;
    uint samples = clamp(gViewSamples, 1u, 64u);
    float previousT = startT;
    [loop]
    for (uint sampleIndex = 0; sampleIndex < samples; ++sampleIndex)
    {
        float fraction = (float)(sampleIndex + 1u) / (float)samples;
        float nextT = startT + (endT - startT) * fraction * fraction;
        float segmentLength = nextT - previousT;
        float3 positionKm = origin + direction * (0.5f * (previousT + nextT));
        float2 density = Density(positionKm);
        float3 extinction = betaR * density.x + betaMExtinction * density.y;
        float3 source = (betaR * density.x * phaseR + betaM * density.y * phaseM)
            * gSunIntensity * SunTransmittance(positionKm, toSun, betaR, betaMExtinction);
        scattering += transmittance * source * SegmentIntegral(extinction, segmentLength);
        transmittance *= exp(-extinction * segmentLength);
        previousT = nextT;
    }

    // All terms stay linear HDR. Visible depth is the integration endpoint:
    // no fog or sky from behind an opaque surface is added here.
    return float4(scattering + (surface ? scene * transmittance : 0.0f.xxx), 1.0f);
}
