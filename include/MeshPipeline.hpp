#ifndef MESH_PIPELINE_HPP
#define MESH_PIPELINE_HPP

#include "MeshletModel.hpp"
#include "UploadBuffer.hpp"
#include <array>
#include <memory>

class MeshPipeline {
public:
    // The caller waits for previous draws before reloading. Replacement is atomic:
    // failed validation or I/O leaves the previously loaded GPU model available.
    void Load(ID3D12Device* device, ID3D12CommandQueue* queue, bool useDirectStorage);
    bool Loaded() const { return m_texture != nullptr; }
    bool Supported() const { return m_supported; }
    bool UsesDirectStorage() const { return m_usesDirectStorage; }
    double LoadMilliseconds() const { return m_loadMilliseconds; }
    const meshlet::Model& Model() const { return m_model; }

    // Binds its own descriptor heap and graphics root signature. The caller restores
    // its usual heap before subsequent passes. Object matrices follow ObjectConstants.
    void Draw(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS passCb,
        const ObjectConstants& object, bool useMesh, bool colors, bool single, UINT selected);

private:
    enum BufferIndex { VertexData, IndexData, MeshletData, UniqueData, TriangleData, BufferCount };
    void Initialize(ID3D12Device* device);

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12RootSignature> m_root;
    ComPtr<ID3D12PipelineState> m_meshPso;
    ComPtr<ID3D12PipelineState> m_classicPso;
    std::unique_ptr<UploadBuffer<ObjectConstants>> m_objectCb;
    std::array<ComPtr<ID3D12Resource>, BufferCount> m_buffers;
    ComPtr<ID3D12Resource> m_texture;
    ComPtr<ID3D12DescriptorHeap> m_heap;
    meshlet::Model m_model;
    double m_loadMilliseconds = 0.0;
    bool m_supported = false;
    bool m_usesDirectStorage = false;
};

#endif // MESH_PIPELINE_HPP
