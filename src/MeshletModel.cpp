#include "MeshletModel.hpp"
#include <array>
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace meshlet {
namespace {
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class T>
std::vector<T> ReadSection(std::ifstream& input, std::uint64_t offset, std::uint64_t count)
{
    std::vector<T> values(static_cast<std::size_t>(count));
    input.seekg(static_cast<std::streamoff>(offset));
    input.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(count * sizeof(T)));
    Require(static_cast<bool>(input), "Cannot read mesh geometry section.");
    return values;
}
}

Model LoadAndValidate(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    Require(static_cast<bool>(input), "Cannot open mesh model; run from the repository root.");
    const auto end = input.tellg();
    Require(end >= static_cast<std::streamoff>(sizeof(Header)) && end <= 512ll * 1024 * 1024,
        "Mesh model file size is invalid.");
    Model model;
    auto& header = model.header;
    input.seekg(0);
    input.read(reinterpret_cast<char*>(&header), sizeof(header));
    Require(static_cast<bool>(input), "Cannot read mesh file header.");
    Require(header.Magic == FileMagic && header.Version == FileVersion, "Unsupported mesh file format.");
    Require(header.VertexStride == sizeof(Vertex) && header.MaxVertices == MaxVertices
        && header.MaxTriangles == MaxTriangles, "Mesh file limits do not match the shader.");
    Require(header.FileBytes == static_cast<std::uint64_t>(end), "Mesh file size does not match its header.");
    Require(header.VertexCount > 0 && header.TriangleCount > 0 && header.MeshletCount > 0
        && header.UniqueIndexCount > 0, "Mesh model contains no geometry.");
    Require(header.TextureWidth == 256 && header.TextureHeight == 256,
        "Mesh texture dimensions do not match the demonstration format.");
    Require(header.Reserved == 0, "Invalid reserved mesh header field.");
    for (const auto padding : header.Padding) Require(padding == 0, "Invalid mesh header padding.");

    const std::array<std::uint64_t, 5> offsets = { header.VertexOffset, header.IndexOffset,
        header.MeshletOffset, header.UniqueIndexOffset, header.TriangleOffset };
    const std::array<std::uint64_t, 5> sizes = {
        std::uint64_t(header.VertexCount) * sizeof(Vertex),
        std::uint64_t(header.TriangleCount) * 3 * sizeof(std::uint32_t),
        std::uint64_t(header.MeshletCount) * sizeof(Desc),
        std::uint64_t(header.UniqueIndexCount) * sizeof(std::uint32_t),
        std::uint64_t(header.TriangleCount) * sizeof(std::uint32_t) };
    std::uint64_t previousEnd = sizeof(Header);
    for (std::size_t section = 0; section < offsets.size(); ++section) {
        Require(offsets[section] % 512 == 0 && offsets[section] >= previousEnd
            && offsets[section] <= header.FileBytes && sizes[section] <= header.FileBytes - offsets[section],
            "Mesh file section is misaligned, overlapping or out of bounds.");
        previousEnd = offsets[section] + sizes[section];
    }
    Require(previousEnd == header.FileBytes, "Unexpected data after mesh file sections.");

    model.Vertices = ReadSection<Vertex>(input, header.VertexOffset, header.VertexCount);
    model.Indices = ReadSection<std::uint32_t>(input, header.IndexOffset, std::uint64_t(header.TriangleCount) * 3);
    model.Meshlets = ReadSection<Desc>(input, header.MeshletOffset, header.MeshletCount);
    model.UniqueIndices = ReadSection<std::uint32_t>(input, header.UniqueIndexOffset, header.UniqueIndexCount);
    model.Triangles = ReadSection<std::uint32_t>(input, header.TriangleOffset, header.TriangleCount);
    for (const auto& vertex : model.Vertices) {
        const float fields[] = { vertex.Pos.x, vertex.Pos.y, vertex.Pos.z,
            vertex.Normal.x, vertex.Normal.y, vertex.Normal.z,
            vertex.Color.x, vertex.Color.y, vertex.Color.z, vertex.Color.w,
            vertex.TexC.x, vertex.TexC.y, vertex.Tangent.x, vertex.Tangent.y, vertex.Tangent.z };
        for (float value : fields) Require(std::isfinite(value), "Mesh vertex contains a non-finite attribute.");
    }
    for (const auto index : model.Indices) Require(index < header.VertexCount, "Original mesh index is out of bounds.");
    for (const auto index : model.UniqueIndices) Require(index < header.VertexCount, "Meshlet global index is out of bounds.");

    std::uint64_t totalVertices = 0;
    std::uint64_t totalTriangles = 0;
    for (const auto& block : model.Meshlets) {
        Require(block.VertexCount > 0 && block.VertexCount <= MaxVertices
            && block.TriangleCount > 0 && block.TriangleCount <= MaxTriangles, "Meshlet exceeds shader output limits.");
        Require(block.VertexOffset == totalVertices && block.TriangleOffset == totalTriangles
            && totalVertices + block.VertexCount <= model.UniqueIndices.size()
            && totalTriangles + block.TriangleCount <= model.Triangles.size(),
            "Meshlet spans overlap or omit geometry.");
        for (std::uint32_t localTriangle = 0; localTriangle < block.TriangleCount; ++localTriangle) {
            const std::uint64_t triangle = totalTriangles + localTriangle;
            const std::uint32_t packed = model.Triangles[static_cast<std::size_t>(triangle)];
            Require((packed & 0xff000000u) == 0, "Meshlet triangle has unexpected packed bits.");
            for (std::uint32_t corner = 0; corner < 3; ++corner) {
                const std::uint32_t local = (packed >> (corner * 8)) & 0xffu;
                Require(local < block.VertexCount, "Meshlet local triangle index is out of bounds.");
                Require(model.UniqueIndices[block.VertexOffset + local]
                    == model.Indices[static_cast<std::size_t>(triangle * 3 + corner)],
                    "Expanded meshlet triangle differs from the original model.");
            }
        }
        totalVertices += block.VertexCount;
        totalTriangles += block.TriangleCount;
    }
    Require(totalVertices == header.UniqueIndexCount && totalTriangles == header.TriangleCount,
        "Meshlet totals do not cover the complete original model.");
    return model;
}
} // namespace meshlet
