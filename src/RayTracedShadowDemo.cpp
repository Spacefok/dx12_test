#include "Framework.hpp"
#include <cmath>

using namespace DirectX;

namespace {
void PlaceBox(Framework::SceneObject& object, const XMFLOAT3& center, const XMFLOAT3& halfSize)
{
    const XMMATRIX world = XMMatrixScaling(halfSize.x, halfSize.y, halfSize.z)
        * XMMatrixTranslation(center.x, center.y, center.z);
    XMStoreFloat4x4(&object.Constants.World, XMMatrixTranspose(world));
    XMStoreFloat4x4(&object.Constants.WorldInvTranspose, XMMatrixInverse(nullptr, world));
    object.Bounds = { { center.x - halfSize.x, center.y - halfSize.y, center.z - halfSize.z },
        { center.x + halfSize.x, center.y + halfSize.y, center.z + halfSize.z } };
}
}

bool Framework::IsRayTracedShadowScene() const
{
    return !m_sceneDefinitions.empty() && m_sceneDefinitions[m_currentSceneIndex].EnableRayTracedShadowDemo;
}

void Framework::BuildRayTracedShadowDemoObjects()
{
    m_rtShadowMode = 2;
    m_rtShadowSamples = 8;
    m_rtShadowFrame = 0;
    m_rtLightRadius = 3.0f;
    m_rtBlockerHeight = 3.0f;
    m_rtShadowFilter = true;
    m_rtShadowMask = false;
    m_rtOffscreenCaster = true;
    m_animationPaused = true;

    auto Material = [&](XMFLOAT4 color) {
        ModelMaterial material;
        material.DiffuseAlbedo = color;
        material.Flags = MaterialFlagSolidColor;
        material.AlphaCutoff = 0.0f;
        const UINT index = static_cast<UINT>(m_modelMaterials.size());
        material.SrvBaseIndex = index * MaterialTextureSlotCount;
        m_modelMaterials.push_back(material);
        return index;
    };
    const UINT floor = Material({ 0.48f, 0.50f, 0.53f, 1 });
    const UINT orange = Material({ 0.65f, 0.19f, 0.045f, 1 });
    const UINT blue = Material({ 0.055f, 0.28f, 0.60f, 1 });
    const UINT white = Material({ 0.72f, 0.72f, 0.68f, 1 });
    auto Box = [&](XMFLOAT3 center, XMFLOAT3 halfSize, UINT material) {
        SceneObject object;
        object.Geometry = SceneObjectGeometry::Box;
        object.MaterialIndex = material;
        PlaceBox(object, center, halfSize);
        object.Constants.PreviousWorld = object.Constants.World;
        const UINT index = static_cast<UINT>(m_sceneObjects.size());
        m_sceneObjects.push_back(object);
        ++m_boxObjectCount;
        return index;
    };
    Box({ 0, -0.1f, 6 }, { 24, 0.1f, 24 }, floor);
    Box({ -2, 1, 1 }, { 0.7f, 1, 0.7f }, orange);
    m_rtRaisedObject = Box({ 2.5f, m_rtBlockerHeight, 2.5f }, { 0.9f, 0.15f, 0.9f }, blue);
    m_rtMovingObject = Box({ 0, 0.8f, 6 }, { 0.6f, 0.8f, 0.6f }, orange);
    // Outside the initial camera frustum; its shadow falls onto the visible floor.
    m_rtOffscreenObject = Box({ -15, 8, 1 }, { 1.4f, 0.4f, 1.4f }, white);
    // A step and a thin upright expose filtering across different surfaces.
    Box({ 5.0f, 0.45f, 6.5f }, { 1.5f, 0.45f, 1.8f }, white);
    Box({ 4.5f, 1.4f, 5.0f }, { 0.10f, 1.4f, 0.8f }, blue);
    m_normalizedSceneBounds = { { -24, -0.2f, -18 }, { 24, 9, 30 } };
    m_visibleObjectCount = static_cast<UINT>(m_sceneObjects.size());
}

void Framework::UpdateRayTracedShadowDemoObjects()
{
    if (!IsRayTracedShadowScene()) return;
    PlaceBox(m_sceneObjects[m_rtMovingObject], { 1.5f * std::sin(m_animationTime), 0.8f, 6 }, { 0.6f, 0.8f, 0.6f });
    PlaceBox(m_sceneObjects[m_rtRaisedObject], { 2.5f, m_rtBlockerHeight, 2.5f }, { 0.9f, 0.15f, 0.9f });
    PlaceBox(m_sceneObjects[m_rtOffscreenObject], { m_rtOffscreenCaster ? -15.0f : -80.0f, 8, 1 }, { 1.4f, 0.4f, 1.4f });
    if (!m_animationPaused) {
        // Receiver motion vectors cannot reproject a shadow cast by a moving object.
        // Keep spatial denoising, but do not accumulate stale shadow lighting.
        m_taaHistoryValid = false;
        m_taaHistoryFrames = 0;
    }
}

bool Framework::HandleRayTracedShadowKey(UINT key)
{
    if (!IsRayTracedShadowScene()) return false;
    switch (key) {
    case VK_F6: m_rtShadowMode = (m_rtShadowMode + 1) % 3; break;
    case VK_F7: m_rtShadowMask = !m_rtShadowMask; break;
    case VK_F8: m_rtShadowFilter = !m_rtShadowFilter; break;
    case VK_OEM_4: m_rtLightRadius = (std::max)(0.0f, m_rtLightRadius - 0.5f); break;
    case VK_OEM_6: m_rtLightRadius = (std::min)(8.0f, m_rtLightRadius + 0.5f); break;
    case 'O': m_rtBlockerHeight = m_rtBlockerHeight > 1.0f ? 0.2f : 3.0f; break;
    case 'B': m_rtOffscreenCaster = !m_rtOffscreenCaster; break;
    case 'N': m_rtShadowSamples = m_rtShadowSamples == 1 ? 4 : (m_rtShadowSamples == 4 ? 8 : (m_rtShadowSamples == 8 ? 16 : 1)); break;
    default: return false;
    }
    ResetTemporalHistory();
    return true;
}
