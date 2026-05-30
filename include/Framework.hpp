#ifndef FRAMEWORK_HPP
#define FRAMEWORK_HPP

#include <array>
#include <string>
#include <memory>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <Windows.h>
#include <windowsx.h>
#include "Window.hpp"
#include "Timer.hpp"
#include "Dx12Common.hpp"
#include "UploadBuffer.hpp"
#include "RenderStructs.hpp"
#include "Gbuffer.hpp"
#include "RenderingSystem.hpp"

class Framework : public IWindowMessageHandler {
public:
	explicit Framework(int width, int height, const wchar_t* title);
	virtual ~Framework();

	bool Init();
	int Run();

	LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

protected:
	virtual void CreateRtvAndDsvDescriptorHeaps();
	virtual void OnResize();
	virtual void Update(const double& dt);
	virtual void Draw();

	virtual void OnMouseDown(HWND hwnd, WPARAM btnState, int x, int y);
	virtual void OnMouseUp(HWND hwnd, WPARAM btnState, int x, int y);
	virtual void OnMouseMove(HWND hwnd, WPARAM btnState, int x, int y);

	HWND MainWnd() const { return m_window ? m_window->GetHWND() : nullptr; }
	int ClientWidth() const { return m_clientWidth; }
	int ClientHeight() const { return m_clientHeight; }

	Timer m_timer;

private:
	int m_initWidth = 0;
	int m_initHeight = 0;
	const wchar_t* m_title = nullptr;

	std::unique_ptr<Window> m_window;

	int m_clientWidth = 0;
	int m_clientHeight = 0;

	bool m_appPaused = false;
	bool m_minimized = false;
	bool m_maximized = false;
	bool m_resizing = false;

	HINSTANCE m_hInstance = nullptr;

	POINT m_lastMousePos = { 0,0 };

	ComPtr<IDXGIFactory4> m_dxgiFactory;
	ComPtr<IDXGIAdapter1> m_dxgiAdapter;
	ComPtr<ID3D12Device> m_device;
	std::wstring m_adapterName;

	ComPtr<ID3D12CommandQueue> m_commandQueue;
	ComPtr<ID3D12CommandAllocator> m_directCmdListAlloc;
	ComPtr<ID3D12GraphicsCommandList> m_commandList;

	ComPtr<ID3D12Fence> m_fence;
	UINT64 m_currentFence = 0;
	HANDLE m_fenceEvent = nullptr;

	static const int SwapChainBufferCount = 2;

	ComPtr<IDXGISwapChain4> m_swapChain;
	int m_currBackBuffer = 0;

	DXGI_FORMAT m_backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	static constexpr float CameraFovY = 0.7853981633974483f;
	static constexpr float CameraNearZ = 0.1f;
	static constexpr float CameraFarZ = 1000.0f;
	static constexpr UINT ShadowMapSize = 2048;
	static constexpr float ShadowCascadeLambda = 0.72f;

	ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	ComPtr<ID3D12DescriptorHeap> m_dsvHeap;

	UINT m_rtvDescriptorSize = 0;
	UINT m_dsvDescriptorSize = 0;
	UINT m_cbvSrvUavDescriptorSize = 0;

	ComPtr<ID3D12Resource> m_swapChainBuffer[SwapChainBufferCount];
	ComPtr<ID3D12Resource> m_depthStencilBuffer;
	ComPtr<ID3D12Resource> m_shadowMap;

	DXGI_FORMAT m_depthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
	DXGI_FORMAT m_depthStencilResourceFormat = DXGI_FORMAT_R24G8_TYPELESS;
	DXGI_FORMAT m_depthStencilSrvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	DXGI_FORMAT m_shadowMapResourceFormat = DXGI_FORMAT_R32_TYPELESS;
	DXGI_FORMAT m_shadowMapDsvFormat = DXGI_FORMAT_D32_FLOAT;
	DXGI_FORMAT m_shadowMapSrvFormat = DXGI_FORMAT_R32_FLOAT;
	D3D12_VIEWPORT m_screenViewport = {};
	D3D12_VIEWPORT m_shadowViewport = {};
	D3D12_RECT m_scissorRect = {};
	D3D12_RECT m_shadowScissorRect = {};

	Gbuffer m_gbuffer;
	RenderingSystem m_renderingSystem;

	std::unique_ptr<UploadBuffer<ObjectConstants>> m_objectCB;
	std::unique_ptr<UploadBuffer<PassConstants>>   m_passCB;
	std::unique_ptr<UploadBuffer<DeferredPassConstants>> m_deferredPassCB;
	std::unique_ptr<UploadBuffer<PassConstants>> m_shadowPassCB;
	std::unique_ptr<UploadBuffer<GpuDirectionalLight>> m_directionalLightSB;
	std::unique_ptr<UploadBuffer<GpuPointLight>> m_pointLightSB;
	std::unique_ptr<UploadBuffer<GpuSpotLight>> m_spotLightSB;
	std::unique_ptr<UploadBuffer<ParticleSimConstants>> m_particleSimCB;

	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_cbvHeap;

	UINT m_textureSrvBaseIndex = 2;
	UINT m_textureSrvCount = 0;
	UINT m_deferredPassCbvIndex = 0;
	UINT m_gbufferSrvBaseIndex = 0;
	UINT m_depthSrvIndex = 0;
	UINT m_directionalLightSrvIndex = 0;
	UINT m_pointLightSrvIndex = 0;
	UINT m_spotLightSrvIndex = 0;
	UINT m_shadowSrvIndex = 0;
	UINT m_particleUavBaseIndex = 0;
	UINT m_particleSrvBaseIndex = 0;
	UINT m_particleSortUavIndex = 0;
	UINT m_particleSortSrvIndex = 0;

	std::array<GpuDirectionalLight, MaxDirectionalLights> m_directionalLights{};
	std::array<GpuPointLight, MaxPointLights> m_pointLights{};
	std::array<GpuSpotLight, MaxSpotLights> m_spotLights{};
	std::array<DirectX::XMFLOAT4X4, ShadowCascadeCount> m_shadowViewProj = {};
	std::array<float, ShadowCascadeCount> m_shadowCascadeSplits = {};
	UINT m_directionalLightCount = 0;
	UINT m_pointLightCount = 0;
	UINT m_spotLightCount = 0;

	static constexpr UINT ParticleBufferCount = 2;
	static constexpr UINT MaxParticles = 4096;
	static constexpr UINT ParticleThreadGroupSize = 256;
	std::array<ComPtr<ID3D12Resource>, ParticleBufferCount> m_particleBuffers;
	std::array<ComPtr<ID3D12Resource>, ParticleBufferCount> m_particleCounters;
	std::array<D3D12_RESOURCE_STATES, ParticleBufferCount> m_particleBufferStates = {
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_COMMON
	};
	std::array<D3D12_RESOURCE_STATES, ParticleBufferCount> m_particleCounterStates = {
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_COMMON
	};
	ComPtr<ID3D12Resource> m_particleCounterResetUpload;
	ComPtr<ID3D12Resource> m_particleCounterInitialUpload;
	ComPtr<ID3D12Resource> m_particleDrawArgs;
	ComPtr<ID3D12Resource> m_particleDrawArgsUpload;
	ComPtr<ID3D12Resource> m_particleSortBuffer;
	ComPtr<ID3D12CommandSignature> m_particleDrawCommandSignature;
	D3D12_RESOURCE_STATES m_particleDrawArgsState = D3D12_RESOURCE_STATE_COMMON;
	D3D12_RESOURCE_STATES m_particleSortBufferState = D3D12_RESOURCE_STATE_COMMON;
	UINT m_particleReadBufferIndex = 0;

	void InitDxgi();
	void PickAdapter();
	void LogAdapters();
	void LogAdapterOutputs(IDXGIAdapter1* adapter);
	void InitD3D12Device();
	void CreateCommandObjects();
	void CreateFence();
	void FlushCommandQueue();
	void CreateSwapChain();
	void BuildShaders();
	void BuildConstantBuffers();
	void BuildCbvHeap();
	void BuildCbvViews();
	void BuildShadowResources();
	void UpdateCascadedShadowMaps(const DirectX::XMMATRIX& view);
	void RenderSceneToShadowMap();
	void BuildRootSignature();
	void BuildPSO();
	void BuildSceneGeometryUpload();
	void BuildObjVB_Upload();
	void BuildSceneLights();
	void BuildParticleSystem();
	void BuildParticleDescriptors();
	void UpdateParticleSimConstants(double dt);
	void SimulateParticles();
	void SortParticlesOnGpu();
	void DrawTransparentParticles();
	void TransitionParticleResource(
		ID3D12Resource* resource,
		D3D12_RESOURCE_STATES& currentState,
		D3D12_RESOURCE_STATES targetState);
	void InitializeSceneDefinitions();
	void LoadScene(size_t sceneIndex, bool resetCamera);
	void ResetCameraForCurrentScene();
	void UpdateWindowTitle() const;
	void BuildSceneObjects();
	void BuildObjectConstantBuffer();
	void BuildOctree();
	void UpdateVisibleObjects(const DirectX::XMMATRIX& viewProj);
	void UpdateDynamicSceneObjects();

	void BuildBoxGeometry();

	ComPtr<ID3D12Resource> m_boxVB;
	ComPtr<ID3D12Resource> m_boxIB;

	ComPtr<ID3D12Resource> m_boxVBUpload;
	ComPtr<ID3D12Resource> m_boxIBUpload;

	D3D12_VERTEX_BUFFER_VIEW m_boxVBView = {};
	D3D12_INDEX_BUFFER_VIEW  m_boxIBView = {};

	UINT m_boxIndexCount = 0;

	struct ModelMaterial {
		DirectX::XMFLOAT4 DiffuseAlbedo = { 1.0f, 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT2 UvTiling = { 1.0f, 1.0f };
		DirectX::XMFLOAT2 UvOffset = { 0.0f, 0.0f };
		DirectX::XMFLOAT4 WindParams = { 0.0f, 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT4 WaterParams = { 0.0f, 0.0f, 0.0f, 0.0f };
		std::array<UINT, MaterialTextureSlotCount> TextureIndices = {
			0u, 1u, 2u, 3u
		};
		std::uint32_t Flags = 0;
		float DisplacementScale = 0.0f;
		float DisplacementBias = 0.0f;
		float AlphaCutoff = 0.33f;
		bool Transparent = false;
		bool Occluder = true;
		UINT SrvBaseIndex = 0;
	};

	struct ModelSubset {
		UINT StartVertex = 0;
		UINT VertexCount = 0;
		UINT MaterialIndex = 0;
	};

public:
	struct Aabb {
		DirectX::XMFLOAT3 Min = { 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT3 Max = { 0.0f, 0.0f, 0.0f };
	};

	enum class SceneObjectGeometry {
		SceneModel,
		Box,
		TreeModel,
		TreeBillboard,
	};

	struct SceneObject {
		SceneObjectGeometry Geometry = SceneObjectGeometry::SceneModel;
		UINT MaterialIndex = 0;
		ObjectConstants Constants = {};
		Aabb Bounds = {};
		bool Occluder = true;
		DirectX::XMFLOAT3 Anchor = { 0.0f, 0.0f, 0.0f };
		DirectX::XMFLOAT2 BillboardScale = { 1.0f, 1.0f };
		float LodMinDistance = 0.0f;
		float LodMaxDistance = (std::numeric_limits<float>::max)();
	};

	struct OctreeNode {
		Aabb Bounds = {};
		std::array<int, 8> Children = { -1, -1, -1, -1, -1, -1, -1, -1 };
		std::vector<UINT> ObjectIndices;
	};

private:

	enum class SceneAssetFormat {
		Obj,
		Fbx,
		Procedural,
	};

	enum class SceneLightingPreset {
		Default,
		Bistro,
		SanMiguel,
	};

	struct SceneDefinition {
		std::wstring Name;
		std::vector<std::filesystem::path> ModelPaths;
		SceneAssetFormat Format = SceneAssetFormat::Obj;
		SceneLightingPreset LightingPreset = SceneLightingPreset::Default;
		DirectX::XMFLOAT3 CameraPos = { 2.0f, 2.0f, -5.0f };
		DirectX::XMFLOAT3 CameraTarget = { 0.0f, 0.0f, 0.0f };
		float CameraMoveSpeed = 3.0f;
		float TessellationMinDistance = 0.75f;
		float TessellationMaxDistance = 3.0f;
		float TessellationMinFactor = 1.0f;
		float TessellationMaxFactor = 6.0f;
		float DefaultDisplacementScale = 0.05f;
		float DefaultDisplacementBias = 0.0f;
		float AlphaCutoff = 0.33f;
		bool EnableWindAnimation = true;
		bool EnableUvScroll = true;
		bool AllowKeywordedBumpAsDisplacement = false;
		DirectX::XMFLOAT2 GlobalUvTiling = { 1.0f, 1.0f };
		DirectX::XMFLOAT4 ForwardAmbient = { 0.2f, 0.2f, 0.2f, 1.0f };
		DirectX::XMFLOAT4 ForwardDiffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
		DirectX::XMFLOAT4 ForwardSpecular = { 1.0f, 1.0f, 1.0f, 1.0f };
		float ForwardSpecPower = 32.0f;
		float DeferredAmbientIntensity = 0.18f;
		DirectX::XMFLOAT4 DeferredAmbientColor = { 1.0f, 1.0f, 1.0f, 1.0f };
		bool EnableWaterPlane = false;
		DirectX::XMFLOAT2 WaterPlaneSize = { 0.55f, 0.32f }; // relative to scene X/Z bounds
		float WaterPlaneHeight = 0.08f; // relative to scene Y bounds from minY
		float WaterPlaneUvScale = 1.0f;
		DirectX::XMFLOAT4 WaterPlaneColor = { 0.20f, 0.57f, 0.86f, 1.0f };
		DirectX::XMFLOAT4 WaterWaveParams = { 0.028f, 5.5f, 1.15f, 0.42f };
		bool EnableScatterField = false;
		DirectX::XMFLOAT3 ScatterFieldHalfExtents = { 10.0f, 4.0f, 10.0f };
		UINT ScatterOccluderCount = 18;
		UINT ScatterBoxCount = 1536;
		DirectX::XMFLOAT2 ScatterBoxScaleRange = { 0.018f, 0.045f };
		UINT ScatterTreeCount = 24;
		DirectX::XMFLOAT2 ScatterTreeScaleRange = { 0.040f, 0.060f };
		float ScatterTreeBillboardDistance = 14.0f;
	};

	Microsoft::WRL::ComPtr<ID3D12Resource> m_modelVB;
	Microsoft::WRL::ComPtr<ID3D12Resource> m_modelVBUpload;
	D3D12_VERTEX_BUFFER_VIEW m_modelVBV{};
	UINT m_modelVertexCount = 0;
	std::vector<ModelSubset> m_modelSubsets;
	Microsoft::WRL::ComPtr<ID3D12Resource> m_treeVB;
	Microsoft::WRL::ComPtr<ID3D12Resource> m_treeVBUpload;
	D3D12_VERTEX_BUFFER_VIEW m_treeVBV{};
	UINT m_treeVertexCount = 0;
	std::vector<ModelSubset> m_treeModelSubsets;
	Microsoft::WRL::ComPtr<ID3D12Resource> m_treeBillboardVB;
	Microsoft::WRL::ComPtr<ID3D12Resource> m_treeBillboardVBUpload;
	D3D12_VERTEX_BUFFER_VIEW m_treeBillboardVBV{};
	UINT m_treeBillboardVertexCount = 0;
	std::vector<ModelSubset> m_treeBillboardSubsets;
	std::vector<ModelMaterial> m_modelMaterials;
	std::vector<UINT> m_scatterMaterialIndices;
	std::vector<SceneObject> m_sceneObjects;
	std::vector<UINT> m_visibleObjectIndices;
	std::vector<UINT> m_visibleOpaqueObjectIndices;
	std::vector<UINT> m_visibleTransparentObjectIndices;
	std::vector<OctreeNode> m_octreeNodes;
	Aabb m_normalizedSceneBounds = {};
	bool m_enableFrustumCulling = true;
	bool m_useOctreeForCulling = false;
	bool m_enableOcclusionCulling = true;
	UINT m_visibleObjectCount = 0;
	UINT m_boxObjectCount = 0;
	UINT m_occlusionCulledObjectCount = 0;
	UINT m_objectPassCbvPairCount = 1;

	std::vector<ComPtr<ID3D12Resource>> m_textureResources;
	std::vector<ComPtr<ID3D12Resource>> m_textureUploadResources;

	DirectX::XMFLOAT3 m_modelCenter = { 0.0f, 0.0f, 0.0f };
	float m_modelScale = 1.0f;
	Aabb m_treeLocalBounds = {};
	float m_treeLocalRadius = 0.0f;
	float m_treeLocalHeight = 0.0f;
	UINT m_treeBarkMaterialIndex = 0;
	UINT m_treeLeafMaterialIndex = 0;
	DirectX::XMFLOAT2 m_uvAnimation = { 0.0f, 0.0f };
	DirectX::XMFLOAT2 m_uvAnimationSpeed = { 0.08f, 0.0f };
	DirectX::XMFLOAT2 m_uvGlobalTiling = { 1.0f, 1.0f };
	std::vector<SceneDefinition> m_sceneDefinitions;
	size_t m_currentSceneIndex = 0;
	std::array<bool, 256> m_keyDown{}; // состояние VK_*
	bool m_showBufferDebug = false;
	float m_tessellationMinDistance = 0.75f;
	float m_tessellationMaxDistance = 3.0f;
	float m_tessellationMinFactor = 1.0f;
	float m_tessellationMaxFactor = 6.0f;

	float m_cameraMoveSpeed = 3.0f;   // units/sec, подстрой под сцену

	DirectX::XMFLOAT3 m_camPos = { 2.0f, 2.0f, -5.0f };
	DirectX::XMFLOAT3 m_camTarget = { 0.0f, 0.0f,  0.0f };
	DirectX::XMFLOAT3 m_camUp = { 0.0f, 1.0f,  0.0f };

	// --- Mouse look state ---
	bool  m_rmbDown = false;

	// углы камеры
	float m_yaw = 0.0f;   // поворот вокруг Y
	float m_pitch = 0.0f;   // наклон вверх/вниз

	// чувствительность мыши
	float m_mouseSensitivity = 0.0025f; // радиан на пиксель (подстрой)

	// дистанция до target (если хочешь "orbital"), для FPS не нужна
	// float m_camDistance = 5.0f;

	bool m_comInitialized = false;


	ID3D12Resource* CurrentBackBuffer() const {
		return m_swapChainBuffer[m_currBackBuffer].Get();
	}

	D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView() const {
		D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += static_cast<SIZE_T>(m_currBackBuffer) * m_rtvDescriptorSize;
		return h;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const {
		return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
	}

	D3D12_CPU_DESCRIPTOR_HANDLE ShadowCascadeDepthStencilView(UINT cascadeIndex) const {
		D3D12_CPU_DESCRIPTOR_HANDLE h = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += static_cast<SIZE_T>(1u + cascadeIndex) * m_dsvDescriptorSize;
		return h;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE CbvSrvCpuHandle(UINT index) const {
		D3D12_CPU_DESCRIPTOR_HANDLE h = m_cbvHeap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += static_cast<SIZE_T>(index) * m_cbvSrvUavDescriptorSize;
		return h;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE CbvSrvGpuHandle(UINT index) const {
		D3D12_GPU_DESCRIPTOR_HANDLE h = m_cbvHeap->GetGPUDescriptorHandleForHeapStart();
		h.ptr += static_cast<SIZE_T>(index) * m_cbvSrvUavDescriptorSize;
		return h;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE ObjectPassGpuHandle(UINT objectIndex) const {
		return CbvSrvGpuHandle(objectIndex * 2);
	}
};

#endif // FRAMEWORK_HPP
