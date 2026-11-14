#ifndef RAY_TRACED_SHADOWS_HPP
#define RAY_TRACED_SHADOWS_HPP

#include "Dx12Common.hpp"
#include "UploadBuffer.hpp"
#include <DirectXMath.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Matches RayTracedShadowCB in RayTracedShadows.hlsl. Matrices are transposed for HLSL.
struct alignas(16) RayTracedShadowConstants {
    DirectX::XMFLOAT4X4 InvViewProj = {};
    DirectX::XMFLOAT4X4 View = {};
    DirectX::XMFLOAT3 ToLight = { 0.0f, 1.0f, 0.0f };
    float TanLightRadius = 0.0f;
    DirectX::XMUINT2 ImageSize = {};
    std::uint32_t Mode = 2; // 0: no shadows, 1: hard, 2: soft.
    std::uint32_t FrameIndex = 0;
    std::uint32_t Samples = 8;
    std::uint32_t FilterEnabled = 1;
    float NormalBias = 0.002f;
    float MaxDistance = 1000.0f;
    float ProjectionScale = 1.0f;
    float MaxFilterPixels = 3.0f;
    float Padding[2] = {};
};

static_assert(sizeof(RayTracedShadowConstants) == 192, "ShadowCB size mismatch.");
static_assert(offsetof(RayTracedShadowConstants, View) == 64, "ShadowCB view offset mismatch.");
static_assert(offsetof(RayTracedShadowConstants, ToLight) == 128
    && offsetof(RayTracedShadowConstants, TanLightRadius) == 140, "ShadowCB light offset mismatch.");
static_assert(offsetof(RayTracedShadowConstants, ImageSize) == 144
    && offsetof(RayTracedShadowConstants, Mode) == 152
    && offsetof(RayTracedShadowConstants, FrameIndex) == 156, "ShadowCB image offset mismatch.");
static_assert(offsetof(RayTracedShadowConstants, Samples) == 160
    && offsetof(RayTracedShadowConstants, NormalBias) == 168
    && offsetof(RayTracedShadowConstants, ProjectionScale) == 176, "ShadowCB filter offset mismatch.");

class RayTracedShadows {
public:
    bool Initialize(ID3D12Device* device);
    bool Supported() const { return m_supported; }

    // Call after waiting for outstanding GPU work, as with the other resize resources.
    void Resize(UINT width, UINT height, ID3D12Resource* depth, DXGI_FORMAT depthSrvFormat,
        ID3D12Resource* normal, DXGI_FORMAT normalFormat);
    void InvalidateGeometry();

    // The laboratory shares one indexed mesh between all supplied opaque instances.
    // Worlds use ObjectConstants::World's transposed convention. Include offscreen casters.
    // Depth/normal must be NON_PIXEL_SHADER_RESOURCE; both outputs finish as PIXEL_SHADER_RESOURCE.
    // This binds a private descriptor heap; the caller restores its heap afterward.
    void Run(ID3D12GraphicsCommandList* commandList, ID3D12Resource* vb, ID3D12Resource* ib,
        const D3D12_VERTEX_BUFFER_VIEW& vertexView, const D3D12_INDEX_BUFFER_VIEW& indexView,
        UINT indexCount, const std::vector<DirectX::XMFLOAT4X4>& worlds,
        const RayTracedShadowConstants& constants);

    ID3D12Resource* Output(bool filtered) const { return filtered ? m_filtered.Get() : m_raw.Get(); }

private:
    void BuildBottomLevel(ID3D12GraphicsCommandList4* commandList, ID3D12Resource* vb,
        ID3D12Resource* ib, const D3D12_VERTEX_BUFFER_VIEW& vertexView,
        const D3D12_INDEX_BUFFER_VIEW& indexView, UINT indexCount);
    void UpdateTopLevel(ID3D12GraphicsCommandList4* commandList,
        const std::vector<DirectX::XMFLOAT4X4>& worlds);
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(UINT index) const;

    ComPtr<ID3D12Device5> m_device;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_tracePso;
    ComPtr<ID3D12PipelineState> m_filterPso;
    ComPtr<ID3D12DescriptorHeap> m_heap;
    ComPtr<ID3D12DescriptorHeap> m_clearHeap;
    std::unique_ptr<UploadBuffer<RayTracedShadowConstants>> m_constants;
    ComPtr<ID3D12Resource> m_raw;
    ComPtr<ID3D12Resource> m_filtered;
    ComPtr<ID3D12Resource> m_blas;
    ComPtr<ID3D12Resource> m_blasScratch;
    ComPtr<ID3D12Resource> m_tlas;
    ComPtr<ID3D12Resource> m_tlasScratch;
    ComPtr<ID3D12Resource> m_instances;
    UINT m_descriptorSize = 0;
    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_instanceCount = 0;
    UINT m_geometryIndexCount = 0;
    D3D12_GPU_VIRTUAL_ADDRESS m_geometryVertexAddress = 0;
    D3D12_GPU_VIRTUAL_ADDRESS m_geometryIndexAddress = 0;
    bool m_supported = false;
    bool m_topLevelBuilt = false;
};

#endif // RAY_TRACED_SHADOWS_HPP
