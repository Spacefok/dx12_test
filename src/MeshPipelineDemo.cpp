#include "Framework.hpp"
#include <cmath>

using namespace DirectX;

bool Framework::IsMeshPipelineScene() const
{
    return !m_sceneDefinitions.empty() && m_sceneDefinitions[m_currentSceneIndex].EnableMeshPipelineDemo;
}

void Framework::BuildMeshPipelineDemo()
{
    m_meshEnabled = true;
    m_meshColors = m_meshSingle = false;
    m_meshSelection = 0;
    m_meshAngle = 0.0f;
    m_meshScale = 1.0f;
    m_meshObject = {};
    m_animationPaused = true;
    try {
        m_meshPipeline.Load(m_device.Get(), m_commandQueue.Get(), true);
    } catch (const std::exception& error) {
        const std::string reason = error.what();
        m_meshPipeline.Load(m_device.Get(), m_commandQueue.Get(), false);
        const std::string message = "DirectStorage loading failed. The model was loaded using CPU upload.\n\n"
            + reason + "\n\nPress L to retry DirectStorage.";
        MessageBoxA(MainWnd(), message.c_str(), "Model loading", MB_OK | MB_ICONWARNING);
    }
    m_normalizedSceneBounds = { { -3, -3, -1 }, { 3, 3, 1 } };
}

void Framework::UpdateMeshPipelineDemo()
{
    if (!IsMeshPipelineScene()) return;
    const XMMATRIX world = XMMatrixScaling(m_meshScale, m_meshScale, m_meshScale)
        * XMMatrixRotationY(XMConvertToRadians(m_meshAngle));
    XMStoreFloat4x4(&m_meshObject.World, XMMatrixTranspose(world));
    XMStoreFloat4x4(&m_meshObject.WorldInvTranspose, XMMatrixInverse(nullptr, world));
    if (!m_previousFrameValid) m_meshObject.PreviousWorld = m_meshObject.World;
}

bool Framework::HandleMeshPipelineKey(UINT key)
{
    if (!IsMeshPipelineScene() || !m_meshPipeline.Loaded()) return false;
    const UINT last = m_meshPipeline.Model().header.MeshletCount - 1;
    switch (key) {
    case VK_F6: m_meshEnabled = !m_meshEnabled; break;
    case VK_F7: m_meshColors = !m_meshColors; break;
    case VK_F8: m_meshSingle = !m_meshSingle; break;
    case VK_OEM_PLUS: case VK_ADD: m_meshSelection = (std::min)(last, m_meshSelection + 1); break;
    case VK_OEM_MINUS: case VK_SUBTRACT: if (m_meshSelection) --m_meshSelection; break;
    case VK_PRIOR: m_meshSelection = (std::min)(last, m_meshSelection + 64); break;
    case VK_NEXT: m_meshSelection = m_meshSelection > 64 ? m_meshSelection - 64 : 0; break;
    case VK_END: m_meshSelection = last; break;
    case VK_OEM_4: m_meshAngle = std::remainder(m_meshAngle - 15.0f, 360.0f); break;
    case VK_OEM_6: m_meshAngle = std::remainder(m_meshAngle + 15.0f, 360.0f); break;
    case 'O': m_meshScale = m_meshScale > 0.8f ? 0.65f : 1.0f; break;
    case 'L':
        FlushCommandQueue();
        try {
            m_meshPipeline.Load(m_device.Get(), m_commandQueue.Get(), !m_meshPipeline.UsesDirectStorage());
        } catch (const std::exception& error) {
            MessageBoxA(MainWnd(), error.what(), "Model loading", MB_OK | MB_ICONERROR);
        }
        break;
    default: return false;
    }
    ResetTemporalHistory();
    return true;
}

void Framework::DrawMeshPipeline()
{
    if (!IsMeshPipelineScene() || !m_meshPipeline.Loaded()) return;
    m_meshPipeline.Draw(m_commandList.Get(), m_passCB->Resource()->GetGPUVirtualAddress(),
        m_meshObject, m_meshEnabled, m_meshColors, m_meshSingle, m_meshSelection);
    ID3D12DescriptorHeap* heaps[] = { m_cbvHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, heaps);
}
