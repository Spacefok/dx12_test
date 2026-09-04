#ifndef TERRAIN_HPP
#define TERRAIN_HPP

#include "RenderStructs.hpp"

#include <array>
#include <filesystem>
#include <limits>
#include <vector>

namespace terrain {

struct Settings {
    unsigned TilesX = 2;
    unsigned TilesZ = 2;
    unsigned TileSamples = 129;
    unsigned PatchCells = 16;
    float WorldSize = 64.0f;
    float HeightScale = 14.0f;
    float SkirtDepth = 2.0f;
};

struct Node {
    static constexpr unsigned NoChild = (std::numeric_limits<unsigned>::max)();

    DirectX::XMFLOAT3 Min = {};
    DirectX::XMFLOAT3 Max = {};
    unsigned StartVertex = 0;
    unsigned VertexCount = 0;
    unsigned Depth = 0;
    unsigned SampleX = 0;
    unsigned SampleZ = 0;
    unsigned SampleCells = 0;
    std::array<unsigned, 4> Children = { NoChild, NoChild, NoChild, NoChild };
    float Error = 0.0f;
};

struct Stats {
    unsigned SelectedPatches = 0;
    unsigned VisitedNodes = 0;
    unsigned CulledNodes = 0;
    unsigned Triangles = 0;
    unsigned MinDepth = 0;
    unsigned MaxDepth = 0;
};

class Terrain {
public:
    // Tiles form a square power-of-two grid; adjacent RAW tiles share one sample.
    // Heights are unsigned little-endian 16-bit samples, stored in increasing Z rows.
    void Load(const std::filesystem::path& directory, const Settings& settings = {});

    // viewProjection is the untransposed DirectX row-vector matrix. projectionScale
    // is viewportHeight * projection._22 / 2, in pixels, for the active camera.
    void Select(const DirectX::XMFLOAT3& camera, DirectX::FXMMATRIX viewProjection,
        float projectionScale, float maxPixelError, bool frustumCulling);

    const std::vector<Vertex>& Vertices() const { return mVertices; }
    const std::vector<Node>& Nodes() const { return mNodes; }
    const std::vector<unsigned>& SelectedNodes() const { return mSelectedNodes; }
    const Stats& Statistics() const { return mStats; }
    bool Empty() const { return mNodes.empty(); }
    void Clear();

private:
    unsigned BuildNode(unsigned sampleX, unsigned sampleZ, unsigned sampleCells, unsigned depth);
    void BuildMesh(unsigned nodeIndex, float skirtDepth);
    Vertex MakeVertex(unsigned sampleX, unsigned sampleZ) const;
    float Height(unsigned sampleX, unsigned sampleZ) const;
    void SelectNode(unsigned nodeIndex, const DirectX::XMFLOAT3& camera,
        const std::array<DirectX::XMFLOAT4, 6>& planes, float projectionScale,
        float maxPixelError, bool frustumCulling);

    Settings mSettings;
    unsigned mSamples = 0;
    float mSpacing = 0.0f;
    std::vector<float> mHeights;
    std::vector<Vertex> mVertices;
    std::vector<Node> mNodes;
    std::vector<unsigned> mSelectedNodes;
    Stats mStats;
};

} // namespace terrain

#endif // TERRAIN_HPP
