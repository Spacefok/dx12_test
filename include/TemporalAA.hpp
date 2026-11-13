#ifndef TEMPORAL_AA_HPP
#define TEMPORAL_AA_HPP

#include <DirectXMath.h>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

// Keep this layout identical to TemporalCB in TemporalAA.hlsl.
struct alignas(16) TemporalAAConstants {
    DirectX::XMFLOAT2 InvResolution = { 1.0f, 1.0f };
    DirectX::XMFLOAT2 JitterUV = { 0.0f, 0.0f };
    float HistoryWeight = 0.9f;
    std::uint32_t HistoryValid = 0;
    std::uint32_t ReprojectionEnabled = 1;
    std::uint32_t ClampingEnabled = 1;
    std::uint32_t ShowVelocity = 0;
    std::uint32_t Padding[3] = {};
};

static_assert(sizeof(TemporalAAConstants) == 48, "TemporalCB size mismatch.");
static_assert(offsetof(TemporalAAConstants, HistoryWeight) == 16, "TemporalCB blend offset mismatch.");
static_assert(offsetof(TemporalAAConstants, ShowVelocity) == 32, "TemporalCB flags offset mismatch.");

namespace taa {

constexpr std::uint32_t SampleCount = 16;

struct JitterOffset {
    DirectX::XMFLOAT2 Pixel = { 0.0f, 0.0f };
    DirectX::XMFLOAT2 UV = { 0.0f, 0.0f };
};

inline float Halton(std::uint32_t index, std::uint32_t base)
{
    if (base < 2) {
        throw std::invalid_argument("Halton base must be at least two.");
    }
    float result = 0.0f;
    float place = 1.0f;
    while (index != 0) {
        place /= static_cast<float>(base);
        result += static_cast<float>(index % base) * place;
        index /= base;
    }
    return result;
}

// Positive UV shifts the rasterized image right/down. For a row-vector
// perspective projection add (2*UV.x, -2*UV.y) to (_31, _32).
inline JitterOffset Jitter(std::uint64_t frameIndex, std::uint32_t width,
    std::uint32_t height, bool enabled)
{
    if (!enabled) {
        return {};
    }
    if (width == 0 || height == 0) {
        throw std::invalid_argument("Temporal jitter requires a nonzero resolution.");
    }
    const std::uint32_t index = static_cast<std::uint32_t>(frameIndex % SampleCount) + 1;
    JitterOffset result;
    result.Pixel = { Halton(index, 2) - 0.5f, Halton(index, 3) - 0.5f };
    result.UV = { result.Pixel.x / static_cast<float>(width),
        result.Pixel.y / static_cast<float>(height) };
    return result;
}

} // namespace taa

#endif // TEMPORAL_AA_HPP
