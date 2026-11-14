#include "MeshPipeline.hpp"
#include "DirectStorageLoader.hpp"
#include "Gbuffer.hpp"
#include "MeshPipelineMS.h"
#include "MeshPipelineVS.h"
#include "MeshPipelinePS.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>

namespace {
template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, class T>
struct alignas(void*) StreamObject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Kind = Type;
    T Value = {};
};

struct MeshStateStream {
    StreamObject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> Root;
    StreamObject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> Mesh;
    StreamObject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> Pixel;
    StreamObject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> Blend;
    StreamObject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> Raster;
    StreamObject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> Depth;
    StreamObject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> Targets;
    StreamObject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT> DepthFormat;
    StreamObject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> Samples;
    StreamObject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> SampleMask;
    StreamObject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> Topology;
};

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

ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    ThrowIfFailed(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&buffer)));
    return buffer;
}

void FillUpload(ID3D12Resource* resource, const void* data, std::size_t bytes)
{
    const D3D12_RANGE noRead = { 0, 0 };
    void* mapped = nullptr;
    ThrowIfFailed(resource->Map(0, &noRead, &mapped));
    std::memcpy(mapped, data, bytes);
    resource->Unmap(0, nullptr);
}

void SubmitAndWait(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list)
{
    ComPtr<ID3D12Fence> fence;
    ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    struct EventCloser { void operator()(void* value) const { if (value) CloseHandle(value); } };
    std::unique_ptr<void, EventCloser> event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!event) throw std::runtime_error("Cannot create mesh upload completion event.");
    ThrowIfFailed(list->Close());
    ID3D12CommandList* lists[] = { list };
    queue->ExecuteCommandLists(1, lists);
    ThrowIfFailed(queue->Signal(fence.Get(), 1));
    ThrowIfFailed(fence->SetEventOnCompletion(1, event.get()));
    if (WaitForSingleObject(event.get(), INFINITE) != WAIT_OBJECT_0)
        throw std::runtime_error("Mesh upload completion wait failed.");
    ThrowIfFailed(device->GetDeviceRemovedReason());
}
}

void MeshPipeline::Initialize(ID3D12Device* device)
{
    if (m_device) {
        if (m_device.Get() != device) throw std::invalid_argument("Mesh pipeline device changed.");
        return;
    }
    D3D12_FEATURE_DATA_SHADER_MODEL model = { D3D_SHADER_MODEL_6_5 };
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)))
        || model.HighestShaderModel < D3D_SHADER_MODEL_6_5)
        throw std::runtime_error("Mesh Pipeline Lab requires Shader Model 6.5, including its indexed reference shaders.");
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options = {};
    m_supported = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options, sizeof(options)))
        && options.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1;

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 8;
    D3D12_ROOT_PARAMETER roots[5] = {};
    roots[0].ParameterType = roots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    roots[0].Descriptor.ShaderRegister = 0;
    roots[1].Descriptor.ShaderRegister = 1;
    roots[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    roots[2].Constants = { 2, 0, sizeof(MaterialConstants) / sizeof(UINT) };
    roots[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    roots[3].DescriptorTable = { 1, &range };
    roots[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    roots[4].Constants = { 3, 0, 4 };
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC signature = {};
    signature.NumParameters = 5;
    signature.pParameters = roots;
    signature.NumStaticSamplers = 1;
    signature.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> serialized, errors;
    const HRESULT result = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
    ThrowIfFailed(result);
    ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_root)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC classic = {};
    classic.pRootSignature = m_root.Get();
    classic.VS = { g_MeshPipelineVS, sizeof(g_MeshPipelineVS) };
    classic.PS = { g_MeshPipelinePS, sizeof(g_MeshPipelinePS) };
    classic.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    classic.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    classic.RasterizerState.DepthClipEnable = TRUE;
    for (auto& blend : classic.BlendState.RenderTarget) {
        blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    classic.DepthStencilState.DepthEnable = TRUE;
    classic.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    classic.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    classic.DepthStencilState.FrontFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
        D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
    classic.DepthStencilState.BackFace = classic.DepthStencilState.FrontFace;
    classic.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    classic.SampleMask = UINT(-1);
    classic.SampleDesc.Count = 1;
    classic.NumRenderTargets = Gbuffer::kTargetCount;
    classic.RTVFormats[0] = Gbuffer::kAlbedoFormat;
    classic.RTVFormats[1] = Gbuffer::kNormalFormat;
    classic.RTVFormats[2] = Gbuffer::kPositionSpecFormat;
    classic.RTVFormats[3] = Gbuffer::kVelocityFormat;
    classic.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    ThrowIfFailed(device->CreateGraphicsPipelineState(&classic, IID_PPV_ARGS(&m_classicPso)));
    if (m_supported) {
        MeshStateStream stream;
        stream.Root.Value = m_root.Get();
        stream.Mesh.Value = { g_MeshPipelineMS, sizeof(g_MeshPipelineMS) };
        stream.Pixel.Value = classic.PS;
        stream.Blend.Value = classic.BlendState;
        stream.Raster.Value = classic.RasterizerState;
        stream.Depth.Value = classic.DepthStencilState;
        stream.Targets.Value.NumRenderTargets = classic.NumRenderTargets;
        std::memcpy(stream.Targets.Value.RTFormats, classic.RTVFormats, sizeof(classic.RTVFormats));
        stream.DepthFormat.Value = classic.DSVFormat;
        stream.Samples.Value = classic.SampleDesc;
        stream.SampleMask.Value = classic.SampleMask;
        stream.Topology.Value = classic.PrimitiveTopologyType;
        ComPtr<ID3D12Device2> device2;
        ThrowIfFailed(device->QueryInterface(IID_PPV_ARGS(&device2)));
        D3D12_PIPELINE_STATE_STREAM_DESC desc = { sizeof(stream), &stream };
        ThrowIfFailed(device2->CreatePipelineState(&desc, IID_PPV_ARGS(&m_meshPso)));
    }
    m_objectCb = std::make_unique<UploadBuffer<ObjectConstants>>(device, 1, true);
    m_device = device;
}

void MeshPipeline::Load(ID3D12Device* device, ID3D12CommandQueue* queue, bool useDirectStorage)
{
    const auto start = std::chrono::steady_clock::now();
    if (!device || !queue) throw std::invalid_argument("Mesh loading needs a device and graphics queue.");
    Initialize(device);
    const std::filesystem::path geometryPath = L"assets/mesh_pipeline/model.bin";
    const std::filesystem::path texturePath = L"assets/mesh_pipeline/albedo.rgba";
    auto model = meshlet::LoadAndValidate(geometryPath);
    const auto& header = model.header;
    const UINT64 sizes[BufferCount] = { model.Vertices.size() * sizeof(Vertex), model.Indices.size() * sizeof(UINT),
        model.Meshlets.size() * sizeof(meshlet::Desc), model.UniqueIndices.size() * sizeof(UINT), model.Triangles.size() * sizeof(UINT) };
    const UINT64 offsets[BufferCount] = { header.VertexOffset, header.IndexOffset, header.MeshletOffset,
        header.UniqueIndexOffset, header.TriangleOffset };
    std::array<ComPtr<ID3D12Resource>, BufferCount> buffers;
    for (UINT index = 0; index < BufferCount; ++index) buffers[index] = Buffer(device, sizes[index], D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES heapProperties = {};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CreationNodeMask = heapProperties.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = header.TextureWidth;
    textureDesc.Height = header.TextureHeight;
    textureDesc.DepthOrArraySize = textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> texture;
    ThrowIfFailed(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &textureDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture)));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 textureBytes = 0;
    device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, nullptr, nullptr, &textureBytes);
    if (textureBytes != UINT64(header.TextureWidth) * header.TextureHeight * 4)
        throw std::runtime_error("Prepared texture row layout does not match the GPU footprint.");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    std::array<ComPtr<ID3D12Resource>, BufferCount + 1> uploads;
    if (useDirectStorage) {
        std::vector<storage::BufferRequest> requests;
        for (UINT index = 0; index < BufferCount; ++index)
            requests.push_back({ offsets[index], static_cast<UINT>(sizes[index]), buffers[index].Get() });
        storage::Load(device, geometryPath, texturePath, requests, texture.Get(), static_cast<UINT>(textureBytes));
        for (UINT index = 0; index < BufferCount; ++index)
            Transition(list.Get(), buffers[index].Get(), D3D12_RESOURCE_STATE_COMMON,
                index == IndexData ? D3D12_RESOURCE_STATE_INDEX_BUFFER : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    else {
        const void* source[BufferCount] = { model.Vertices.data(), model.Indices.data(), model.Meshlets.data(),
            model.UniqueIndices.data(), model.Triangles.data() };
        for (UINT index = 0; index < BufferCount; ++index) {
            uploads[index] = Buffer(device, sizes[index], D3D12_HEAP_TYPE_UPLOAD);
            FillUpload(uploads[index].Get(), source[index], static_cast<std::size_t>(sizes[index]));
            Transition(list.Get(), buffers[index].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(buffers[index].Get(), 0, uploads[index].Get(), 0, sizes[index]);
            Transition(list.Get(), buffers[index].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                index == IndexData ? D3D12_RESOURCE_STATE_INDEX_BUFFER : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        std::ifstream input(texturePath, std::ios::binary | std::ios::ate);
        if (!input || input.tellg() != static_cast<std::streamoff>(textureBytes))
            throw std::runtime_error("Mesh texture is missing or has an invalid size.");
        std::vector<char> pixels(static_cast<std::size_t>(textureBytes));
        input.seekg(0);
        input.read(pixels.data(), static_cast<std::streamsize>(pixels.size()));
        if (!input) throw std::runtime_error("Cannot read mesh texture.");
        uploads[BufferCount] = Buffer(device, textureBytes, D3D12_HEAP_TYPE_UPLOAD);
        FillUpload(uploads[BufferCount].Get(), pixels.data(), pixels.size());
        Transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION sourceLocation = {};
        sourceLocation.pResource = uploads[BufferCount].Get();
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = texture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&destination, 0, 0, 0, &sourceLocation, nullptr);
        Transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    SubmitAndWait(device, queue, list.Get());

    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 8;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)));
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    const UINT increment = device->GetDescriptorHandleIncrementSize(heapDesc.Type);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = textureDesc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    for (UINT slot = 0; slot < 4; ++slot) {
        device->CreateShaderResourceView(slot == 0 ? texture.Get() : nullptr, &srv, handle);
        handle.ptr += increment;
    }
    const UINT resourceIndices[4] = { VertexData, MeshletData, UniqueData, TriangleData };
    const UINT strides[4] = { sizeof(Vertex), sizeof(meshlet::Desc), sizeof(UINT), sizeof(UINT) };
    for (UINT slot = 0; slot < 4; ++slot) {
        srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Buffer.StructureByteStride = strides[slot];
        srv.Buffer.NumElements = static_cast<UINT>(sizes[resourceIndices[slot]] / strides[slot]);
        device->CreateShaderResourceView(buffers[resourceIndices[slot]].Get(), &srv, handle);
        handle.ptr += increment;
    }
    m_buffers = std::move(buffers);
    m_texture = std::move(texture);
    m_heap = std::move(heap);
    m_model = std::move(model);
    m_usesDirectStorage = useDirectStorage;
    m_loadMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void MeshPipeline::Draw(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS passCb,
    const ObjectConstants& object, bool useMesh, bool colors, bool single, UINT selected)
{
    if (!Loaded()) return;
    m_objectCb->CopyData(0, object);
    ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(m_root.Get());
    list->SetGraphicsRootConstantBufferView(0, m_objectCb->Resource()->GetGPUVirtualAddress());
    list->SetGraphicsRootConstantBufferView(1, passCb);
    MaterialConstants material;
    material.Flags = colors ? 0 : MaterialFlagHasBaseColorTexture;
    material.AlphaCutoff = 0.0f;
    list->SetGraphicsRoot32BitConstants(2, sizeof(material) / sizeof(UINT), &material, 0);
    list->SetGraphicsRootDescriptorTable(3, m_heap->GetGPUDescriptorHandleForHeapStart());
    selected = (std::min)(selected, static_cast<UINT>(m_model.Meshlets.size() - 1));
    const UINT begin = single ? selected : 0;
    const UINT count = single ? 1 : static_cast<UINT>(m_model.Meshlets.size());
    auto SetDraw = [&](UINT first, UINT batch) {
        const UINT values[4] = { first, batch, colors ? 1u : 0u, 0u };
        list->SetGraphicsRoot32BitConstants(4, 4, values, 0);
    };
    if (useMesh && m_supported) {
        ComPtr<ID3D12GraphicsCommandList6> meshList;
        ThrowIfFailed(list->QueryInterface(IID_PPV_ARGS(&meshList)));
        list->SetPipelineState(m_meshPso.Get());
        for (UINT offset = 0; offset < count; offset += 65535) {
            const UINT batch = (std::min)(65535u, count - offset);
            SetDraw(begin + offset, batch);
            meshList->DispatchMesh(batch, 1, 1);
        }
    }
    else {
        list->SetPipelineState(m_classicPso.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_INDEX_BUFFER_VIEW view = { m_buffers[IndexData]->GetGPUVirtualAddress(),
            static_cast<UINT>(m_model.Indices.size() * sizeof(UINT)), DXGI_FORMAT_R32_UINT };
        list->IASetIndexBuffer(&view);
        if (single || colors) {
            for (UINT index = begin; index < begin + count; ++index) {
                const auto& block = m_model.Meshlets[index];
                SetDraw(index, 1);
                list->DrawIndexedInstanced(block.TriangleCount * 3, 1, block.TriangleOffset * 3, 0, 0);
            }
        }
        else {
            SetDraw(0, count);
            list->DrawIndexedInstanced(static_cast<UINT>(m_model.Indices.size()), 1, 0, 0, 0);
        }
    }
}
