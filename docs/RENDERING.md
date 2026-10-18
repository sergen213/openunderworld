# Rendering architecture

Status labels: **observed** (run and checked on the dev machine), **inferred** (reasoned, not
measured), **unknown** (not yet researched).

## Layers

```
formats (src/formats)  ->  scene::SceneData (src/scene/Assets.hpp, CPU only)
                                   |
app (src/app/viewer)   ->  render::Renderer (src/render/Renderer.hpp)
                             |- gl/GlRenderer.cpp   OpenGL 4.6 core, DSA, libepoxy
                             '- vk/VkRenderer.cpp   Vulkan 1.3, dynamic rendering
render/RenderCommon.*  ->  backend-free helpers shared by both (font atlas, overlay
                           quads, mip-chain validation, BCn block sizes)
```

- Format parsers and scene code never include a GPU header. `scene::SceneData` is the only
  contract; fields are added, never repurposed.
- Apps pick a backend at start-up (`ou-viewer --renderer opengl|vulkan`). There is no runtime
  switch; `preWindowSetup()` failing means "backend unavailable here" and the viewer exits 77
  (ctest skip).
- Both backends draw the same thing: offscreen scene target (clear 0.08, 0.09, 0.11), CCW front
  faces, one lambert sun `normalize(-0.4, -0.5, 0.75)` (`RenderCommon kSunDirection`) with 0.45
  ambient and two-sided normals, per-submesh material state (alpha test, blend mode, cull; see
  docs/formats/MATERIALS.md): opaque first (LESS + depth writes), then blended submeshes back to
  front per mesh (LEQUAL, no writes), debug lines, then a fullscreen post pass to the back buffer
  and the overlay. Gamma-space (UNORM) textures and targets.

## Graphics presets (`ou::config::GraphicsConfig`, `FrameOptions::{msaa, shadows, fxaa, hdr}`)

| Setting | Original Fidelity (default) | Enhanced | Flag / config key |
|---|---|---|---|
| MSAA | 1x | 4x (GL: MS renderbuffers + blit resolve; VK: dynamic-rendering resolve) | `--msaa 1/2/4/8`, `graphics.msaa` |
| Sun shadow map | off | 2048² depth, ortho fitted to the view slice [near, 8000] with texel snapping, depth spanning the scene; slope-scaled bias (2, 2); 3x3 hardware-PCF taps; opaque + alpha-blended (cut at 50 %) casters | `--shadows on/off`, `graphics.shadows` |
| FXAA | off | off (optional) | `--fxaa on/off`, `graphics.fxaa` |
| HDR | off | off (framework: RGBA16F target + ACES tonemap in the post pass) | `--hdr on/off`, `graphics.hdr` |
| Ray tracing (Vulkan) | off (not allowed) | off (optional, see "Ray tracing") | `--rt off/shadows/full`, `--rt-quality low/medium/high`, `graphics.rt`, `graphics.rt_quality` |

`--preset original|enhanced` / `graphics.preset` picks the defaults; explicit settings override
(`Config.GraphicsPresets`). Anisotropic filtering (max supported) is on in both. [I] The original
game also renders sun shadows (its vertex shaders output `WorldToPSSM0` coordinates, MATERIALS.md),
so `--preset original --shadows on` is allowed, but our shadow map is not a reproduction of its PSSM.

**Observed** GL vs Vulkan parity (`ou-imgdiff`, 1792x1008, RADV/radeonsi, 2026-10-18):

| Scene | Preset | mean \|Δ\| /255 | PSNR |
|---|---|---|---|
| placeholder | original | 0.041 | 56.2 dB |
| placeholder | enhanced | 0.088 | 50.1 dB |
| placeholder | enhanced + 8x MSAA + FXAA + HDR | 0.062 | 54.7 dB |
| l4_ballcourt `player` start (`--cam 26971,23597,3000,-100,-5`) | original | 0.029 | 54.5 dB |
| same | enhanced | 0.087 | 52.4 dB |
| l4_ballcourt court (`--cam -3000,26000,9000,-100,-35`) | original | 0.045 | 48.9 dB |
| same | enhanced | 0.253 | 43.9 dB |

Viewed screenshots [O]: at the `player` start the ground plants and palms that rendered black/opaque
before now show alpha-cut, textured leaves (GL and Vulkan); in the court view Enhanced shows palm
and statue shadows on the rock and stairs, and canopy shadow on the path.

**Observed** frame times (`ou-viewer --no-vsync --no-overlay --frames 600`, Release build, 1792x1008,
RX 7900 XTX, average after 11 warm-up frames, l4_ballcourt `--level`, 4380 meshes):

| Camera | GL original | GL enhanced | VK original | VK enhanced |
|---|---|---|---|---|
| `player` start | 2.56 ms | 4.11 ms | 0.92 ms | 1.74 ms |
| court | 2.54 ms | 4.07 ms | 0.91 ms | 1.66 ms |

Shadows dominate the Enhanced cost (a second pass over every mesh; `original --shadows on`: GL 4.14,
VK 1.66 ms); FXAA + HDR add ≤ 0.07 ms. GL is CPU/draw-call bound (one draw per submesh).

## Vulkan backend (`src/render/vk/VkRenderer.cpp`)

| Piece | Choice |
|---|---|
| Loading | link `libvulkan` (loader) and call core entry points directly; extension functions (`vkCreateDebugUtilsMessengerEXT`) via `vkGetInstanceProcAddr`. No volk. |
| Instance | API 1.3; SDL3's required surface extensions; `VK_EXT_debug_utils` when present; `VK_LAYER_KHRONOS_validation` when installed (Debug default on, `OU_VK_VALIDATION=0/1` overrides). |
| Device | first Vulkan ≥ 1.3 device with a graphics queue that can present to the SDL surface and has `VK_KHR_swapchain`; discrete GPUs preferred. Features: `dynamicRendering`, `synchronization2`, plus `textureCompressionBC` / `samplerAnisotropy` when supported. |
| Swapchain | UNORM BGRA/RGBA, SRGB_NONLINEAR colour space; FIFO with vsync, MAILBOX → IMMEDIATE without. Recreated (with `oldSwapchain`) when the SDL pixel size changes (`resize()`), on `VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR`, on a vsync toggle, or when the drawFrame viewport differs from the last request. Zero-size (minimised) frames are skipped. |
| Rendering | `vkCmdBeginRendering` (no render passes / framebuffers). Negative-height viewport so GL clip space and CCW winding carry over; GL projection z remapped to [0, w] by one matrix. Cull mode is dynamic state. |
| Frames in flight | 2. Per frame: command pool + buffer, fence (signalled at creation), acquire semaphore, 640 KiB host-coherent overlay vertex buffer. One present semaphore per swapchain image (safe reuse). Fence reset only after a successful acquire. Depth image shared between frames, ordered by a depth-write barrier. |
| Memory | `LinearAllocator`: bump allocation inside 64 MiB `VkDeviceMemory` blocks per memory type, aligned to `max(alignment, bufferImageGranularity)`, host-visible blocks persistently mapped. Arenas: static (font, white, overlay buffers), scene (released on every `upload()`), and exact-size single-resource arenas for depth, staging, readback and the per-frame debug-line buffers. No per-object free. |
| Uploads | one staging buffer per batch sized to the sum of all regions (16-byte aligned for BCn), one command buffer with sync2 barriers, waited on a fence. |
| Textures | `VK_FORMAT_BC1_RGBA_UNORM_BLOCK` (DXT1 with 1-bit alpha, as D3D9), `BC2_UNORM`, `BC3_UNORM`, `R8G8B8A8_UNORM`, `B8G8R8A8_UNORM`; full validated mip chains (`usableMips`). One combined-image-sampler descriptor set per texture from a per-scene pool; one repeat/linear/trilinear sampler with max anisotropy. |
| Shaders | GLSL in `src/render/vk/shaders/`, compiled by `glslc --target-env=vulkan1.3 -O -mfmt=num` at build time into `build/vk_shaders/*.inc`, `#include`d as `uint32_t[]`; `scene.vert/frag` a second time with `-DOU_RT` (`scene_rt.*.inc`). Push constants: scene/shadow `{mat4 viewProj, model, lightViewProj; int lighting, shadows; float alphaRef; int rt; vec4 eye}` (224 B, checked against `maxPushConstantsSize`), overlay `{vec2 screen}`, post `{int fxaa, hdr}`. Pipelines: one per `scene::Blend` + lines (rebuilt with the MSAA count / HDR format / RT on-off), shadow (depth only, depth bias), RT depth pre-pass, post and overlay (swapchain format). `shaderDemoteToHelperInvocation` enabled for `discard`. |
| Pipeline cache | loaded from / saved to `$XDG_CACHE_HOME/openunderworld/vk_pipeline_cache.bin` (fallback `~/.cache/...`); drivers ignore incompatible blobs. |
| Screenshot | after `drawFrame`, a one-shot copy PRESENT_SRC → TRANSFER_SRC → buffer → PRESENT_SRC, BGRA swizzled to RGB. |
| CPU skinning | `updateVertices(meshIndex, vertices)`: scene-mesh → GPU-slot map, vertex count checked, staged copy into the device-local vertex buffer behind a write-after-read barrier, waited on a fence (one queue round-trip per call). |
| Debug lines | `FrameOptions::debugLines`: expanded to `{vec3, unorm4}` vertices in a per-frame host-coherent buffer (grown to the next power of two), `LINE_LIST` pipeline after the scene, depth test LEQUAL without depth writes, `z -= 0.5e-4·w` (the GL backend's `1e-4·w` in GL's [-w, w] clip range), unlit. |
| Ray tracing | optional, see "Ray tracing" below: `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` (+ `deferred_host_operations`), `bufferDeviceAddress`, descriptor indexing, `shaderInt64` enabled when all are present. `VK_KHR_ray_tracing_pipeline` is not used. |

Known limits (deliberate):
- `upload()` and swapchain recreation call `vkDeviceWaitIdle`; fine for a viewer, not for streaming.
- Readback stalls the queue; it is a debugging/testing path.
- No sRGB. Light maps, fog, wind and material tints come from the original shaders (SHADERS.md).

## Ray tracing (Vulkan, Enhanced, optional)

`--rt shadows` traces the sun shadow; `--rt full` also mirrors the scene in water. Needs `--preset
enhanced` (`config::resolve` rejects RT with `original`); default off. `--rt-quality low|medium|high`
= 1 / 2 / 4 shadow rays per pixel. The overlay line `RT ...` shows the mode, whether rays are
traced, BLAS / geometry / instance counts (water instances), TLAS builds / updates and BLAS refits /
rebuilds (`Renderer::rtStatus()`, `rtSummary`).

| Piece | Choice |
|---|---|
| Support | `probeRayTracing()`: the extensions + features above and ≥ 4098 sampled images per stage. Anything missing (or `OU_VK_NO_RT=1`, the simulated-unsupported switch) leaves the device without RT: `rtStatus().supported = false` with the reason, and a requested RT mode renders with the shadow map (`--rt` never fails). The GL backend reports unsupported. |
| Acceleration structures | Built by the first frame that asks for RT after `upload()` (nothing is built while RT is off). One BLAS per mesh with one triangle geometry per submesh that casts in the shadow map too (`Blend::Opaque` + `Blend::Alpha`; Additive / Average neither cast nor reflect), vertex/index buffers used in place via buffer device addresses. Static BLASes `PREFER_FAST_TRACE`; skinned meshes (`MeshData::skin`) `ALLOW_UPDATE + PREFER_FAST_BUILD`. One TLAS instance per BLAS with the mesh's model matrix (static: `GpuMesh::model` never changes), both faces (`TRIANGLE_FACING_CULL_DISABLE`), custom index = first geometry record; mask 2 for water, 1 otherwise. TLAS `ALLOW_UPDATE + PREFER_FAST_TRACE`. Upload builds are batched into ≤ 64 MiB of shared scratch. |
| Dynamic meshes | `updateVertices()` records the BLAS work into the copy's own submission: refit (mode UPDATE) for skinned meshes (Lara, enemies), full in-place rebuild for others (pistols in ou-game are rigid and rebuilt). The next RT frame updates the TLAS in place once (not rebuilt). Choice [I]: refits keep the bind-pose BVH topology, so traversal degrades with large poses but stays correct. |
| Geometry records | storage buffer, one `{vertex address, index address, first index, texture slot, alpha ref}` per geometry, indexed by `instanceCustomIndex + geometryIndex` in the shader. |
| Alpha test (any-hit equivalent) | Non-opaque geometries (alpha ref ≠ 0: `Blend::Alpha` cut at 50 % as the shadow map, or an alpha-tested opaque material) are tested in the `rayQueryProceedEXT` loop: barycentric UV from the index/vertex buffers, `textureLod(..., 0)` of the bindless texture array (set 2, 4096 slots, partially bound), texture alpha only (as `shadow.frag`); candidates that pass are confirmed. |
| Shadows | `scene_rt.frag`: N rays per pixel toward the sun inside a cone of tan 0.012 (golden-angle spiral rotated per pixel by interleaved gradient noise), `TerminateOnFirstHit`, origin offset along the normal by 2 + 2e-4 · distance units. Spatial denoise: the visibility is averaged over the lanes of the 2x2 pixel quad that trace (fine derivatives; quads never span primitives, so no bleeding across edges); helper lanes, alpha-discarded lanes and lanes facing away from the sun trace nothing. No temporal accumulation (no history buffer / TAA yet): the per-pixel noise is fixed, so still frames are stable. |
| Overdraw | RT adds a depth pre-pass over opaque submeshes (`prepass.vert/frag`: same alpha test, `invariant gl_Position` as `scene_rt.vert`); the opaque colour pass then tests LEQUAL without writes, so each visible opaque pixel traces once. Blended submeshes (no depth writes, drawn layer over layer) keep sampling the shadow map, which is still rendered in RT mode: tracing them cost ~10 of 15 ms in the ballcourt foliage (measured). |
| Reflections (`full`) | Draws of water meshes trace one mirror ray (mask 1: water never reflects itself) with closest hit, alpha-tested like shadows, and shade the hit with texture × vertex colour × the raster lambert (no shadow ray at the hit; misses = clear colour), mixed in by Schlick Fresnel with F0 = 0.02 (alpha raised by the same factor). Water = meshes whose origin names a `*watersurface*` object [I] (l4_pool: `ca_goo_pool_watersurface`, the surface intro of the pool's `watervolume`, GAMEPLAY.md "Water"). No roughness: TRU's material blobs give none we decode [U], so water is a perfect mirror and wet surfaces get no reflections. |
| Not done | 1-bounce GI / AO (not started); temporal denoise; merged static BLASes (traversal cost); ray cones for reflection mip selection (mip from hit distance only). |

Verification [O] (RX 7900 XTX, RADV Mesa 26.2.4, 2026-10-18):
- Validation layer clean (`OU_VK_VALIDATION=1`): `ou_vk_tests`, `ou-viewer --level` l4_ballcourt / l4_pool with `--rt shadows/full`, and `ou-game` l4_pool `--rt full` for 60 frames (Lara skinned: 638 BLAS refits, 116 rigid rebuilds, 57 TLAS updates, 2 TLAS builds across a unit stream-in).
- Screenshots viewed: l4_ballcourt court (`--cam -3000,26000,9000,-100,-35`): the crenellated court wall, platforms and statues cast soft shadows on the court floor and stairs that the shadow map misses (it ends 8000 units from the camera); `player` start: same plants/shadows as the map. l4_pool (`--cam -700,-1400,2950,90,-8`): the pool mirrors the rocks, the yellow posts, the red markers and the cyan slab, strongest at grazing angles (Fresnel); RT off shows only the water texture.
- RT off is the previous Enhanced output: l4_ballcourt Enhanced + FXAA + HDR byte-identical to the pre-RT build; l4_pool identical to one of the pre-RT build's own run-to-run variants (that build is not deterministic there either).
- `OU_VK_NO_RT=1 --rt full` renders the shadow-map image (`VulkanRenderer.RayTracingFallsBackToShadowMap`).

**Observed** frame times (`ou-viewer --level --renderer vulkan --preset enhanced --no-vsync --no-overlay
--frames 600`, Release, 1792x1008, RX 7900 XTX / RADV, average after 11 warm-up frames, 2026-10-18):

| Scene (`--cam`) | shadow map | `--rt shadows` low | medium | high | `--rt full` medium | `OU_VK_NO_RT=1 --rt full` |
|---|---|---|---|---|---|---|
| l4_ballcourt `player` start | 1.87 ms | 6.17 ms | 9.86 ms | 17.12 ms | 9.87 ms | 1.88 ms |
| l4_ballcourt court | 1.81 ms | 5.71 ms | 8.83 ms | 14.95 ms | 8.83 ms | 1.79 ms |
| l4_pool water (`-700,-1400,2950,90,-8`) | 0.27 ms | 0.51 ms | 0.78 ms | 1.29 ms | 1.04 ms | 0.25 ms |

Cost scales with rays per pixel; in the jungle it is dominated by alpha-tested casters (measured on
the `player` start at 1 ray: 6.2 ms, 3.4 ms with the alpha test forced to pass). Before the depth
pre-pass and the shadow-map fallback for blended receivers the same view took 22 ms at 1 ray.

## Original shaders (`--shaders original|generic`, `graphics.original_shaders`; default original)

Materials draw with TRU's own D3D9 SM3 programs translated to GLSL (`render/ShaderTranslate`; GL compiles the GLSL,
Vulkan compiles it to SPIR-V with libshaderc at `upload()`) fed with the engine's register files (`render/OriginalShading`,
shared): frame UBO, material UBO + 16 samplers, object block (GL UBO range / Vulkan push constants). Programs that need
irradiance-volume lighting (dynamic objects), and materials without composite programs, keep the generic shading below.
Not with the RT depth pre-pass (RT keeps the generic pipelines). Details, survey and evidence: docs/formats/SHADERS.md.
Vulkan: one pipeline per (program, blend) created on first use, one descriptor set per distinct (program, material,
instance textures, alpha ref), only consumed vertex attributes declared (validation-clean).

**Observed** GL vs Vulkan (`ou-imgdiff`, 1792x1008, original shaders, 2026-10-18): l4_ballcourt `player` start 0.025/255
(58.7 dB), court 0.085 (51.5 dB), l4_pool (`--cam -700,-1400,2950,90,-8`) 1.34 (40.1 dB), l3_main_bedroom ou-game start
1.56 (HUD/overlay included).

**Observed** frame times (`ou-viewer --no-vsync --no-overlay --frames 600`, Release, 1792x1008, RX 7900 XTX, l4_ballcourt
court camera): GL 2.69 ms generic → 4.32 ms original; Vulkan 1.18 → 1.97 ms (the original pixel shaders average 270
instructions vs our 20-line lambert).

Viewed: ballcourt path/court show light-mapped multi-layer terrain, sun-lit court, foliage with wind; l4_pool water uses
the original swirl shader; the l3_main_bedroom corridor is light-map + ambient only (its torch lights are not fed) and so
much darker than the generic lambert; the 060_med_natla cutscene keeps Lara/actors on generic shading.

## Original Fidelity vs Enhanced (plan)

Two render modes share the scene graph and simulation; only the backend's frame graph differs.

**Original Fidelity** (default; the reference for screenshots and regression tests)
- Reproduce TRU's Direct3D 9 output: the game's own shader programs (`shaderlib` sections in
  DRMs, currently unused — 59 in `lara.drm`, **observed**) translated to SPIR-V/GLSL, its
  material and blend states, fog, light rig and post effects, in gamma space unless research
  shows sRGB states [unknown: TRU's D3D9 sampler/render states not yet verified].
- Native resolution and aspect (Hor+ already, **observed**), high refresh rate decoupled from the
  simulation tick, anisotropic filtering. Nothing else changes the image.
- Verification: frame captures from the original game under the same camera, compared with
  `ou-imgdiff`-style metrics plus side-by-side review.

**Enhanced** (opt-in, Vulkan only)
- Linear-space lighting, higher-precision shadows, MSAA/TAA, SSAO, HDR output.
- Optional ray tracing on hardware exposing `VK_KHR_ray_query` + `VK_KHR_acceleration_structure`:
  RT sun shadows and water reflections via ray queries in the raster pipeline, shadow map as
  fallback (implemented, "Ray tracing" above); RT AO / GI not started.
- Any enhancement must be switchable per feature so a difference can be traced back to it.

The OpenGL backend stays the portability/reference path for Original Fidelity; Enhanced
features land in Vulkan first.
