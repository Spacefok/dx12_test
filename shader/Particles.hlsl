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
        particle.PositionAge.xyz += (particle.VelocityLifetime.xyz + shimmer) * dt;
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
    float3 streakDir = SafeNormalize(input[0].VelocityW, float3(0.0f, -1.0f, 0.0f));
    float3 right = SafeNormalize(cross(normalW, streakDir), float3(1.0f, 0.0f, 0.0f));

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
        output.PosH = mul(float4(posW, 1.0f), gViewProj);
        stream.Append(output);
    }

    stream.RestartStrip();
}

float4 PSRainParticle(ParticleVertex input) : SV_Target
{
    float widthFade = 1.0f - abs(input.TexC.x * 2.0f - 1.0f);
    float endFade = smoothstep(0.0f, 0.18f, input.TexC.y) * smoothstep(1.0f, 0.78f, input.TexC.y);
    float alpha = input.Color.a * saturate(widthFade * endFade);
    clip(alpha - 0.01f);

    float3 lightDir = SafeNormalize(-gLightDirW, float3(0.0f, 1.0f, 0.0f));
    float lit = lerp(0.58f, 1.0f, saturate(dot(input.NormalW, lightDir) * 0.5f + 0.5f));
    return float4(input.Color.rgb * lit, alpha);
}
