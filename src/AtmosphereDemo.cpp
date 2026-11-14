#include "Framework.hpp"
#include <algorithm>

void Framework::BuildAtmosphereDemoObjects()
{
	using namespace DirectX;

	auto AddMaterial = [&](const XMFLOAT4& color) -> UINT
	{
		const UINT index = static_cast<UINT>(m_modelMaterials.size());
		ModelMaterial material;
		material.DiffuseAlbedo = color;
		material.Flags = MaterialFlagSolidColor;
		material.AlphaCutoff = 0.0f;
		material.SrvBaseIndex = index * MaterialTextureSlotCount;
		m_modelMaterials.push_back(material);
		return index;
	};

	const UINT ground = AddMaterial({ 0.20f, 0.23f, 0.20f, 1.0f });
	const UINT dark = AddMaterial({ 0.018f, 0.018f, 0.018f, 1.0f });
	const UINT white = AddMaterial({ 0.80f, 0.80f, 0.80f, 1.0f });
	const UINT frame = AddMaterial({ 0.12f, 0.15f, 0.18f, 1.0f });
	const UINT marker = AddMaterial({ 0.80f, 0.27f, 0.06f, 1.0f });
	const UINT distant = AddMaterial({ 0.19f, 0.22f, 0.26f, 1.0f });

	auto AddBox = [&](const XMFLOAT3& center, const XMFLOAT3& halfSize, UINT material)
	{
		const XMMATRIX world = XMMatrixScaling(halfSize.x, halfSize.y, halfSize.z)
			* XMMatrixTranslation(center.x, center.y, center.z);
		SceneObject object;
		object.Geometry = SceneObjectGeometry::Box;
		object.MaterialIndex = material;
		XMStoreFloat4x4(&object.Constants.World, XMMatrixTranspose(world));
		XMStoreFloat4x4(&object.Constants.WorldInvTranspose, XMMatrixInverse(nullptr, world));
		object.Constants.PreviousWorld = object.Constants.World;
		object.Bounds = {
			{ center.x - halfSize.x, center.y - halfSize.y, center.z - halfSize.z },
			{ center.x + halfSize.x, center.y + halfSize.y, center.z + halfSize.z }
		};
		m_sceneObjects.push_back(object);
		++m_boxObjectCount;
	};

	// World units represent 100 metres in this scene; the camera starts 200 m above ground.
	AddBox({ 0.0f, -0.10f, 90.0f }, { 140.0f, 0.10f, 140.0f }, ground);

	struct TargetRow { float X, Z, Scale; };
	const TargetRow rows[] = {
		{ -5.0f, 5.0f, 1.0f },
		{ 0.0f, 25.0f, 37.0f / 17.0f },
		{ 22.0f, 65.0f, 77.0f / 17.0f }
	};
	for (const auto& row : rows)
	{
		// Identical normals, altitude and apparent size isolate distance-dependent contrast loss.
		const float halfSize = 0.35f * row.Scale;
		for (UINT side = 0; side < 2; ++side)
		{
			const float x = row.X + (side == 0 ? -0.45f : 0.45f) * row.Scale;
			AddBox({ x, 2.0f, row.Z }, { halfSize, halfSize, 0.025f * row.Scale }, side == 0 ? dark : white);
			const float postHeight = 2.0f - halfSize;
			AddBox({ x, postHeight * 0.5f, row.Z + 0.06f * row.Scale },
				{ 0.035f * row.Scale, postHeight * 0.5f, 0.035f * row.Scale }, frame);
		}
		AddBox({ row.X, 2.0f + halfSize + 0.045f * row.Scale, row.Z },
			{ 0.82f * row.Scale, 0.035f * row.Scale, 0.035f * row.Scale }, marker);
	}

	// Distant silhouettes give a second, less controlled reference for aerial perspective.
	for (int index = 0; index < 7; ++index)
	{
		const float x = -110.0f + static_cast<float>(index) * 10.0f;
		const float height = 3.0f + static_cast<float>((index * 3) % 5);
		AddBox({ x, height, 170.0f + static_cast<float>(index % 2) * 12.0f },
			{ 3.0f, height, 3.0f }, distant);
	}
	m_normalizedSceneBounds = { { -140.0f, -0.2f, -50.0f }, { 140.0f, 14.0f, 230.0f } };
	m_visibleObjectCount = static_cast<UINT>(m_sceneObjects.size());
}
