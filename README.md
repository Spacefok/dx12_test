# DX12 Scene Renderer

A Windows-focused DirectX 12 renderer written in C++17 for experimenting with real-time scene rendering, scene loading, tessellation, and visibility systems.

## Highlights

- Deferred lighting pipeline with a G-buffer pass and fullscreen lighting pass
- Adaptive tessellation and displacement-aware materials
- OBJ and FBX scene loading via bundled `tinyobjloader` and `ufbx`
- Seven demo scenes: `Sponza`, `Bistro`, `San Miguel`, procedural `Culling Lab`, `Terrain`, `TAA Lab`, and `Atmosphere Lab`
- Native-resolution temporal anti-aliasing with Halton jitter, motion vectors, and neighborhood history clamping
- Single-scattering Rayleigh/Mie atmosphere with sky and depth-limited aerial perspective
- Frustum culling, octree culling, and occlusion culling toggles for side-by-side comparison
- Procedural water surface, scatter-field props, and tree billboard LODs
- Tiled heightmap terrain with quadtree LOD, screen-space error selection, and frustum culling

## Controls

- `W`, `A`, `S`, `D` move the camera
- `Space` / `Ctrl` move up and down
- `Shift` boosts camera speed
- Hold RMB and move the mouse to look around
- `1`-`7` switch scenes (`5` selects Terrain, `6` TAA Lab, `7` Atmosphere Lab)
- `Home` resets the current scene's camera
- `F1` toggles G-buffer debug view
- `F2` toggles frustum culling
- `F3` toggles octree culling
- `F4` toggles occlusion culling
- `F5` toggles local lights
- `T` toggles TAA and projection jitter together
- `V` shows motion vectors (black means zero motion)
- `R` toggles history reprojection; `C` toggles neighborhood clamping
- `P` pauses scene animation; `Home` also clears temporal history
- `G` toggles atmosphere; `H` toggles its effect on opaque objects
- `F9` selects clean air; `F10` selects aerosol-heavy air with the same camera, sun, and exposure
- `U` / `J` increase / decrease Rayleigh strength; `I` / `K` increase / decrease Mie strength
- With atmosphere enabled, hold arrow keys to change sun azimuth and elevation
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

## Atmosphere Demo

~~~powershell
.\x64\Debug\DX12SceneRenderer.exe --atmosphere
~~~

The self-contained scene places matching black/white panel pairs at increasing
distances. Compare `H` on/off, then `F9`/`F10` without moving the camera.
Both presets use Rayleigh scale 1; Mie scale changes from 1 to 20. Exposure is
fixed at 1.18. Individual strengths can reach zero to isolate either scattering
term. The title shows current values; arrow keys move the sun in world space.

Distances inside the atmosphere shader are kilometres. In Atmosphere Lab,
one world unit represents 100 metres; other scenes use one metre per unit.
The spherical model uses a 6360 km planet, a 100 km atmosphere, and exponential
density scale heights of 8 km (Rayleigh) and 1.2 km (Mie).

The fullscreen pass reconstructs the surface from depth and integrates only
the camera-to-surface segment, or the atmospheric segment for a sky pixel.
It applies Beer-Lambert transmittance and adds single-scattered sunlight in
linear HDR before TAA and tone mapping. The sun rays include atmospheric
attenuation and planet occlusion. Sky history uses camera rotation reprojection;
parameter changes reset accumulation.

Implementation starts in `shader/Atmosphere.hlsl`, `include/Atmosphere.hpp`,
and `src/AtmosphereRendering.cpp`. The model targets a camera near the ground.
It does not include multiple scattering, clouds, ozone, or volumetric shadows
from scene objects. Forward transparent objects and particles are drawn later
and do not receive this depth-based atmospheric pass.

## TAA Demo

Start the self-contained temporal anti-aliasing scene from the repository root:

~~~powershell
.\x64\Debug\DX12SceneRenderer.exe --taa-demo
~~~

The moving red box reveals a striped wall; thin rails and floor lines make
subpixel aliasing visible. TAA starts enabled. Pause with `P`, compare `T`,
then resume animation and compare `R` and `C`. The title shows every toggle
and the history frame count. Resize, scene changes, camera cuts, and `Home`
invalidate history. Motion debug bypasses accumulation and post effects.

The opaque HDR lighting result is accumulated before tone mapping. Transparent
objects and particles are drawn afterwards and are not part of TAA history.

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
