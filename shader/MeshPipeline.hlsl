#include "DeferredGeometry.hlsl"

struct MeshletDesc
{
    uint VertexOffset;
    uint VertexCount;
    uint TriangleOffset;
    uint TriangleCount;
};

StructuredBuffer<VertexIn> gMeshVertices : register(t4);
StructuredBuffer<MeshletDesc> gMeshlets : register(t5);
StructuredBuffer<uint> gMeshletVertices : register(t6);
StructuredBuffer<uint> gMeshletTriangles : register(t7);

cbuffer MeshDrawCB : register(b3)
{
    uint gFirstMeshlet;
    uint gMeshletCount;
    uint gColorMeshlets;
    uint gMeshDrawPadding;
};

float3 MeshletColor(uint meshletIndex)
{
    uint value = meshletIndex + 1u;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return 0.20f + 0.75f * float3(value & 255u,
        (value >> 8) & 255u, (value >> 16) & 255u) / 255.0f;
}

GeometryPSInput MakeMeshVertex(uint vertexIndex, uint meshletIndex)
{
    // Use the ordinary geometry path's transforms, UVs and motion-vector clips.
    ControlPointData controlPoint = BuildControlPointData(gMeshVertices[vertexIndex]);
    GeometryPSInput output = MakeGeometryOutput(controlPoint, controlPoint.PosW, controlPoint.PreviousPosW,
        ComputeMaterialUv(controlPoint.BaseTexC), 0.0f, 0.0f);
    if (gColorMeshlets != 0u)
        output.Color = float4(MeshletColor(meshletIndex), 1.0f);
    return output;
}

[numthreads(64, 1, 1)]
[outputtopology("triangle")]
void MSMain(uint3 groupId : SV_GroupID, uint threadIndex : SV_GroupIndex,
    out vertices GeometryPSInput vertices[64], out indices uint3 triangles[126])
{
    uint meshletIndex = gFirstMeshlet + groupId.x;
    MeshletDesc meshlet = (MeshletDesc)0;
    if (groupId.x < gMeshletCount)
        meshlet = gMeshlets[meshletIndex];

    // Uniform counts are set once, before any output write. CPU validation
    // guarantees the descriptor fits these fixed vertex/primitive limits.
    SetMeshOutputCounts(meshlet.VertexCount, meshlet.TriangleCount);
    if (threadIndex < meshlet.VertexCount)
    {
        uint vertexIndex = gMeshletVertices[meshlet.VertexOffset + threadIndex];
        vertices[threadIndex] = MakeMeshVertex(vertexIndex, meshletIndex);
    }
    for (uint primitiveIndex = threadIndex; primitiveIndex < meshlet.TriangleCount; primitiveIndex += 64u)
    {
        uint packed = gMeshletTriangles[meshlet.TriangleOffset + primitiveIndex];
        triangles[primitiveIndex] = uint3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u);
    }
}

// The indexed reference path fetches the same original vertex without an IA
// layout. For diagnostic colors the caller issues one indexed range per meshlet.
GeometryPSInput VSReference(uint vertexIndex : SV_VertexID)
{
    return MakeMeshVertex(vertexIndex, gFirstMeshlet);
}
