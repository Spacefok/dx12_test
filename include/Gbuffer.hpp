#ifndef GBUFFER_HPP
#define GBUFFER_HPP

#include <array>
#include "Dx12Common.hpp"

class Gbuffer {
public:
	static constexpr UINT kTargetCount = 3;
	static constexpr DXGI_FORMAT kAlbedoFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	static constexpr DXGI_FORMAT kNormalFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	static constexpr DXGI_FORMAT kPositionSpecFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

	void Resize(ID3D12Device* device, UINT width, UINT height, UINT rtvDescriptorSize);
	void CreateSrvDescriptors(ID3D12Device* device, D3D12_CPU_DESCRIPTOR_HANDLE srvStart, UINT descriptorSize) const;

	void TransitionToRenderTargets(ID3D12GraphicsCommandList* commandList);
	void TransitionToShaderResources(ID3D12GraphicsCommandList* commandList);

	void Clear(ID3D12GraphicsCommandList* commandList) const;
	void BindAsRenderTargets(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE dsv) const;

	bool IsValid() const { return m_targets[0] != nullptr; }

private:
	void EnsureRtvHeap(ID3D12Device* device, UINT rtvDescriptorSize);
	D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle(UINT index) const;
	DXGI_FORMAT TargetFormat(UINT index) const;

	UINT m_width = 0;
	UINT m_height = 0;
	UINT m_rtvDescriptorSize = 0;

	ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
	std::array<ComPtr<ID3D12Resource>, kTargetCount> m_targets{};
	std::array<D3D12_RESOURCE_STATES, kTargetCount> m_states{
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_COMMON
	};
};

#endif // GBUFFER_HPP
