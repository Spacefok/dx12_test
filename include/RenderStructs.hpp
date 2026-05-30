#ifndef RENDER_STRUCTS_HPP
#define RENDER_STRUCTS_HPP

#include <DirectXMath.h>
#include <cstdint>
#include <array>

namespace dx {
	inline DirectX::XMFLOAT4X4 Identity4x4() {
		DirectX::XMFLOAT4X4 m;
		DirectX::XMStoreFloat4x4(&m, DirectX::XMMatrixIdentity());
		return m;
	}
}

struct Vertex {
	DirectX::XMFLOAT3 Pos;
	DirectX::XMFLOAT3 Normal;
	DirectX::XMFLOAT4 Color;
	DirectX::XMFLOAT2 TexC = { 0.0f, 0.0f };
	DirectX::XMFLOAT3 Tangent = { 1.0f, 0.0f, 0.0f };
};

constexpr std::uint32_t MaterialTextureBaseColorSlot = 0;
constexpr std::uint32_t MaterialTextureNormalSlot = 1;
constexpr std::uint32_t MaterialTextureDisplacementSlot = 2;
constexpr std::uint32_t MaterialTextureOpacitySlot = 3;
constexpr std::uint32_t MaterialTextureSlotCount = 4;

constexpr std::uint32_t MaterialFlagHasBaseColorTexture = 1u << 0;
constexpr std::uint32_t MaterialFlagHasNormalTexture = 1u << 1;
constexpr std::uint32_t MaterialFlagHasDisplacementTexture = 1u << 2;
constexpr std::uint32_t MaterialFlagHasOpacityTexture = 1u << 3;
constexpr std::uint32_t MaterialFlagDisplacementFromNormal = 1u << 4;
constexpr std::uint32_t MaterialFlagUseTessellation = 1u << 5;
constexpr std::uint32_t MaterialFlagProceduralWater = 1u << 6;

struct alignas(16) ObjectConstants {
	DirectX::XMFLOAT4X4 World = dx::Identity4x4();
	DirectX::XMFLOAT4X4 WorldInvTranspose = dx::Identity4x4();
};

struct alignas(16) PassConstants {
	DirectX::XMFLOAT4X4 ViewProj = dx::Identity4x4();

	DirectX::XMFLOAT3 EyePosW = { .0f, .0f, .0f };
	float _pad0 = .0f;

	DirectX::XMFLOAT3 LightDirW = { .0f, .0f, .0f };
	float _pad1 = .0f;

	DirectX::XMFLOAT4 Ambient = { 0.2f, 0.2f, 0.2f, 1.0f };
	DirectX::XMFLOAT4 Diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
	DirectX::XMFLOAT4 Specular = { 1.0f, 1.0f, 1.0f, 1.0f };

	float SpecPower = 32.0f;
	DirectX::XMFLOAT3 _pad2 = { 0.0f, 0.0f, 0.0f };

	DirectX::XMFLOAT2 UvScroll = { 0.0f, 0.0f };
	DirectX::XMFLOAT2 UvTiling = { 1.0f, 1.0f };

	float Time = 0.0f;
	DirectX::XMFLOAT3 _pad3 = { 0.0f, 0.0f, 0.0f };

	DirectX::XMFLOAT4 TessellationParams = { 0.75f, 3.0f, 1.0f, 6.0f }; // x=min dist, y=max dist, z=min factor, w=max factor
};

struct alignas(16) MaterialConstants {
	DirectX::XMFLOAT4 DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
	DirectX::XMFLOAT4 UvTilingOffset = { 1.0f, 1.0f, 0.0f, 0.0f };
	std::uint32_t Flags = 0;
	float DisplacementScale = 0.0f;
	float DisplacementBias = 0.0f;
	float AlphaCutoff = 0.33f;
	DirectX::XMFLOAT4 WindParams = { 0.0f, 0.0f, 0.0f, 0.0f }; // x = enabled, y = amplitude, z = spatial frequency, w = speed
	DirectX::XMFLOAT4 WaterParams = { 0.0f, 0.0f, 0.0f, 0.0f }; // x = amplitude, y = uv frequency, z = speed, w = secondary wave ratio
};

constexpr std::uint32_t MaxDirectionalLights = 4;
constexpr std::uint32_t MaxPointLights = 32;
constexpr std::uint32_t MaxSpotLights = 16;
constexpr std::uint32_t ShadowCascadeCount = 4;

struct alignas(16) GpuDirectionalLight {
	DirectX::XMFLOAT4 DirectionIntensity = { 0.0f, -1.0f, 0.0f, 0.0f }; // xyz = direction, w = intensity
	DirectX::XMFLOAT4 Color = { 1.0f, 1.0f, 1.0f, 1.0f };
};

struct alignas(16) GpuPointLight {
	DirectX::XMFLOAT4 PositionRange = { 0.0f, 0.0f, 0.0f, 1.0f }; // xyz = position, w = range
	DirectX::XMFLOAT4 ColorIntensity = { 1.0f, 1.0f, 1.0f, 1.0f }; // xyz = color, w = intensity
};

struct alignas(16) GpuSpotLight {
	DirectX::XMFLOAT4 PositionRange = { 0.0f, 0.0f, 0.0f, 1.0f }; // xyz = position, w = range
	DirectX::XMFLOAT4 DirectionCosInner = { 0.0f, -1.0f, 0.0f, 0.9f }; // xyz = direction, w = cos(inner cone)
	DirectX::XMFLOAT4 ColorIntensity = { 1.0f, 1.0f, 1.0f, 1.0f }; // xyz = color, w = intensity
	DirectX::XMFLOAT4 Params = { 0.75f, 0.0f, 0.0f, 0.0f }; // x = cos(outer cone)
};

struct alignas(16) DeferredPassConstants {
	DirectX::XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
	float AmbientIntensity = 0.2f;
	DirectX::XMFLOAT4 AmbientColor = { 1.0f, 1.0f, 1.0f, 1.0f };
	DirectX::XMFLOAT4X4 InvViewProj = dx::Identity4x4();
	DirectX::XMFLOAT4X4 View = dx::Identity4x4();
	std::array<DirectX::XMFLOAT4X4, ShadowCascadeCount> ShadowViewProj = {};
	DirectX::XMFLOAT4 ShadowCascadeSplits = { 0.0f, 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT4 ShadowParams = { 0.0f, 0.0f, 0.0f, 0.0f }; // x=texel size, y=enabled, z=receiver depth bias, w=cascade count
	std::uint32_t DirectionalLightCount = 0;
	std::uint32_t PointLightCount = 0;
	std::uint32_t SpotLightCount = 0;
	std::uint32_t DebugViewEnabled = 0;
};

struct alignas(16) PostProcessConstants {
	DirectX::XMFLOAT2 InvSceneSize = { 1.0f, 1.0f };
	DirectX::XMFLOAT2 InvPostSize = { 1.0f, 1.0f };
	float Exposure = 1.15f;
	float BloomStrength = 0.55f;
	float BloomThreshold = 1.05f;
	float BloomKnee = 0.35f;
	float FocusDistance = 4.0f;
	float FocusRange = 2.0f;
	float MaxDofBlur = 0.85f;
	float Time = 0.0f;
	float VignetteStrength = 0.22f;
	float ChromaticAberration = 1.15f;
	float GrainStrength = 0.010f;
	float Gamma = 2.2f;
	DirectX::XMFLOAT2 CameraNearFar = { 0.1f, 1000.0f };
	float FisheyeStrength = 0.22f;
	float FisheyeZoom = 1.08f;
};

struct alignas(16) GpuParticle {
	DirectX::XMFLOAT4 PositionAge = { 0.0f, 0.0f, 0.0f, 0.0f };       // xyz = position, w = age
	DirectX::XMFLOAT4 VelocityLifetime = { 0.0f, 0.0f, 0.0f, 1.0f };  // xyz = velocity, w = lifetime
	DirectX::XMFLOAT4 ColorAlpha = { 0.62f, 0.78f, 1.0f, 0.28f };
	DirectX::XMFLOAT4 SizeSeed = { 0.004f, 0.075f, 0.0f, 0.0f };      // x = half-width, y = half-length, zw = random seed
};

struct alignas(16) ParticleSimConstants {
	float DeltaTime = 0.0f;
	float TotalTime = 0.0f;
	std::uint32_t MaxParticles = 0;
	std::uint32_t _pad0 = 0;
	DirectX::XMFLOAT4 RainArea = { 5.0f, 4.0f, 5.0f, -1.0f };        // xz = half-extents, y = spawn height, w = floor
	DirectX::XMFLOAT4 RainCenter = { 0.0f, 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT4 Acceleration = { 0.0f, -0.10f, 0.0f, 0.0f };
	DirectX::XMFLOAT4 CollisionBoundsMin = { 0.0f, 0.0f, 0.0f, 0.0f }; // xyz = footprint min/floor, w = enabled
	DirectX::XMFLOAT4 CollisionBoundsMax = { 0.0f, 0.0f, 0.0f, 0.035f }; // xyz = footprint max, w = bounce scale
};

static_assert(sizeof(ObjectConstants) % 16 == 0, "ObjectConstants must be 16-byte aligned sized.");
static_assert(sizeof(PassConstants) % 16 == 0, "PassConstants must be 16-byte aligned sized.");
static_assert(sizeof(MaterialConstants) % 16 == 0, "MaterialConstants must be 16-byte aligned sized.");
static_assert(sizeof(DeferredPassConstants) % 16 == 0, "DeferredPassConstants must be 16-byte aligned sized.");
static_assert(sizeof(PostProcessConstants) % 16 == 0, "PostProcessConstants must be 16-byte aligned sized.");
static_assert(sizeof(GpuParticle) % 16 == 0, "GpuParticle must be 16-byte aligned sized.");
static_assert(sizeof(ParticleSimConstants) % 16 == 0, "ParticleSimConstants must be 16-byte aligned sized.");
#endif // !RENDER_STRUCTS_HPP
