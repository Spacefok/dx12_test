#include "Framework.hpp"
#include <DirectXColors.h>
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <cmath>
#include <string>
#include <wincodec.h>

#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

#include <filesystem>
#include <vector>
#include <algorithm>
#include <cfloat>
#include <fstream>
#include <cwctype>

#if defined(_DEBUG)
#include <d3d12sdklayers.h>
#endif

using namespace DirectX;

#pragma comment(lib, "windowscodecs.lib")

namespace {
struct RgbaImage {
	UINT Width = 1;
	UINT Height = 1;
	std::vector<std::uint8_t> Pixels = { 255, 255, 255, 255 };
};

bool LoadImageFromTga(const std::filesystem::path& imagePath, RgbaImage& outImage) {
#pragma pack(push, 1)
	struct TgaHeader {
		std::uint8_t idLength;
		std::uint8_t colorMapType;
		std::uint8_t imageType;
		std::uint16_t colorMapOrigin;
		std::uint16_t colorMapLength;
		std::uint8_t colorMapDepth;
		std::uint16_t xOrigin;
		std::uint16_t yOrigin;
		std::uint16_t width;
		std::uint16_t height;
		std::uint8_t pixelDepth;
		std::uint8_t imageDescriptor;
	};
#pragma pack(pop)

	std::ifstream file(imagePath, std::ios::binary);
	if (!file) {
		return false;
	}

	TgaHeader hdr = {};
	file.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
	if (!file) {
		return false;
	}

	if (hdr.colorMapType != 0) {
		return false;
	}
	if (hdr.width == 0 || hdr.height == 0) {
		return false;
	}
	if (hdr.pixelDepth != 24 && hdr.pixelDepth != 32) {
		return false;
	}
	if (hdr.imageType != 2 && hdr.imageType != 10) {
		return false;
	}

	if (hdr.idLength > 0) {
		file.seekg(hdr.idLength, std::ios::cur);
		if (!file) {
			return false;
		}
	}

	const UINT width = hdr.width;
	const UINT height = hdr.height;
	const size_t bytesPerPixel = hdr.pixelDepth / 8;
	const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);

	std::vector<std::uint8_t> srcData(pixelCount * bytesPerPixel);

	if (hdr.imageType == 2)
	{
		file.read(reinterpret_cast<char*>(srcData.data()), static_cast<std::streamsize>(srcData.size()));
		if (!file) {
			return false;
		}
	}
	else
	{
		size_t outPixel = 0;
		while (outPixel < pixelCount && file)
		{
			std::uint8_t packetHeader = 0;
			file.read(reinterpret_cast<char*>(&packetHeader), 1);
			if (!file) {
				return false;
			}

			const size_t runLength = static_cast<size_t>(packetHeader & 0x7F) + 1;
			if (runLength == 0 || outPixel + runLength > pixelCount) {
				return false;
			}

			if (packetHeader & 0x80)
			{
				std::array<std::uint8_t, 4> px = {};
				file.read(reinterpret_cast<char*>(px.data()), static_cast<std::streamsize>(bytesPerPixel));
				if (!file) {
					return false;
				}

				for (size_t i = 0; i < runLength; ++i)
				{
					memcpy(srcData.data() + (outPixel + i) * bytesPerPixel, px.data(), bytesPerPixel);
				}
			}
			else
			{
				const size_t bytesToRead = runLength * bytesPerPixel;
				file.read(
					reinterpret_cast<char*>(srcData.data() + outPixel * bytesPerPixel),
					static_cast<std::streamsize>(bytesToRead));
				if (!file) {
					return false;
				}
			}

			outPixel += runLength;
		}

		if (outPixel != pixelCount) {
			return false;
		}
	}

	outImage.Width = width;
	outImage.Height = height;
	outImage.Pixels.resize(pixelCount * 4);

	const bool originTop = (hdr.imageDescriptor & 0x20) != 0;
	const bool originRight = (hdr.imageDescriptor & 0x10) != 0;

	for (UINT y = 0; y < height; ++y)
	{
		const UINT srcY = originTop ? y : (height - 1 - y);
		for (UINT x = 0; x < width; ++x)
		{
			const UINT srcX = originRight ? (width - 1 - x) : x;
			const size_t srcPixel = static_cast<size_t>(srcY) * width + srcX;
			const size_t srcIndex = srcPixel * bytesPerPixel;

			const size_t dstPixel = static_cast<size_t>(y) * width + x;
			const size_t dstIndex = dstPixel * 4;

			outImage.Pixels[dstIndex + 0] = srcData[srcIndex + 2];
			outImage.Pixels[dstIndex + 1] = srcData[srcIndex + 1];
			outImage.Pixels[dstIndex + 2] = srcData[srcIndex + 0];
			outImage.Pixels[dstIndex + 3] = (bytesPerPixel == 4) ? srcData[srcIndex + 3] : 255;
		}
	}

	return true;
}

bool LoadImageFromFileWic(const std::filesystem::path& imagePath, RgbaImage& outImage) {
	ComPtr<IWICImagingFactory> factory;
	HRESULT hr = CoCreateInstance(
		CLSID_WICImagingFactory,
		nullptr,
		CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&factory));
	if (FAILED(hr)) {
		return false;
	}

	ComPtr<IWICBitmapDecoder> decoder;
	hr = factory->CreateDecoderFromFilename(
		imagePath.wstring().c_str(),
		nullptr,
		GENERIC_READ,
		WICDecodeMetadataCacheOnLoad,
		&decoder);
	if (FAILED(hr)) {
		return false;
	}

	ComPtr<IWICBitmapFrameDecode> frame;
	hr = decoder->GetFrame(0, &frame);
	if (FAILED(hr)) {
		return false;
	}

	ComPtr<IWICFormatConverter> converter;
	hr = factory->CreateFormatConverter(&converter);
	if (FAILED(hr)) {
		return false;
	}

	hr = converter->Initialize(
		frame.Get(),
		GUID_WICPixelFormat32bppRGBA,
		WICBitmapDitherTypeNone,
		nullptr,
		0.0f,
		WICBitmapPaletteTypeCustom);
	if (FAILED(hr)) {
		return false;
	}

	UINT width = 0;
	UINT height = 0;
	hr = converter->GetSize(&width, &height);
	if (FAILED(hr) || width == 0 || height == 0) {
		return false;
	}

	const UINT rowPitch = width * 4;
	outImage.Width = width;
	outImage.Height = height;
	outImage.Pixels.resize(static_cast<size_t>(rowPitch) * static_cast<size_t>(height));

	hr = converter->CopyPixels(nullptr, rowPitch, static_cast<UINT>(outImage.Pixels.size()), outImage.Pixels.data());
	if (FAILED(hr)) {
		return false;
	}

	return true;
}

bool LoadImageFromFile(const std::filesystem::path& imagePath, RgbaImage& outImage)
{
	if (LoadImageFromFileWic(imagePath, outImage)) {
		return true;
	}

	std::wstring ext = imagePath.extension().wstring();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });

	if (ext == L".tga") {
		return LoadImageFromTga(imagePath, outImage);
	}

	return false;
}
} // namespace

Framework::Framework(int width, int height, const wchar_t* title)
	: m_initWidth(width)
	, m_initHeight(height)
	, m_title(title ? title : L"")
	, m_clientWidth(width)
	, m_clientHeight(height)
{
}

Framework::~Framework() {
	if (m_device)
		FlushCommandQueue();

	if (m_fenceEvent) {
		CloseHandle(m_fenceEvent);
		m_fenceEvent = nullptr;
	}

	if (m_comInitialized) {
		CoUninitialize();
		m_comInitialized = false;
	}
}

bool Framework::Init() {
	const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (SUCCEEDED(coHr)) {
		m_comInitialized = true;
	}
	else if (coHr != RPC_E_CHANGED_MODE) {
		ThrowIfFailed(coHr);
	}

	m_window = std::make_unique<Window>(m_initWidth, m_initHeight, m_title, this);

	InitDxgi();
	InitD3D12Device();
	m_cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	CreateCommandObjects();
	CreateFence();
	CreateSwapChain();
	CreateRtvAndDsvDescriptorHeaps();
	BuildShaders();
	BuildConstantBuffers();
	BuildBoxGeometry();
	BuildObjVB_Upload();
	BuildCbvHeap();
	BuildCbvViews();
	BuildRootSignature();
	BuildPSO();

	OnResize();

	return MainWnd() != nullptr;
}

int Framework::Run() {
	m_timer.Reset();

	while (m_window->ProcessMessages()) {
		m_timer.Tick();

		if (!m_appPaused) {
			const double dt = m_timer.DeltaTime();
			Update(dt);
			Draw();
		}
		else {
			Sleep(100);
		}
	}

	return 0;
}

LRESULT Framework::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	switch (msg) {
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;

	case WM_SIZE:
		m_clientWidth = LOWORD(lParam);
		m_clientHeight = HIWORD(lParam);

		if (wParam == SIZE_MINIMIZED) {
			m_appPaused = true;
			m_minimized = true;
			m_maximized = false;
			m_timer.Stop();
		}
		else if (wParam == SIZE_MAXIMIZED) {
			m_appPaused = false;
			m_minimized = false;
			m_maximized = true;
			m_timer.Start();
			OnResize();
		}
		else if (wParam == SIZE_RESTORED) {
			if (m_minimized) {
				m_appPaused = false;
				m_minimized = false;
				m_timer.Start();
				OnResize();
			}
			else if (m_maximized) {
				m_appPaused = false;
				m_maximized = false;
				m_timer.Start();
				OnResize();
			}
			else if (m_resizing) {

			}
			else {
				OnResize();
			}
		}

		return 0;

	case WM_ACTIVATEAPP:
		if (wParam == FALSE)
		{
			m_appPaused = true;
			m_timer.Stop();
		}
		else
		{
			m_appPaused = false;
			m_timer.Start();
		}
		return 0;

	case WM_ENTERSIZEMOVE:
		m_appPaused = true;
		m_resizing = true;
		m_timer.Stop();
		return 0;

	case WM_EXITSIZEMOVE:
		m_appPaused = false;
		m_resizing = false;
		m_timer.Start();
		OnResize();
		return 0;

	case WM_LBUTTONDOWN:
	case WM_MBUTTONDOWN:
	case WM_RBUTTONDOWN:
		OnMouseDown(hwnd, wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;

	case WM_LBUTTONUP:
	case WM_MBUTTONUP:
	case WM_RBUTTONUP:
		OnMouseUp(hwnd, wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;

	case WM_MOUSEMOVE:
		OnMouseMove(hwnd, wParam, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
		return 0;

	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
	{
		const uint8_t vk = static_cast<uint8_t>(wParam);
		m_keyDown[vk] = true;
		return 0;
	}

	case WM_KEYUP:
	case WM_SYSKEYUP:
	{
		const uint8_t vk = static_cast<uint8_t>(wParam);
		m_keyDown[vk] = false;
		return 0;
	}

	// чтобы при потере фокуса не было "залипших" клавиш
	case WM_KILLFOCUS:
	{
		m_keyDown.fill(false);
		return 0;
	}

	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void Framework::CreateRtvAndDsvDescriptorHeaps()
{
	m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	m_dsvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	m_cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));

	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
	dsvHeapDesc.NumDescriptors = 1;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&m_dsvHeap)));
}

void Framework::OnResize()
{
	if (!m_device || !m_swapChain || !m_commandQueue || !m_directCmdListAlloc || !m_commandList)
		return;

	FlushCommandQueue();

	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	for (UINT i = 0; i < SwapChainBufferCount; ++i) {
		m_swapChainBuffer[i].Reset();
	}

	m_depthStencilBuffer.Reset();

	ThrowIfFailed(m_swapChain->ResizeBuffers(SwapChainBufferCount, m_clientWidth, m_clientHeight, m_backBufferFormat, 0));

	m_currBackBuffer = static_cast<int>(m_swapChain->GetCurrentBackBufferIndex());

	D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

	for (UINT i = 0; i < SwapChainBufferCount; ++i) {
		ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_swapChainBuffer[i])));
		m_device->CreateRenderTargetView(m_swapChainBuffer[i].Get(), nullptr, rtvHandle);
		rtvHandle.ptr += m_rtvDescriptorSize;
	}

	D3D12_RESOURCE_DESC depthDesc = {};
	depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthDesc.Alignment = 0;
	depthDesc.Width = static_cast<UINT64>(m_clientWidth);
	depthDesc.Height = static_cast<UINT64>(m_clientHeight);
	depthDesc.DepthOrArraySize = 1;
	depthDesc.MipLevels = 1;
	depthDesc.Format = m_depthStencilFormat;
	depthDesc.SampleDesc.Count = 1;
	depthDesc.SampleDesc.Quality = 0;
	depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_CLEAR_VALUE optClear = {};
	optClear.Format = m_depthStencilFormat;
	optClear.DepthStencil.Depth = 1.0f;
	optClear.DepthStencil.Stencil = 0;

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
	heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	heapProps.CreationNodeMask = 1;
	heapProps.VisibleNodeMask = 1;

	ThrowIfFailed(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc, D3D12_RESOURCE_STATE_COMMON, &optClear, IID_PPV_ARGS(&m_depthStencilBuffer)));

	D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
	dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsvDesc.Format = m_depthStencilFormat;
	dsvDesc.Texture2D.MipSlice = 0;
	m_device->CreateDepthStencilView(m_depthStencilBuffer.Get(), &dsvDesc, DepthStencilView());

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = m_depthStencilBuffer.Get();
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &barrier);

	ThrowIfFailed(m_commandList->Close());
	ID3D12CommandList* cmdsLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(1, cmdsLists);

	FlushCommandQueue();

	m_screenViewport.TopLeftX = 0.0f;
	m_screenViewport.TopLeftY = 0.0f;
	m_screenViewport.Width = static_cast<float>(m_clientWidth);
	m_screenViewport.Height = static_cast<float>(m_clientHeight);
	m_screenViewport.MinDepth = 0.0f;
	m_screenViewport.MaxDepth = 1.0f;

	m_scissorRect = { 0, 0, m_clientWidth, m_clientHeight };
}

void Framework::Update(const double& dt)
{
	m_uvAnimation.x += m_uvAnimationSpeed.x * static_cast<float>(dt);
	m_uvAnimation.y += m_uvAnimationSpeed.y * static_cast<float>(dt);

	m_uvAnimation.x -= std::floor(m_uvAnimation.x);
	m_uvAnimation.y -= std::floor(m_uvAnimation.y);

	ObjectConstants obj = {};
	XMMATRIX world =
		XMMatrixTranslation(-m_modelCenter.x, -m_modelCenter.y, -m_modelCenter.z) *
		XMMatrixScaling(m_modelScale, m_modelScale, m_modelScale);
	XMMATRIX worldInvTranspose = XMMatrixTranspose(XMMatrixInverse(nullptr, world));

	XMStoreFloat4x4(&obj.World, XMMatrixTranspose(world));
	XMStoreFloat4x4(&obj.WorldInvTranspose, worldInvTranspose);

	m_objectCB->CopyData(0, obj);

	XMVECTOR pos = XMLoadFloat3(&m_camPos);
	XMVECTOR target = XMLoadFloat3(&m_camTarget);
	XMVECTOR up = XMVector3Normalize(XMLoadFloat3(&m_camUp));

	XMVECTOR forward = XMVector3Normalize(target - pos);
	XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, forward));

	float speed = m_cameraMoveSpeed;
	if (m_keyDown[VK_SHIFT]) speed *= 3.0f; 

	float step = speed * static_cast<float>(dt);
	XMVECTOR move = XMVectorZero();

	if (m_keyDown['W']) move += forward;
	if (m_keyDown['S']) move -= forward;
	if (m_keyDown['D']) move += right;
	if (m_keyDown['A']) move -= right;

	if (m_keyDown[VK_SPACE])   move += up;
	if (m_keyDown[VK_CONTROL]) move -= up;

	if (!XMVector3Equal(move, XMVectorZero()))
		move = XMVector3Normalize(move) * step;

	pos += move;
	target += move;

	XMStoreFloat3(&m_camPos, pos);
	XMStoreFloat3(&m_camTarget, target);

	XMMATRIX view = XMMatrixLookAtLH(pos, target, up);

	float aspect = (float)m_clientWidth / (float)m_clientHeight;
	XMMATRIX proj = XMMatrixPerspectiveFovLH(0.25f * XM_PI, aspect, 0.1f, 1000.0f);

	XMMATRIX viewProj = view * proj;

	PassConstants pass{};
	XMStoreFloat4x4(&pass.ViewProj, XMMatrixTranspose(viewProj));

	XMStoreFloat3(&pass.EyePosW, pos);

	pass.LightDirW = { 0.577f, -0.3f, 0.577f };
	pass.Ambient = { 0.2f, 0.2f, 0.2f, 1.0f };
	pass.Diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
	pass.Specular = { 1.0f, 1.0f, 1.0f, 1.0f };
	pass.SpecPower = 32.0f;
	pass.UvScroll = m_uvAnimation;
	pass.UvTiling = m_uvGlobalTiling;

	m_passCB->CopyData(0, pass);
}

void Framework::Draw()
{
	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), m_pso.Get()));

	D3D12_RESOURCE_BARRIER toRT{};
	toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toRT.Transition.pResource = CurrentBackBuffer();
	toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &toRT);

	m_commandList->RSSetViewports(1, &m_screenViewport);
	m_commandList->RSSetScissorRects(1, &m_scissorRect);

	m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());

	ID3D12DescriptorHeap* heaps[] = { m_cbvHeap.Get() };
	m_commandList->SetDescriptorHeaps(_countof(heaps), heaps);

	m_commandList->SetGraphicsRootDescriptorTable(
		0,
		m_cbvHeap->GetGPUDescriptorHandleForHeapStart()
	);

	auto BindMaterial = [&](const Framework::ModelMaterial& srcMaterial) {
		MaterialConstants mat = {};
		mat.DiffuseAlbedo = srcMaterial.DiffuseAlbedo;
		mat.UvTilingOffset = {
			srcMaterial.UvTiling.x,
			srcMaterial.UvTiling.y,
			srcMaterial.UvOffset.x,
			srcMaterial.UvOffset.y
		};
		mat.HasTexture = srcMaterial.HasTexture ? 1u : 0u;

		m_commandList->SetGraphicsRoot32BitConstants(
			1,
			static_cast<UINT>(sizeof(MaterialConstants) / sizeof(UINT32)),
			&mat,
			0);

		D3D12_GPU_DESCRIPTOR_HANDLE textureHandle = m_cbvHeap->GetGPUDescriptorHandleForHeapStart();
		const UINT textureIndex = srcMaterial.TextureIndex;
		textureHandle.ptr += static_cast<SIZE_T>(2 + textureIndex) * m_cbvSrvUavDescriptorSize;
		m_commandList->SetGraphicsRootDescriptorTable(2, textureHandle);
	};

	D3D12_CPU_DESCRIPTOR_HANDLE rtv = CurrentBackBufferView();
	D3D12_CPU_DESCRIPTOR_HANDLE dsv = DepthStencilView();
	m_commandList->OMSetRenderTargets(1, &rtv, TRUE, &dsv);

	m_commandList->ClearRenderTargetView(rtv, DirectX::Colors::CornflowerBlue, 0, nullptr);
	m_commandList->ClearDepthStencilView(
		dsv,
		D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
		1.0f,
		0,
		0,
		nullptr
	);

	m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	if (m_modelVB && !m_modelSubsets.empty())
	{
		m_commandList->IASetVertexBuffers(0, 1, &m_modelVBV);

		for (const ModelSubset& subset : m_modelSubsets)
		{
			const UINT materialIndex = (subset.MaterialIndex < m_modelMaterials.size()) ? subset.MaterialIndex : 0;
			const ModelMaterial& material = m_modelMaterials[materialIndex];

			BindMaterial(material);
			m_commandList->DrawInstanced(subset.VertexCount, 1, subset.StartVertex, 0);
		}
	}
	else
	{
		BindMaterial(ModelMaterial{});
		m_commandList->IASetVertexBuffers(0, 1, &m_boxVBView);
		m_commandList->IASetIndexBuffer(&m_boxIBView);
		m_commandList->DrawIndexedInstanced(m_boxIndexCount, 1, 0, 0, 0);
	}

	D3D12_RESOURCE_BARRIER toPresent{};
	toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	toPresent.Transition.pResource = CurrentBackBuffer();
	toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &toPresent);

	ThrowIfFailed(m_commandList->Close());

	ID3D12CommandList* cmdsLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	ThrowIfFailed(m_swapChain->Present(0, 0));
	m_currBackBuffer = (m_currBackBuffer + 1) % SwapChainBufferCount;

	FlushCommandQueue();
}


void Framework::InitDxgi() {
	UINT factoryFlags = 0;

#if defined(_DEBUG)
	factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

	ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_dxgiFactory)));

#if defined(_DEBUG)
	LogAdapters();
#endif

	PickAdapter();
}

void Framework::PickAdapter() {
	m_dxgiAdapter.Reset();
	m_adapterName.clear();

	ComPtr<IDXGIFactory6> factory6;

	if (SUCCEEDED(m_dxgiFactory.As(&factory6))) {
		for (UINT i = 0;; ++i) {
			ComPtr<IDXGIAdapter1> adapter;

			if (factory6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
				break;

			DXGI_ADAPTER_DESC1 desc = {};
			ThrowIfFailed(adapter->GetDesc1(&desc));

			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
				continue;

			ComPtr<ID3D12Device> testDevice;

			if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&testDevice)))) {
				m_dxgiAdapter = adapter;
				m_adapterName = desc.Description;
				break;
			}
		}
	}

	if (!m_dxgiAdapter) {
		for (UINT i = 0;; ++i) {
			ComPtr<IDXGIAdapter1> adapter;

			if (m_dxgiFactory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
				break;

			DXGI_ADAPTER_DESC1 desc = {};
			ThrowIfFailed(adapter->GetDesc1(&desc));

			if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
				continue;

			ComPtr<ID3D12Device> testDevice;

			if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&testDevice)))) {
				m_dxgiAdapter = adapter;
				m_adapterName = desc.Description;
				break;
			}
		}
	}

	if (!m_dxgiAdapter) {
		throw std::runtime_error("No suitable DXGI adapter found (D3D12-capable).");
	}

#if defined(_DEBUG)
	std::wstring msg = L"[DXGI] Using adapter: " + m_adapterName + L"\n";
	OutputDebugStringW(msg.c_str());
#endif
}

void Framework::LogAdapters() {
#if defined(_DEBUG)
	OutputDebugStringW(L"[DXGI] Adapters:\n");

	for (UINT i = 0;; ++i) {
		ComPtr<IDXGIAdapter1> adapter;
		
		HRESULT hr = m_dxgiFactory->EnumAdapters1(i, &adapter);
		if (hr == DXGI_ERROR_NOT_FOUND) break;
		ThrowIfFailed(hr);

		if (m_dxgiFactory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
			break;

		DXGI_ADAPTER_DESC1 desc = {};
		ThrowIfFailed(adapter->GetDesc1(&desc));

		std::wstring line = L"  -  ";
		line += desc.Description;
		line += (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? L" (SOPTWARE)\n" : L"\n";
		OutputDebugStringW(line.c_str());

		LogAdapterOutputs(adapter.Get());
	}
#endif
}

void Framework::LogAdapterOutputs(IDXGIAdapter1* adapter) {
#if defined(_DEBUG)
	for (UINT j = 0;; ++j) {
		ComPtr<IDXGIOutput> output;

		if (adapter->EnumOutputs(j, &output) == DXGI_ERROR_NOT_FOUND)
			break;

		DXGI_OUTPUT_DESC outDesc = {};
		ThrowIfFailed(output->GetDesc(&outDesc));

		std::wstring line = L"		Ouput: ";
		line += outDesc.DeviceName;
		line += L"\n";
		OutputDebugStringW(line.c_str());
	}
#endif
}

void Framework::InitD3D12Device() {
#if defined(_DEBUG)
	ComPtr<ID3D12Debug> debugController;

	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
		debugController->EnableDebugLayer();
		OutputDebugStringW(L"[D3d12] Debug layer enabled\n");
	}
	else {
		OutputDebugStringW(L"[D3D12] Debug layer NOT available (Graphics Tools may be missing)\n");
	}
#endif

	HRESULT hr = D3D12CreateDevice(m_dxgiAdapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));

	if (FAILED(hr)) {
		OutputDebugStringW(L"[D3D12] Hardware device failed, falling back to WARP\n");

		ThrowIfFailed(m_dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&m_dxgiAdapter)));
		ThrowIfFailed(D3D12CreateDevice(m_dxgiAdapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)));
	}

#if defined(_DEBUG)
	OutputDebugStringW(L"[D3D12] Device created \n");

	ComPtr<ID3D12InfoQueue> infoQueue;
	if (SUCCEEDED(m_device.As(&infoQueue))) {
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
		infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, TRUE);
	}
#endif
}

void Framework::CreateCommandObjects() {
	D3D12_COMMAND_QUEUE_DESC qdesc = {};
	qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

	ThrowIfFailed(m_device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&m_commandQueue)));

	ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_directCmdListAlloc)));

	ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_directCmdListAlloc.Get(), nullptr, IID_PPV_ARGS(&m_commandList)));

	ThrowIfFailed(m_commandList->Close());

#if defined(_DEBUG)
	OutputDebugStringW(L"[D3D12] Command queue/allocator/list created\n");
#endif
}

void Framework::CreateFence() {
	ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));

	m_currentFence = 0;

	m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

	if (m_fenceEvent == nullptr)
		throw std::runtime_error("CreateEvent failed for fence event.");
}

void Framework::FlushCommandQueue() {
	if (!m_commandQueue || !m_fence || !m_fenceEvent)
		return;

	++m_currentFence;
	ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_currentFence));

	if (m_fence->GetCompletedValue() < m_currentFence) {
		ThrowIfFailed(m_fence->SetEventOnCompletion(m_currentFence, m_fenceEvent));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}
}

void Framework::CreateSwapChain() {
	m_swapChain.Reset();

	DXGI_SWAP_CHAIN_DESC1 sd = {};
	sd.Width = m_clientWidth;
	sd.Height = m_clientHeight;
	sd.Format = m_backBufferFormat;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = SwapChainBufferCount;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Scaling = DXGI_SCALING_STRETCH;
	sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	sd.Flags = 0;

	ComPtr<IDXGISwapChain1> swapChain1;
	ThrowIfFailed(m_dxgiFactory->CreateSwapChainForHwnd(m_commandQueue.Get(), MainWnd(), &sd, nullptr, nullptr, &swapChain1));
	ThrowIfFailed(m_dxgiFactory->MakeWindowAssociation(MainWnd(), DXGI_MWA_NO_ALT_ENTER));

	ThrowIfFailed(swapChain1.As(&m_swapChain));
	m_currBackBuffer = static_cast<int>(m_swapChain->GetCurrentBackBufferIndex());
}

void Framework::BuildShaders()
{
	const std::wstring shaderFile = L"shader\\Phong.hlsl";

	m_vsByteCode = CompileShader(shaderFile, nullptr, "VS", "vs_5_1");
	m_psByteCode = CompileShader(shaderFile, nullptr, "PS", "ps_5_1");
}

void Framework::BuildConstantBuffers()
{
	m_objectCB = std::make_unique<UploadBuffer<ObjectConstants>>(m_device.Get(), 1, true);
	m_passCB = std::make_unique<UploadBuffer<PassConstants>>(m_device.Get(), 1, true);
}

void Framework::BuildCbvHeap()
{
	D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
	const UINT textureCount = std::max<UINT>(1u, static_cast<UINT>(m_textureResources.size()));
	heapDesc.NumDescriptors = 2 + textureCount;
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

	ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(m_cbvHeap.GetAddressOf())));
}

void Framework::BuildCbvViews()
{
	{
		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
		cbvDesc.BufferLocation = m_objectCB->Resource()->GetGPUVirtualAddress();
		cbvDesc.SizeInBytes = CalcConstantBufferByteSize(sizeof(ObjectConstants));

		D3D12_CPU_DESCRIPTOR_HANDLE h = m_cbvHeap->GetCPUDescriptorHandleForHeapStart();
		m_device->CreateConstantBufferView(&cbvDesc, h);
	}

	{
		D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
		cbvDesc.BufferLocation = m_passCB->Resource()->GetGPUVirtualAddress();
		cbvDesc.SizeInBytes = CalcConstantBufferByteSize(sizeof(PassConstants));

		D3D12_CPU_DESCRIPTOR_HANDLE h = m_cbvHeap->GetCPUDescriptorHandleForHeapStart();
		h.ptr += (SIZE_T)m_cbvSrvUavDescriptorSize;
		m_device->CreateConstantBufferView(&cbvDesc, h);
	}

	if (!m_textureResources.empty())
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.PlaneSlice = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

		for (UINT i = 0; i < static_cast<UINT>(m_textureResources.size()); ++i)
		{
			ID3D12Resource* texture = m_textureResources[i].Get();
			if (!texture) {
				continue;
			}

			srvDesc.Format = texture->GetDesc().Format;

			D3D12_CPU_DESCRIPTOR_HANDLE h = m_cbvHeap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += static_cast<SIZE_T>(2 + i) * m_cbvSrvUavDescriptorSize;
			m_device->CreateShaderResourceView(texture, &srvDesc, h);
		}
	}
}

void Framework::BuildRootSignature()
{
	D3D12_DESCRIPTOR_RANGE cbvRange = {};
	cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
	cbvRange.NumDescriptors = 2;
	cbvRange.BaseShaderRegister = 0;
	cbvRange.RegisterSpace = 0;
	cbvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_DESCRIPTOR_RANGE srvRange = {};
	srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srvRange.NumDescriptors = 1;
	srvRange.BaseShaderRegister = 0;
	srvRange.RegisterSpace = 0;
	srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER rootParams[3] = {};

	rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
	rootParams[0].DescriptorTable.pDescriptorRanges = &cbvRange;
	rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	rootParams[1].Constants.ShaderRegister = 2;
	rootParams[1].Constants.RegisterSpace = 0;
	rootParams[1].Constants.Num32BitValues = static_cast<UINT>(sizeof(MaterialConstants) / sizeof(UINT32));
	rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
	rootParams[2].DescriptorTable.pDescriptorRanges = &srvRange;
	rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	sampler.MipLODBias = 0.0f;
	sampler.MaxAnisotropy = 1;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
	sampler.MinLOD = 0.0f;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderRegister = 0;
	sampler.RegisterSpace = 0;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
	rootSigDesc.NumParameters = _countof(rootParams);
	rootSigDesc.pParameters = rootParams;
	rootSigDesc.NumStaticSamplers = 1;
	rootSigDesc.pStaticSamplers = &sampler;
	rootSigDesc.Flags =
		D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

	ComPtr<ID3DBlob> serializedRootSig;
	ComPtr<ID3DBlob> errorBlob;

	HRESULT hr = D3D12SerializeRootSignature(
		&rootSigDesc,
		D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(),
		errorBlob.GetAddressOf());

	if (errorBlob != nullptr)
		OutputDebugStringA((char*)errorBlob->GetBufferPointer());

	ThrowIfFailed(hr);

	ThrowIfFailed(m_device->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(m_rootSignature.GetAddressOf())));
}

void Framework::BuildPSO()
{
	D3D12_INPUT_ELEMENT_DESC inputLayout[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

		{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },

		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40,
		  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	D3D12_RASTERIZER_DESC rasterDesc = {};
	rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
	rasterDesc.CullMode = D3D12_CULL_MODE_BACK;
	rasterDesc.FrontCounterClockwise = FALSE;
	rasterDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
	rasterDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	rasterDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	rasterDesc.DepthClipEnable = TRUE;
	rasterDesc.MultisampleEnable = FALSE;
	rasterDesc.AntialiasedLineEnable = FALSE;
	rasterDesc.ForcedSampleCount = 0;
	rasterDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

	D3D12_BLEND_DESC blendDesc = {};
	blendDesc.AlphaToCoverageEnable = FALSE;
	blendDesc.IndependentBlendEnable = FALSE;
	{
		D3D12_RENDER_TARGET_BLEND_DESC rt = {};
		rt.BlendEnable = FALSE;
		rt.LogicOpEnable = FALSE;
		rt.SrcBlend = D3D12_BLEND_ONE;
		rt.DestBlend = D3D12_BLEND_ZERO;
		rt.BlendOp = D3D12_BLEND_OP_ADD;
		rt.SrcBlendAlpha = D3D12_BLEND_ONE;
		rt.DestBlendAlpha = D3D12_BLEND_ZERO;
		rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		rt.LogicOp = D3D12_LOGIC_OP_NOOP;
		rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		blendDesc.RenderTarget[0] = rt;
	}

	D3D12_DEPTH_STENCIL_DESC dsDesc = {};
	dsDesc.DepthEnable = TRUE;
	dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
	dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
	dsDesc.StencilEnable = FALSE;
	dsDesc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
	dsDesc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
	dsDesc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
	dsDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	dsDesc.BackFace = dsDesc.FrontFace;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
	psoDesc.pRootSignature = m_rootSignature.Get();
	psoDesc.VS = { m_vsByteCode->GetBufferPointer(), m_vsByteCode->GetBufferSize() };
	psoDesc.PS = { m_psByteCode->GetBufferPointer(), m_psByteCode->GetBufferSize() };
	psoDesc.RasterizerState = rasterDesc;
	psoDesc.BlendState = blendDesc;
	psoDesc.DepthStencilState = dsDesc;
	psoDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = m_backBufferFormat;
	psoDesc.DSVFormat = m_depthStencilFormat;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.SampleDesc.Quality = 0;

	ThrowIfFailed(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(m_pso.GetAddressOf())));
}

void Framework::BuildBoxGeometry()
{
	auto ColorFromPos = [](float x, float y, float z)
		{
			return DirectX::XMFLOAT4(
				(x + 1.0f) * 0.5f,
				(y + 1.0f) * 0.5f,
				(z + 1.0f) * 0.5f,
				1.0f
			);
		};

	std::array<Vertex, 24> vertices =
	{
		Vertex{ { -1.0f, -1.0f, -1.0f }, { 0.0f,  0.0f, -1.0f }, ColorFromPos(-1.0f, -1.0f, -1.0f) },
		Vertex{ { -1.0f,  1.0f, -1.0f }, { 0.0f,  0.0f, -1.0f }, ColorFromPos(-1.0f,  1.0f, -1.0f) },
		Vertex{ {  1.0f,  1.0f, -1.0f }, { 0.0f,  0.0f, -1.0f }, ColorFromPos(1.0f,  1.0f, -1.0f) },
		Vertex{ {  1.0f, -1.0f, -1.0f }, { 0.0f,  0.0f, -1.0f }, ColorFromPos(1.0f, -1.0f, -1.0f) },

		Vertex{ {  1.0f, -1.0f,  1.0f }, { 0.0f,  0.0f,  1.0f }, ColorFromPos(1.0f, -1.0f,  1.0f) },
		Vertex{ {  1.0f,  1.0f,  1.0f }, { 0.0f,  0.0f,  1.0f }, ColorFromPos(1.0f,  1.0f,  1.0f) },
		Vertex{ { -1.0f,  1.0f,  1.0f }, { 0.0f,  0.0f,  1.0f }, ColorFromPos(-1.0f,  1.0f,  1.0f) },
		Vertex{ { -1.0f, -1.0f,  1.0f }, { 0.0f,  0.0f,  1.0f }, ColorFromPos(-1.0f, -1.0f,  1.0f) },

		Vertex{ { -1.0f, -1.0f,  1.0f }, { -1.0f, 0.0f,  0.0f }, ColorFromPos(-1.0f, -1.0f,  1.0f) },
		Vertex{ { -1.0f,  1.0f,  1.0f }, { -1.0f, 0.0f,  0.0f }, ColorFromPos(-1.0f,  1.0f,  1.0f) },
		Vertex{ { -1.0f,  1.0f, -1.0f }, { -1.0f, 0.0f,  0.0f }, ColorFromPos(-1.0f,  1.0f, -1.0f) },
		Vertex{ { -1.0f, -1.0f, -1.0f }, { -1.0f, 0.0f,  0.0f }, ColorFromPos(-1.0f, -1.0f, -1.0f) },

		Vertex{ {  1.0f, -1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, ColorFromPos(1.0f, -1.0f, -1.0f) },
		Vertex{ {  1.0f,  1.0f, -1.0f }, { 1.0f, 0.0f, 0.0f }, ColorFromPos(1.0f,  1.0f, -1.0f) },
		Vertex{ {  1.0f,  1.0f,  1.0f }, { 1.0f, 0.0f, 0.0f }, ColorFromPos(1.0f,  1.0f,  1.0f) },
		Vertex{ {  1.0f, -1.0f,  1.0f }, { 1.0f, 0.0f, 0.0f }, ColorFromPos(1.0f, -1.0f,  1.0f) },

		Vertex{ { -1.0f,  1.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, ColorFromPos(-1.0f,  1.0f, -1.0f) },
		Vertex{ { -1.0f,  1.0f,  1.0f }, { 0.0f, 1.0f, 0.0f }, ColorFromPos(-1.0f,  1.0f,  1.0f) },
		Vertex{ {  1.0f,  1.0f,  1.0f }, { 0.0f, 1.0f, 0.0f }, ColorFromPos(1.0f,  1.0f,  1.0f) },
		Vertex{ {  1.0f,  1.0f, -1.0f }, { 0.0f, 1.0f, 0.0f }, ColorFromPos(1.0f,  1.0f, -1.0f) },

		Vertex{ {  1.0f, -1.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, ColorFromPos(1.0f, -1.0f, -1.0f) },
		Vertex{ {  1.0f, -1.0f,  1.0f }, { 0.0f, -1.0f, 0.0f }, ColorFromPos(1.0f, -1.0f,  1.0f) },
		Vertex{ { -1.0f, -1.0f,  1.0f }, { 0.0f, -1.0f, 0.0f }, ColorFromPos(-1.0f, -1.0f,  1.0f) },
		Vertex{ { -1.0f, -1.0f, -1.0f }, { 0.0f, -1.0f, 0.0f }, ColorFromPos(-1.0f, -1.0f, -1.0f) },
	};

	std::array<std::uint16_t, 36> indices =
	{
		0, 1, 2,  0, 2, 3,
		4, 5, 6,  4, 6, 7,
		8, 9,10,  8,10,11,
		12,13,14, 12,14,15,
		16,17,18, 16,18,19,
		20,21,22, 20,22,23
	};

	m_boxIndexCount = (UINT)indices.size();

	const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
	const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint16_t);

	auto MakeBufferDesc = [](UINT64 byteSize)
		{
			D3D12_RESOURCE_DESC d = {};
			d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			d.Alignment = 0;
			d.Width = byteSize;
			d.Height = 1;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = DXGI_FORMAT_UNKNOWN;
			d.SampleDesc.Count = 1;
			d.SampleDesc.Quality = 0;
			d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			d.Flags = D3D12_RESOURCE_FLAG_NONE;
			return d;
		};

	D3D12_HEAP_PROPERTIES defaultHeap = {};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_HEAP_PROPERTIES uploadHeap = {};
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

	auto vbDesc = MakeBufferDesc(vbByteSize);
	auto ibDesc = MakeBufferDesc(ibByteSize);

	ThrowIfFailed(m_device->CreateCommittedResource(
		&defaultHeap,
		D3D12_HEAP_FLAG_NONE,
		&vbDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(m_boxVB.GetAddressOf())));

	ThrowIfFailed(m_device->CreateCommittedResource(
		&defaultHeap,
		D3D12_HEAP_FLAG_NONE,
		&ibDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(m_boxIB.GetAddressOf())));

	ThrowIfFailed(m_device->CreateCommittedResource(
		&uploadHeap,
		D3D12_HEAP_FLAG_NONE,
		&vbDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(m_boxVBUpload.GetAddressOf())));

	ThrowIfFailed(m_device->CreateCommittedResource(
		&uploadHeap,
		D3D12_HEAP_FLAG_NONE,
		&ibDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(m_boxIBUpload.GetAddressOf())));

	{
		void* mapped = nullptr;
		ThrowIfFailed(m_boxVBUpload->Map(0, nullptr, &mapped));
		memcpy(mapped, vertices.data(), vbByteSize);
		m_boxVBUpload->Unmap(0, nullptr);
	}
	{
		void* mapped = nullptr;
		ThrowIfFailed(m_boxIBUpload->Map(0, nullptr, &mapped));
		memcpy(mapped, indices.data(), ibByteSize);
		m_boxIBUpload->Unmap(0, nullptr);
	}

	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	m_commandList->CopyBufferRegion(m_boxVB.Get(), 0, m_boxVBUpload.Get(), 0, vbByteSize);
	m_commandList->CopyBufferRegion(m_boxIB.Get(), 0, m_boxIBUpload.Get(), 0, ibByteSize);

	D3D12_RESOURCE_BARRIER barriers[2] = {};

	barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[0].Transition.pResource = m_boxVB.Get();
	barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
	barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[1].Transition.pResource = m_boxIB.Get();
	barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_INDEX_BUFFER;
	barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	m_commandList->ResourceBarrier(2, barriers);

	ThrowIfFailed(m_commandList->Close());
	ID3D12CommandList* cmds[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(1, cmds);

	FlushCommandQueue();

	m_boxVBView.BufferLocation = m_boxVB->GetGPUVirtualAddress();
	m_boxVBView.StrideInBytes = sizeof(Vertex);
	m_boxVBView.SizeInBytes = vbByteSize;

	m_boxIBView.BufferLocation = m_boxIB->GetGPUVirtualAddress();
	m_boxIBView.Format = DXGI_FORMAT_R16_UINT;
	m_boxIBView.SizeInBytes = ibByteSize;

	m_boxVBUpload.Reset();
	m_boxIBUpload.Reset();
}

void Framework::BuildObjVB_Upload()
{
	using namespace DirectX;

	const std::filesystem::path objPath = std::filesystem::path(L"assets") / L"sponza.obj";
	const std::string objPathUtf8 = objPath.u8string();
	const std::string baseDirUtf8 = objPath.parent_path().u8string();

	tinyobj::attrib_t attrib;
	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> tinyMaterials;
	std::string warn, err;

	bool ok = tinyobj::LoadObj(
		&attrib, &shapes, &tinyMaterials,
		&warn, &err,
		objPathUtf8.c_str(),
		baseDirUtf8.empty() ? nullptr : baseDirUtf8.c_str(),
		true);

	if (!warn.empty())
		OutputDebugStringA(("[tinyobj warn] " + warn + "\n").c_str());
	if (!err.empty())
		OutputDebugStringA(("[tinyobj err ] " + err + "\n").c_str());

	if (!ok) {
		throw std::runtime_error("tinyobj::LoadObj failed (see Output window).");
	}

	m_modelMaterials.clear();
	m_modelSubsets.clear();
	m_textureResources.clear();
	m_textureUploadResources.clear();
	m_modelVB.Reset();
	m_modelVBUpload.Reset();
	m_modelVertexCount = 0;

	std::vector<RgbaImage> textureImages;
	textureImages.reserve(32);
	textureImages.push_back(RgbaImage{});

	std::unordered_map<std::wstring, UINT> textureIndexByPath;
	textureIndexByPath.reserve(64);

	ModelMaterial defaultMaterial = {};
	defaultMaterial.HasTexture = true;
	defaultMaterial.TextureIndex = 0;
	m_modelMaterials.push_back(defaultMaterial);

	for (const tinyobj::material_t& srcMaterial : tinyMaterials)
	{
		ModelMaterial mat = {};
		mat.DiffuseAlbedo = {
			static_cast<float>(srcMaterial.diffuse[0]),
			static_cast<float>(srcMaterial.diffuse[1]),
			static_cast<float>(srcMaterial.diffuse[2]),
			static_cast<float>(srcMaterial.dissolve > 0.0f ? srcMaterial.dissolve : 1.0f)
		};

		float uScale = static_cast<float>(srcMaterial.diffuse_texopt.scale[0]);
		float vScale = static_cast<float>(srcMaterial.diffuse_texopt.scale[1]);
		if (std::fabs(uScale) < 1e-6f) uScale = 1.0f;
		if (std::fabs(vScale) < 1e-6f) vScale = 1.0f;

		mat.UvTiling = { uScale, vScale };
		mat.UvOffset = {
			static_cast<float>(srcMaterial.diffuse_texopt.origin_offset[0]),
			static_cast<float>(srcMaterial.diffuse_texopt.origin_offset[1])
		};
		mat.HasTexture = false;
		mat.TextureIndex = 0;

		if (!srcMaterial.diffuse_texname.empty())
		{
			std::filesystem::path texturePath = std::filesystem::u8path(srcMaterial.diffuse_texname);
			if (!texturePath.is_absolute()) {
				texturePath = objPath.parent_path() / texturePath;
			}
			texturePath = texturePath.lexically_normal();

			const std::wstring key = texturePath.wstring();
			auto existing = textureIndexByPath.find(key);
			if (existing != textureIndexByPath.end()) {
				mat.TextureIndex = existing->second;
				mat.HasTexture = true;
			}
			else {
				RgbaImage loadedImage;
				if (LoadImageFromFile(texturePath, loadedImage)) {
					const UINT textureIndex = static_cast<UINT>(textureImages.size());
					textureImages.push_back(std::move(loadedImage));
					textureIndexByPath.emplace(key, textureIndex);

					mat.TextureIndex = textureIndex;
					mat.HasTexture = true;
				}
				else {
					std::wstring warnLine = L"[Texture] Failed to load: ";
					warnLine += texturePath.wstring();
					warnLine += L"\n";
					OutputDebugStringW(warnLine.c_str());
				}
			}
		}

		m_modelMaterials.push_back(mat);
	}

	const bool hasNormals = !attrib.normals.empty();
	const bool hasTexcoords = !attrib.texcoords.empty();

	XMFLOAT3 minP = { +FLT_MAX, +FLT_MAX, +FLT_MAX };
	XMFLOAT3 maxP = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	auto ExpandBounds = [&](const XMFLOAT3& p) {
		if (p.x < minP.x) minP.x = p.x;
		if (p.y < minP.y) minP.y = p.y;
		if (p.z < minP.z) minP.z = p.z;
		if (p.x > maxP.x) maxP.x = p.x;
		if (p.y > maxP.y) maxP.y = p.y;
		if (p.z > maxP.z) maxP.z = p.z;
	};

	auto ReadPos = [&](int vIdx) -> XMFLOAT3 {
		if (vIdx < 0) {
			return { 0.0f, 0.0f, 0.0f };
		}
		const size_t i = static_cast<size_t>(vIdx);
		return {
			attrib.vertices[3 * i + 0],
			attrib.vertices[3 * i + 1],
			attrib.vertices[3 * i + 2]
		};
	};

	auto ReadNormal = [&](int nIdx) -> XMFLOAT3 {
		if (!hasNormals || nIdx < 0) {
			return { 0.0f, 1.0f, 0.0f };
		}
		const size_t i = static_cast<size_t>(nIdx);
		return {
			attrib.normals[3 * i + 0],
			attrib.normals[3 * i + 1],
			attrib.normals[3 * i + 2]
		};
	};

	auto ReadTexCoord = [&](int tIdx) -> XMFLOAT2 {
		if (!hasTexcoords || tIdx < 0) {
			return { 0.0f, 0.0f };
		}
		const size_t i = static_cast<size_t>(tIdx);
		return {
			attrib.texcoords[2 * i + 0],
			1.0f - attrib.texcoords[2 * i + 1]
		};
	};

	std::vector<std::vector<Vertex>> materialVertices(m_modelMaterials.size());

	for (const tinyobj::shape_t& shape : shapes)
	{
		size_t indexOffset = 0;
		for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); ++f)
		{
			const int fv = shape.mesh.num_face_vertices[f];
			if (fv != 3) {
				indexOffset += static_cast<size_t>(fv);
				continue;
			}

			UINT materialIndex = 0;
			if (f < shape.mesh.material_ids.size()) {
				const int tinyMatId = shape.mesh.material_ids[f];
				if (tinyMatId >= 0 && tinyMatId < static_cast<int>(tinyMaterials.size())) {
					materialIndex = static_cast<UINT>(tinyMatId + 1);
				}
			}
			if (materialIndex >= materialVertices.size()) {
				materialIndex = 0;
			}

			Vertex tri[3] = {};
			for (int v = 0; v < 3; ++v)
			{
				const tinyobj::index_t idx = shape.mesh.indices[indexOffset + static_cast<size_t>(v)];
				tri[v].Pos = ReadPos(idx.vertex_index);
				tri[v].Normal = ReadNormal(idx.normal_index);
				tri[v].Color = { 1.0f, 1.0f, 1.0f, 1.0f };
				tri[v].TexC = ReadTexCoord(idx.texcoord_index);
			}

			if (!hasNormals ||
				shape.mesh.indices[indexOffset + 0].normal_index < 0 ||
				shape.mesh.indices[indexOffset + 1].normal_index < 0 ||
				shape.mesh.indices[indexOffset + 2].normal_index < 0)
			{
				const XMVECTOR a = XMLoadFloat3(&tri[0].Pos);
				const XMVECTOR b = XMLoadFloat3(&tri[1].Pos);
				const XMVECTOR c = XMLoadFloat3(&tri[2].Pos);
				const XMVECTOR n = XMVector3Normalize(XMVector3Cross(b - a, c - a));
				XMFLOAT3 normal;
				XMStoreFloat3(&normal, n);
				tri[0].Normal = normal;
				tri[1].Normal = normal;
				tri[2].Normal = normal;
			}

			materialVertices[materialIndex].push_back(tri[0]);
			materialVertices[materialIndex].push_back(tri[1]);
			materialVertices[materialIndex].push_back(tri[2]);

			ExpandBounds(tri[0].Pos);
			ExpandBounds(tri[1].Pos);
			ExpandBounds(tri[2].Pos);

			indexOffset += 3;
		}
	}

	std::vector<Vertex> vertices;
	vertices.reserve(500000);
	for (UINT materialIndex = 0; materialIndex < static_cast<UINT>(materialVertices.size()); ++materialIndex)
	{
		const std::vector<Vertex>& bucket = materialVertices[materialIndex];
		if (bucket.empty()) {
			continue;
		}

		ModelSubset subset = {};
		subset.MaterialIndex = materialIndex;
		subset.StartVertex = static_cast<UINT>(vertices.size());
		subset.VertexCount = static_cast<UINT>(bucket.size());
		m_modelSubsets.push_back(subset);

		vertices.insert(vertices.end(), bucket.begin(), bucket.end());
	}

	if (vertices.empty()) {
		throw std::runtime_error("OBJ loaded but produced 0 vertices.");
	}

	m_modelCenter = {
		0.5f * (minP.x + maxP.x),
		0.5f * (minP.y + maxP.y),
		0.5f * (minP.z + maxP.z)
	};

	float maxDim = maxP.x - minP.x;
	const float dimY = maxP.y - minP.y;
	const float dimZ = maxP.z - minP.z;
	if (dimY > maxDim) maxDim = dimY;
	if (dimZ > maxDim) maxDim = dimZ;
	m_modelScale = (maxDim > 1e-6f) ? (2.0f / maxDim) : 1.0f;

	m_modelVertexCount = static_cast<UINT>(vertices.size());
	const UINT vbByteSize = static_cast<UINT>(vertices.size() * sizeof(Vertex));

	D3D12_HEAP_PROPERTIES defaultHeap = {};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_HEAP_PROPERTIES uploadHeap = {};
	uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

	auto MakeBufferDesc = [](UINT64 byteSize) {
		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = byteSize;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		return desc;
	};

	ThrowIfFailed(m_directCmdListAlloc->Reset());
	ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

	m_textureResources.reserve(textureImages.size());
	m_textureUploadResources.reserve(textureImages.size());

	for (size_t texIdx = 0; texIdx < textureImages.size(); ++texIdx)
	{
		const RgbaImage& image = textureImages[texIdx];

		D3D12_RESOURCE_DESC textureDesc = {};
		textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		textureDesc.Width = image.Width;
		textureDesc.Height = image.Height;
		textureDesc.DepthOrArraySize = 1;
		textureDesc.MipLevels = 1;
		textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

		ComPtr<ID3D12Resource> texture;
		ThrowIfFailed(m_device->CreateCommittedResource(
			&defaultHeap,
			D3D12_HEAP_FLAG_NONE,
			&textureDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(texture.GetAddressOf())));

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
		UINT numRows = 0;
		UINT64 uploadSize = 0;
		m_device->GetCopyableFootprints(
			&textureDesc,
			0,
			1,
			0,
			&footprint,
			&numRows,
			nullptr,
			&uploadSize);

		ComPtr<ID3D12Resource> upload;
		D3D12_RESOURCE_DESC uploadDesc = MakeBufferDesc(uploadSize);
		ThrowIfFailed(m_device->CreateCommittedResource(
			&uploadHeap,
			D3D12_HEAP_FLAG_NONE,
			&uploadDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(upload.GetAddressOf())));

		void* mapped = nullptr;
		ThrowIfFailed(upload->Map(0, nullptr, &mapped));
		const UINT srcRowPitch = image.Width * 4;
		for (UINT row = 0; row < numRows; ++row)
		{
			const std::uint8_t* src = image.Pixels.data() + static_cast<size_t>(row) * srcRowPitch;
			std::uint8_t* dst = reinterpret_cast<std::uint8_t*>(mapped)
				+ footprint.Offset
				+ static_cast<size_t>(row) * footprint.Footprint.RowPitch;
			memcpy(dst, src, srcRowPitch);
		}
		upload->Unmap(0, nullptr);

		D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
		dstLoc.pResource = texture.Get();
		dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLoc.SubresourceIndex = 0;

		D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
		srcLoc.pResource = upload.Get();
		srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLoc.PlacedFootprint = footprint;

		m_commandList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = texture.Get();
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		m_commandList->ResourceBarrier(1, &barrier);

		m_textureResources.push_back(texture);
		m_textureUploadResources.push_back(upload);
	}

	const D3D12_RESOURCE_DESC vbDesc = MakeBufferDesc(vbByteSize);
	ThrowIfFailed(m_device->CreateCommittedResource(
		&defaultHeap,
		D3D12_HEAP_FLAG_NONE,
		&vbDesc,
		D3D12_RESOURCE_STATE_COPY_DEST,
		nullptr,
		IID_PPV_ARGS(m_modelVB.GetAddressOf())));

	ThrowIfFailed(m_device->CreateCommittedResource(
		&uploadHeap,
		D3D12_HEAP_FLAG_NONE,
		&vbDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(m_modelVBUpload.GetAddressOf())));

	void* vbMapped = nullptr;
	ThrowIfFailed(m_modelVBUpload->Map(0, nullptr, &vbMapped));
	memcpy(vbMapped, vertices.data(), vbByteSize);
	m_modelVBUpload->Unmap(0, nullptr);

	m_commandList->CopyBufferRegion(m_modelVB.Get(), 0, m_modelVBUpload.Get(), 0, vbByteSize);

	D3D12_RESOURCE_BARRIER vbBarrier = {};
	vbBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	vbBarrier.Transition.pResource = m_modelVB.Get();
	vbBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	vbBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
	vbBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	m_commandList->ResourceBarrier(1, &vbBarrier);

	ThrowIfFailed(m_commandList->Close());
	ID3D12CommandList* cmdLists[] = { m_commandList.Get() };
	m_commandQueue->ExecuteCommandLists(1, cmdLists);
	FlushCommandQueue();

	m_textureUploadResources.clear();
	m_modelVBUpload.Reset();

	m_modelVBV.BufferLocation = m_modelVB->GetGPUVirtualAddress();
	m_modelVBV.StrideInBytes = sizeof(Vertex);
	m_modelVBV.SizeInBytes = vbByteSize;
}

void Framework::OnMouseDown(HWND hwnd, WPARAM btnState, int x, int y)
{
	if (btnState & MK_RBUTTON)
	{
		m_rmbDown = true;
		m_lastMousePos.x = x;
		m_lastMousePos.y = y;

		// Захват мыши, чтобы события шли даже если вышли за окно
		SetCapture(hwnd);

		// Опционально: скрыть курсор
		// ShowCursor(FALSE);
	}
}

void Framework::OnMouseUp(HWND hwnd, WPARAM btnState, int x, int y)
{
	(void)btnState; (void)x; (void)y;

	if (m_rmbDown)
	{
		m_rmbDown = false;
		ReleaseCapture();

		// Опционально вернуть курсор
		// ShowCursor(TRUE);
	}
}

void Framework::OnMouseMove(HWND hwnd,	WPARAM btnState, int x, int y)
{
	(void)btnState;

	if (!m_rmbDown)
	{
		m_lastMousePos.x = x;
		m_lastMousePos.y = y;
		return;
	}

	int dx = x - m_lastMousePos.x;
	int dy = y - m_lastMousePos.y;

	m_lastMousePos.x = x;
	m_lastMousePos.y = y;

	// yaw: вправо -> положительный
	m_yaw += dx * m_mouseSensitivity;

	// pitch: вверх обычно уменьшает y (dy отрицательный),
	// поэтому делаем "-" чтобы вверх => положительный pitch
	m_pitch -= dy * m_mouseSensitivity;

	// Ограничим pitch, чтобы не переворачивалась камера
	const float limit = DirectX::XM_PIDIV2 - 0.1f; // ~ 89°
	if (m_pitch > limit) m_pitch = limit;
	if (m_pitch < -limit) m_pitch = -limit;

	using namespace DirectX;

	// Вектор "вперёд" из yaw/pitch (LH система)
	XMVECTOR forward = XMVectorSet(
		cosf(m_pitch) * sinf(m_yaw),
		sinf(m_pitch),
		cosf(m_pitch) * cosf(m_yaw),
		0.0f);

	forward = XMVector3Normalize(forward);

	XMVECTOR pos = XMLoadFloat3(&m_camPos);
	XMVECTOR tgt = pos + forward;

	XMStoreFloat3(&m_camTarget, tgt);
}

