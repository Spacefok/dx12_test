#include "TemporalAA.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

bool Close(float a, float b, float tolerance = 0.000001f)
{
    return std::abs(a - b) < tolerance;
}

DirectX::XMFLOAT2 ScreenUV(DirectX::FXMVECTOR point, DirectX::FXMMATRIX transform)
{
    const auto clip = DirectX::XMVector4Transform(point, transform);
    const auto ndc = DirectX::XMVectorScale(clip, 1.0f / DirectX::XMVectorGetW(clip));
    return { DirectX::XMVectorGetX(ndc) * 0.5f + 0.5f,
        0.5f - DirectX::XMVectorGetY(ndc) * 0.5f };
}

void RunTests()
{
    Require(Close(taa::Halton(1, 2), 0.5f) && Close(taa::Halton(2, 2), 0.25f)
        && Close(taa::Halton(3, 2), 0.75f), "Base-two Halton reference values differ");
    Require(Close(taa::Halton(1, 3), 1.0f / 3.0f) && Close(taa::Halton(3, 3), 1.0f / 9.0f),
        "Base-three Halton reference values differ");

    DirectX::XMFLOAT2 sum = {};
    for (unsigned frame = 0; frame < taa::SampleCount; ++frame) {
        const auto offset = taa::Jitter(frame, 1280, 720, true);
        Require(offset.Pixel.x >= -0.5f && offset.Pixel.x < 0.5f
            && offset.Pixel.y >= -0.5f && offset.Pixel.y < 0.5f, "Jitter exceeds a half pixel");
        Require(Close(offset.UV.x * 1280, offset.Pixel.x) && Close(offset.UV.y * 720, offset.Pixel.y),
            "Jitter is not scaled to the current resolution");
        const auto doubleSize = taa::Jitter(frame, 2560, 1440, true);
        Require(Close(offset.UV.x, doubleSize.UV.x * 2) && Close(offset.UV.y, doubleSize.UV.y * 2),
            "Resolution changes do not preserve pixel-space sample positions");
        const auto repeated = taa::Jitter(frame + taa::SampleCount, 1280, 720, true);
        Require(Close(offset.UV.x, repeated.UV.x) && Close(offset.UV.y, repeated.UV.y),
            "Sample period does not wrap deterministically");
        for (unsigned earlier = 0; earlier < frame; ++earlier) {
            const auto previous = taa::Jitter(earlier, 1280, 720, true);
            Require(!Close(offset.Pixel.x, previous.Pixel.x) || !Close(offset.Pixel.y, previous.Pixel.y),
                "Duplicate sample within one jitter cycle");
        }
        sum.x += offset.Pixel.x;
        sum.y += offset.Pixel.y;
    }
    Require(std::abs(sum.x / taa::SampleCount) < 0.05f && std::abs(sum.y / taa::SampleCount) < 0.05f,
        "Jitter sequence has a large directional bias");
    const auto disabled = taa::Jitter(7, 0, 0, false);
    Require(disabled.Pixel.x == 0 && disabled.Pixel.y == 0 && disabled.UV.x == 0 && disabled.UV.y == 0,
        "Disabling TAA did not completely disable jitter");
    bool rejected = false;
    try { taa::Jitter(0, 0, 720, true); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "Zero-width jitter input was not rejected");
    std::cout << "PASS Halton reference values, centered samples, cycle, resolution scaling and disable\n";

    using namespace DirectX;
    const auto projection = XMMatrixPerspectiveFovLH(XM_PIDIV4, 1280.0f / 720, 0.1f, 100.0f);
    const auto point = XMVectorSet(0.75f, 0.4f, 5.0f, 1.0f);
    const auto stable = ScreenUV(point, projection);
    for (unsigned frame = 0; frame < taa::SampleCount; ++frame) {
        const auto offset = taa::Jitter(frame, 1280, 720, true);
        XMFLOAT4X4 matrix;
        XMStoreFloat4x4(&matrix, projection);
        matrix._31 += 2.0f * offset.UV.x;
        matrix._32 -= 2.0f * offset.UV.y;
        const auto raster = ScreenUV(point, XMLoadFloat4x4(&matrix));
        Require(Close(raster.x - offset.UV.x, stable.x)
            && Close(raster.y - offset.UV.y, stable.y),
            "Jitter sign or UV conversion is incompatible with the projection");
    }

    // Independent projected points establish the velocity convention. An object
    // moving right must fetch history to its left, with no extra jitter delta.
    const auto previous = ScreenUV(point, projection);
    const auto moved = ScreenUV(point, XMMatrixTranslation(0.25f, 0.1f, 0) * projection);
    const XMFLOAT2 velocity = { moved.x - previous.x, moved.y - previous.y };
    Require(velocity.x > 0 && velocity.y < 0, "Motion axes do not match top-left screen UV");
    Require(Close(moved.x - velocity.x, previous.x) && Close(moved.y - velocity.y, previous.y),
        "Reprojection uses the wrong velocity sign");
    const auto cameraMoved = ScreenUV(point, XMMatrixTranslation(-0.25f, 0, 0) * projection);
    Require(cameraMoved.x < previous.x, "Camera translation should move projected content oppositely");
    std::cout << "PASS projection jitter sign, stable reconstruction coordinates and motion convention\n";
}

} // namespace

int main()
{
    try { RunTests(); return 0; }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
