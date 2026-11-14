cbuffer TemporalCB : register(b0)
{
    float2 gInvResolution;
    float2 gJitterUV;
    float gHistoryWeight;
    uint gHistoryValid;
    uint gReprojectionEnabled;
    uint gClampingEnabled;
    uint gShowVelocity;
    uint gSkyReprojection;
    uint2 gTemporalPadding;
    float4x4 gClipToPreviousSky;
};

Texture2D<float4> gCurrentColor : register(t0);
Texture2D<float4> gHistoryColor : register(t1);
Texture2D<float2> gVelocity : register(t2);
Texture2D<float> gCurrentDepth : register(t3);
SamplerState gPointClamp : register(s0);
SamplerState gLinearClamp : register(s1);

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

// Separable Mitchell-Netravali reconstruction, B=C=1/3.
// It reconstructs an unjittered pixel center from this frame's subpixel samples.
float ReconstructionWeight(float distance)
{
    float x = abs(distance);
    float weight = 0.0f;
    if (x < 1.0f)
        weight = ((7.0f * x - 12.0f) * x * x + 16.0f / 3.0f) / 6.0f;
    else if (x < 2.0f)
        weight = (((-7.0f / 3.0f * x + 12.0f) * x - 20.0f) * x + 32.0f / 3.0f) / 6.0f;
    return weight;
}

float3 MotionColor(float2 velocity)
{
    float2 pixels = velocity / gInvResolution;
    float speed = length(pixels);
    float3 color = 0.0f.xxx;
    if (speed >= 0.001f)
    {
        float2 direction = pixels / speed;
        color = float3(max(direction.x, 0.0f), max(direction.y, 0.0f),
            max(-direction.x, 0.0f) + max(-direction.y, 0.0f));
        color = saturate(color) * saturate(speed * 0.125f);
    }
    return color;
}

float4 PSTemporalAA(FullscreenOut input) : SV_Target
{
    uint width, height;
    gCurrentColor.GetDimensions(width, height);
    int2 lastPixel = int2(width, height) - 1;

    // History is stored on a stable, unjittered grid. Only current-frame fetches
    // include jitter; velocity already excludes both current and previous jitter.
    float2 currentUV = input.UV + gJitterUV;
    float2 samplePosition = currentUV / gInvResolution - 0.5f;
    int2 center = int2(floor(samplePosition + 0.5f));
    int2 centerClamped = clamp(center, int2(0, 0), lastPixel);
    float2 centerVelocity = gVelocity.Load(int3(centerClamped, 0));
    bool skyPixel = gSkyReprojection != 0u
        && gCurrentDepth.Load(int3(centerClamped, 0)) >= 1.0f - 1e-7f;
    if (skyPixel)
    {
        float2 currentNdc = input.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
        float4 previousClip = mul(float4(currentNdc, 1.0f, 1.0f), gClipToPreviousSky);
        float2 previousUV = previousClip.xy / max(previousClip.w, 1e-6f)
            * float2(0.5f, -0.5f) + 0.5f;
        centerVelocity = previousClip.w > 0.0f ? input.UV - previousUV : float2(2.0f, 2.0f);
    }
    if (gShowVelocity != 0u)
        return float4(MotionColor(centerVelocity), 1.0f);

    float3 neighborhoodMin = 65504.0f.xxx;
    float3 neighborhoodMax = 0.0f.xxx;
    float3 reconstructed = 0.0f.xxx;
    float reconstructionSum = 0.0f;
    float closestDepth = 2.0f;
    int2 velocityPixel = centerClamped;

    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            int2 location = center + int2(x, y);
            int2 fetch = clamp(location, int2(0, 0), lastPixel);
            float3 color = max(gCurrentColor.Load(int3(fetch, 0)).rgb, 0.0f.xxx);
            float2 distance = float2(location) - samplePosition;
            float weight = ReconstructionWeight(distance.x) * ReconstructionWeight(distance.y);
            reconstructed += color * weight;
            reconstructionSum += weight;
            neighborhoodMin = min(neighborhoodMin, color);
            neighborhoodMax = max(neighborhoodMax, color);

            // This renderer uses conventional depth (near=0, far=1).
            // Foreground motion dilation protects silhouette pixels.
            float depth = gCurrentDepth.Load(int3(fetch, 0));
            if (depth < closestDepth)
            {
                closestDepth = depth;
                velocityPixel = fetch;
            }
        }
    }

    float3 current = clamp(reconstructed / max(reconstructionSum, 0.00001f),
        neighborhoodMin, neighborhoodMax);
    if (gHistoryValid == 0u)
        return float4(current, 1.0f);

    // A sky center follows a direction at infinity, not a nearby silhouette's motion.
    float2 motion = gReprojectionEnabled != 0u
        ? (skyPixel ? centerVelocity : gVelocity.Load(int3(velocityPixel, 0))) : 0.0f.xx;
    float2 historyUV = input.UV - motion;
    // Reject rather than clamp invalid history coordinates at the image edge.
    if (any(historyUV < 0.0f.xx) || any(historyUV > 1.0f.xx))
        return float4(current, 1.0f);

    float3 history = max(gHistoryColor.SampleLevel(gLinearClamp, historyUV, 0.0f).rgb, 0.0f.xxx);
    if (gClampingEnabled != 0u)
        history = clamp(history, neighborhoodMin, neighborhoodMax);

    return float4(lerp(current, history, saturate(gHistoryWeight)), 1.0f);
}
