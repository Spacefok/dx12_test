#include "Framework.hpp"

using namespace DirectX;

namespace
{
    void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }
}

void Framework::ResetTemporalHistory()
{
    m_taaHistoryValid = false;
    m_previousFrameValid = false;
    m_taaHistoryFrames = 0;
    m_taaFrameIndex = 0;
}

void Framework::BuildTemporalResources()
{
    ResetTemporalHistory();
    m_taaReadIndex = 0;
    if (!m_taaRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 2;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_taaRtvHeap)));
    }
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = m_clientWidth;
    desc.Height = m_clientHeight;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = HdrRenderTargetFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = desc.Format;
    clear.Color[3] = 1.0f;
    auto rtv = m_taaRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < 2; ++i) {
        m_taaHistory[i].Reset();
        ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&m_taaHistory[i])));
        m_taaHistory[i]->SetName(i == 0 ? L"TAA history A" : L"TAA history B");
        m_device->CreateRenderTargetView(m_taaHistory[i].Get(), nullptr, rtv);
        rtv.ptr += m_rtvDescriptorSize;
    }
}

void Framework::CreateTemporalDescriptors()
{
    if (!m_cbvHeap || !m_taaHistory[0]) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = HdrRenderTargetFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    // Each immutable table points to one history reader; the other texture is the writer.
    for (UINT i = 0; i < 2; ++i) {
        const UINT base = m_taaSrvBaseIndex + i * 4;
        m_device->CreateShaderResourceView(m_postProcessTextures[PostProcessHdrScene].Get(), &srv, CbvSrvCpuHandle(base));
        m_device->CreateShaderResourceView(m_taaHistory[i].Get(), &srv, CbvSrvCpuHandle(base + 1));
        auto velocitySrv = srv;
        velocitySrv.Format = Gbuffer::kVelocityFormat;
        m_device->CreateShaderResourceView(m_gbuffer.Resource(Gbuffer::kVelocityIndex), &velocitySrv, CbvSrvCpuHandle(base + 2));
        auto depthSrv = srv;
        depthSrv.Format = m_depthStencilSrvFormat;
        m_device->CreateShaderResourceView(m_depthStencilBuffer.Get(), &depthSrv, CbvSrvCpuHandle(base + 3));
    }
}

void Framework::RunTemporalAA()
{
    if (!m_taaEnabled && !m_taaShowVelocity) return;
    TemporalAAConstants constants = {};
    constants.InvResolution = { 1.0f / m_clientWidth, 1.0f / m_clientHeight };
    constants.JitterUV = m_taaJitter.UV;
    constants.HistoryValid = m_taaHistoryValid ? 1u : 0u;
    constants.ReprojectionEnabled = m_taaReprojection ? 1u : 0u;
    constants.ClampingEnabled = m_taaClamping ? 1u : 0u;
    constants.ShowVelocity = m_taaShowVelocity ? 1u : 0u;
    m_taaCB->CopyData(0, constants);

    const UINT writeIndex = 1u - m_taaReadIndex;
    auto* output = m_taaHistory[writeIndex].Get();
    TransitionPostProcessTexture(PostProcessHdrScene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(m_commandList.Get(), output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto rtv = m_taaRtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(writeIndex) * m_rtvDescriptorSize;
    m_commandList->OMSetRenderTargets(1, &rtv, TRUE, nullptr);
    m_commandList->SetGraphicsRootSignature(m_renderingSystem.TemporalRootSignature());
    m_commandList->SetPipelineState(m_renderingSystem.TemporalPSO());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_taaCB->Resource()->GetGPUVirtualAddress());
    m_commandList->SetGraphicsRootDescriptorTable(1, CbvSrvGpuHandle(m_taaSrvBaseIndex + m_taaReadIndex * 4));
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(3, 1, 0, 0);

    // Preserve opaque-only history; existing forward overlays keep using the HDR scene target.
    Transition(m_commandList.Get(), output, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionPostProcessTexture(PostProcessHdrScene, D3D12_RESOURCE_STATE_COPY_DEST);
    m_commandList->CopyResource(m_postProcessTextures[PostProcessHdrScene].Get(), output);
    Transition(m_commandList.Get(), output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    TransitionPostProcessTexture(PostProcessHdrScene, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_taaReadIndex = writeIndex;
    m_taaHistoryValid = m_taaEnabled && !m_taaShowVelocity;
    if (m_taaHistoryValid) ++m_taaHistoryFrames;
}

void Framework::FinishTemporalFrame()
{
    // Previous means the last rendered frame, not the last call to Update.
    m_previousViewProj = m_currentUnjitteredViewProj;
    m_previousCameraPos = m_camPos;
    XMStoreFloat3(&m_previousCameraForward,
        XMVector3Normalize(XMLoadFloat3(&m_camTarget) - XMLoadFloat3(&m_camPos)));
    m_previousAnimationTime = m_animationTime;
    for (auto& object : m_sceneObjects)
        object.Constants.PreviousWorld = object.Constants.World;
    m_previousFrameValid = true;
    if (m_taaEnabled) ++m_taaFrameIndex;
    UpdateWindowTitle();
}
