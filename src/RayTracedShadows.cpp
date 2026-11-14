#include "RayTracedShadows.hpp"
#include "RayTracedShadowTrace.h"
#include "RayTracedShadowFilter.h"
#include <algorithm>
#include <cstring>

namespace {

constexpr DXGI_FORMAT ShadowFormat = DXGI_FORMAT_R16G16_FLOAT;

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}

void UavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    list->ResourceBarrier(1, &barrier);
}

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, UINT64 bytes,
    D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = heapType;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (bytes + 255u) & ~UINT64(255u);
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        state, nullptr, IID_PPV_ARGS(&resource)));
    return resource;
}

} // namespace

bool RayTracedShadows::Initialize(ID3D12Device* device)
{
    if (m_supported) return true;
    if (!device) return false;

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options = {};
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = { D3D_SHADER_MODEL_6_5 };
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options, sizeof(options)))
        || options.RaytracingTier < D3D12_RAYTRACING_TIER_1_1
        || FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)))
        || shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_5
        || FAILED(device->QueryInterface(IID_PPV_ARGS(&m_device)))) {
        return false;
    }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT format = { ShadowFormat };
    ThrowIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof(format)));
    if ((format.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) return false;

    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 5;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_heap)));
    heap.NumDescriptors = 2;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_clearHeap)));
    m_descriptorSize = device->GetDescriptorHandleIncrementSize(heap.Type);

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[0].BaseShaderRegister = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 2;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER parameters[4] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[1].Descriptor.ShaderRegister = 0;
    for (UINT index = 0; index < 2; ++index) {
        parameters[index + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[index + 2].DescriptorTable.NumDescriptorRanges = 1;
        parameters[index + 2].DescriptorTable.pDescriptorRanges = &ranges[index];
    }
    D3D12_ROOT_SIGNATURE_DESC root = {};
    root.NumParameters = 4;
    root.pParameters = parameters;
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    const HRESULT rootResult = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors);
    if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
    ThrowIfFailed(rootResult);
    ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = m_rootSignature.Get();
    pipeline.CS = { g_RayTracedShadowTrace, sizeof(g_RayTracedShadowTrace) };
    ThrowIfFailed(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&m_tracePso)));
    pipeline.CS = { g_RayTracedShadowFilter, sizeof(g_RayTracedShadowFilter) };
    ThrowIfFailed(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&m_filterPso)));
    m_constants = std::make_unique<UploadBuffer<RayTracedShadowConstants>>(device, 1, true);
    m_supported = true;
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE RayTracedShadows::CpuHandle(UINT index) const
{
    auto handle = m_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * m_descriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE RayTracedShadows::GpuHandle(UINT index) const
{
    auto handle = m_heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * m_descriptorSize;
    return handle;
}

void RayTracedShadows::Resize(UINT width, UINT height, ID3D12Resource* depth,
    DXGI_FORMAT depthSrvFormat, ID3D12Resource* normal, DXGI_FORMAT normalFormat)
{
    if (!m_supported) return;
    if (width == 0 || height == 0 || !depth || !normal)
        throw std::invalid_argument("Invalid ray-traced shadow render targets.");
    m_width = width;
    m_height = height;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = ShadowFormat;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    m_raw.Reset();
    m_filtered.Reset();
    ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_raw)));
    ThrowIfFailed(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&m_filtered)));
    m_raw->SetName(L"RT shadow visibility and penumbra");
    m_filtered->SetName(L"Filtered RT shadow visibility and penumbra");

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = depthSrvFormat;
    m_device->CreateShaderResourceView(depth, &srv, CpuHandle(0));
    srv.Format = normalFormat;
    m_device->CreateShaderResourceView(normal, &srv, CpuHandle(1));
    srv.Format = ShadowFormat;
    m_device->CreateShaderResourceView(m_raw.Get(), &srv, CpuHandle(2));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = ShadowFormat;
    m_device->CreateUnorderedAccessView(m_raw.Get(), nullptr, &uav, CpuHandle(3));
    m_device->CreateUnorderedAccessView(m_filtered.Get(), nullptr, &uav, CpuHandle(4));
    auto clearHandle = m_clearHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateUnorderedAccessView(m_raw.Get(), nullptr, &uav, clearHandle);
    clearHandle.ptr += m_descriptorSize;
    m_device->CreateUnorderedAccessView(m_filtered.Get(), nullptr, &uav, clearHandle);
}

void RayTracedShadows::InvalidateGeometry()
{
    m_blas.Reset();
    m_blasScratch.Reset();
    m_tlas.Reset();
    m_tlasScratch.Reset();
    m_instances.Reset();
    m_instanceCount = 0;
    m_geometryIndexCount = 0;
    m_geometryVertexAddress = m_geometryIndexAddress = 0;
    m_topLevelBuilt = false;
}

void RayTracedShadows::BuildBottomLevel(ID3D12GraphicsCommandList4* list,
    ID3D12Resource* vb, ID3D12Resource* ib, const D3D12_VERTEX_BUFFER_VIEW& vertexView,
    const D3D12_INDEX_BUFFER_VIEW& indexView, UINT indexCount)
{
    const UINT indexSize = indexView.Format == DXGI_FORMAT_R16_UINT ? 2u : 4u;
    if (!vb || !ib || vertexView.StrideInBytes < sizeof(DirectX::XMFLOAT3)
        || vertexView.SizeInBytes < vertexView.StrideInBytes
        || (indexView.Format != DXGI_FORMAT_R16_UINT && indexView.Format != DXGI_FORMAT_R32_UINT)
        || indexCount == 0 || indexCount % 3 != 0 || indexCount > indexView.SizeInBytes / indexSize) {
        throw std::invalid_argument("Invalid indexed mesh for ray-traced shadows.");
    }
    D3D12_RAYTRACING_GEOMETRY_DESC geometry = {};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.Triangles.VertexBuffer.StartAddress = vertexView.BufferLocation;
    geometry.Triangles.VertexBuffer.StrideInBytes = vertexView.StrideInBytes;
    geometry.Triangles.VertexCount = vertexView.SizeInBytes / vertexView.StrideInBytes;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.IndexBuffer = indexView.BufferLocation;
    geometry.Triangles.IndexCount = indexCount;
    geometry.Triangles.IndexFormat = indexView.Format;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = 1;
    inputs.pGeometryDescs = &geometry;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0)
        throw std::runtime_error("Cannot size shadow BLAS.");
    m_blas = CreateBuffer(m_device.Get(), info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_blasScratch = CreateBuffer(m_device.Get(), info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_blas->SetName(L"Shared box shadow BLAS");

    Transition(list, vb, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(list, ib, D3D12_RESOURCE_STATE_INDEX_BUFFER, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(list, m_blasScratch.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
    build.Inputs = inputs;
    build.DestAccelerationStructureData = m_blas->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = m_blasScratch->GetGPUVirtualAddress();
    list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    UavBarrier(list, m_blas.Get());
    Transition(list, m_blasScratch.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    Transition(list, vb, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    Transition(list, ib, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    m_geometryVertexAddress = vertexView.BufferLocation;
    m_geometryIndexAddress = indexView.BufferLocation;
    m_geometryIndexCount = indexCount;
}

void RayTracedShadows::UpdateTopLevel(ID3D12GraphicsCommandList4* list,
    const std::vector<DirectX::XMFLOAT4X4>& worlds)
{
    if (worlds.size() > 0x1000000u) throw std::invalid_argument("Too many shadow instances.");
    const UINT count = static_cast<UINT>(worlds.size());
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = count;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE
        | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    if (!m_tlas || m_instanceCount != count) {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info = {};
        m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0)
            throw std::runtime_error("Cannot size shadow TLAS.");
        m_tlas = CreateBuffer(m_device.Get(), info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        m_tlasScratch = CreateBuffer(m_device.Get(), (std::max)(info.ScratchDataSizeInBytes,
            info.UpdateScratchDataSizeInBytes), D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        m_instances = CreateBuffer(m_device.Get(), UINT64(count) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC),
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
        m_tlas->SetName(L"Opaque scene shadow TLAS");
        m_instanceCount = count;
        m_topLevelBuilt = false;
    }

    D3D12_RAYTRACING_INSTANCE_DESC* mapped = nullptr;
    const D3D12_RANGE noRead = { 0, 0 };
    ThrowIfFailed(m_instances->Map(0, &noRead, reinterpret_cast<void**>(&mapped)));
    for (UINT index = 0; index < count; ++index) {
        D3D12_RAYTRACING_INSTANCE_DESC instance = {};
        // ObjectConstants stores transpose(world), exactly the DXR row-major 3x4 layout.
        std::memcpy(instance.Transform, &worlds[index], sizeof(instance.Transform));
        instance.InstanceID = index;
        instance.InstanceMask = 0xff;
        instance.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        instance.AccelerationStructure = m_blas->GetGPUVirtualAddress();
        mapped[index] = instance;
    }
    m_instances->Unmap(0, nullptr);
    inputs.InstanceDescs = m_instances->GetGPUVirtualAddress();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
    build.Inputs = inputs;
    build.DestAccelerationStructureData = m_tlas->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = m_tlasScratch->GetGPUVirtualAddress();
    if (m_topLevelBuilt) {
        // Keep every instance, including those culled from the camera's raster pass.
        build.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
        build.SourceAccelerationStructureData = build.DestAccelerationStructureData;
    }
    Transition(list, m_tlasScratch.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    UavBarrier(list, m_tlas.Get());
    Transition(list, m_tlasScratch.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    m_topLevelBuilt = true;
}

void RayTracedShadows::Run(ID3D12GraphicsCommandList* commandList, ID3D12Resource* vb,
    ID3D12Resource* ib, const D3D12_VERTEX_BUFFER_VIEW& vertexView,
    const D3D12_INDEX_BUFFER_VIEW& indexView, UINT indexCount,
    const std::vector<DirectX::XMFLOAT4X4>& worlds, const RayTracedShadowConstants& constants)
{
    if (!m_supported || !m_raw || !m_filtered) return;
    if (!commandList) throw std::invalid_argument("Missing ray-traced shadow command list.");
    if (constants.ImageSize.x != m_width || constants.ImageSize.y != m_height)
        throw std::invalid_argument("Shadow constants do not match the render target size.");

    ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    Transition(commandList, m_raw.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(commandList, m_filtered.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (worlds.empty()) {
        const float unshadowed[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        auto clearHandle = m_clearHeap->GetCPUDescriptorHandleForHeapStart();
        commandList->ClearUnorderedAccessViewFloat(GpuHandle(3), clearHandle, m_raw.Get(), unshadowed, 0, nullptr);
        clearHandle.ptr += m_descriptorSize;
        commandList->ClearUnorderedAccessViewFloat(GpuHandle(4), clearHandle, m_filtered.Get(), unshadowed, 0, nullptr);
        Transition(commandList, m_raw.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(commandList, m_filtered.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return;
    }

    ComPtr<ID3D12GraphicsCommandList4> list;
    ThrowIfFailed(commandList->QueryInterface(IID_PPV_ARGS(&list)));
    if (m_geometryVertexAddress != vertexView.BufferLocation
        || m_geometryIndexAddress != indexView.BufferLocation || m_geometryIndexCount != indexCount) {
        InvalidateGeometry();
    }
    if (!m_blas) BuildBottomLevel(list.Get(), vb, ib, vertexView, indexView, indexCount);
    UpdateTopLevel(list.Get(), worlds);
    m_constants->CopyData(0, constants);
    commandList->SetComputeRootSignature(m_rootSignature.Get());
    commandList->SetComputeRootConstantBufferView(0, m_constants->Resource()->GetGPUVirtualAddress());
    commandList->SetComputeRootShaderResourceView(1, m_tlas->GetGPUVirtualAddress());
    commandList->SetComputeRootDescriptorTable(2, GpuHandle(0));
    commandList->SetComputeRootDescriptorTable(3, GpuHandle(3));
    commandList->SetPipelineState(m_tracePso.Get());
    commandList->Dispatch((m_width + 7u) / 8u, (m_height + 7u) / 8u, 1);
    Transition(commandList, m_raw.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commandList->SetPipelineState(m_filterPso.Get());
    commandList->Dispatch((m_width + 7u) / 8u, (m_height + 7u) / 8u, 1);
    Transition(commandList, m_filtered.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(commandList, m_raw.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}
