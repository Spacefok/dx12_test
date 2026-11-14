// Inline DXR visibility for one directional light with a finite angular radius.
// Raw output: visible fraction in R, estimated penumbra radius in pixels in G.
cbuffer RayTracedShadowCB : register(b0)
{
    float4x4 gInvViewProj;
    float4x4 gView;
    float3 gToLight;
    float gTanLightRadius;
    uint2 gImageSize;
    uint gMode;
    uint gFrameIndex;
    uint gSamples;
    uint gFilterEnabled;
    float gNormalBias;
    float gMaxDistance;
    float gProjectionScale;
    float gMaxFilterPixels;
    float2 gPadding;
};

RaytracingAccelerationStructure gScene : register(t0);
Texture2D<float> gDepth : register(t1);
Texture2D<float4> gNormal : register(t2);
Texture2D<float2> gRaw : register(t3);
RWTexture2D<float2> gRawOutput : register(u0);
RWTexture2D<float2> gFilteredOutput : register(u1);

static const float PI = 3.14159265358979323846f;

float3 WorldPosition(uint2 pixel, float depth)
{
    float2 uv = (float2(pixel) + 0.5f) / float2(gImageSize);
    float4 position = mul(float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f),
        depth, 1.0f), gInvViewProj);
    return position.xyz / position.w;
}

float3 SurfaceNormal(uint2 pixel)
{
    // The G-buffer stores signed world-space normals, not an encoded normal map.
    float3 normal = gNormal.Load(int3(pixel, 0)).xyz;
    return normal * rsqrt(max(dot(normal, normal), 1e-8f));
}

uint Hash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float RandomFloat(uint value)
{
    return float(Hash(value) >> 8) * (1.0f / 16777216.0f);
}

float TraceBlocker(float3 origin, float3 direction)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = max(gNormalBias * 0.25f, 0.0001f);
    ray.TMax = gMaxDistance;
    // Complete traversal keeps the nearest hit, which is needed for the radius
    // estimate. ACCEPT_FIRST_HIT would provide visibility but not that distance.
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
    query.TraceRayInline(gScene, RAY_FLAG_NONE, 0xff, ray);
    while (query.Proceed()) { }
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? query.CommittedRayT() : -1.0f;
}

[numthreads(8, 8, 1)]
void CSTraceShadows(uint3 dispatchThread : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThread.xy;
    if (any(pixel >= gImageSize)) return;
    float depth = gDepth.Load(int3(pixel, 0));
    if (gMode == 0u || depth >= 1.0f)
    {
        gRawOutput[pixel] = float2(1.0f, 0.0f);
        return;
    }

    float3 position = WorldPosition(pixel, depth);
    float3 normal = SurfaceNormal(pixel);
    float3 toLight = normalize(gToLight);
    if (dot(normal, toLight) <= 0.0f)
    {
        // The direct-light BRDF is already zero on a back-facing surface.
        gRawOutput[pixel] = float2(1.0f, 0.0f);
        return;
    }
    float3 origin = position + normal * max(gNormalBias, 0.0f);
    float nearestBlocker = TraceBlocker(origin, toLight);
    if (gMode == 1u || gTanLightRadius <= 1e-6f)
    {
        gRawOutput[pixel] = float2(nearestBlocker < 0.0f ? 1.0f : 0.0f, 0.0f);
        return;
    }

    float3 helper = abs(toLight.y) < 0.99f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tangent = normalize(cross(helper, toLight));
    float3 bitangent = cross(toLight, tangent);
    uint seed = Hash(pixel.x + pixel.y * gImageSize.x) ^ Hash(gFrameIndex + 1u);
    float rotation = RandomFloat(seed) * (2.0f * PI);
    uint samples = clamp(gSamples, 1u, 32u);
    float visible = 0.0f;
    [loop]
    for (uint sampleIndex = 0; sampleIndex < samples; ++sampleIndex)
    {
        // Stratified disk area (sqrt radius), with a rotated golden-angle sequence.
        float radius = sqrt((float(sampleIndex) + RandomFloat(seed + sampleIndex + 1u)) / float(samples));
        float angle = rotation + float(sampleIndex) * 2.39996323f;
        float2 disk = radius * float2(cos(angle), sin(angle));
        float3 direction = normalize(toLight + gTanLightRadius * (tangent * disk.x + bitangent * disk.y));
        float blocker = TraceBlocker(origin, direction);
        visible += blocker < 0.0f ? 1.0f : 0.0f;
        if (blocker >= 0.0f)
            nearestBlocker = nearestBlocker < 0.0f ? blocker : min(nearestBlocker, blocker);
    }
    float viewDepth = max(mul(float4(position, 1.0f), gView).z, 0.01f);
    // A directional disk subtends radius tan(angle) * receiver-to-blocker distance.
    // Project that world-space footprint to choose the denoising support below.
    float penumbra = nearestBlocker >= 0.0f
        ? gTanLightRadius * nearestBlocker * gProjectionScale / viewDepth : 0.0f;
    gRawOutput[pixel] = float2(visible / float(samples), min(penumbra, 512.0f));
}

[numthreads(8, 8, 1)]
void CSFilterShadows(uint3 dispatchThread : SV_DispatchThreadID)
{
    uint2 pixel = dispatchThread.xy;
    if (any(pixel >= gImageSize)) return;
    float2 center = gRaw.Load(int3(pixel, 0));
    float depth = gDepth.Load(int3(pixel, 0));
    if (gMode != 2u || gFilterEnabled == 0u || gTanLightRadius <= 1e-6f || depth >= 1.0f)
    {
        gFilteredOutput[pixel] = center;
        return;
    }
    float3 position = WorldPosition(pixel, depth);
    float3 normal = SurfaceNormal(pixel);
    float viewDepth = max(mul(float4(position, 1.0f), gView).z, 0.01f);
    float pixelWorldSize = viewDepth / max(gProjectionScale, 1.0f);
    float planeTolerance = max(2.0f * gNormalBias, pixelWorldSize * 0.5f);
    float radius = clamp(center.y * 0.35f, 1.0f, max(gMaxFilterPixels, 1.0f));
    float weightedVisibility = 0.0f;
    float totalWeight = 0.0f;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            int2 offset = int2(round(float2(x, y) * radius * 0.5f));
            int2 samplePixel = int2(pixel) + offset;
            if (any(samplePixel < 0) || any(samplePixel >= int2(gImageSize))) continue;
            float sampleDepth = gDepth.Load(int3(samplePixel, 0));
            if (sampleDepth >= 1.0f) continue;
            float3 sampleNormal = SurfaceNormal(uint2(samplePixel));
            float normalAgreement = dot(normal, sampleNormal);
            if (normalAgreement < 0.95f) continue;
            float3 delta = WorldPosition(uint2(samplePixel), sampleDepth) - position;
            // Symmetric plane tests reject depth discontinuities, even when two
            // disconnected parallel surfaces happen to have the same normal.
            float planeDistance = max(abs(dot(delta, normal)), abs(dot(delta, sampleNormal)));
            if (planeDistance > planeTolerance) continue;
            float weight = exp(-0.5f * float(x * x + y * y))
                * exp(-2.0f * planeDistance / planeTolerance);
            weightedVisibility += gRaw.Load(int3(samplePixel, 0)).x * weight;
            totalWeight += weight;
        }
    }
    gFilteredOutput[pixel] = float2(weightedVisibility / max(totalWeight, 1e-6f), center.y);
}
