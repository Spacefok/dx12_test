struct Particle
{
    float4 PositionAge;       // xyz = position, w = age
    float4 VelocityLifetime;  // xyz = velocity, w = lifetime
    float4 ColorAlpha;        // rgba
    float4 SizeSeed;          // x = half-width, y = half-length, zw = seed
};

cbuffer ParticleSimCB : register(b0)
{
    float gDeltaTime;
    float gTotalTime;
    uint gMaxParticles;
    uint gParticlePad0;
    float4 gRainArea;         // xz = half-extents, y = spawn height, w = floor
    float4 gRainCenter;
    float4 gAcceleration;
    float4 gCollisionBoundsMin; // xyz = footprint min/floor, w = enabled
    float4 gCollisionBoundsMax; // xyz = footprint max, w = bounce scale
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

cbuffer ParticleSortCB : register(b2)
{
    uint gSortLevel;
    uint gSortLevelMask;
    uint2 gSortPad;
};

ConsumeStructuredBuffer<Particle> gInputParticles : register(u0);
AppendStructuredBuffer<Particle> gOutputParticles : register(u1);
RWStructuredBuffer<uint2> gSortEntriesOut : register(u2);

StructuredBuffer<Particle> gDrawParticles : register(t0);
StructuredBuffer<uint2> gSortedEntries : register(t1);

struct ParticlePoint
{
    float3 PosW : POSITION;
    float3 VelocityW : TEXCOORD0;
    float4 Color : COLOR0;
    float HalfWidth : TEXCOORD1;
    float HalfLength : TEXCOORD2;
    float AgeRatio : TEXCOORD3;
};

struct ParticleVertex
{
    float4 PosH : SV_POSITION;
    float3 PosW : TEXCOORD0;
    float3 NormalW : TEXCOORD1;
    float4 Color : COLOR0;
    float2 TexC : TEXCOORD2;
    float Droplet : TEXCOORD3;
};

float Hash11(float value)
{
    return frac(sin(value) * 43758.5453123f);
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    float lenSq = dot(value, value);
    return (lenSq > 1e-8f) ? (value * rsqrt(lenSq)) : fallback;
}

void ApplyRainCollision(inout Particle particle, float3 previousPosition, float3 proposedPosition, uint particleId, float dt)
{
    if (gCollisionBoundsMin.w <= 0.5f)
    {
        particle.PositionAge.xyz = proposedPosition;
        return;
    }

    float2 boundsMin = gCollisionBoundsMin.xz + 0.004f;
    float2 boundsMax = gCollisionBoundsMax.xz - 0.004f;
    if (any(boundsMax <= boundsMin))
    {
        particle.PositionAge.xyz = proposedPosition;
        return;
    }

    float floorY = gCollisionBoundsMin.y + 0.004f;
    bool crossingFloor = previousPosition.y >= floorY && proposedPosition.y <= floorY;
    if (!crossingFloor || particle.VelocityLifetime.y >= 0.0f)
    {
        particle.PositionAge.xyz = proposedPosition;
        return;
    }

    float denom = max(previousPosition.y - proposedPosition.y, 1e-5f);
    float hitT = saturate((previousPosition.y - floorY) / denom);
    float3 hitPosition = lerp(previousPosition, proposedPosition, hitT);
    bool insideFootprint =
        hitPosition.x >= boundsMin.x && hitPosition.x <= boundsMax.x &&
        hitPosition.z >= boundsMin.y && hitPosition.z <= boundsMax.y;
    if (!insideFootprint)
    {
        particle.PositionAge.xyz = proposedPosition;
        return;
    }

    float seedBase = dot(particle.SizeSeed.zw, float2(23.71f, 41.19f)) + (float)particleId * 0.017f + gTotalTime;
    float r0 = Hash11(seedBase + 0.31f);
    float r1 = Hash11(seedBase + 1.73f);
    float r2 = Hash11(seedBase + 3.19f);

    float coneAngle = r0 * 6.2831853f;
    float coneRadius = lerp(0.05f, 0.23f, sqrt(r1));
    float3 coneDir = SafeNormalize(
        float3(cos(coneAngle) * coneRadius, 1.0f, sin(coneAngle) * coneRadius),
        float3(0.0f, 1.0f, 0.0f));

    float3 velocity = particle.VelocityLifetime.xyz;
    float speed = max(length(velocity), 0.5f);
    float bounceScale = saturate(gCollisionBoundsMax.w);
    particle.VelocityLifetime.xyz = coneDir * speed * bounceScale * lerp(0.75f, 1.25f, r2);

    float remainingDt = dt * saturate(1.0f - hitT);
    particle.PositionAge.xyz = hitPosition + float3(0.0f, 0.006f, 0.0f) + particle.VelocityLifetime.xyz * remainingDt * 0.45f;

    float splashLifetime = lerp(0.07f, 0.16f, r1);
    particle.VelocityLifetime.w = min(particle.VelocityLifetime.w, particle.PositionAge.w + splashLifetime);
    particle.ColorAlpha.a = min(particle.ColorAlpha.a * 1.12f, 0.30f);

    float dropletRadius = lerp(0.0045f, 0.0070f, r0);
    particle.SizeSeed.x = dropletRadius;
    particle.SizeSeed.y = dropletRadius * lerp(0.85f, 1.18f, r1);
}

Particle RespawnParticle(Particle particle, uint particleId)
{
    float seedBase = dot(particle.SizeSeed.zw, float2(37.719f, 11.135f))
        + (float)particleId * 0.071f
        + gTotalTime * 5.173f;

    float r0 = Hash11(seedBase + 0.11f);
    float r1 = Hash11(seedBase + 1.37f);
    float r2 = Hash11(seedBase + 2.59f);
    float r3 = Hash11(seedBase + 3.83f);
    float r4 = Hash11(seedBase + 5.17f);
    float r5 = Hash11(seedBase + 7.31f);

    float x = gRainCenter.x + (r0 * 2.0f - 1.0f) * gRainArea.x;
    float z = gRainCenter.z + (r1 * 2.0f - 1.0f) * gRainArea.z;
    float y = gRainArea.y + r2 * max(gRainArea.y - gRainArea.w, 0.25f) * 0.16f;

    float fallSpeed = lerp(2.8f, 4.8f, r3);
    float windX = lerp(-0.20f, 0.20f, r4);
    float windZ = lerp(-0.08f, 0.08f, r5);
    float lifetime = max((y - gRainArea.w) / max(fallSpeed, 0.1f), 0.35f);

    particle.PositionAge = float4(x, y, z, 0.0f);
    particle.VelocityLifetime = float4(windX, -fallSpeed, windZ, lifetime);
    particle.ColorAlpha = float4(0.56f + r5 * 0.10f, 0.70f + r4 * 0.12f, 1.0f, lerp(0.18f, 0.34f, r2));
    particle.SizeSeed = float4(lerp(0.0020f, 0.0042f, r0), lerp(0.045f, 0.095f, r1), frac(r4 + 0.173f), frac(r5 + 0.619f));
    return particle;
}

[numthreads(256, 1, 1)]
void CSUpdateParticles(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gMaxParticles)
    {
        return;
    }

    Particle particle = gInputParticles.Consume();

    float dt = min(gDeltaTime, 0.05f);
    particle.PositionAge.w += dt;

    bool expired = particle.PositionAge.w >= particle.VelocityLifetime.w;
    bool fellBelowRain = particle.PositionAge.y < gRainArea.w;
    if (expired || fellBelowRain)
    {
        particle = RespawnParticle(particle, dispatchThreadId.x);
    }
    else
    {
        float phase = dot(particle.SizeSeed.zw, float2(17.7f, 9.3f)) + gTotalTime * 1.6f;
        float3 shimmer = float3(sin(phase), 0.0f, cos(phase * 1.37f)) * 0.010f;
        particle.VelocityLifetime.xyz += gAcceleration.xyz * dt;
        float3 previousPosition = particle.PositionAge.xyz;
        float3 proposedPosition = previousPosition + (particle.VelocityLifetime.xyz + shimmer) * dt;
        ApplyRainCollision(particle, previousPosition, proposedPosition, dispatchThreadId.x, dt);
    }

    gOutputParticles.Append(particle);
}

[numthreads(256, 1, 1)]
void CSInitParticleSort(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint particleId = dispatchThreadId.x;
    if (particleId >= gMaxParticles)
    {
        return;
    }

    Particle particle = gDrawParticles[particleId];
    float3 toEye = gEyePosW - particle.PositionAge.xyz;
    float distSq = dot(toEye, toEye);
    uint depthKey = (uint)min(distSq * 1048576.0f, 4294967294.0f);
    gSortEntriesOut[particleId] = uint2(0xffffffffu - depthKey, particleId);
}

[numthreads(256, 1, 1)]
void CSBitonicSort(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint index = dispatchThreadId.x;
    if (index >= gMaxParticles)
    {
        return;
    }

    uint partner = index ^ gSortLevelMask;
    if (partner <= index || partner >= gMaxParticles)
    {
        return;
    }

    uint2 left = gSortEntriesOut[index];
    uint2 right = gSortEntriesOut[partner];
    bool ascending = (index & gSortLevel) == 0u;
    bool swapEntries = ascending ? (left.x > right.x) : (left.x < right.x);

    if (swapEntries)
    {
        gSortEntriesOut[index] = right;
        gSortEntriesOut[partner] = left;
    }
}

ParticlePoint VSParticle(uint vertexId : SV_VertexID)
{
    uint particleId = gSortedEntries[vertexId].y;
    Particle particle = gDrawParticles[particleId];
    float lifetime = max(particle.VelocityLifetime.w, 1e-4f);
    float ageRatio = saturate(particle.PositionAge.w / lifetime);

    ParticlePoint output;
    output.PosW = particle.PositionAge.xyz;
    output.VelocityW = particle.VelocityLifetime.xyz;
    output.Color = particle.ColorAlpha;
    output.Color.a *= lerp(0.70f, 1.0f, 1.0f - abs(ageRatio * 2.0f - 1.0f));
    output.HalfWidth = particle.SizeSeed.x;
    output.HalfLength = particle.SizeSeed.y;
    output.AgeRatio = ageRatio;
    return output;
}

[maxvertexcount(4)]
void GSBillboard(point ParticlePoint input[1], inout TriangleStream<ParticleVertex> stream)
{
    float3 center = input[0].PosW;
    float3 normalW = SafeNormalize(gEyePosW - center, float3(0.0f, 0.0f, -1.0f));
    float droplet = input[0].HalfLength <= input[0].HalfWidth * 1.35f ? 1.0f : 0.0f;
    float3 streakDir = SafeNormalize(input[0].VelocityW, float3(0.0f, -1.0f, 0.0f));
    float3 cameraRight = SafeNormalize(cross(float3(0.0f, 1.0f, 0.0f), normalW), float3(1.0f, 0.0f, 0.0f));
    float3 cameraUp = SafeNormalize(cross(normalW, cameraRight), float3(0.0f, 1.0f, 0.0f));
    float3 longAxis = SafeNormalize(lerp(streakDir, cameraUp, droplet), cameraUp);
    float3 right = SafeNormalize(cross(normalW, longAxis), cameraRight);

    float2 corners[4] =
    {
        float2(-1.0f, -1.0f),
        float2( 1.0f, -1.0f),
        float2(-1.0f,  1.0f),
        float2( 1.0f,  1.0f)
    };

    float2 texCoords[4] =
    {
        float2(0.0f, 1.0f),
        float2(1.0f, 1.0f),
        float2(0.0f, 0.0f),
        float2(1.0f, 0.0f)
    };

    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        float3 posW = center
            + right * corners[i].x * input[0].HalfWidth
            + streakDir * corners[i].y * input[0].HalfLength;

        ParticleVertex output;
        output.PosW = posW;
        output.NormalW = normalW;
        output.Color = input[0].Color;
        output.TexC = texCoords[i];
        output.Droplet = droplet;
        output.PosH = mul(float4(posW, 1.0f), gViewProj);
        stream.Append(output);
    }

    stream.RestartStrip();
}

float4 PSRainParticle(ParticleVertex input) : SV_Target
{
    float widthFade = 1.0f - abs(input.TexC.x * 2.0f - 1.0f);
    float endFade = smoothstep(0.0f, 0.18f, input.TexC.y) * smoothstep(1.0f, 0.78f, input.TexC.y);
    float streakAlpha = saturate(widthFade * endFade);

    float2 dropletUv = input.TexC * 2.0f - 1.0f;
    float dropletRadius = length(dropletUv);
    float dropletAlpha = smoothstep(1.0f, 0.35f, dropletRadius);
    float alpha = input.Color.a * lerp(streakAlpha, dropletAlpha, input.Droplet);
    clip(alpha - 0.01f);

    float3 lightDir = SafeNormalize(-gLightDirW, float3(0.0f, 1.0f, 0.0f));
    float lit = lerp(0.58f, 1.0f, saturate(dot(input.NormalW, lightDir) * 0.5f + 0.5f));
    float highlight = smoothstep(0.42f, 0.02f, length(dropletUv - float2(-0.28f, -0.32f))) * input.Droplet;
    float3 color = input.Color.rgb * lit + highlight * 0.28f;
    return float4(color, alpha);
}
