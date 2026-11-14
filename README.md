# DX12 Scene Renderer

A Windows-focused DirectX 12 renderer written in C++17 for experimenting with real-time scene rendering, scene loading, tessellation, and visibility systems.

## Highlights

- Deferred lighting pipeline with a G-buffer pass and fullscreen lighting pass
- Adaptive tessellation and displacement-aware materials
- OBJ and FBX scene loading via bundled `tinyobjloader` and `ufbx`
- Nine demo scenes: `Sponza`, `Bistro`, `San Miguel`, procedural `Culling Lab`, `Terrain`, `TAA Lab`, `Atmosphere Lab`, `RT Shadows Lab`, and `Mesh Pipeline Lab`
- Native-resolution temporal anti-aliasing with Halton jitter, motion vectors, and neighborhood history clamping
- Single-scattering Rayleigh/Mie atmosphere with sky and depth-limited aerial perspective
- Inline ray-traced hard and soft shadows with a refittable TLAS and edge-aware denoising
- Mesh shader rendering with meshlet diagnostics and an indexed reference path
- DirectStorage loading of mesh geometry and texture into GPU resources
- Frustum culling, octree culling, and occlusion culling toggles for side-by-side comparison
- Procedural water surface, scatter-field props, and tree billboard LODs
- Tiled heightmap terrain with quadtree LOD, screen-space error selection, and frustum culling

## Controls

- `W`, `A`, `S`, `D` move the camera
- `Space` / `Ctrl` move up and down
- `Shift` boosts camera speed
- Hold RMB and move the mouse to look around
- `1`-`9` switch scenes (`5` selects Terrain, `6` TAA Lab, `7` Atmosphere Lab, `8` RT Shadows Lab, `9` Mesh Pipeline Lab)
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
- Recent Windows 10/11 SDK including DXC (Shader Model 6.5)
- A DirectX 12 capable GPU

Build steps:

1. Open `DX12SceneRenderer.sln` in Visual Studio 2022.
2. Select `x64` and either `Debug` or `Release`.
3. Restore NuGet packages, then build. The project uses `Microsoft.Direct3D.DirectStorage` 1.3.0.
4. Run from the repository root so relative `assets/` and `shader/` paths resolve correctly.

From a Visual Studio developer shell, restore and build with:

~~~powershell
MSBuild DX12SceneRenderer.sln /restore /p:RestorePackagesConfig=true /m /p:Configuration=Debug /p:Platform=x64
~~~

NuGet downloads the SDK into the ignored `packages/` directory and copies
`dstorage.dll` and `dstoragecore.dll` beside the executable. Keep both DLLs
with the application when copying a build.

The build compiles the RT and mesh pipeline shaders with the SDK's DXC into intermediate
headers embedded in the executable. No extra compiler DLL is needed at runtime.
RT Shadows Lab requires DXR Tier 1.1 and Shader Model 6.5. Other scenes keep
their existing shadow maps; an unsupported adapter is reported in the title.

## Mesh Pipeline Demo

~~~powershell
.\x64\Debug\DX12SceneRenderer.exe --mesh-pipeline
~~~

The packaged torus knot contains 66,177 vertices, 131,072 triangles, and 2,130
meshlets. Each meshlet holds at most 64 vertices and 126 triangles. Its local
vertex table refers to the original vertex array; each triangle packs three
local indices into one `uint`. The final partial meshlet has 58 vertices and
56 triangles. Loading validates every index and verifies that expanding all
meshlets reproduces the original triangle list exactly.

| Key | Action |
| --- | --- |
| `F6` | Switch Mesh Shader / classic indexed rendering |
| `F7` | Toggle a distinct color for each meshlet |
| `F8` | Show one selected meshlet / the entire model |
| `+` / `-` | Select the next / previous meshlet |
| `PageUp` / `PageDown` | Move the selection by 64 meshlets |
| `End` | Select the last, partial meshlet |
| `[` / `]` | Rotate the object around Y by 15 degrees |
| `O` | Switch object scale between 1 and 0.65 |
| `L` | Reload using CPU upload / DirectStorage, preserving the view and selection |
| `9` | Reload the demonstration defaults with DirectStorage |

Mesh Shader Tier 1 and Shader Model 6.5 are required for `DispatchMesh`.
With Shader Model 6.5 but no mesh shader support, the scene uses the indexed
reference path and reports `MeshSupport:No`. The mesh shader runs one group
per meshlet, calls `SetMeshOutputCounts` with its actual counts, and emits
positions, normals, UVs, colors, and motion data for the existing G-buffer
pixel shader. The reference path reads the same vertices and original indices.
Both paths support the same color and single-meshlet modes. Turn TAA off with
`T` for a direct image comparison, and rotate the object if a selected meshlet
is on its back side.

The default loader submits five geometry buffer requests and one RGBA8 texture
request through a DirectStorage file queue. Completion status and a fence are
checked before resource transitions and rendering. `IO` in the title identifies
the selected loader. `Load` measures the entire load, including CPU validation,
resource creation, and GPU waits; it is not a storage benchmark. CPU validation
reads the source file before DirectStorage, so these timings are not cold-cache
measurements. The ordinary loader provides a comparison using the same assets.
If DirectStorage fails at scene startup, a successful CPU load keeps the demo
available and displays the error. Press `L` to retry DirectStorage. A failed
reload preserves the previously loaded model.

`assets/mesh_pipeline/model.bin` stores the versioned header, original vertices
and indices, meshlet descriptors, global vertex mappings, and packed triangles.
Sections begin at 512-byte boundaries. `albedo.rgba` is a 256 x 256, single-mip
RGBA8 texture with a 1,024-byte row pitch. Both files can be regenerated with
Python's standard library:

~~~powershell
py -3 tools/generate_mesh_model.py
~~~

Start reading at `src/MeshletModel.cpp`, `src/MeshPipeline.cpp`,
`shader/MeshPipeline.hlsl`, and `src/DirectStorageLoader.cpp`. This scene uses
one static model without an amplification shader or meshlet culling; the
existing lighting, camera, and TAA passes remain shared with the renderer.

## RT Shadows Demo

~~~powershell
.\x64\Debug\DX12SceneRenderer.exe --rt-shadows
~~~

The scene starts paused with soft shadows, an angular light radius of 3 degrees,
8 stochastic samples per pixel, spatial filtering, and TAA. A separate central
ray also measures the blocker distance. All geometry is opaque and shares
one indexed box BLAS. The TLAS contains every instance, including the box outside
the initial camera frustum, and is refitted from current transforms each frame.

| Key | Action |
| --- | --- |
| `F6` | Cycle soft / off / hard shadows |
| `F7` | Show the visibility mask without TAA or tone mapping |
| `F8` | Toggle spatial filtering |
| `[` / `]` | Decrease / increase light angular radius, from 0 to 8 degrees |
| `N` | Cycle 1 / 4 / 8 / 16 rays per pixel |
| `O` | Lower / raise the blue plate to compare contact and distant shadows |
| `B` | Move the offscreen caster away / back |
| `P` | Animate / pause the moving box |
| `T` | Toggle TAA; use with `F8` to compare raw noise and denoising |
| `8` | Reload the demonstration defaults |

Each visible surface sends shadow rays through an angular disk around a single
directional light. Unblocked-ray fraction modulates only that light's direct
diffuse/specular contribution. Ambient lighting stays unchanged. A normal offset
and minimum ray distance reduce self-intersections. The closest blocker distance
and angular radius estimate the penumbra footprint used by a small spatial filter.
Normal and plane-distance checks reject samples on other surfaces.

At zero angular radius, soft mode produces the exact hard visibility mask.
Increasing the radius or lifting the plate broadens its penumbra. The `B` comparison
demonstrates that camera culling does not remove shadow casters. During caster
animation, temporal lighting history is rejected because surface motion vectors
do not describe a moving shadow on a stationary receiver; spatial denoising remains.

Start reading at `shader/RayTracedShadows.hlsl`, `src/RayTracedShadows.cpp`,
and `src/RayTracedShadowRendering.cpp`. This laboratory traces the procedural
opaque scene only; imported alpha-tested, displaced, and transparent geometry
continues using the existing rendering path.

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

## Repository Layout

- `src/` application code, scene loading, camera logic, and render orchestration
- `include/` public headers and shared renderer data structures
- `shader/` HLSL programs for geometry and lighting passes
- `assets/` packaged demo scenes, textures, and asset metadata
- `tools/` standalone asset generation utilities
