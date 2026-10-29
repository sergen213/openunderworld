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
  and the overlay. Gamma-space (UNORM) textures and targets, as tru.exe (see "Colour space" below).

## Graphics presets (`ou::config::GraphicsConfig`, `FrameOptions::{msaa, shadows, fxaa, hdr, rt, brightness, water}`)

Defaults (no config, no flags): renderer `auto` = Vulkan when a Vulkan 1.3 device is usable, else OpenGL with the reason
logged (`render::selectRenderer`, test `RendererSelect.AutoFallsBackToOpenGl`); preset `enhanced`; `rt auto` = sun
shadows by ray query on capable Vulkan devices (the shadow map elsewhere); the original TRU shaders in both presets.

| Setting | Original Fidelity (purist) | Enhanced (default) | Flag / config key |
|---|---|---|---|
| MSAA | 1x | 4x (GL: MS renderbuffers + blit resolve; VK: dynamic-rendering resolve) | `--msaa 1/2/4/8`, `graphics.msaa` |
| Sun shadows | off | cascaded shadow map ("Sun shadows" below), or rays (`rt`) | `--shadows on/off`, `graphics.shadows` |
| FXAA | off | off (optional) | `--fxaa on/off`, `graphics.fxaa` |
| HDR | off | off: the original shaders write LDR gamma-space colour; the ACES tonemap (RGBA16F target) only compresses it | `--hdr on/off`, `graphics.hdr` |
| Ray tracing (Vulkan) | off (not allowed) | `auto` = shadows when supported | `--rt auto/off/shadows/full`, `--rt-quality low/medium/high`, `graphics.rt`, `graphics.rt_quality` |
| Water | TRU's own water shaders | enhanced water ("Water" below): waves, refraction, SSR, ripples, splashes, underwater | `--water original/enhanced`, `graphics.water` |
| Brightness | 1.0 | 1.0 | `--brightness 0.5-2.0`, `graphics.brightness` |

`--preset original|enhanced` / `graphics.preset` picks the defaults; explicit settings override
(`Config.GraphicsPresets`). Anisotropic filtering (max supported: 16x on RADV) is on in both. [I] The original
game also renders sun shadows (its vertex shaders output `WorldToPSSM0` coordinates, MATERIALS.md),
so `--preset original --shadows on` is allowed, but our shadow map is not a reproduction of its PSSM.

The original's screen filters (class-5 effects: ColorDOF overlay / scene matrix and Finish bloom, docs/formats/EFFECTS.md
"Screen filters") are applied in the post pass before the brightness gamma (`shaders/filter.glsl`, both renderers): blur
levels are mips of a half-size copy of the resolved scene, taken only while a filter runs.

Brightness is a display gamma in the post pass (`out = in^(1 / brightness)`, after tonemap/FXAA), standing in for the
original's Brightness option (tru.exe stores it as registry `Brightness` × 1000, clamped to [0, 0.1], 0x51E3xx [O];
how the engine applies it is [U]).

Colour space [O]: tru.exe's sampler-state manager only sets address/filter/anisotropy states (5-8, 10, 1-3; decomp
around 0x5274xx) and never `D3DSAMP_SRGBTEXTURE` (11) or `D3DRS_SRGBWRITEENABLE` (194): the original samples, shades and
writes in gamma space. Both backends match that: UNORM textures, UNORM scene targets and swapchain (SRGB_NONLINEAR
colour space, no sRGB view), no conversion anywhere. Washed-out/dark differences are lighting, not gamma.

## Sun shadows (`render/sun.glsl`, `RenderCommon sunShadow`)

One GLSL library serves the generic scene shaders (GL `kSceneFs`, `vk/shaders/scene.frag`, `#include`d by glslc) and
the translated original pixel shaders (embedded as `sun_glsl.inc` and appended by `originalPixelSource`):

- Sun direction: the unit's sun (`Environment::sunDirection`, c209, negated) when it has one, else `kSunDirection`;
  the generic lambert uses the same direction, so shading and shadows agree.
- Cascaded shadow map: 3 cascades over the view distances [near, 1500], [1500, 5000], [5000, 16000], each a 2048²
  tile of one 6144×2048 depth atlas; per cascade an ortho fitted to the slice's bounding sphere with texel snapping,
  depth spanning the whole scene; slope-scaled bias (2, 2); opaque + alpha-blended (cut at 50 %) casters. Lookup: the
  finest cascade whose tile holds the point, 3x3 hardware-PCF taps.
- Per-frame `OuShadow` UBO (cascade matrices, sun, eye, camera ray basis, RT mode): GL binding 3 + texture unit 16;
  Vulkan set 1 (generic) / set 2 (translated), one set + host-coherent buffer per frame in flight.
- Translated shaders (`ShaderTranslate`): reads of the SunLight colour c212 become `c212 * ouSunVis`, so the shadow
  scales the sun's diffuse and specular only (light maps, ambient, point lights untouched). `ouSunVis` is computed first
  thing in `main()` (uniform control flow) from the fragment's world position — camera ray × view depth
  `1 / gl_FragCoord.w`, exact to float precision — and the face normal from its derivatives. `ouSunEnd` mixes in RT
  water reflections. Programs without c212 call nothing.


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
| Textures | `VK_FORMAT_BC1_RGBA_UNORM_BLOCK` (DXT1 with 1-bit alpha, as D3D9), `BC2_UNORM`, `BC3_UNORM`, `R8G8B8A8_UNORM`, `B8G8R8A8_UNORM`; full validated mip chains (`usableMips`). One combined-image-sampler descriptor set per texture from a per-scene pool; one repeat/linear/trilinear sampler with max anisotropy (with FSR a second one with negative mip bias, "FSR … Texture mip bias"). |
| Shaders | GLSL in `src/render/vk/shaders/`, compiled by `glslc --target-env=vulkan1.3 -O -mfmt=num` at build time into `build/vk_shaders/*.inc`, `#include`d as `uint32_t[]`; `scene.vert/frag` a second time with `-DOU_RT` (`scene_rt.*.inc`). Push constants: scene/shadow `{mat4 viewProj, model; int lighting; float alphaRef; int rt, pad}` (144 B, checked against `maxPushConstantsSize`; sun/eye/rays live in the `OuShadow` UBO), overlay `{vec2 screen}`, post `{int fxaa, hdr; float brightness}`. Pipelines: one per `scene::Blend` + lines (rebuilt with the MSAA count / HDR format / RT on-off), shadow (depth only, depth bias), RT depth pre-pass, post and overlay (swapchain format). `shaderDemoteToHelperInvocation` enabled for `discard`. |
| Pipeline cache | loaded from / saved to `$XDG_CACHE_HOME/openunderworld/vk_pipeline_cache.bin` (fallback `~/.cache/...`); drivers ignore incompatible blobs. |
| Screenshot | after `drawFrame`, a one-shot copy PRESENT_SRC → TRANSFER_SRC → buffer → PRESENT_SRC, BGRA swizzled to RGB. |
| CPU skinning | `updateVertices(meshIndex, vertices)`: scene-mesh → GPU-slot map, vertex count checked, copied into the current frame-in-flight's host-visible staging buffer (grown on demand; repeated updates before a draw overwrite their slot). `drawFrame` records the staged copies at the start of its command buffer behind one write-after-read barrier (no queue round-trip; the old per-call fence wait serialised CPU and GPU). See "FSR … Performance" for the l4_ballcourt profile. |
| Debug lines | `FrameOptions::debugLines`: expanded to `{vec3, unorm4}` vertices in a per-frame host-coherent buffer (grown to the next power of two), `LINE_LIST` pipeline after the scene, depth test LEQUAL without depth writes, `z -= 0.5e-4·w` (the GL backend's `1e-4·w` in GL's [-w, w] clip range), unlit. |
| Ray tracing | optional, see "Ray tracing" below: `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` (+ `deferred_host_operations`), `bufferDeviceAddress`, descriptor indexing, `shaderInt64` enabled when all are present. `VK_KHR_ray_tracing_pipeline` is not used. |

Known limits (deliberate):
- `upload()` and swapchain recreation call `vkDeviceWaitIdle`; fine for a viewer, not for streaming.
- Readback stalls the queue; it is a debugging/testing path.
- No sRGB, as tru.exe ("Colour space" above). Light maps, fog, wind and material tints come from the original shaders (SHADERS.md).

## Ray tracing (Vulkan, Enhanced, optional)

`--rt shadows` traces the sun shadow; `--rt full` also mirrors the scene in water. Needs `--preset
enhanced` (`config::resolve` rejects RT with `original`); default `auto` = shadows. Works inside the original
translated shaders ("Sun shadows": the traced visibility scales c212) and the generic ones alike. `--rt-quality low|medium|high`
= 1 / 2 / 4 shadow rays per pixel. The overlay line `RT ...` shows the mode, whether rays are
traced, BLAS / geometry / instance counts (water instances), TLAS builds / updates and BLAS refits /
rebuilds (`Renderer::rtStatus()`, `rtSummary`).

| Piece | Choice |
|---|---|
| Support | `probeRayTracing()`: the extensions + features above and ≥ 4098 sampled images per stage. Anything missing (or `OU_VK_NO_RT=1`, the simulated-unsupported switch) leaves the device without RT: `rtStatus().supported = false` with the reason, and a requested RT mode renders with the shadow map (`--rt` never fails). The GL backend reports unsupported. |
| Acceleration structures | Built by the first frame that asks for RT after `upload()` (nothing is built while RT is off). One BLAS per mesh with one triangle geometry per submesh that casts in the shadow map too (`Blend::Opaque` + `Blend::Alpha`; Additive / Average neither cast nor reflect), vertex/index buffers used in place via buffer device addresses. Static BLASes `PREFER_FAST_TRACE`; skinned meshes (`MeshData::skin`) `ALLOW_UPDATE + PREFER_FAST_BUILD`. One TLAS instance per BLAS with the mesh's model matrix (static: `GpuMesh::model` never changes), both faces (`TRIANGLE_FACING_CULL_DISABLE`), custom index = first geometry record; mask 2 for water, 1 otherwise. TLAS `ALLOW_UPDATE + PREFER_FAST_TRACE`. Upload builds are batched into ≤ 64 MiB of shared scratch. |
| Dynamic meshes | The frame's recorded vertex copies are followed by the BLAS work: refit (mode UPDATE) for skinned meshes (Lara, enemies), full in-place rebuild for others (pistols in ou-game are rigid and rebuilt), once per frame however often the mesh was updated, batched into as few build calls as the shared scratch holds. The TLAS is then updated in place once (not rebuilt). Choice [I]: refits keep the bind-pose BVH topology, so traversal degrades with large poses but stays correct. |
| Geometry records | storage buffer, one `{vertex address, index address, first index, texture slot, alpha ref}` per geometry, indexed by `instanceCustomIndex + geometryIndex` in the shader. |
| Alpha test (any-hit equivalent) | Non-opaque geometries (alpha ref ≠ 0: `Blend::Alpha` cut at 50 % as the shadow map, or an alpha-tested opaque material) are tested in the `rayQueryProceedEXT` loop: barycentric UV from the index/vertex buffers, `textureLod(..., 0)` of the bindless texture array (set 2, 4096 slots, partially bound), texture alpha only (as `shadow.frag`); candidates that pass are confirmed. |
| Shadows | `scene_rt.frag`: N rays per pixel toward the sun inside a cone of tan 0.012 (golden-angle spiral rotated per pixel by interleaved gradient noise), `TerminateOnFirstHit`, origin offset along the normal by 2 + 2e-4 · distance units. Spatial denoise: the visibility is averaged over the lanes of the 2x2 pixel quad that trace (fine derivatives; quads never span primitives, so no bleeding across edges); helper lanes, alpha-discarded lanes and lanes facing away from the sun trace nothing. No temporal accumulation (no history buffer / TAA yet): the per-pixel noise is fixed, so still frames are stable. |
| Overdraw | RT adds a depth pre-pass over opaque submeshes (`prepass.vert/frag`: same alpha test, `invariant gl_Position` as `scene_rt.vert`; translated programs pre-pass with their own invariant VS and the plain PS, i.e. their own alpha test and no sun term); the opaque colour pass then tests LEQUAL without writes, so each visible opaque pixel traces once. Blended submeshes (no depth writes, drawn layer over layer) keep sampling the shadow map, which is still rendered in RT mode: tracing them cost ~10 of 15 ms in the ballcourt foliage (measured). |
| Reflections (`full`) | Draws of water meshes trace one mirror ray (mask 1: water never reflects itself) with closest hit, alpha-tested like shadows, and shade the hit with texture × vertex colour × the raster lambert (no shadow ray at the hit; misses = clear colour), mixed in by Schlick Fresnel with F0 = 0.02 (alpha raised by the same factor). Water = meshes whose origin names a `*watersurface*` object [I] (l4_pool: `ca_goo_pool_watersurface`, the surface intro of the pool's `watervolume`, GAMEPLAY.md "Water"). No roughness: TRU's material blobs give none we decode [U], so water is a perfect mirror and wet surfaces get no reflections. |
| Non-finite inputs | Ray-query origins / directions must be finite (Vulkan spec); RADV hangs the GPU on others. [O] l4_ballcourt (`--spawn -20066,14150,3540,0`) and (reported by a sibling run) l1_kraken_room spawns lost the device (`ring gfx timeout`, SQC GPUVM fault) on the first RT frame, already at c3c69f2 (v0.9.0+2), only with the translated shaders' rays variant (generic shaders, `--rt off`, and the shadow-map variant in its place were fine; `RADV_DEBUG=syncshaders` and GPU-assisted validation still hung); skipping rays whose point or normal is not finite (`ouRayInputsFinite`, sun.glsl: shadow term from the quad's other lanes, no reflection) ends it, 6 × 2000 frames clean. [I] The culprits are fragments where the translated point (1 / `gl_FragCoord.w`) or its derivative normal is not finite; none reach the final image. ctest `game_vulkan_rt_smoke` runs both spawns. |
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
With RT: depth pre-pass, then opaque draws use the ray-traced pixel-shader variant (water: + reflections), blended ones
the shadow-map variant. Details, survey and evidence: docs/formats/SHADERS.md.
Vulkan: one pipeline per (program, pixel-shader variant plain / shadow map / rays, blend), the ones the current RT / shadow
settings select created on worker threads before the first frame that can draw them (after upload or a settings change); all
variants' SPIR-V is compiled at upload on up to 16 threads (compiling them on first draw cost ~30 % of l4_ballcourt's
frames as materials came into view; rays only on RT-capable devices), one descriptor set per distinct (program, material,
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

## Water (Enhanced, GL + Vulkan; `graphics.water`, `--water original|enhanced`)

Original (`water = 0`) draws water-surface meshes with TRU's own translated shaders, as before.
Enhanced (`water = 1`, the enhanced-preset default) replaces them with our own water shading
(`src/render/shaders/water.{vert,frag}`, shared by both backends; `src/render/Water.{hpp,cpp}`).
Water surfaces are meshes whose object name contains `watersurface` [I], e.g.
`ca_goo_pool_watersurface` in l4_pool; `render::isWaterSurface` is the shared test.

Technique (written for this project, no third-party shader code):
- Normals: four directional waves with analytic slopes, plus the game's ripple height field
  (central differences). Procedural wave amplitudes are ours [I].
- Refraction: a single-sample copy of the opaque scene colour + depth, distorted by the normal;
  foreground pixels are rejected. Thickness drives Beer-Lambert absorption (red attenuates
  fastest) into a blue-green tint, and caustics (animated ridged filaments) on the bottom.
- Reflections: Schlick Fresnel (F0 0.02) with screen-space ray marching against the opaque
  depth (20 steps, radial hit test), fog colour as the miss fallback. Sun glints from the unit's
  sun. The Original RT water-reflection path is not used in Enhanced; Enhanced reflections are
  screen-space in both backends.
- Foam where the bottom is shallow (shore/intersection) and on ripple crests.
- Splash droplets: camera-facing soft discs (`render::splashVertices`, `splash.{vert,frag}`).
- Underwater: when the camera is inside a `WaterVolume` below its surface, the post pass fogs
  into the same tint by reconstructed depth and adds caustics; the surface seen from below is
  tinted instead of mirrored. With `water = original` the post pass is byte-identical to before.

Data flow: `game::WaterSim` (docs/GAMEPLAY.md "Water") produces the ripple field, splash
droplets and the water volumes; ou-game fills `FrameOptions::{ripples, waterVolumes, splashes}`
every frame. `render::WaterBlock` (UBO: camera, inverse view-projection, sun, fog, ripple
transform, absorption) feeds the water, splash and post shaders.

Backend plumbing: GL blits the scene colour/depth into `opaqueColor/opaqueDepth` after the scene
pass (MSAA resolve included), then draws water + splashes into the same framebuffer. Vulkan
splits dynamic rendering into two passes: pass 1 ends with a colour resolve and, with MSAA, a
`SAMPLE_ZERO` depth resolve; without MSAA the depth is copied. Descriptors: water set 0 =
UBO + 3 textures, post set 1 = UBO + depth. The ripple field uploads as R32F only when
`WaterSim::version` changes.

Measured (2026-10-27, l4_pool viewer, Vulkan, 1792x1008): Original 4.80 ms, Enhanced 5.01 ms
average frame time over 49 frames. Viewed screenshots (GL and Vulkan, same camera): reflective,
refractive pool with shore foam and caustics; Lara treading with a wake; underwater tint + AIR bar.

Limits [I]: SSR misses off-screen geometry (fog fallback), one simulated surface (the one Lara
is in or nearest), no water flow/buoyancy, procedural caustics are not light-accurate, foam noise
is hashed cells, no ripples in ou-viewer (it has no simulation).

### Water detail, debug views (`graphics.water_detail`, `--water-detail`, `--water-debug`, F7)

`water_detail = high` (the enhanced-preset default with Enhanced water; `FrameOptions::waterDetail`,
`WaterBlock::detail`) adds, in the same shaders: a swell layer (two broad value-noise layers with
analytic gradients drifting apart) and a flow layer domain-warped by the swell and counter-rotated
against a second layer, faded where it would alias (`fwidth`); a geometry swell in `water.vert`
(±10 units, only where the mesh has interior vertices); a sun glint made of a broad sheen, a sharp
sparkle modulated by the wave noise and a broken "glitter path", hidden where an 8-step march toward
the sun through the opaque depth hits an occluder; thickness-tested SSR (24 steps, hit only when the
ray passes behind the depth buffer by less than 40 + 0.1·t units, 4 bisection steps, confidence
faded at the screen border and with distance) blended into an environment colour (fog colour, a
brighter sky term and a sun lobe) instead of a hard fog fallback; open water with no floor in the
depth buffer is treated as deep instead of refracting the sky; reflections of the four lamps
nearest the camera (unit point/spot lights, Schlick-weighted specular); crest foam from the wave
noise. `standard` is the v0.5 water unchanged.

Debug views (`--water-debug N` in ou-game / ou-viewer, F7 cycles in ou-game; `WaterBlock::detail.y`):
1 wave normals, 2 water mask (magenta, shallow) to depth (blue, deep), 3 SSR hit (green) vs
environment fallback (red), 4 sun glint (white) and its sun occlusion (blue), 5 Fresnel term.

Water surface for the atmosphere: the Enhanced water pass does not write depth, so the post pass
intersects the view ray with the tops of the eight nearest `WaterVolume`s (`WaterBlock::volumes`,
surface = max.z) and ends the fog / shaft medium there, not at the sea bed.

## Atmosphere (Enhanced, GL + Vulkan; `graphics.atmosphere`, `--atmosphere`, `graphics.light_shafts`, `--shafts`)

Post pass (`src/render/shaders/atmosphere.glsl`, included after `underwater.glsl`; applied after tonemap /
brightness, before the underwater effect, which owns the submerged view). `off` returns the input unchanged.
- `low`: height fog along the view ray up to the scene, the water surface (above) or 60000 units:
  a haze layer (1/60000 per unit, falloff 1/2200) and a ground mist (1/30000, falloff 1/300) based
  at the nearest water surface or 250 units below the camera, constant below their base so pits stay
  finite (closed-form integral, no marching). In-scattering = the unit's fog colour plus
  Henyey-Greenstein (g 0.6) forward scattering of the unit's sun.
- `medium` (enhanced default): + sun shafts for units with a sun: 24 taps from the pixel toward the
  sun's screen position (jittered), open where the depth is beyond 0.6 of the medium range (sky),
  weighted by a 0.965 decay, HG (g 0.8) and the air in front of the pixel; fades as the sun leaves
  the screen. `high`: 48 taps. `light_shafts = false` keeps the fog only.
Values are ours [I]. Screen-space shafts miss off-screen occluders.

## Weather (Enhanced, GL + Vulkan; `graphics.weather`, `--weather auto|off|rain|snow`)

`auto` (enhanced default) shows the precipitation the original places in the unit: survey of all
units (`GameReal.UnitWeatherSurvey`, `game::unitWeather`): snow where there are `fx_snow_blizzard_*`
/ `fx_snow_white_out` (l5_startsnow, l7_thin_ice, l7_himalaya_end; intensity 1) or
`fx_snow_falling_*` (l5_valaskjalf, l5_valgrind, l7_monastery; 0.55); rain where `rainfx` is placed
(l6_ship_exterior; 0.8; `rainfx_mex` is in no unit). Rain sheets / gutters (water pouring through
openings, Kraken rooms, Mexico temples) and cinematic-only effects are local effects, not weather.
The original's own particle effects, glows and effect lights run separately (`src/fx`, docs/formats/EFFECTS.md;
drawn in every preset; effect lights use 4 reserved environment-light slots after the gadget lights, linear
falloff, and endless effect lights born with placed objects join the scene lights; material particles read a
single-sample copy of the scene colour / depth taken once before the effects — GL `drawFx`, Vulkan the second pass's
`recordWaterCopies` — for refraction and soft depth fades, FxBatch::screen). ou-game eases the intensity to 0 while
collision lies within 3000 units above the camera (roofs, caves).
Post pass, same file: four layers on camera-centred cylinders (radius 110, 240, 500, 1000), anchored
in azimuth and world height so turning does not drag them; each layer is skipped where the scene is
nearer (depth-tested against walls and roofs). Rain: thin wind-slanted streaks falling 1100 units/s,
antialiased with `fwidth`; snow: soft flakes falling 85 units/s with a sinusoidal drift. Rain also
darkens upward-facing surfaces (normals from depth derivatives; water surfaces from the volume
intersection) and draws expanding splash rings on them. Skipped [I]: lens droplets, wet specular,
snow accumulation.

Gap list against modern-wow-renderer (read for techniques only, no code; RESEARCH.md §8.6):

| Technique there | Before (v0.5 Enhanced water) | Now |
|---|---|---|
| Noise swell, warped/counter-rotated flow normals | four sine waves + ripple field | water detail: swell + flow layers |
| Geometry waves | none | water detail: vertex swell (coarse meshes show little) |
| Sun glint: broad + sparkle, glitter variation, sun path | one specular lobe | water detail |
| Glint occluded by depth-buffer march toward the sun | none | water detail (8 steps) |
| Thickness-tested SSR, border fade, environment fallback | radial hit test, fog fallback | water detail (24 steps + bisection) |
| Open water without floor = deep | sky colour refracted | water detail |
| Fresnel, depth colour, shore / crest foam | yes | + noise crest foam |
| Local light reflections on water | none | water detail: 4 nearest lamps |
| Water mask + surface depth for the atmosphere | none | water volume tops intersected in the post pass |
| Water diagnostics / highlight views | none | `--water-debug`, F7: 5 views |
| Two-layer analytic height fog, HG lobe, rolling noise | none | atmosphere low+ (no noise layer) |
| Sun shafts (screen-space radial there) | none | shadow-map ray march, medium/high (handles off-screen occluders) |
| Low-res atmosphere + temporal reprojection, local fog field, contact shadows | none | skipped (full-res single pass is cheap enough; no temporal history) |
| Depth-tested rain / snow layers, roof occlusion | none | weather: 4 layers, depth test, camera shelter ray |
| Surface splashes, wetness | none | rain rings + darkening |
| Lens droplets, sandstorm, weather audio | none | skipped (TRU has no sandstorm; lens/audio not requested by data) |

Cost (2026-10-28, RX 7900 XTX, 1280x720, ou-viewer frame-time average over 289 frames in an invisible KWin):
l4_pool water view 4.98 ms standard water, 4.82 ms water detail, 4.83 ms detail + rain; l4_ballcourt foliage
view 124 / 112 / 114 / 111 ms for atmosphere off / low / medium / high (that view is CPU-bound; differences are
run-to-run noise). No cost is measurable above the noise of these runs; GPU timestamps were not taken.

Techniques credited in docs/RESEARCH.md §8.6 (modern-wow-renderer: inspiration only, no code).

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

**Enhanced** (opt-in per feature, GL + Vulkan)
- Linear-space lighting, higher-precision shadows, MSAA/TAA, SSAO, HDR output.
- Enhanced water (both backends): waves, refraction, depth absorption, SSR reflections, foam,
  caustics, interactive ripples and splashes, underwater fog ("Water" above).
- Optional ray tracing on hardware exposing `VK_KHR_ray_query` + `VK_KHR_acceleration_structure`:
  RT sun shadows and water reflections via ray queries in the raster pipeline, shadow map as
  fallback (implemented, "Ray tracing" above); RT AO / GI not started.
- Any enhancement must be switchable per feature so a difference can be traced back to it.

The OpenGL backend stays the portability/reference path for Original Fidelity; Enhanced
features land in Vulkan first.

## FSR upscaling + frame generation (Vulkan only; `graphics.upscaler off|fsr`, `--upscaler`)

AMD FSR SDK **2.4.0** (ffx-api) built from source (`third_party/fidelityfx-sdk/README.md`): FSR 3.1.6 upscaler and
FSR 3.1.6 frame generation, Vulkan backend, providers linked statically. OpenGL ignores it ("FSR: Vulkan only" in F1).

- **Why not FSR 4 / the 2.4.0 binaries**: FSR 4.1.2 ML upscaling, FSR 4.0.x frame generation and the ffx loader ship
  only as signed Windows DLLs (`signedbin/amd_fidelityfx_{loader,upscaler,framegeneration}_vk.dll`, PE, binary-only
  licence forbidding reverse engineering) — they cannot load in a native Linux process. The open part has no build
  system, no shader permutation headers / generator and HLSL-only shaders; we build it with our CMake and DXC
  (`tools/ffx_shaders.py`). The Vulkan frame-generation swapchain is Win32-bound, so presentation is ours.
- **Settings**: `graphics.fsr_quality` / `--fsr-quality native|quality|balanced|performance|ultra_performance` (render
  scale 1, 1/1.5, 1/1.7, 1/2, 1/3), `graphics.sharpness` / `--sharpness 0..1` (RCAS, default 0.5),
  `graphics.frame_generation` / `--frame-generation on|off` (needs FSR). Launcher: Graphics page. With FSR on, MSAA and
  FXAA are off (FSR anti-aliases). `OU_VK_NO_FSR=1` disables it.
- **Frame**: the scene (all passes incl. water, fx, post: fog, underwater, tonemap, brightness) renders at the render
  resolution with FSR's Halton jitter (`Camera::jitter`, applied to the generic and the translated programs'
  projections). Then a mask pass (`fsr_camera.frag`, `fsr_mask.*`) writes motion vectors (UV, previous − current,
  unjittered; RG16F): camera motion of every pixel from depth, overwritten for meshes moved by `updateVertices`
  (CPU-skinned characters, animated objects; the pose the last frame drew is copied GPU-side into the mesh's
  `prevVertices` at the start of the frame, before the new upload lands — no CPU copy, no re-upload) — and
  reactivity (R8, max-blended): blended submeshes 0.9 × alpha, Enhanced water 0.6, original fx sprites
  (`fsr_fx.frag`: alpha blend → alpha, additive / subtract → max(rgb · a), multiply → |rgb + a − 1|) and splash droplets
  (`fsr_splash.frag`) 0.9 × their coverage, rain / snow 0.9 × the post pass's precipitation coverage
  (`fsr_weather.frag`, `atmosphere.glsl` `ouPrecipitation`). The reactive draws are skipped when the provider does not
  consume a reactive mask (ffx resource-requirements query; FSR 3.1 takes it as optional input). The upscaler context
  carries `ffxCreateContextDescUpscaleVersion` (SDK 2.1+ contract). The post pass writes `fsrInput_` (LDR,
  display-referred), FSR upscales into an RGBA8
  image, which is blitted into the swapchain; the UI / HUD / overlay is drawn after the upscale at display resolution.
  Depth is standard (0 near, 1 far, finite far plane). Exposure 1 (input already tonemapped). Camera cuts
  (> 500 units or > 45°) reset the history.
- **Frame generation**: per frame, FSR FG prepares from depth + motion and interpolates between the previous and this
  frame's HUD-less upscaled image; the UI is drawn on both. A present thread (started with the first generated frame;
  afterwards every present goes through it, in order) presents the generated image no earlier than half an interval after
  the previous real one, then the real one half an interval later, so the render thread goes straight on with the next
  frame. Interval = 0.9/0.1 moving average of the render thread's frame time without its waits for swapchain images
  (counting those would lock the pacing at any interval it once had). While further frames are queued the thread presents
  without waiting. Queue submits / presents and swapchain acquires share one mutex (Vulkan external synchronisation);
  acquires poll with timeout 0 under it, so the render thread never blocks inside the driver on an image only the present
  thread can free; device-idle waits drain the present queue first. The swapchain gets minImageCount + 2 images. Each
  frame shows the previous real frame's successor half a frame late (latency +½ frame).
- **Performance** (`tests/replays/ballcourt_door_lever.txt` in l4_ballcourt, 400 frames, `OU_VK_PRESENT_STATS=1`, no vsync, RT on, RX 7900 XTX):
  `perf` showed the game CPU-bound at 21.7 fps (1440p native) on three things, all fixed: CPU skinning of every
  enemy every frame (32 % of the main thread; now enemies outside the view cone beyond 1500 units keep their pose, and
  single-bone vertices reuse one world × palette matrix per bone, bit-identical), compiling the translated programs'
  shadow-map / ray-traced variants on first draw (29 %; now at upload, in parallel), and `updateVertices`' per-call
  fence wait (now recorded into the frame). That made it GPU-bound; per-pass GPU timestamps (`OU_VK_PRESENT_STATS`) then
  showed, at 1440p native: scene 11.0 ms (8 of them ray-traced sun shadows), shadow map 3.4 ms, uploads + BLAS refits
  1.7 ms. Second round, all output-identical:
  - shadow cascades skip meshes whose bounding sphere misses the cascade's x/y clip range (`outsideOrtho`; GL too):
    3.4 → 1.0 ms;
  - per-frame BLAS refits / rebuilds batched into as few `vkCmdBuildAccelerationStructuresKHR` calls as the shared
    scratch holds (was one build + two barriers per mesh): 51 → 58 fps;
  - the visible enemies' meshes skin on worker threads, uploads stay in order on the render thread (GL 51 → 65 fps);
  - translated pipelines for the current RT / shadow settings are created on worker threads before the first frame
    that can draw them (after upload or a settings change), not on first draw: with cold driver and pipeline caches the
    p99 present interval drops 25.8 → 19.3 ms; the original light set is computed once per mesh and frame, not per
    draw, by insertion instead of `std::sort`;
  - level load: resource relocations are scanned linearly instead of building a map per lookup (l4_ballcourt load
    19 → 13 s).
  Host-visible VRAM staging for the vertex uploads was tried and rejected (GPU copies 1.7 → 0.4 ms, but the CPU writes
  dropped 58 → 39 fps). Presented frames per second over the 400 frames, before (58c5aeb) → after:

  | Scene / output | Native | FSR Quality | FSR Performance | Quality + FG (presented; real = half) |
  |---|---|---|---|---|
  | l4_ballcourt 2560×1440 | 49.9 → 67.2 | 59.1 → 83.2 | 65.1 → 93.8 | 110.8 → 151.0 |
  | l4_ballcourt 3840×2160 | 35.8 → 44.0 | 46.9 → 61.8 | 55.4 → 76.5 | 84.2 → 104.2 |
  | l4_pool 2560×1440 | 269 → 445 | 258 → 411 | 269 → 445 | 395 → 576 |
  | l4_pool 3840×2160 | 222 → 328 | 211 → 309 | 231 → 346 | 271 → 354 |

  (l4_pool: `--spawn -6045,-6100,2844,-90` + `tests/replays/pool_gadgets.txt`; ~2 ms of GPU work, CPU-bound, so FSR
  only adds its own cost there.) Frame generation costs about 1 ms of real frame time (l4_ballcourt 1440p: 83.2 real
  fps without, 75.5 with) and doubles presented frames. Original preset with FSR off, and the Enhanced defaults on
  Vulkan, are byte-identical before / after (ou-imgdiff, frame 240 of both replays, GL and Vulkan: mean 0.000).
- **Texture mip bias**: with FSR active the scene's texture and material sets sample with a second sampler biased by
  log2(render / display width) − 1 (AMD's FSR guidance: −1.58 Quality, −2 Performance), so upscaled textures keep
  display-resolution detail; recreated with the swapchain, native rendering keeps the unbiased sampler (and so do the
  RT alpha tests).
- **Diagnostics**: F1 line `FSR 3.1.6 quality 1280x720 -> 1920x1080 | frame generation on, FG 3.1.6 (N generated)`.
  `OU_VK_PRESENT_STATS=1` prints presented images/s, present spacing (mean / median / sd / p99, alternating intervals)
  and the render thread's per-frame fence / acquire waits and `vkQueuePresentKHR` time at exit, and the GPU time per
  frame of vertex uploads + BLAS refits, TLAS update, shadow map, scene, FSR masks + post and FSR / FG (timestamp
  queries); with OpenGL it prints presented frames per second. `OU_VK_CAPTURE_DIR=dir`
  writes every presented image (`NNNNNN-real.ppm`, `NNNNNN-gen.ppm`).
- **Known gaps**: translated vertex animation (wind) has no motion vectors; ray-traced sun shadows remain the largest
  GPU cost (~8 ms of l4_ballcourt's 1440p frame). Khronos validation reports upstream SDK issues only: the FG pass writes a
  3-component texel to the RGBA8 output (`VUID-vkCmdDispatch-OpImageWrite-08795`, harmless on RADV), and the upscaler's
  storage images declare `Rg32f` / `R32f` formats over our R16G16 / R16 float images (warnings, once per pass at start); SDK patch 6 (8
  queued frames) fixed FG destroying image views still in flight. Unsupported devices (no formatless storage images, compute derivatives, independent blend,
  subgroup ops) or a build without DXC fall back to native rendering with the reason in F1.
