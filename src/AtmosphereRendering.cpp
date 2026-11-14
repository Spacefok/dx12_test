#include "Framework.hpp"
#include <algorithm>
#include <cmath>

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

bool Framework::HandleAtmosphereKey(UINT key)
{
    switch (key) {
    case 'G':
        m_atmosphereEnabled = !m_atmosphereEnabled;
        BuildSceneLights();
        break;
    case 'H': m_atmosphereSettings.ObjectsEnabled ^= 1u; break;
    case VK_F9:
        m_atmosphereSettings.RayleighScale = 1.0f;
        m_atmosphereSettings.MieScale = 1.0f;
        break;
    case VK_F10:
        m_atmosphereSettings.RayleighScale = 1.0f;
        m_atmosphereSettings.MieScale = 20.0f;
        break;
    case 'U': m_atmosphereSettings.RayleighScale = (std::min)(8.0f, m_atmosphereSettings.RayleighScale + 0.25f); break;
    case 'J': m_atmosphereSettings.RayleighScale = (std::max)(0.0f, m_atmosphereSettings.RayleighScale - 0.25f); break;
    case 'I': m_atmosphereSettings.MieScale = (std::min)(40.0f, m_atmosphereSettings.MieScale + 1.0f); break;
    case 'K': m_atmosphereSettings.MieScale = (std::max)(0.0f, m_atmosphereSettings.MieScale - 1.0f); break;
    default: return false;
    }
    ResetTemporalHistory();
    return true;
}

void Framework::UpdateAtmosphereSun(float dt)
{
    const float azimuthDelta = static_cast<float>(m_keyDown[VK_RIGHT]) - static_cast<float>(m_keyDown[VK_LEFT]);
    const float elevationDelta = static_cast<float>(m_keyDown[VK_UP]) - static_cast<float>(m_keyDown[VK_DOWN]);
    if (m_atmosphereEnabled && (azimuthDelta != 0.0f || elevationDelta != 0.0f)) {
        m_sunAzimuth = std::remainder(m_sunAzimuth + azimuthDelta * dt * 0.5f, XM_2PI);
        m_sunElevation = std::clamp(m_sunElevation + elevationDelta * dt * 0.35f,
            XMConvertToRadians(-5.0f), XMConvertToRadians(85.0f));
        ResetTemporalHistory();
    }
    const XMFLOAT3 toSun = atmosphere::SunDirection(m_sunAzimuth, m_sunElevation);
    m_atmosphereSettings.SunDirection = toSun;
    if (m_atmosphereEnabled && m_directionalLightCount > 0) {
        // Deferred lighting stores the direction light travels, opposite to the sky's TO-sun direction.
        m_directionalLights[0].DirectionIntensity = {
            -toSun.x, -toSun.y, -toSun.z, m_sunElevation > 0.0f ? 1.25f : 0.0f
        };
    }
}

void Framework::BuildAtmosphereResources()
{
    if (!m_atmosphereRtvHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_atmosphereRtvHeap)));
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
    m_atmosphereOutput.Reset();
    ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, IID_PPV_ARGS(&m_atmosphereOutput)));
    m_atmosphereOutput->SetName(L"Atmosphere HDR");
    m_device->CreateRenderTargetView(m_atmosphereOutput.Get(), nullptr,
        m_atmosphereRtvHeap->GetCPUDescriptorHandleForHeapStart());
}

void Framework::CreateAtmosphereDescriptors()
{
    if (!m_cbvHeap || !m_atmosphereOutput) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = HdrRenderTargetFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    m_device->CreateShaderResourceView(m_postProcessTextures[PostProcessHdrScene].Get(), &srv,
        CbvSrvCpuHandle(m_atmosphereSrvBaseIndex));
    srv.Format = m_depthStencilSrvFormat;
    m_device->CreateShaderResourceView(m_depthStencilBuffer.Get(), &srv,
        CbvSrvCpuHandle(m_atmosphereSrvBaseIndex + 1));
}

void Framework::RunAtmosphere()
{
    if (!m_atmosphereEnabled || m_showBufferDebug || m_taaShowVelocity) return;
    m_atmosphereCB->CopyData(0, m_atmosphereSettings);
    TransitionPostProcessTexture(PostProcessHdrScene, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(m_commandList.Get(), m_atmosphereOutput.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto rtv = m_atmosphereRtvHeap->GetCPUDescriptorHandleForHeapStart();
    m_commandList->OMSetRenderTargets(1, &rtv, TRUE, nullptr);
    m_commandList->SetGraphicsRootSignature(m_renderingSystem.AtmosphereRootSignature());
    m_commandList->SetPipelineState(m_renderingSystem.AtmospherePSO());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_atmosphereCB->Resource()->GetGPUVirtualAddress());
    m_commandList->SetGraphicsRootDescriptorTable(1, CbvSrvGpuHandle(m_atmosphereSrvBaseIndex));
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(3, 1, 0, 0);

    // Keep the existing TAA/postprocess input and never read/write the same HDR texture.
    Transition(m_commandList.Get(), m_atmosphereOutput.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionPostProcessTexture(PostProcessHdrScene, D3D12_RESOURCE_STATE_COPY_DEST);
    m_commandList->CopyResource(m_postProcessTextures[PostProcessHdrScene].Get(), m_atmosphereOutput.Get());
    Transition(m_commandList.Get(), m_atmosphereOutput.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    TransitionPostProcessTexture(PostProcessHdrScene, D3D12_RESOURCE_STATE_RENDER_TARGET);
}
