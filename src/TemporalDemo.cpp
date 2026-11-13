#include "Framework.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>

namespace {
using namespace DirectX;

void SetDemoWorld(Framework::SceneObject& object, FXMMATRIX world)
{
	XMStoreFloat4x4(&object.Constants.World, XMMatrixTranspose(world));
	XMStoreFloat4x4(&object.Constants.WorldInvTranspose, XMMatrixInverse(nullptr, world));

	XMFLOAT3 minimum = { FLT_MAX, FLT_MAX, FLT_MAX };
	XMFLOAT3 maximum = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (UINT corner = 0; corner < 8; ++corner)
	{
		const XMVECTOR local = XMVectorSet(
			(corner & 1u) ? 1.0f : -1.0f,
			(corner & 2u) ? 1.0f : -1.0f,
			(corner & 4u) ? 1.0f : -1.0f, 1.0f);
		XMFLOAT3 point;
		XMStoreFloat3(&point, XMVector3TransformCoord(local, world));
		minimum.x = (std::min)(minimum.x, point.x);
		minimum.y = (std::min)(minimum.y, point.y);
		minimum.z = (std::min)(minimum.z, point.z);
		maximum.x = (std::max)(maximum.x, point.x);
		maximum.y = (std::max)(maximum.y, point.y);
		maximum.z = (std::max)(maximum.z, point.z);
	}
	object.Bounds = { minimum, maximum };
}

XMMATRIX MovingBoxWorld(float time)
{
	return XMMatrixScaling(0.72f, 1.0f, 0.72f)
		* XMMatrixRotationY(time * 0.55f)
		* XMMatrixTranslation(3.6f * std::sin(time * 0.7f), 1.05f, 1.4f);
}
}

void Framework::BuildTaaDemoObjects()
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

	const UINT floor = AddMaterial({ 0.24f, 0.27f, 0.31f, 1.0f });
	const UINT dark = AddMaterial({ 0.018f, 0.022f, 0.030f, 1.0f });
	const UINT white = AddMaterial({ 0.92f, 0.94f, 0.98f, 1.0f });
	const UINT red = AddMaterial({ 0.90f, 0.06f, 0.025f, 1.0f });
	const UINT cyan = AddMaterial({ 0.025f, 0.72f, 0.88f, 1.0f });

	auto AddBox = [&](FXMMATRIX world, UINT material) -> UINT
	{
		SceneObject object;
		object.Geometry = SceneObjectGeometry::Box;
		object.MaterialIndex = material;
		SetDemoWorld(object, world);
		object.Constants.PreviousWorld = object.Constants.World;
		const UINT index = static_cast<UINT>(m_sceneObjects.size());
		m_sceneObjects.push_back(object);
		++m_boxObjectCount;
		return index;
	};

	AddBox(XMMatrixScaling(9.0f, 0.15f, 7.0f) * XMMatrixTranslation(0.0f, -0.15f, 3.0f), floor);
	AddBox(XMMatrixScaling(8.0f, 2.6f, 0.12f) * XMMatrixTranslation(0.0f, 2.6f, 6.0f), dark);

	// Thin geometry makes subpixel flicker visible when temporal accumulation is off.
	for (int index = 0; index < 49; ++index)
	{
		const float x = -7.68f + 0.32f * static_cast<float>(index);
		AddBox(XMMatrixScaling(0.025f, 2.5f, 0.025f) * XMMatrixTranslation(x, 2.6f, 5.84f), white);
	}
	for (int index = 0; index < 7; ++index)
	{
		const float x = -5.4f + 1.8f * static_cast<float>(index);
		AddBox(XMMatrixScaling(0.028f, 2.25f, 0.028f)
			* XMMatrixRotationZ(0.55f)
			* XMMatrixTranslation(x, 2.6f, 5.70f), cyan);
	}
	for (int index = 0; index < 21; ++index)
	{
		const float x = -8.0f + 0.8f * static_cast<float>(index);
		AddBox(XMMatrixScaling(0.012f, 0.008f, 5.0f) * XMMatrixTranslation(x, 0.01f, 3.0f), white);
	}

	// The foreground box uncovers the striped wall to expose disocclusion trails.
	m_taaDemoMovingObject = AddBox(MovingBoxWorld(m_animationTime), red);
	m_normalizedSceneBounds = { { -9.0f, -0.3f, -4.0f }, { 9.0f, 5.2f, 10.0f } };
}

void Framework::UpdateTaaDemoObjects()
{
	if (m_taaDemoMovingObject >= m_sceneObjects.size()) {
		return;
	}

	// PreviousWorld belongs to the last rendered frame; Update can run more than once.
	SetDemoWorld(m_sceneObjects[m_taaDemoMovingObject], MovingBoxWorld(m_animationTime));
}
