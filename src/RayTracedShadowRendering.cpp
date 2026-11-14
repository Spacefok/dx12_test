#include "Framework.hpp"
#include <cmath>

void Framework::CreateRayTracedShadowDescriptor()
{
    if (!m_cbvHeap) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R16G16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    // A null SRV is valid on adapters without DXR; the lighting branch never reads it.
    m_device->CreateShaderResourceView(m_rtShadows.Output(true), &srv, CbvSrvCpuHandle(m_rtShadowSrvIndex));
}

void Framework::RunRayTracedShadows()
{
    if (!IsRayTracedShadowScene() || !m_rtShadows.Supported()) return;
    std::vector<DirectX::XMFLOAT4X4> worlds;
    for (const auto& object : m_sceneObjects) {
        if (object.Geometry == SceneObjectGeometry::Box && !m_modelMaterials[object.MaterialIndex].Transparent)
            worlds.push_back(object.Constants.World);
    }
    m_rtShadowSettings.ImageSize = { static_cast<UINT>(m_clientWidth), static_cast<UINT>(m_clientHeight) };
    m_rtShadowSettings.Mode = m_rtShadowMode;
    m_rtShadowSettings.FrameIndex = m_rtShadowFrame++;
    m_rtShadowSettings.Samples = m_rtShadowSamples;
    m_rtShadowSettings.FilterEnabled = m_rtShadowFilter ? 1u : 0u;
    m_rtShadowSettings.TanLightRadius = std::tan(DirectX::XMConvertToRadians(m_rtLightRadius));
    m_rtShadowSettings.ProjectionScale = m_clientHeight / (2.0f * std::tan(CameraFovY * 0.5f));

    // G-buffer state tracking remains pixel-only outside this compute pass.
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    for (UINT index = 0; index < 2; ++index) {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[index].Transition.pResource = index == 0 ? m_depthStencilBuffer.Get() : m_gbuffer.Resource(1);
        barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    m_commandList->ResourceBarrier(2, barriers);
    m_rtShadows.Run(m_commandList.Get(), m_boxVB.Get(), m_boxIB.Get(), m_boxVBView,
        m_boxIBView, m_boxIndexCount, worlds, m_rtShadowSettings);
    for (auto& barrier : barriers)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    m_commandList->ResourceBarrier(2, barriers);
    ID3D12DescriptorHeap* heaps[] = { m_cbvHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, heaps);
}
