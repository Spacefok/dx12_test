#include "RenderingSystem.hpp"
#include "Gbuffer.hpp"
#include "RenderStructs.hpp"

namespace {
ComPtr<ID3D12RootSignature> CreateRootSignature(
	ID3D12Device* device,
	const D3D12_ROOT_SIGNATURE_DESC& desc)
{
	ComPtr<ID3DBlob> serializedRootSig;
	ComPtr<ID3DBlob> errorBlob;

	const HRESULT hr = D3D12SerializeRootSignature(
		&desc,
		D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(),
		errorBlob.GetAddressOf());

	if (errorBlob) {
		OutputDebugStringA(reinterpret_cast<const char*>(errorBlob->GetBufferPointer()));
	}

	ThrowIfFailed(hr);

	ComPtr<ID3D12RootSignature> rootSignature;
	ThrowIfFailed(device->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(&rootSignature)));

	return rootSignature;
}

D3D12_RASTERIZER_DESC DefaultRasterizer(D3D12_CULL_MODE cullMode)
{
	D3D12_RASTERIZER_DESC rasterDesc = {};
	rasterDesc.FillMode = D3D12_FILL_MODE_SOLID;
	rasterDesc.CullMode = cullMode;
	rasterDesc.FrontCounterClockwise = FALSE;
	rasterDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
	rasterDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
	rasterDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
	rasterDesc.DepthClipEnable = TRUE;
	rasterDesc.MultisampleEnable = FALSE;
	rasterDesc.AntialiasedLineEnable = FALSE;
	rasterDesc.ForcedSampleCount = 0;
	rasterDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
	return rasterDesc;
}

D3D12_RASTERIZER_DESC ShadowRasterizer()
{
	D3D12_RASTERIZER_DESC rasterDesc = DefaultRasterizer(D3D12_CULL_MODE_BACK);
	rasterDesc.DepthBias = 2000;
	rasterDesc.DepthBiasClamp = 0.005f;
	rasterDesc.SlopeScaledDepthBias = 2.0f;
	return rasterDesc;
}

constexpr DXGI_FORMAT kHdrRenderTargetFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
} // namespace

void RenderingSystem::BuildShaders()
{
	m_geometryBasicVsByteCode = CompileShader(L"shader\\DeferredGeometry.hlsl", nullptr, "VSBasic", "vs_5_1");
	m_geometryControlPointVsByteCode = CompileShader(L"shader\\DeferredGeometry.hlsl", nullptr, "VSControlPoint", "vs_5_1");
	m_geometryHsByteCode = CompileShader(L"shader\\DeferredGeometry.hlsl", nullptr, "HSMain", "hs_5_1");
	m_geometryDsByteCode = CompileShader(L"shader\\DeferredGeometry.hlsl", nullptr, "DSMain", "ds_5_1");
	m_geometryPsByteCode = CompileShader(L"shader\\DeferredGeometry.hlsl", nullptr, "PSGBuffer", "ps_5_1");
	m_forwardTransparentPsByteCode = CompileShader(L"shader\\DeferredGeometry.hlsl", nullptr, "PSTransparent", "ps_5_1");

	m_lightingVsByteCode = CompileShader(L"shader\\DeferredLighting.hlsl", nullptr, "VSFullscreen", "vs_5_1");
	m_lightingPsByteCode = CompileShader(L"shader\\DeferredLighting.hlsl", nullptr, "PSLighting", "ps_5_1");

	m_postProcessVsByteCode = CompileShader(L"shader\\PostProcess.hlsl", nullptr, "VSFullscreen", "vs_5_1");
	m_postDownsamplePsByteCode = CompileShader(L"shader\\PostProcess.hlsl", nullptr, "PSDownsample", "ps_5_1");
	m_postBrightPassPsByteCode = CompileShader(L"shader\\PostProcess.hlsl", nullptr, "PSBrightPass", "ps_5_1");
	m_postBlurHorizontalPsByteCode = CompileShader(L"shader\\PostProcess.hlsl", nullptr, "PSBlurHorizontal", "ps_5_1");
	m_postBlurVerticalPsByteCode = CompileShader(L"shader\\PostProcess.hlsl", nullptr, "PSBlurVertical", "ps_5_1");
	m_postFinalPsByteCode = CompileShader(L"shader\\PostProcess.hlsl", nullptr, "PSFinalComposite", "ps_5_1");

	m_shadowBasicVsByteCode = CompileShader(L"shader\\ShadowMap.hlsl", nullptr, "VSShadowBasic", "vs_5_1");
	m_shadowControlPointVsByteCode = CompileShader(L"shader\\ShadowMap.hlsl", nullptr, "VSShadowControlPoint", "vs_5_1");
	m_shadowHsByteCode = CompileShader(L"shader\\ShadowMap.hlsl", nullptr, "HSShadow", "hs_5_1");
	m_shadowDsByteCode = CompileShader(L"shader\\ShadowMap.hlsl", nullptr, "DSShadow", "ds_5_1");
	m_shadowPsByteCode = CompileShader(L"shader\\ShadowMap.hlsl", nullptr, "PSShadowDepth", "ps_5_1");

	m_particleVsByteCode = CompileShader(L"shader\\Particles.hlsl", nullptr, "VSParticle", "vs_5_1");
	m_particleGsByteCode = CompileShader(L"shader\\Particles.hlsl", nullptr, "GSBillboard", "gs_5_1");
	m_particlePsByteCode = CompileShader(L"shader\\Particles.hlsl", nullptr, "PSRainParticle", "ps_5_1");
	m_particleCsByteCode = CompileShader(L"shader\\Particles.hlsl", nullptr, "CSUpdateParticles", "cs_5_1");
	m_particleSortInitCsByteCode = CompileShader(L"shader\\Particles.hlsl", nullptr, "CSInitParticleSort", "cs_5_1");
	m_particleSortStepCsByteCode = CompileShader(L"shader\\Particles.hlsl", nullptr, "CSBitonicSort", "cs_5_1");
}

void RenderingSystem::BuildRootSignatures(ID3D12Device* device)
{
	{
		D3D12_DESCRIPTOR_RANGE cbvRange = {};
		cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
		cbvRange.NumDescriptors = 2;
		cbvRange.BaseShaderRegister = 0;
		cbvRange.RegisterSpace = 0;
		cbvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_DESCRIPTOR_RANGE srvRange = {};
		srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		srvRange.NumDescriptors = MaterialTextureSlotCount;
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
		rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

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
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
		rootSigDesc.NumParameters = _countof(rootParams);
		rootSigDesc.pParameters = rootParams;
		rootSigDesc.NumStaticSamplers = 1;
		rootSigDesc.pStaticSamplers = &sampler;
		rootSigDesc.Flags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

		m_geometryRootSignature = CreateRootSignature(device, rootSigDesc);
	}

	{
		D3D12_DESCRIPTOR_RANGE srvRange = {};
		srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		srvRange.NumDescriptors = MaterialTextureSlotCount;
		srvRange.BaseShaderRegister = 0;
		srvRange.RegisterSpace = 0;
		srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER rootParams[4] = {};

		rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParams[0].Descriptor.ShaderRegister = 0;
		rootParams[0].Descriptor.RegisterSpace = 0;
		rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParams[1].Descriptor.ShaderRegister = 1;
		rootParams[1].Descriptor.RegisterSpace = 0;
		rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		rootParams[2].Constants.ShaderRegister = 2;
		rootParams[2].Constants.RegisterSpace = 0;
		rootParams[2].Constants.Num32BitValues = static_cast<UINT>(sizeof(MaterialConstants) / sizeof(UINT32));
		rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
		rootParams[3].DescriptorTable.pDescriptorRanges = &srvRange;
		rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

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
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
		rootSigDesc.NumParameters = _countof(rootParams);
		rootSigDesc.pParameters = rootParams;
		rootSigDesc.NumStaticSamplers = 1;
		rootSigDesc.pStaticSamplers = &sampler;
		rootSigDesc.Flags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

		m_shadowRootSignature = CreateRootSignature(device, rootSigDesc);
	}

	{
		D3D12_DESCRIPTOR_RANGE cbvRange = {};
		cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
		cbvRange.NumDescriptors = 1;
		cbvRange.BaseShaderRegister = 0;
		cbvRange.RegisterSpace = 0;
		cbvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_DESCRIPTOR_RANGE srvRange = {};
		srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		srvRange.NumDescriptors = Gbuffer::kTargetCount + 5;
		srvRange.BaseShaderRegister = 0;
		srvRange.RegisterSpace = 0;
		srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER rootParams[2] = {};

		rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
		rootParams[0].DescriptorTable.pDescriptorRanges = &cbvRange;
		rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
		rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
		rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
		samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[0].MipLODBias = 0.0f;
		samplers[0].MaxAnisotropy = 1;
		samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
		samplers[0].MinLOD = 0.0f;
		samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
		samplers[0].ShaderRegister = 0;
		samplers[0].RegisterSpace = 0;
		samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
		samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		samplers[1].MipLODBias = 0.0f;
		samplers[1].MaxAnisotropy = 1;
		samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
		samplers[1].MinLOD = 0.0f;
		samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
		samplers[1].ShaderRegister = 1;
		samplers[1].RegisterSpace = 0;
		samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
		rootSigDesc.NumParameters = _countof(rootParams);
		rootSigDesc.pParameters = rootParams;
		rootSigDesc.NumStaticSamplers = _countof(samplers);
		rootSigDesc.pStaticSamplers = samplers;
		rootSigDesc.Flags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

		m_lightingRootSignature = CreateRootSignature(device, rootSigDesc);
	}

	{
		D3D12_DESCRIPTOR_RANGE sourceRange = {};
		sourceRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		sourceRange.NumDescriptors = 1;
		sourceRange.BaseShaderRegister = 0;
		sourceRange.RegisterSpace = 0;
		sourceRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_DESCRIPTOR_RANGE finalRange = {};
		finalRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		finalRange.NumDescriptors = 3;
		finalRange.BaseShaderRegister = 1;
		finalRange.RegisterSpace = 0;
		finalRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER rootParams[3] = {};

		rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParams[0].Descriptor.ShaderRegister = 0;
		rootParams[0].Descriptor.RegisterSpace = 0;
		rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
		rootParams[1].DescriptorTable.pDescriptorRanges = &sourceRange;
		rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
		rootParams[2].DescriptorTable.pDescriptorRanges = &finalRange;
		rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
		samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		samplers[0].MipLODBias = 0.0f;
		samplers[0].MaxAnisotropy = 1;
		samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
		samplers[0].MinLOD = 0.0f;
		samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
		samplers[0].ShaderRegister = 0;
		samplers[0].RegisterSpace = 0;
		samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		samplers[1] = samplers[0];
		samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		samplers[1].ShaderRegister = 1;

		D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
		rootSigDesc.NumParameters = _countof(rootParams);
		rootSigDesc.pParameters = rootParams;
		rootSigDesc.NumStaticSamplers = _countof(samplers);
		rootSigDesc.pStaticSamplers = samplers;
		rootSigDesc.Flags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

		m_postProcessRootSignature = CreateRootSignature(device, rootSigDesc);
	}

	{
		D3D12_DESCRIPTOR_RANGE particleSrvRange = {};
		particleSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		particleSrvRange.NumDescriptors = 2;
		particleSrvRange.BaseShaderRegister = 0;
		particleSrvRange.RegisterSpace = 0;
		particleSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER rootParams[2] = {};

		rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParams[0].Descriptor.ShaderRegister = 1;
		rootParams[0].Descriptor.RegisterSpace = 0;
		rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
		rootParams[1].DescriptorTable.pDescriptorRanges = &particleSrvRange;
		rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

		D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
		rootSigDesc.NumParameters = _countof(rootParams);
		rootSigDesc.pParameters = rootParams;
		rootSigDesc.NumStaticSamplers = 0;
		rootSigDesc.pStaticSamplers = nullptr;
		rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

		m_particleGraphicsRootSignature = CreateRootSignature(device, rootSigDesc);
	}

	{
		D3D12_DESCRIPTOR_RANGE consumeRange = {};
		consumeRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		consumeRange.NumDescriptors = 1;
		consumeRange.BaseShaderRegister = 0;
		consumeRange.RegisterSpace = 0;
		consumeRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_DESCRIPTOR_RANGE appendRange = {};
		appendRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		appendRange.NumDescriptors = 1;
		appendRange.BaseShaderRegister = 1;
		appendRange.RegisterSpace = 0;
		appendRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_DESCRIPTOR_RANGE particleSrvRange = {};
		particleSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		particleSrvRange.NumDescriptors = 1;
		particleSrvRange.BaseShaderRegister = 0;
		particleSrvRange.RegisterSpace = 0;
		particleSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_DESCRIPTOR_RANGE sortUavRange = {};
		sortUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		sortUavRange.NumDescriptors = 1;
		sortUavRange.BaseShaderRegister = 2;
		sortUavRange.RegisterSpace = 0;
		sortUavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

		D3D12_ROOT_PARAMETER rootParams[7] = {};

		rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParams[0].Descriptor.ShaderRegister = 0;
		rootParams[0].Descriptor.RegisterSpace = 0;
		rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
		rootParams[1].DescriptorTable.pDescriptorRanges = &consumeRange;
		rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
		rootParams[2].DescriptorTable.pDescriptorRanges = &appendRange;
		rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParams[3].Descriptor.ShaderRegister = 1;
		rootParams[3].Descriptor.RegisterSpace = 0;
		rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
		rootParams[4].DescriptorTable.pDescriptorRanges = &particleSrvRange;
		rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rootParams[5].DescriptorTable.NumDescriptorRanges = 1;
		rootParams[5].DescriptorTable.pDescriptorRanges = &sortUavRange;
		rootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		rootParams[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		rootParams[6].Constants.ShaderRegister = 2;
		rootParams[6].Constants.RegisterSpace = 0;
		rootParams[6].Constants.Num32BitValues = 2;
		rootParams[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
		rootSigDesc.NumParameters = _countof(rootParams);
		rootSigDesc.pParameters = rootParams;
		rootSigDesc.NumStaticSamplers = 0;
		rootSigDesc.pStaticSamplers = nullptr;
		rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		m_particleComputeRootSignature = CreateRootSignature(device, rootSigDesc);
	}
}

void RenderingSystem::BuildPSOs(ID3D12Device* device, DXGI_FORMAT backBufferFormat, DXGI_FORMAT depthStencilFormat)
{
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

			{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48,
			  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};

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
		psoDesc.pRootSignature = m_geometryRootSignature.Get();
		psoDesc.VS = { m_geometryBasicVsByteCode->GetBufferPointer(), m_geometryBasicVsByteCode->GetBufferSize() };
		psoDesc.PS = { m_geometryPsByteCode->GetBufferPointer(), m_geometryPsByteCode->GetBufferSize() };
		psoDesc.RasterizerState = DefaultRasterizer(D3D12_CULL_MODE_BACK);
		psoDesc.BlendState = blendDesc;
		psoDesc.DepthStencilState = dsDesc;
		psoDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = Gbuffer::kTargetCount;
		psoDesc.RTVFormats[0] = Gbuffer::kAlbedoFormat;
		psoDesc.RTVFormats[1] = Gbuffer::kNormalFormat;
		psoDesc.RTVFormats[2] = Gbuffer::kPositionSpecFormat;
		psoDesc.DSVFormat = depthStencilFormat;
		psoDesc.SampleDesc.Count = 1;
		psoDesc.SampleDesc.Quality = 0;

		ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_geometryBasicPso)));
	}

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

			{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48,
			  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};

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
		psoDesc.pRootSignature = m_geometryRootSignature.Get();
		psoDesc.VS = { m_geometryControlPointVsByteCode->GetBufferPointer(), m_geometryControlPointVsByteCode->GetBufferSize() };
		psoDesc.HS = { m_geometryHsByteCode->GetBufferPointer(), m_geometryHsByteCode->GetBufferSize() };
		psoDesc.DS = { m_geometryDsByteCode->GetBufferPointer(), m_geometryDsByteCode->GetBufferSize() };
		psoDesc.PS = { m_geometryPsByteCode->GetBufferPointer(), m_geometryPsByteCode->GetBufferSize() };
		psoDesc.RasterizerState = DefaultRasterizer(D3D12_CULL_MODE_BACK);
		psoDesc.BlendState = blendDesc;
		psoDesc.DepthStencilState = dsDesc;
		psoDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
		psoDesc.NumRenderTargets = Gbuffer::kTargetCount;
		psoDesc.RTVFormats[0] = Gbuffer::kAlbedoFormat;
		psoDesc.RTVFormats[1] = Gbuffer::kNormalFormat;
		psoDesc.RTVFormats[2] = Gbuffer::kPositionSpecFormat;
		psoDesc.DSVFormat = depthStencilFormat;
		psoDesc.SampleDesc.Count = 1;
		psoDesc.SampleDesc.Quality = 0;

		ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_geometryTessellationPso)));
	}

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

			{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48,
			  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};

		D3D12_BLEND_DESC blendDesc = {};
		blendDesc.AlphaToCoverageEnable = FALSE;
		blendDesc.IndependentBlendEnable = FALSE;
		{
			D3D12_RENDER_TARGET_BLEND_DESC rt = {};
			rt.BlendEnable = TRUE;
			rt.LogicOpEnable = FALSE;
			rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
			rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			rt.BlendOp = D3D12_BLEND_OP_ADD;
			rt.SrcBlendAlpha = D3D12_BLEND_ONE;
			rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			rt.LogicOp = D3D12_LOGIC_OP_NOOP;
			rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			blendDesc.RenderTarget[0] = rt;
		}

		D3D12_DEPTH_STENCIL_DESC dsDesc = {};
		dsDesc.DepthEnable = TRUE;
		dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
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
		psoDesc.pRootSignature = m_geometryRootSignature.Get();
		psoDesc.VS = { m_geometryBasicVsByteCode->GetBufferPointer(), m_geometryBasicVsByteCode->GetBufferSize() };
		psoDesc.PS = { m_forwardTransparentPsByteCode->GetBufferPointer(), m_forwardTransparentPsByteCode->GetBufferSize() };
		psoDesc.RasterizerState = DefaultRasterizer(D3D12_CULL_MODE_BACK);
		psoDesc.BlendState = blendDesc;
		psoDesc.DepthStencilState = dsDesc;
		psoDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = kHdrRenderTargetFormat;
		psoDesc.DSVFormat = depthStencilFormat;
		psoDesc.SampleDesc.Count = 1;
		psoDesc.SampleDesc.Quality = 0;

		ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_forwardTransparentPso)));
	}

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

			{ "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48,
			  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};

		D3D12_BLEND_DESC blendDesc = {};
		blendDesc.AlphaToCoverageEnable = FALSE;
		blendDesc.IndependentBlendEnable = FALSE;

		D3D12_DEPTH_STENCIL_DESC dsDesc = {};
		dsDesc.DepthEnable = TRUE;
		dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		dsDesc.StencilEnable = FALSE;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
		psoDesc.pRootSignature = m_shadowRootSignature.Get();
		psoDesc.VS = { m_shadowBasicVsByteCode->GetBufferPointer(), m_shadowBasicVsByteCode->GetBufferSize() };
		psoDesc.PS = { m_shadowPsByteCode->GetBufferPointer(), m_shadowPsByteCode->GetBufferSize() };
		psoDesc.RasterizerState = ShadowRasterizer();
		psoDesc.BlendState = blendDesc;
		psoDesc.DepthStencilState = dsDesc;
		psoDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 0;
		psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		psoDesc.SampleDesc.Count = 1;
		psoDesc.SampleDesc.Quality = 0;

		ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_shadowBasicPso)));

		psoDesc.VS = { m_shadowControlPointVsByteCode->GetBufferPointer(), m_shadowControlPointVsByteCode->GetBufferSize() };
		psoDesc.HS = { m_shadowHsByteCode->GetBufferPointer(), m_shadowHsByteCode->GetBufferSize() };
		psoDesc.DS = { m_shadowDsByteCode->GetBufferPointer(), m_shadowDsByteCode->GetBufferSize() };
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;

		ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_shadowTessellationPso)));
	}

	{
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
		dsDesc.DepthEnable = FALSE;
		dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		dsDesc.StencilEnable = FALSE;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.InputLayout = { nullptr, 0 };
		psoDesc.pRootSignature = m_lightingRootSignature.Get();
		psoDesc.VS = { m_lightingVsByteCode->GetBufferPointer(), m_lightingVsByteCode->GetBufferSize() };
		psoDesc.PS = { m_lightingPsByteCode->GetBufferPointer(), m_lightingPsByteCode->GetBufferSize() };
		psoDesc.RasterizerState = DefaultRasterizer(D3D12_CULL_MODE_NONE);
		psoDesc.BlendState = blendDesc;
		psoDesc.DepthStencilState = dsDesc;
		psoDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = kHdrRenderTargetFormat;
		psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
		psoDesc.SampleDesc.Count = 1;
		psoDesc.SampleDesc.Quality = 0;

		ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_lightingPso)));
	}

	{
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
		dsDesc.DepthEnable = FALSE;
		dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		dsDesc.StencilEnable = FALSE;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.InputLayout = { nullptr, 0 };
		psoDesc.pRootSignature = m_postProcessRootSignature.Get();
		psoDesc.VS = { m_postProcessVsByteCode->GetBufferPointer(), m_postProcessVsByteCode->GetBufferSize() };
		psoDesc.PS = { m_postDownsamplePsByteCode->GetBufferPointer(), m_postDownsamplePsByteCode->GetBufferSize() };
		psoDesc.RasterizerState = DefaultRasterizer(D3D12_CULL_MODE_NONE);
		psoDesc.BlendState = blendDesc;
		psoDesc.DepthStencilState = dsDesc;
		psoDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = kHdrRenderTargetFormat;
		psoDesc.DSVFormat = DXGI_FORMAT_UNKNOWN;
		psoDesc.SampleDesc.Count = 1;
		psoDesc.SampleDesc.Quality = 0;

		ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_postDownsamplePso)));

		psoDesc.PS = { m_postBrightPassPsByteCode->GetBufferPointer(), m_postBrightPassPsByteCode->GetBufferSize() };
		ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_postBrightPassPso)));

		psoDesc.PS = { m_postBlurHorizontalPsByteCode->GetBufferPointer(), m_postBlurHorizontalPsByteCode->GetBufferSize() };
		ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_postBlurHorizontalPso)));

		psoDesc.PS = { m_postBlurVerticalPsByteCode->GetBufferPointer(), m_postBlurVerticalPsByteCode->GetBufferSize() };
		ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_postBlurVerticalPso)));

		psoDesc.PS = { m_postFinalPsByteCode->GetBufferPointer(), m_postFinalPsByteCode->GetBufferSize() };
		psoDesc.RTVFormats[0] = backBufferFormat;
		ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_postFinalPso)));
	}

	{
		D3D12_BLEND_DESC blendDesc = {};
		blendDesc.AlphaToCoverageEnable = FALSE;
		blendDesc.IndependentBlendEnable = FALSE;
		{
			D3D12_RENDER_TARGET_BLEND_DESC rt = {};
			rt.BlendEnable = TRUE;
			rt.LogicOpEnable = FALSE;
			rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
			rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			rt.BlendOp = D3D12_BLEND_OP_ADD;
			rt.SrcBlendAlpha = D3D12_BLEND_ONE;
			rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			rt.LogicOp = D3D12_LOGIC_OP_NOOP;
			rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			blendDesc.RenderTarget[0] = rt;
		}

		D3D12_DEPTH_STENCIL_DESC dsDesc = {};
		dsDesc.DepthEnable = TRUE;
		dsDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		dsDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		dsDesc.StencilEnable = FALSE;
		dsDesc.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
		dsDesc.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
		dsDesc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		dsDesc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		dsDesc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		dsDesc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		dsDesc.BackFace = dsDesc.FrontFace;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.InputLayout = { nullptr, 0 };
		psoDesc.pRootSignature = m_particleGraphicsRootSignature.Get();
		psoDesc.VS = { m_particleVsByteCode->GetBufferPointer(), m_particleVsByteCode->GetBufferSize() };
		psoDesc.GS = { m_particleGsByteCode->GetBufferPointer(), m_particleGsByteCode->GetBufferSize() };
		psoDesc.PS = { m_particlePsByteCode->GetBufferPointer(), m_particlePsByteCode->GetBufferSize() };
		psoDesc.RasterizerState = DefaultRasterizer(D3D12_CULL_MODE_NONE);
		psoDesc.BlendState = blendDesc;
		psoDesc.DepthStencilState = dsDesc;
		psoDesc.SampleMask = D3D12_DEFAULT_SAMPLE_MASK;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = kHdrRenderTargetFormat;
		psoDesc.DSVFormat = depthStencilFormat;
		psoDesc.SampleDesc.Count = 1;
		psoDesc.SampleDesc.Quality = 0;

		ThrowIfFailed(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_particleGraphicsPso)));
	}

	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = m_particleComputeRootSignature.Get();
		psoDesc.CS = { m_particleCsByteCode->GetBufferPointer(), m_particleCsByteCode->GetBufferSize() };
		psoDesc.NodeMask = 0;
		psoDesc.CachedPSO = {};
		psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

		ThrowIfFailed(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_particleComputePso)));
	}

	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = m_particleComputeRootSignature.Get();
		psoDesc.CS = { m_particleSortInitCsByteCode->GetBufferPointer(), m_particleSortInitCsByteCode->GetBufferSize() };
		psoDesc.NodeMask = 0;
		psoDesc.CachedPSO = {};
		psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

		ThrowIfFailed(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_particleSortInitPso)));
	}

	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = m_particleComputeRootSignature.Get();
		psoDesc.CS = { m_particleSortStepCsByteCode->GetBufferPointer(), m_particleSortStepCsByteCode->GetBufferSize() };
		psoDesc.NodeMask = 0;
		psoDesc.CachedPSO = {};
		psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

		ThrowIfFailed(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_particleSortStepPso)));
	}
}
