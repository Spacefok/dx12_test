#include "Gbuffer.hpp"

void Gbuffer::EnsureRtvHeap(ID3D12Device* device, UINT rtvDescriptorSize)
{
	if (m_rtvHeap && m_rtvDescriptorSize == rtvDescriptorSize) {
		return;
	}

	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.NumDescriptors = kTargetCount;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

	ThrowIfFailed(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)));
	m_rtvDescriptorSize = rtvDescriptorSize;
}

D3D12_CPU_DESCRIPTOR_HANDLE Gbuffer::RtvHandle(UINT index) const
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
	handle.ptr += static_cast<SIZE_T>(index) * m_rtvDescriptorSize;
	return handle;
}

DXGI_FORMAT Gbuffer::TargetFormat(UINT index) const
{
	switch (index) {
	case 0: return kAlbedoFormat;
	case 1: return kNormalFormat;
	case 2: return kPositionSpecFormat;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

void Gbuffer::Resize(ID3D12Device* device, UINT width, UINT height, UINT rtvDescriptorSize)
{
	if (width == 0 || height == 0) {
		return;
	}

	EnsureRtvHeap(device, rtvDescriptorSize);

	if (m_width == width && m_height == height && m_targets[0] != nullptr) {
		return;
	}

	m_width = width;
	m_height = height;

	D3D12_HEAP_PROPERTIES defaultHeap = {};
	defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

	for (UINT i = 0; i < kTargetCount; ++i)
	{
		m_targets[i].Reset();
		m_states[i] = D3D12_RESOURCE_STATE_COMMON;

		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Alignment = 0;
		desc.Width = width;
		desc.Height = height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = TargetFormat(i);
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		D3D12_CLEAR_VALUE clearValue = {};
		clearValue.Format = desc.Format;
		if (i == 1) {
			clearValue.Color[0] = 0.0f;
			clearValue.Color[1] = 0.0f;
			clearValue.Color[2] = 1.0f;
			clearValue.Color[3] = 0.0f;
		}
		else {
			clearValue.Color[0] = 0.0f;
			clearValue.Color[1] = 0.0f;
			clearValue.Color[2] = 0.0f;
			clearValue.Color[3] = 0.0f;
		}

		ThrowIfFailed(device->CreateCommittedResource(
			&defaultHeap,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_COMMON,
			&clearValue,
			IID_PPV_ARGS(&m_targets[i])));

		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = desc.Format;
		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		rtvDesc.Texture2D.MipSlice = 0;
		rtvDesc.Texture2D.PlaneSlice = 0;

		device->CreateRenderTargetView(m_targets[i].Get(), &rtvDesc, RtvHandle(i));
	}
}

void Gbuffer::CreateSrvDescriptors(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE srvStart, UINT descriptorSize) const
{
	if (!m_targets[0]) {
		return;
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.PlaneSlice = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

	for (UINT i = 0; i < kTargetCount; ++i)
	{
		D3D12_CPU_DESCRIPTOR_HANDLE handle = srvStart;
		handle.ptr += static_cast<SIZE_T>(i) * descriptorSize;

		srvDesc.Format = TargetFormat(i);
		device->CreateShaderResourceView(m_targets[i].Get(), &srvDesc, handle);
	}
}

void Gbuffer::TransitionToRenderTargets(ID3D12GraphicsCommandList* commandList)
{
	D3D12_RESOURCE_BARRIER barriers[kTargetCount] = {};
	UINT barrierCount = 0;

	for (UINT i = 0; i < kTargetCount; ++i)
	{
		if (m_states[i] == D3D12_RESOURCE_STATE_RENDER_TARGET) {
			continue;
		}

		D3D12_RESOURCE_BARRIER& barrier = barriers[barrierCount++];
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = m_targets[i].Get();
		barrier.Transition.StateBefore = m_states[i];
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

		m_states[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
	}

	if (barrierCount > 0) {
		commandList->ResourceBarrier(barrierCount, barriers);
	}
}

void Gbuffer::TransitionToShaderResources(ID3D12GraphicsCommandList* commandList)
{
	D3D12_RESOURCE_BARRIER barriers[kTargetCount] = {};
	UINT barrierCount = 0;

	for (UINT i = 0; i < kTargetCount; ++i)
	{
		if (m_states[i] == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
			continue;
		}

		D3D12_RESOURCE_BARRIER& barrier = barriers[barrierCount++];
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = m_targets[i].Get();
		barrier.Transition.StateBefore = m_states[i];
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

		m_states[i] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	}

	if (barrierCount > 0) {
		commandList->ResourceBarrier(barrierCount, barriers);
	}
}

void Gbuffer::Clear(ID3D12GraphicsCommandList* commandList) const
{
	static const float kClearColors[kTargetCount][4] =
	{
		{ 0.0f, 0.0f, 0.0f, 0.0f },
		{ 0.0f, 0.0f, 1.0f, 0.0f },
		{ 0.0f, 0.0f, 0.0f, 0.0f }
	};

	for (UINT i = 0; i < kTargetCount; ++i) {
		commandList->ClearRenderTargetView(RtvHandle(i), kClearColors[i], 0, nullptr);
	}
}

void Gbuffer::BindAsRenderTargets(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE dsv) const
{
	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kTargetCount> rtvs = {
		RtvHandle(0),
		RtvHandle(1),
		RtvHandle(2)
	};

	commandList->OMSetRenderTargets(
		static_cast<UINT>(rtvs.size()),
		rtvs.data(),
		FALSE,
		&dsv);
}
