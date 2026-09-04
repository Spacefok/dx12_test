#!/usr/bin/env python3
"""Generate the four shared-border R16 heightmaps used by the terrain scene."""

import argparse
import math
from pathlib import Path
import struct


DEFAULT_SEED = 1337
TILE_COUNT = 2
TILE_SAMPLES = 129
WORLD_SIZE = 64.0
HEIGHT_SCALE = 14.0
MAX_SAMPLE = 65535


def lattice_value(x, z, seed):
    """An integer hash gives each noise lattice point a repeatable value."""
    value = (x * 0x1F123BB5 + z * 0x5F356495 + seed * 0x6C8E9CF5) & 0xFFFFFFFF
    value ^= value >> 16
    value = (value * 0x7FEB352D) & 0xFFFFFFFF
    value ^= value >> 15
    value = (value * 0x846CA68B) & 0xFFFFFFFF
    value ^= value >> 16
    return value / 0xFFFFFFFF


def smoothstep(value):
    value = min(1.0, max(0.0, value))
    return value * value * (3.0 - 2.0 * value)


def noise(x, z, seed):
    ix, iz = math.floor(x), math.floor(z)
    tx, tz = smoothstep(x - ix), smoothstep(z - iz)
    lower = lattice_value(ix, iz, seed) * (1.0 - tx) + lattice_value(ix + 1, iz, seed) * tx
    upper = lattice_value(ix, iz + 1, seed) * (1.0 - tx) + lattice_value(ix + 1, iz + 1, seed) * tx
    return lower * (1.0 - tz) + upper * tz


def terrain_height(x, z, seed):
    """Rolling ridges around a winding valley, in world-space metres."""
    broad = noise(x * 0.045, z * 0.045, seed)
    ridge = 1.0 - abs(2.0 * noise(x * 0.095 + 6.0, z * 0.095, seed + 1) - 1.0)
    detail = (
        0.55 * noise(x * 0.18, z * 0.18, seed + 2)
        + 0.30 * noise(x * 0.36, z * 0.36, seed + 3)
        + 0.15 * noise(x * 0.72, z * 0.72, seed + 4)
    )
    valley_center = 4.5 * math.sin(z * 0.085) + 0.12 * z
    upland = smoothstep((abs(x - valley_center) - 2.0) / 12.0)
    return 0.55 + upland * (3.0 + 5.2 * broad + 2.4 * ridge) + 0.6 * detail + 0.3 * math.sin(z * 0.11)


def generate_tiles(seed):
    # Quantize a single continuous map before splitting it. Adjacent tiles copy
    # the same sample row/column, including the shared corner of all four tiles.
    global_samples = TILE_COUNT * (TILE_SAMPLES - 1) + 1
    spacing = WORLD_SIZE / (global_samples - 1)
    heightmap = []
    for iz in range(global_samples):
        z = -WORLD_SIZE * 0.5 + iz * spacing
        row = []
        for ix in range(global_samples):
            x = -WORLD_SIZE * 0.5 + ix * spacing
            height = terrain_height(x, z, seed)
            if not math.isfinite(height) or not 0.0 <= height <= HEIGHT_SCALE:
                raise ValueError("Height outside the representable 0..14 metre range")
            row.append(int(height / HEIGHT_SCALE * MAX_SAMPLE + 0.5))
        heightmap.extend(row)

    tiles = {}
    for tile_z in range(TILE_COUNT):
        for tile_x in range(TILE_COUNT):
            samples = []
            for row in range(TILE_SAMPLES):
                start = (tile_z * (TILE_SAMPLES - 1) + row) * global_samples
                start += tile_x * (TILE_SAMPLES - 1)
                samples.extend(heightmap[start:start + TILE_SAMPLES])
            tiles[tile_x, tile_z] = samples
    return tiles


def validate_tiles(tiles):
    for tile_z in range(TILE_COUNT):
        for tile_x in range(TILE_COUNT):
            samples = tiles[tile_x, tile_z]
            if len(samples) != TILE_SAMPLES * TILE_SAMPLES:
                raise ValueError("Unexpected tile sample count")
            if tile_x + 1 < TILE_COUNT:
                neighbor = tiles[tile_x + 1, tile_z]
                for row in range(TILE_SAMPLES):
                    if samples[row * TILE_SAMPLES + TILE_SAMPLES - 1] != neighbor[row * TILE_SAMPLES]:
                        raise ValueError("Mismatched X border")
            if tile_z + 1 < TILE_COUNT:
                neighbor = tiles[tile_x, tile_z + 1]
                if samples[-TILE_SAMPLES:] != neighbor[:TILE_SAMPLES]:
                    raise ValueError("Mismatched Z border")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, help="Noise seed (default: %(default)s)")
    parser.add_argument(
        "--output", type=Path,
        default=Path(__file__).resolve().parents[1] / "assets" / "terrain",
        help="Destination directory (default: assets/terrain in this repository)",
    )
    args = parser.parse_args()
    if not 0 <= args.seed <= 0xFFFFFFFF:
        parser.error("--seed must be between 0 and 4294967295")

    tiles = generate_tiles(args.seed)
    validate_tiles(tiles)
    args.output.mkdir(parents=True, exist_ok=True)
    for (tile_x, tile_z), samples in tiles.items():
        path = args.output / "height_{}_{}.r16".format(tile_x, tile_z)
        data = struct.pack("<{}H".format(len(samples)), *samples)
        expected_bytes = TILE_SAMPLES * TILE_SAMPLES * 2
        if len(data) != expected_bytes:
            raise ValueError("Unexpected encoded tile size")
        path.write_bytes(data)
        if path.stat().st_size != expected_bytes:
            raise OSError("Unexpected written tile size: {}".format(path))
        print("{}: {} bytes, height {:.3f}..{:.3f} m".format(
            path.name, len(data), min(samples) * HEIGHT_SCALE / MAX_SAMPLE,
            max(samples) * HEIGHT_SCALE / MAX_SAMPLE))
    print("Seed {}; all shared borders match.".format(args.seed))


if __name__ == "__main__":
    main()
