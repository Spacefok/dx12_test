"""Generate the indexed torus knot, meshlet tables and raw RGBA texture for scene 9.

Only Python's standard library is needed. Triangle order is preserved so the
indexed reference path can draw precisely the same range as one meshlet.
"""

import argparse
import math
from pathlib import Path
import struct


MAX_VERTICES = 64
MAX_TRIANGLES = 126
LONGITUDINAL_SEGMENTS = 512
RADIAL_SEGMENTS = 128
TEXTURE_SIZE = 256


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def scale(a, value):
    return tuple(x * value for x in a)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def normalize(a):
    return scale(a, 1.0 / math.sqrt(dot(a, a)))


def curve_frame(t):
    radius = 2.0 + 0.65 * math.cos(3.0 * t)
    derivative = -1.95 * math.sin(3.0 * t)
    cosine, sine = math.cos(2.0 * t), math.sin(2.0 * t)
    center = (radius * cosine, radius * sine, 0.65 * math.sin(3.0 * t))
    tangent = normalize((derivative * cosine - 2.0 * radius * sine,
                         derivative * sine + 2.0 * radius * cosine,
                         1.95 * math.cos(3.0 * t)))
    radial = (cosine, sine, 0.0)
    normal = normalize(sub(radial, scale(tangent, dot(radial, tangent))))
    return center, tangent, normal, cross(tangent, normal)


def surface(t, angle):
    center, _, normal, binormal = curve_frame(t)
    direction = add(scale(normal, math.cos(angle)), scale(binormal, math.sin(angle)))
    return add(center, scale(direction, 0.28)), direction


def make_model():
    vertices = []
    for longitudinal in range(LONGITUDINAL_SEGMENTS + 1):
        # The duplicate last row/column uses the exact first-row position,
        # while UVs retain their seam values.
        t = 2.0 * math.pi * (longitudinal % LONGITUDINAL_SEGMENTS) / LONGITUDINAL_SEGMENTS
        for radial in range(RADIAL_SEGMENTS + 1):
            angle = 2.0 * math.pi * (radial % RADIAL_SEGMENTS) / RADIAL_SEGMENTS
            position, normal = surface(t, angle)
            tangent = normalize(sub(surface(t + 0.0001, angle)[0], surface(t - 0.0001, angle)[0]))
            vertices.append((*position, *normal, 1.0, 1.0, 1.0, 1.0,
                             longitudinal / LONGITUDINAL_SEGMENTS * 8.0,
                             radial / RADIAL_SEGMENTS * 2.0, *tangent))
    indices = []
    for longitudinal in range(LONGITUDINAL_SEGMENTS):
        for radial in range(RADIAL_SEGMENTS):
            first = longitudinal * (RADIAL_SEGMENTS + 1) + radial
            second = first + RADIAL_SEGMENTS + 1
            for triangle in ((first, first + 1, second), (first + 1, second + 1, second)):
                a, b, c = (vertices[index] for index in triangle)
                # Keep outward-oriented triangles independent of frame convention.
                face = cross(sub(b[:3], a[:3]), sub(c[:3], a[:3]))
                assert dot(face, face) > 1e-18
                if dot(face, a[3:6]) < 0:
                    triangle = (triangle[0], triangle[2], triangle[1])
                indices.extend(triangle)
    return vertices, indices


def make_meshlets(indices):
    meshlets, unique_indices, packed_triangles = [], [], []
    local_indices, block_vertices, block_triangles = {}, [], []

    def finish_block():
        if block_triangles:
            meshlets.append((len(unique_indices), len(block_vertices),
                             len(packed_triangles), len(block_triangles)))
            unique_indices.extend(block_vertices)
            packed_triangles.extend(block_triangles)
            local_indices.clear()
            block_vertices.clear()
            block_triangles.clear()

    for start in range(0, len(indices), 3):
        triangle = indices[start:start + 3]
        new_vertices = len(set(triangle) - local_indices.keys())
        if len(block_vertices) + new_vertices > MAX_VERTICES or len(block_triangles) == MAX_TRIANGLES:
            finish_block()
        local_triangle = []
        for index in triangle:
            if index not in local_indices:
                local_indices[index] = len(block_vertices)
                block_vertices.append(index)
            local_triangle.append(local_indices[index])
        block_triangles.append(local_triangle[0] | local_triangle[1] << 8 | local_triangle[2] << 16)
    finish_block()
    assert sum(block[3] for block in meshlets) * 3 == len(indices)
    for vertex_offset, vertex_count, triangle_offset, triangle_count in meshlets:
        assert 0 < vertex_count <= MAX_VERTICES and 0 < triangle_count <= MAX_TRIANGLES
        for triangle in range(triangle_offset, triangle_offset + triangle_count):
            for corner in range(3):
                local = (packed_triangles[triangle] >> (corner * 8)) & 255
                assert local < vertex_count
                assert unique_indices[vertex_offset + local] == indices[triangle * 3 + corner]
    return meshlets, unique_indices, packed_triangles


def write_assets(directory):
    vertices, indices = make_model()
    meshlets, unique_indices, packed_triangles = make_meshlets(indices)
    sections = [b"".join(struct.pack("<15f", *vertex) for vertex in vertices),
                struct.pack(f"<{len(indices)}I", *indices),
                b"".join(struct.pack("<4I", *block) for block in meshlets),
                struct.pack(f"<{len(unique_indices)}I", *unique_indices),
                struct.pack(f"<{len(packed_triangles)}I", *packed_triangles)]
    data = bytearray(128)
    offsets = []
    for section in sections:
        data.extend(bytes((-len(data)) % 512))
        offsets.append(len(data))
        data.extend(section)
    data[:128] = struct.pack("<12I6Q32x", 0x3548534D, 1, len(vertices), len(indices) // 3,
                             len(meshlets), len(unique_indices), 60, MAX_VERTICES, MAX_TRIANGLES,
                             TEXTURE_SIZE, TEXTURE_SIZE, 0, *offsets, len(data))
    texture = bytearray()
    for y in range(TEXTURE_SIZE):
        for x in range(TEXTURE_SIZE):
            light = ((x // 32) + (y // 32)) % 2 == 0
            color = (220, 178, 92) if light else (40, 102, 150)
            if x % 32 < 2 or y % 32 < 2:
                color = (245, 232, 198)
            texture.extend((*color, 255))
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "model.bin").write_bytes(data)
    (directory / "albedo.rgba").write_bytes(texture)
    partial = sum(v < MAX_VERTICES or t < MAX_TRIANGLES for _, v, _, t in meshlets)
    print(f"vertices={len(vertices)} triangles={len(indices) // 3} meshlets={len(meshlets)} "
          f"unique_indices={len(unique_indices)} partial_blocks={partial} "
          f"model_bytes={len(data)} texture_bytes={len(texture)}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=Path(__file__).resolve().parents[1] / "assets" / "mesh_pipeline")
    write_assets(parser.parse_args().output)
