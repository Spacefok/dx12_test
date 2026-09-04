#include "Terrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace terrain {
namespace {

bool IsPowerOfTwo(unsigned value) {
    return value != 0 && (value & (value - 1)) == 0;
}

float Clamp01(float value) {
    return (std::max)(0.0f, (std::min)(1.0f, value));
}

DirectX::XMFLOAT4 Mix(const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b, float amount) {
    amount = Clamp01(amount);
    return { a.x + (b.x - a.x) * amount, a.y + (b.y - a.y) * amount,
        a.z + (b.z - a.z) * amount, 1.0f };
}

bool Outside(const Node& node, const std::array<DirectX::XMFLOAT4, 6>& planes) {
    for (const auto& plane : planes) {
        const float x = plane.x >= 0.0f ? node.Max.x : node.Min.x;
        const float y = plane.y >= 0.0f ? node.Max.y : node.Min.y;
        const float z = plane.z >= 0.0f ? node.Max.z : node.Min.z;
        if (plane.x * x + plane.y * y + plane.z * z + plane.w < -0.001f) {
            return true;
        }
    }
    return false;
}

std::array<DirectX::XMFLOAT4, 6> ExtractPlanes(DirectX::FXMMATRIX viewProjection) {
    DirectX::XMFLOAT4X4 m;
    DirectX::XMStoreFloat4x4(&m, viewProjection);
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned column = 0; column < 4; ++column) {
            if (!std::isfinite(m.m[row][column])) {
                throw std::invalid_argument("Terrain view-projection matrix must be finite.");
            }
        }
    }

    // Row-vector convention: -w <= x,y <= w and 0 <= z <= w in D3D clip space.
    std::array<DirectX::XMFLOAT4, 6> planes = {{
        { m._14 + m._11, m._24 + m._21, m._34 + m._31, m._44 + m._41 },
        { m._14 - m._11, m._24 - m._21, m._34 - m._31, m._44 - m._41 },
        { m._14 + m._12, m._24 + m._22, m._34 + m._32, m._44 + m._42 },
        { m._14 - m._12, m._24 - m._22, m._34 - m._32, m._44 - m._42 },
        { m._13, m._23, m._33, m._43 },
        { m._14 - m._13, m._24 - m._23, m._34 - m._33, m._44 - m._43 }
    }};
    for (auto& plane : planes) {
        const float length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        if (length > 0.000001f) {
            plane.x /= length;
            plane.y /= length;
            plane.z /= length;
            plane.w /= length;
        }
    }
    return planes;
}

} // namespace

void Terrain::Load(const std::filesystem::path& directory, const Settings& settings) {
    if (settings.TilesX != settings.TilesZ || !IsPowerOfTwo(settings.TilesX)
        || settings.TileSamples < 2 || !IsPowerOfTwo(settings.TileSamples - 1)
        || !IsPowerOfTwo(settings.PatchCells)) {
        throw std::invalid_argument("Terrain requires square power-of-two tile counts, "
            "power-of-two patch cells, and 2^n + 1 samples per tile.");
    }
    if (!std::isfinite(settings.WorldSize) || settings.WorldSize <= 0.0f
        || !std::isfinite(settings.HeightScale) || settings.HeightScale <= 0.0f
        || !std::isfinite(settings.SkirtDepth) || settings.SkirtDepth <= 0.0f) {
        throw std::invalid_argument("Terrain world size, height scale, and skirt depth must be positive and finite.");
    }

    const std::uint64_t cells = std::uint64_t(settings.TilesX) * (settings.TileSamples - 1);
    if (cells < settings.PatchCells || cells > 4096) {
        throw std::invalid_argument("Terrain grid must contain between PatchCells and 4096 cells per side.");
    }
    std::uint64_t nodeCount = 1;
    std::uint64_t levelNodes = 1;
    for (std::uint64_t remaining = cells; remaining > settings.PatchCells; remaining /= 2) {
        levelNodes *= 4;
        nodeCount += levelNodes;
    }
    const std::uint64_t verticesPerNode = std::uint64_t(settings.PatchCells) * settings.PatchCells * 6
        + std::uint64_t(settings.PatchCells) * 24;
    constexpr std::uint64_t MaxMeshBytes = 256ull * 1024 * 1024;
    if (nodeCount * verticesPerNode > MaxMeshBytes / sizeof(Vertex)) {
        throw std::invalid_argument("Terrain mesh exceeds the 256 MiB prebuilt vertex budget; "
            "reduce the heightmap resolution or increase PatchCells.");
    }

    // Build separately so a missing/corrupt tile leaves any currently loaded terrain intact.
    Terrain loaded;
    loaded.mSettings = settings;
    loaded.mSamples = static_cast<unsigned>(cells + 1);
    loaded.mSpacing = settings.WorldSize / static_cast<float>(cells);
    const std::size_t sampleCount = std::size_t(loaded.mSamples) * loaded.mSamples;
    std::vector<std::uint16_t> stitched(sampleCount);
    std::vector<bool> assigned(sampleCount, false);
    const std::size_t tileBytes = std::size_t(settings.TileSamples) * settings.TileSamples * 2;
    std::vector<unsigned char> bytes(tileBytes);

    for (unsigned tileZ = 0; tileZ < settings.TilesZ; ++tileZ) {
        for (unsigned tileX = 0; tileX < settings.TilesX; ++tileX) {
            const auto path = directory / ("height_" + std::to_string(tileX) + "_" + std::to_string(tileZ) + ".r16");
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input) {
                throw std::runtime_error("Cannot open terrain heightmap: " + path.string());
            }
            if (input.tellg() != static_cast<std::streamoff>(tileBytes)) {
                throw std::runtime_error("Terrain heightmap has an incorrect byte count: " + path.string());
            }
            input.seekg(0, std::ios::beg);
            if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(tileBytes))) {
                throw std::runtime_error("Cannot read terrain heightmap: " + path.string());
            }
            for (unsigned z = 0; z < settings.TileSamples; ++z) {
                for (unsigned x = 0; x < settings.TileSamples; ++x) {
                    const std::size_t tileOffset = (std::size_t(z) * settings.TileSamples + x) * 2;
                    const auto value = static_cast<std::uint16_t>(std::uint16_t(bytes[tileOffset])
                        | (std::uint16_t(bytes[tileOffset + 1]) << 8));
                    const unsigned sampleX = tileX * (settings.TileSamples - 1) + x;
                    const unsigned sampleZ = tileZ * (settings.TileSamples - 1) + z;
                    const std::size_t destination = std::size_t(sampleZ) * loaded.mSamples + sampleX;
                    if (assigned[destination] && stitched[destination] != value) {
                        throw std::runtime_error("Terrain tile borders do not match: " + path.string());
                    }
                    assigned[destination] = true;
                    stitched[destination] = value;
                }
            }
        }
    }

    loaded.mHeights.resize(sampleCount);
    for (std::size_t i = 0; i < sampleCount; ++i) {
        loaded.mHeights[i] = static_cast<float>(stitched[i]) / 65535.0f * settings.HeightScale;
    }
    loaded.mNodes.reserve(static_cast<std::size_t>(nodeCount));
    loaded.mVertices.reserve(static_cast<std::size_t>(nodeCount * verticesPerNode));
    loaded.mSelectedNodes.reserve(static_cast<std::size_t>(levelNodes));
    loaded.BuildNode(0, 0, static_cast<unsigned>(cells), 0);

    // Each adjacent approximation differs from the source by at most root.Error.
    // Twice that error therefore covers all possible inter-LOD edge mismatches.
    const float skirtDepth = (std::max)(settings.SkirtDepth, loaded.mNodes.front().Error * 2.0f + 0.01f);
    for (unsigned i = 0; i < loaded.mNodes.size(); ++i) {
        loaded.BuildMesh(i, skirtDepth);
    }
    *this = std::move(loaded);
}

float Terrain::Height(unsigned sampleX, unsigned sampleZ) const {
    return mHeights[std::size_t(sampleZ) * mSamples + sampleX];
}

unsigned Terrain::BuildNode(unsigned sampleX, unsigned sampleZ, unsigned sampleCells, unsigned depth) {
    const unsigned index = static_cast<unsigned>(mNodes.size());
    mNodes.emplace_back();
    Node node;
    node.SampleX = sampleX;
    node.SampleZ = sampleZ;
    node.SampleCells = sampleCells;
    node.Depth = depth;
    node.Min = { sampleX * mSpacing - mSettings.WorldSize * 0.5f,
        (std::numeric_limits<float>::max)(), sampleZ * mSpacing - mSettings.WorldSize * 0.5f };
    node.Max = { (sampleX + sampleCells) * mSpacing - mSettings.WorldSize * 0.5f,
        (std::numeric_limits<float>::lowest)(), (sampleZ + sampleCells) * mSpacing - mSettings.WorldSize * 0.5f };
    const unsigned stride = sampleCells / mSettings.PatchCells;

    for (unsigned z = 0; z <= sampleCells; ++z) {
        for (unsigned x = 0; x <= sampleCells; ++x) {
            const float height = Height(sampleX + x, sampleZ + z);
            node.Min.y = (std::min)(node.Min.y, height);
            node.Max.y = (std::max)(node.Max.y, height);
            if (stride == 1) {
                continue;
            }
            const unsigned cellX = (std::min)(x / stride, mSettings.PatchCells - 1);
            const unsigned cellZ = (std::min)(z / stride, mSettings.PatchCells - 1);
            const unsigned originX = sampleX + cellX * stride;
            const unsigned originZ = sampleZ + cellZ * stride;
            const float fx = static_cast<float>(x - cellX * stride) / stride;
            const float fz = static_cast<float>(z - cellZ * stride) / stride;
            const float h00 = Height(originX, originZ);
            const float h10 = Height(originX + stride, originZ);
            const float h01 = Height(originX, originZ + stride);
            const float h11 = Height(originX + stride, originZ + stride);
            const float approximation = fx + fz <= 1.0f
                ? h00 + fx * (h10 - h00) + fz * (h01 - h00)
                : h11 + (1.0f - fx) * (h01 - h11) + (1.0f - fz) * (h10 - h11);
            node.Error = (std::max)(node.Error, std::abs(height - approximation));
        }
    }

    if (sampleCells > mSettings.PatchCells) {
        const unsigned half = sampleCells / 2;
        node.Children = {
            BuildNode(sampleX, sampleZ, half, depth + 1),
            BuildNode(sampleX + half, sampleZ, half, depth + 1),
            BuildNode(sampleX, sampleZ + half, half, depth + 1),
            BuildNode(sampleX + half, sampleZ + half, half, depth + 1)
        };
        for (const auto child : node.Children) {
            node.Error = (std::max)(node.Error, mNodes[child].Error);
        }
    }
    mNodes[index] = node;
    return index;
}

Vertex Terrain::MakeVertex(unsigned sampleX, unsigned sampleZ) const {
    const unsigned left = sampleX == 0 ? 0 : sampleX - 1;
    const unsigned right = (std::min)(sampleX + 1, mSamples - 1);
    const unsigned back = sampleZ == 0 ? 0 : sampleZ - 1;
    const unsigned front = (std::min)(sampleZ + 1, mSamples - 1);
    const float dx = (Height(right, sampleZ) - Height(left, sampleZ)) / ((right - left) * mSpacing);
    const float dz = (Height(sampleX, front) - Height(sampleX, back)) / ((front - back) * mSpacing);
    const float inverseLength = 1.0f / std::sqrt(dx * dx + dz * dz + 1.0f);
    Vertex vertex;
    vertex.Pos = { sampleX * mSpacing - mSettings.WorldSize * 0.5f,
        Height(sampleX, sampleZ), sampleZ * mSpacing - mSettings.WorldSize * 0.5f };
    vertex.Normal = { -dx * inverseLength, inverseLength, -dz * inverseLength };
    const float tangentLength = std::sqrt(1.0f + dx * dx);
    vertex.Tangent = { 1.0f / tangentLength, dx / tangentLength, 0.0f };
    vertex.TexC = { static_cast<float>(sampleX) / (mSamples - 1), static_cast<float>(sampleZ) / (mSamples - 1) };

    const float height = vertex.Pos.y / mSettings.HeightScale;
    const DirectX::XMFLOAT4 sand = { 0.47f, 0.38f, 0.23f, 1.0f };
    const DirectX::XMFLOAT4 grass = { 0.14f, 0.28f, 0.085f, 1.0f };
    const DirectX::XMFLOAT4 rock = { 0.32f, 0.30f, 0.28f, 1.0f };
    const DirectX::XMFLOAT4 snow = { 0.84f, 0.89f, 0.92f, 1.0f };
    vertex.Color = Mix(sand, grass, (height - 0.10f) / 0.13f);
    vertex.Color = Mix(vertex.Color, rock, (1.0f - vertex.Normal.y - 0.12f) / 0.40f);
    vertex.Color = Mix(vertex.Color, snow, (height - 0.68f) / 0.17f * Clamp01((vertex.Normal.y - 0.35f) / 0.45f));
    return vertex;
}

void Terrain::BuildMesh(unsigned nodeIndex, float skirtDepth) {
    Node& node = mNodes[nodeIndex];
    node.StartVertex = static_cast<unsigned>(mVertices.size());
    const unsigned cells = mSettings.PatchCells;
    const unsigned stride = node.SampleCells / cells;
    std::vector<Vertex> grid(std::size_t(cells + 1) * (cells + 1));
    for (unsigned z = 0; z <= cells; ++z) {
        for (unsigned x = 0; x <= cells; ++x) {
            grid[std::size_t(z) * (cells + 1) + x] = MakeVertex(node.SampleX + x * stride, node.SampleZ + z * stride);
        }
    }
    auto at = [&](unsigned x, unsigned z) -> const Vertex& { return grid[std::size_t(z) * (cells + 1) + x]; };
    auto triangle = [&](const Vertex& a, const Vertex& b, const Vertex& c) {
        mVertices.push_back(a);
        mVertices.push_back(b);
        mVertices.push_back(c);
    };
    for (unsigned z = 0; z < cells; ++z) {
        for (unsigned x = 0; x < cells; ++x) {
            triangle(at(x, z), at(x, z + 1), at(x + 1, z));
            triangle(at(x + 1, z), at(x, z + 1), at(x + 1, z + 1));
        }
    }

    auto skirt = [&](const Vertex& a, const Vertex& b) {
        Vertex lowA = a;
        Vertex lowB = b;
        lowA.Pos.y -= skirtDepth;
        lowB.Pos.y -= skirtDepth;
        triangle(a, b, lowA);
        triangle(b, lowB, lowA);
    };
    // Clockwise perimeter viewed from above gives outward-facing vertical walls.
    for (unsigned i = 0; i < cells; ++i) {
        skirt(at(i, 0), at(i + 1, 0));
        skirt(at(cells, i), at(cells, i + 1));
        skirt(at(i + 1, cells), at(i, cells));
        skirt(at(0, i + 1), at(0, i));
    }
    node.VertexCount = static_cast<unsigned>(mVertices.size()) - node.StartVertex;
    node.Min.y -= skirtDepth;
}

void Terrain::Select(const DirectX::XMFLOAT3& camera, DirectX::FXMMATRIX viewProjection,
    float projectionScale, float maxPixelError, bool frustumCulling) {
    mSelectedNodes.clear();
    mStats = {};
    if (Empty()) {
        return;
    }
    if (!std::isfinite(camera.x) || !std::isfinite(camera.y) || !std::isfinite(camera.z)
        || !std::isfinite(projectionScale) || projectionScale <= 0.0f
        || !std::isfinite(maxPixelError) || maxPixelError <= 0.0f) {
        throw std::invalid_argument("Terrain selection requires a finite camera, positive projection scale, and positive pixel error.");
    }
    const auto planes = frustumCulling ? ExtractPlanes(viewProjection) : std::array<DirectX::XMFLOAT4, 6>{};
    mStats.MinDepth = Node::NoChild;
    SelectNode(0, camera, planes, projectionScale, maxPixelError, frustumCulling);
    if (mSelectedNodes.empty()) {
        mStats.MinDepth = 0;
    }
    mStats.SelectedPatches = static_cast<unsigned>(mSelectedNodes.size());
}

void Terrain::SelectNode(unsigned nodeIndex, const DirectX::XMFLOAT3& camera,
    const std::array<DirectX::XMFLOAT4, 6>& planes, float projectionScale,
    float maxPixelError, bool frustumCulling) {
    const Node& node = mNodes[nodeIndex];
    ++mStats.VisitedNodes;
    if (frustumCulling && Outside(node, planes)) {
        ++mStats.CulledNodes;
        return;
    }
    const float dx = (std::max)({ node.Min.x - camera.x, 0.0f, camera.x - node.Max.x });
    const float dy = (std::max)({ node.Min.y - camera.y, 0.0f, camera.y - node.Max.y });
    const float dz = (std::max)({ node.Min.z - camera.z, 0.0f, camera.z - node.Max.z });
    const float distance = (std::max)(std::sqrt(dx * dx + dy * dy + dz * dz), 0.001f);
    const float projectedError = node.Error * projectionScale / distance;
    if (node.Children[0] != Node::NoChild && projectedError > maxPixelError) {
        for (const unsigned child : node.Children) {
            SelectNode(child, camera, planes, projectionScale, maxPixelError, frustumCulling);
        }
        return;
    }
    mSelectedNodes.push_back(nodeIndex);
    mStats.Triangles += node.VertexCount / 3;
    mStats.MinDepth = (std::min)(mStats.MinDepth, node.Depth);
    mStats.MaxDepth = (std::max)(mStats.MaxDepth, node.Depth);
}

void Terrain::Clear() {
    *this = Terrain{};
}

} // namespace terrain
