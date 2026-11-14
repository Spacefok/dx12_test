#ifndef MESHLET_MODEL_HPP
#define MESHLET_MODEL_HPP

#include "RenderStructs.hpp"
#include <filesystem>
#include <vector>

namespace meshlet {

constexpr std::uint32_t FileMagic = 0x3548534d; // MSH5, little endian.
constexpr std::uint32_t FileVersion = 1;
constexpr std::uint32_t MaxVertices = 64;
constexpr std::uint32_t MaxTriangles = 126;

struct Header {
    std::uint32_t Magic = FileMagic;
    std::uint32_t Version = FileVersion;
    std::uint32_t VertexCount = 0;
    std::uint32_t TriangleCount = 0;
    std::uint32_t MeshletCount = 0;
    std::uint32_t UniqueIndexCount = 0;
    std::uint32_t VertexStride = sizeof(Vertex);
    std::uint32_t MaxVertices = meshlet::MaxVertices;
    std::uint32_t MaxTriangles = meshlet::MaxTriangles;
    std::uint32_t TextureWidth = 256;
    std::uint32_t TextureHeight = 256;
    std::uint32_t Reserved = 0;
    std::uint64_t VertexOffset = 0;
    std::uint64_t IndexOffset = 0;
    std::uint64_t MeshletOffset = 0;
    std::uint64_t UniqueIndexOffset = 0;
    std::uint64_t TriangleOffset = 0;
    std::uint64_t FileBytes = 0;
    std::uint64_t Padding[4] = {};
};

struct Desc {
    std::uint32_t VertexOffset = 0;
    std::uint32_t VertexCount = 0;
    std::uint32_t TriangleOffset = 0;
    std::uint32_t TriangleCount = 0;
};

struct Model {
    Header header;
    std::vector<Vertex> Vertices;
    std::vector<std::uint32_t> Indices;
    std::vector<Desc> Meshlets;
    std::vector<std::uint32_t> UniqueIndices;
    std::vector<std::uint32_t> Triangles;
};

static_assert(sizeof(Vertex) == 60 && offsetof(Vertex, Tangent) == 48, "Mesh vertex format mismatch.");
static_assert(sizeof(Header) == 128 && offsetof(Header, VertexOffset) == 48
    && offsetof(Header, FileBytes) == 88, "Mesh file header layout mismatch.");
static_assert(sizeof(Desc) == 16, "Meshlet descriptor must match the HLSL layout.");

// Reject malformed files and prove that expanding local indices reproduces every
// original indexed triangle in order before either rendering path uses the data.
Model LoadAndValidate(const std::filesystem::path& path);

} // namespace meshlet
#endif // MESHLET_MODEL_HPP
