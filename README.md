# DX12 Scene Renderer

A Windows-focused DirectX 12 renderer written in C++17 for experimenting with real-time scene rendering, scene loading, tessellation, and visibility systems.

## Highlights

- Deferred lighting pipeline with a G-buffer pass and fullscreen lighting pass
- Adaptive tessellation and displacement-aware materials
- OBJ and FBX scene loading via bundled `tinyobjloader` and `ufbx`
- Five demo scenes: `Sponza`, `Bistro`, `San Miguel`, procedural `Culling Lab`, and `Terrain`
- Frustum culling, octree culling, and occlusion culling toggles for side-by-side comparison
- Procedural water surface, scatter-field props, and tree billboard LODs
- Tiled heightmap terrain with quadtree LOD, screen-space error selection, and frustum culling

## Controls

- `W`, `A`, `S`, `D` move the camera
- `Space` / `Ctrl` move up and down
- `Shift` boosts camera speed
- Hold RMB and move the mouse to look around
- `1`-`5` switch scenes (`5` selects Terrain)
- `Home` resets the current scene's camera
- `F1` toggles G-buffer debug view
- `F2` toggles frustum culling
- `F3` toggles octree culling
- `F4` toggles occlusion culling
- `F5` toggles local lights
- In Terrain: `F6` toggles LOD colors, `F7` freezes/unfreezes LOD and the visible patch set, and `F8` toggles wireframe
- In Terrain: `+` lowers the error threshold for finer detail; `-` raises it for coarser detail

## Build

Requirements:

- Windows 10/11
- Visual Studio 2022 with MSVC v143
- Windows 10 SDK
- A DirectX 12 capable GPU

Build steps:

1. Open `DX12SceneRenderer.sln` in Visual Studio 2022.
2. Select `x64` and either `Debug` or `Release`.
3. Build and run from the repository root so relative `assets/` and `shader/` paths resolve correctly.

## Terrain Demo

Start directly in the terrain scene without loading any external scene models:

```powershell
.\x64\Debug\DX12SceneRenderer.exe --terrain
```

The checked-in terrain uses a 2 x 2 arrangement of 129 x 129 heightmap tiles,
covering 64 x 64 metres. Each tile shares its border samples with its neighbors.
The files contain unsigned 16-bit little-endian heights, with a 14 metre height
scale. The standalone Python generator produces rolling ridges and a valley;
Python is only needed to regenerate the assets:

```powershell
py -3 tools/generate_terrain.py --seed 1337
```

See [the heightmap format and generator notes](assets/terrain/README.md) for
coordinates, sample spacing, and export requirements.

The tiles form one 257 x 257-sample heightmap with a single quadtree. Selected
nodes render fixed 16 x 16-cell patches;
splitting a node replaces its patch with four children over the same area.
Refinement uses geometric height error projected into screen pixels, so camera
distance, viewport size, and the error threshold affect the selected detail.
Conservative bounds reject nodes entirely outside the camera frustum. Edge
skirts cover gaps between patches at different LODs; switching LODs is discrete
and can produce visible popping.

LOD colors progress from coarse to fine as blue, cyan, green, orange, and red.
The triangle counter includes the skirts; shadow passes are additional draws.

To inspect the implementation:

1. Press `F6` and `F8` to show LOD colors and patch geometry. Move closer to the
   terrain, then farther away, and watch patch and triangle counts in the title.
2. Press `+` or `-` to compare finer and coarser selections from the same viewpoint.
3. Turn toward an edge of the terrain and toggle `F2` to compare frustum culling
   off and on.
4. With culling on, press `F7`, then move or turn the camera to inspect the frozen
   visible patch set. Press `F7` again to resume updates, or `Home` to reset.

Changing the error threshold, toggling frustum culling, or resetting the camera
also resumes live terrain selection. Octree and occlusion toggles apply to the
other scene objects; terrain visibility uses its own quadtree traversal.

## Terrain Tests

From a Visual Studio Developer PowerShell at the repository root:

~~~powershell
New-Item -ItemType Directory -Force x64\TerrainTests | Out-Null
cl /nologo /std:c++17 /EHsc /W4 /Iinclude tests\TerrainTests.cpp src\Terrain.cpp /Fox64\TerrainTests\ /Fex64\TerrainTests\TerrainTests.exe
.\x64\TerrainTests\TerrainTests.exe
~~~

These CPU tests check quadtree coverage, LOD refinement, D3D frustum planes,
mesh winding, bounds, tile seams, and malformed input. Shader compilation and
rendered output still require starting the application.

## Scene Assets

The repository keeps runtime-ready textures and scene metadata in git, but treats large DCC source files as local-only by default. In particular, the Bistro `.fbx` meshes are ignored so they do not bloat day-to-day commits or make the repository page look like an asset dump.

If you want to publish those large source assets, prefer one of these approaches:

- store them with Git LFS
- attach them to GitHub Releases
- document an external download step in this README

Asset-specific attribution and license files are kept next to the corresponding asset packs under `assets/`.

## Repository Layout

- `src/` application code, scene loading, camera logic, and render orchestration
- `include/` public headers and shared renderer data structures
- `shader/` HLSL programs for geometry and lighting passes
- `assets/` packaged demo scenes, textures, and asset metadata
- `tools/` standalone asset generation utilities

## Notes

The renderer ships with large demo assets, so if you later decide to publish source DCC files as well, prefer Git LFS or GitHub Releases instead of regular git history.
