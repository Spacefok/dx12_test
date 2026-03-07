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
};

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
};

struct alignas(16) MaterialConstants {
	DirectX::XMFLOAT4 DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
	DirectX::XMFLOAT4 UvTilingOffset = { 1.0f, 1.0f, 0.0f, 0.0f };
	std::uint32_t HasTexture = 0;
	DirectX::XMFLOAT3 _pad = { 0.0f, 0.0f, 0.0f };
	DirectX::XMFLOAT4 WindParams = { 0.0f, 0.0f, 0.0f, 0.0f }; // x = enabled, y = amplitude, z = spatial frequency, w = speed
};

constexpr std::uint32_t MaxDirectionalLights = 4;
constexpr std::uint32_t MaxPointLights = 32;
constexpr std::uint32_t MaxSpotLights = 16;

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
	std::uint32_t DirectionalLightCount = 0;
	std::uint32_t PointLightCount = 0;
	std::uint32_t SpotLightCount = 0;
	std::uint32_t DebugViewEnabled = 0;
};

static_assert(sizeof(ObjectConstants) % 16 == 0, "ObjectConstants must be 16-byte aligned sized.");
static_assert(sizeof(PassConstants) % 16 == 0, "PassConstants must be 16-byte aligned sized.");
static_assert(sizeof(MaterialConstants) % 16 == 0, "MaterialConstants must be 16-byte aligned sized.");
static_assert(sizeof(DeferredPassConstants) % 16 == 0, "DeferredPassConstants must be 16-byte aligned sized.");
#endif // !RENDER_STRUCTS_HPP
