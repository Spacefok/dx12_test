#ifndef ATMOSPHERE_HPP
#define ATMOSPHERE_HPP

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Distances in the scattering equations are kilometres; coefficients are km^-1.
// This layout must match AtmosphereCB in Atmosphere.hlsl.
struct alignas(16) AtmosphereConstants {
    DirectX::XMFLOAT4X4 InvViewProj = {};
    DirectX::XMFLOAT3 EyePosW = {};
    float WorldToKm = 0.1f;
    DirectX::XMFLOAT3 SunDirection = { 0.0f, 1.0f, 0.0f }; // Point toward the sun.
    float SunIntensity = 12.0f;
    DirectX::XMFLOAT3 RayleighScattering = { 0.0058f, 0.0135f, 0.0331f };
    float RayleighScale = 1.0f;
    float MieScattering = 0.003996f;
    float MieExtinction = 0.00444f;
    float MieScale = 1.0f;
    float MieG = 0.8f;
    float PlanetRadiusKm = 6360.0f;
    float AtmosphereRadiusKm = 6460.0f;
    float RayleighHeightKm = 8.0f;
    float MieHeightKm = 1.2f;
    std::uint32_t ObjectsEnabled = 1;
    std::uint32_t ViewSamples = 24;
    std::uint32_t LightSamples = 8;
    std::uint32_t Padding = 0;
};

static_assert(sizeof(AtmosphereConstants) == 160, "AtmosphereCB size mismatch.");
static_assert(offsetof(AtmosphereConstants, EyePosW) == 64, "AtmosphereCB camera offset mismatch.");
static_assert(offsetof(AtmosphereConstants, SunDirection) == 80, "AtmosphereCB sun offset mismatch.");
static_assert(offsetof(AtmosphereConstants, RayleighScattering) == 96, "AtmosphereCB Rayleigh offset mismatch.");
static_assert(offsetof(AtmosphereConstants, MieScattering) == 112, "AtmosphereCB Mie offset mismatch.");
static_assert(offsetof(AtmosphereConstants, PlanetRadiusKm) == 128, "AtmosphereCB radii offset mismatch.");
static_assert(offsetof(AtmosphereConstants, ObjectsEnabled) == 144, "AtmosphereCB flags offset mismatch.");

namespace atmosphere {

constexpr float Pi = 3.14159265358979323846f;

inline DirectX::XMFLOAT3 SunDirection(float azimuth, float elevation)
{
    const float horizontal = std::cos(elevation);
    return { std::sin(azimuth) * horizontal, std::sin(elevation), std::cos(azimuth) * horizontal };
}

// Reference functions also used by the CPU tests of normalization and energy.
// cosine=dot(camera-to-sample, sample-to-sun), so positive g peaks toward the sun.
inline float RayleighPhase(float cosine)
{
    cosine = std::clamp(cosine, -1.0f, 1.0f);
    return 3.0f * (1.0f + cosine * cosine) / (16.0f * Pi);
}

inline float MiePhase(float cosine, float g)
{
    cosine = std::clamp(cosine, -1.0f, 1.0f);
    g = std::clamp(g, -0.95f, 0.95f);
    const float denominator = 1.0f + g * g - 2.0f * g * cosine;
    return (1.0f - g * g) / (4.0f * Pi * denominator * std::sqrt(denominator));
}

inline float Transmittance(float extinctionPerKm, float opticalLengthKm)
{
    return std::exp(-(std::max)(0.0f, extinctionPerKm) * (std::max)(0.0f, opticalLengthKm));
}

} // namespace atmosphere

#endif // ATMOSPHERE_HPP
