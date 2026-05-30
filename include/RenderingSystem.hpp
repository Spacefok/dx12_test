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

	ID3D12RootSignature* PostProcessRootSignature() const { return m_postProcessRootSignature.Get(); }
	ID3D12PipelineState* PostDownsamplePSO() const { return m_postDownsamplePso.Get(); }
	ID3D12PipelineState* PostBrightPassPSO() const { return m_postBrightPassPso.Get(); }
	ID3D12PipelineState* PostBlurHorizontalPSO() const { return m_postBlurHorizontalPso.Get(); }
	ID3D12PipelineState* PostBlurVerticalPSO() const { return m_postBlurVerticalPso.Get(); }
	ID3D12PipelineState* PostFinalPSO() const { return m_postFinalPso.Get(); }

	ID3D12RootSignature* ShadowRootSignature() const { return m_shadowRootSignature.Get(); }
	ID3D12PipelineState* ShadowBasicPSO() const { return m_shadowBasicPso.Get(); }
	ID3D12PipelineState* ShadowTessellationPSO() const { return m_shadowTessellationPso.Get(); }

	ID3D12RootSignature* ParticleGraphicsRootSignature() const { return m_particleGraphicsRootSignature.Get(); }
	ID3D12PipelineState* ParticleGraphicsPSO() const { return m_particleGraphicsPso.Get(); }
	ID3D12RootSignature* ParticleComputeRootSignature() const { return m_particleComputeRootSignature.Get(); }
	ID3D12PipelineState* ParticleComputePSO() const { return m_particleComputePso.Get(); }
	ID3D12PipelineState* ParticleSortInitPSO() const { return m_particleSortInitPso.Get(); }
	ID3D12PipelineState* ParticleSortStepPSO() const { return m_particleSortStepPso.Get(); }

private:
	ComPtr<ID3DBlob> m_geometryBasicVsByteCode;
	ComPtr<ID3DBlob> m_geometryControlPointVsByteCode;
	ComPtr<ID3DBlob> m_geometryHsByteCode;
	ComPtr<ID3DBlob> m_geometryDsByteCode;
	ComPtr<ID3DBlob> m_geometryPsByteCode;
	ComPtr<ID3DBlob> m_forwardTransparentPsByteCode;
	ComPtr<ID3DBlob> m_lightingVsByteCode;
	ComPtr<ID3DBlob> m_lightingPsByteCode;
	ComPtr<ID3DBlob> m_postProcessVsByteCode;
	ComPtr<ID3DBlob> m_postDownsamplePsByteCode;
	ComPtr<ID3DBlob> m_postBrightPassPsByteCode;
	ComPtr<ID3DBlob> m_postBlurHorizontalPsByteCode;
	ComPtr<ID3DBlob> m_postBlurVerticalPsByteCode;
	ComPtr<ID3DBlob> m_postFinalPsByteCode;
	ComPtr<ID3DBlob> m_shadowBasicVsByteCode;
	ComPtr<ID3DBlob> m_shadowControlPointVsByteCode;
	ComPtr<ID3DBlob> m_shadowHsByteCode;
	ComPtr<ID3DBlob> m_shadowDsByteCode;
	ComPtr<ID3DBlob> m_shadowPsByteCode;
	ComPtr<ID3DBlob> m_particleVsByteCode;
	ComPtr<ID3DBlob> m_particleGsByteCode;
	ComPtr<ID3DBlob> m_particlePsByteCode;
	ComPtr<ID3DBlob> m_particleCsByteCode;
	ComPtr<ID3DBlob> m_particleSortInitCsByteCode;
	ComPtr<ID3DBlob> m_particleSortStepCsByteCode;

	ComPtr<ID3D12RootSignature> m_geometryRootSignature;
	ComPtr<ID3D12RootSignature> m_lightingRootSignature;
	ComPtr<ID3D12RootSignature> m_postProcessRootSignature;
	ComPtr<ID3D12RootSignature> m_shadowRootSignature;
	ComPtr<ID3D12RootSignature> m_particleGraphicsRootSignature;
	ComPtr<ID3D12RootSignature> m_particleComputeRootSignature;

	ComPtr<ID3D12PipelineState> m_geometryBasicPso;
	ComPtr<ID3D12PipelineState> m_geometryTessellationPso;
	ComPtr<ID3D12PipelineState> m_forwardTransparentPso;
	ComPtr<ID3D12PipelineState> m_lightingPso;
	ComPtr<ID3D12PipelineState> m_postDownsamplePso;
	ComPtr<ID3D12PipelineState> m_postBrightPassPso;
	ComPtr<ID3D12PipelineState> m_postBlurHorizontalPso;
	ComPtr<ID3D12PipelineState> m_postBlurVerticalPso;
	ComPtr<ID3D12PipelineState> m_postFinalPso;
	ComPtr<ID3D12PipelineState> m_shadowBasicPso;
	ComPtr<ID3D12PipelineState> m_shadowTessellationPso;
	ComPtr<ID3D12PipelineState> m_particleGraphicsPso;
	ComPtr<ID3D12PipelineState> m_particleComputePso;
	ComPtr<ID3D12PipelineState> m_particleSortInitPso;
	ComPtr<ID3D12PipelineState> m_particleSortStepPso;
};

#endif // RENDERING_SYSTEM_HPP
