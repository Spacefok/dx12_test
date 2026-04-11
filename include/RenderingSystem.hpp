#ifndef RENDERING_SYSTEM_HPP
#define RENDERING_SYSTEM_HPP

#include "Dx12Common.hpp"

class RenderingSystem {
public:
	void BuildShaders();
	void BuildRootSignatures(ID3D12Device* device);
	void BuildPSOs(ID3D12Device* device, DXGI_FORMAT backBufferFormat, DXGI_FORMAT depthStencilFormat);

	ID3D12RootSignature* GeometryRootSignature() const { return m_geometryRootSignature.Get(); }
	ID3D12PipelineState* GeometryBasicPSO() const { return m_geometryBasicPso.Get(); }
	ID3D12PipelineState* GeometryTessellationPSO() const { return m_geometryTessellationPso.Get(); }
	ID3D12PipelineState* ForwardTransparentPSO() const { return m_forwardTransparentPso.Get(); }

	ID3D12RootSignature* LightingRootSignature() const { return m_lightingRootSignature.Get(); }
	ID3D12PipelineState* LightingPSO() const { return m_lightingPso.Get(); }

private:
	ComPtr<ID3DBlob> m_geometryBasicVsByteCode;
	ComPtr<ID3DBlob> m_geometryControlPointVsByteCode;
	ComPtr<ID3DBlob> m_geometryHsByteCode;
	ComPtr<ID3DBlob> m_geometryDsByteCode;
	ComPtr<ID3DBlob> m_geometryPsByteCode;
	ComPtr<ID3DBlob> m_forwardTransparentPsByteCode;
	ComPtr<ID3DBlob> m_lightingVsByteCode;
	ComPtr<ID3DBlob> m_lightingPsByteCode;

	ComPtr<ID3D12RootSignature> m_geometryRootSignature;
	ComPtr<ID3D12RootSignature> m_lightingRootSignature;

	ComPtr<ID3D12PipelineState> m_geometryBasicPso;
	ComPtr<ID3D12PipelineState> m_geometryTessellationPso;
	ComPtr<ID3D12PipelineState> m_forwardTransparentPso;
	ComPtr<ID3D12PipelineState> m_lightingPso;
};

#endif // RENDERING_SYSTEM_HPP
