#include "Framework.hpp"
#include <cmath>

void Framework::UpdateTerrain(const DirectX::XMMATRIX& viewProj)
{
	if (m_terrain.Empty() || m_terrainFreeze) {
		return;
	}

	const float projectionScale = static_cast<float>(m_clientHeight) / (2.0f * std::tan(CameraFovY * 0.5f));
	// Keep off-screen shadow casters at the same LOD as the camera-selected cut.
	m_terrain.Select(m_camPos, viewProj, projectionScale, m_terrainPixelError, false);
	m_terrainShadowNodes = m_terrain.SelectedNodes();
	if (m_enableFrustumCulling) {
		m_terrain.Select(m_camPos, viewProj, projectionScale, m_terrainPixelError, true);
	}
	m_visibleObjectCount = m_terrain.Statistics().SelectedPatches;
}

void Framework::DrawTerrain()
{
	if (m_terrain.Empty() || !m_modelVB) {
		return;
	}

	static const DirectX::XMFLOAT4 lodPalette[] = {
		{ 0.18f, 0.32f, 1.00f, 1.0f },
		{ 0.12f, 0.80f, 0.95f, 1.0f },
		{ 0.20f, 0.85f, 0.28f, 1.0f },
		{ 1.00f, 0.65f, 0.10f, 1.0f },
		{ 0.95f, 0.18f, 0.12f, 1.0f },
	};
	m_commandList->SetPipelineState(m_terrainWireframe
		? m_renderingSystem.GeometryWireframePSO()
		: m_renderingSystem.GeometryBasicPSO());
	m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_commandList->IASetVertexBuffers(0, 1, &m_modelVBV);
	m_commandList->SetGraphicsRootDescriptorTable(0, ObjectPassGpuHandle(0));
	m_commandList->SetGraphicsRootDescriptorTable(2, CbvSrvGpuHandle(m_textureSrvBaseIndex));

	for (unsigned nodeIndex : m_terrain.SelectedNodes()) {
		const auto& node = m_terrain.Nodes()[nodeIndex];
		MaterialConstants material = {};
		material.AlphaCutoff = 0.0f;
		if (m_terrainLodColors) {
			material.Flags = MaterialFlagLodDebug;
			material.DiffuseAlbedo = lodPalette[node.Depth % _countof(lodPalette)];
		}
		m_commandList->SetGraphicsRoot32BitConstants(
			1, static_cast<UINT>(sizeof(MaterialConstants) / sizeof(UINT32)), &material, 0);
		m_commandList->DrawInstanced(node.VertexCount, 1, node.StartVertex, 0);
	}
}
