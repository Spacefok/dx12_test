# DX12 Scene Renderer

A Windows-focused DirectX 12 renderer written in C++17 for experimenting with real-time scene rendering, scene loading, tessellation, and visibility systems.

## Highlights

- Deferred lighting pipeline with a G-buffer pass and fullscreen lighting pass
- Adaptive tessellation and displacement-aware materials
- OBJ and FBX scene loading via bundled `tinyobjloader` and `ufbx`
- Multiple demo scenes: `Sponza`, `Bistro`, `San Miguel`, and a procedural `Culling Lab`
- Frustum culling, octree culling, and occlusion culling toggles for side-by-side comparison
- Procedural water surface, scatter-field props, and tree billboard LODs

## Controls

- `W`, `A`, `S`, `D` move the camera
- `Space` / `Ctrl` move up and down
- `Shift` boosts camera speed
- Hold RMB and move the mouse to look around
- `1`-`4` switch scenes
- `F1` toggles G-buffer debug view
- `F2` toggles frustum culling
- `F3` toggles octree culling
- `F4` toggles occlusion culling

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

## Notes

The renderer ships with large demo assets, so if you later decide to publish source DCC files as well, prefer Git LFS or GitHub Releases instead of regular git history.
