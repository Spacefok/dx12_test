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
};

cbuffer MaterialCB : register(b2)
{
    float4 gMatDiffuseAlbedo;
    float4 gMatUvTilingOffset;
    uint gMatHasTexture;
    float3 gMatPad;
    float4 gMatWindParams;
};

Texture2D gDiffuseMap : register(t0);
SamplerState gSamLinearWrap : register(s0);

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float4 Color : COLOR;
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float3 PosW : TEXCOORD0;
    float3 NormalW : NORMAL;
    float4 Color : COLOR;
    float2 TexC : TEXCOORD1;
};

struct GBufferOut
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 PositionSpec : SV_Target2;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;

    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    if (gMatWindParams.x > 0.5f)
    {
        const float2 windDir = normalize(float2(0.8f, 0.6f));
        float bendWeight = saturate(vin.TexC.y);

        float phase = gTime * gMatWindParams.w + dot(posW.xyz, float3(3.1f, 1.7f, 2.3f)) * gMatWindParams.z;
        float gust = sin(phase) + 0.5f * sin(phase * 1.91f + 1.2f);
        float flutter = sin(phase * 2.7f + vin.TexC.x * 6.2831853f) * 0.25f;
        float wind = (gust + flutter) * gMatWindParams.y * bendWeight;

        posW.x += windDir.x * wind;
        posW.z += windDir.y * wind;
        posW.y += abs(wind) * 0.2f;
    }

    vout.PosW = posW.xyz;
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorldInvTranspose);
    vout.PosH = mul(posW, gViewProj);
    vout.Color = vin.Color;
    vout.TexC = vin.TexC;
    return vout;
}

GBufferOut PS(VertexOut pin)
{
    GBufferOut gout;

    float2 uv = pin.TexC * gMatUvTilingOffset.xy * gUvTiling + gMatUvTilingOffset.zw + gUvScroll;

    float3 texColor = float3(1.0f, 1.0f, 1.0f);
    if (gMatHasTexture != 0)
    {
        texColor = gDiffuseMap.Sample(gSamLinearWrap, uv).rgb;
    }

    float3 albedo = pin.Color.rgb * gMatDiffuseAlbedo.rgb * texColor;
    float alpha = pin.Color.a * gMatDiffuseAlbedo.a;

    float3 normalW = normalize(pin.NormalW);
    if (dot(normalW, normalW) < 1e-6f)
    {
        normalW = float3(0.0f, 1.0f, 0.0f);
    }

    gout.Albedo = float4(albedo, alpha);
    gout.Normal = float4(normalW, 1.0f);
    gout.PositionSpec = float4(pin.PosW, max(gSpecPower, 1.0f));
    return gout;
}
