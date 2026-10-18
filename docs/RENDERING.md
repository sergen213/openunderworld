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
| Shaders | GLSL in `src/render/vk/shaders/`, compiled by `glslc --target-env=vulkan1.3 -O -mfmt=num` at build time into `build/vk_shaders/*.inc`, `#include`d as `uint32_t[]`. Push constants: scene/shadow `{mat4 viewProj, model, lightViewProj; int lighting, shadows; float alphaRef}` (204 B, checked against `maxPushConstantsSize`), overlay `{vec2 screen}`, post `{int fxaa, hdr}`. Pipelines: one per `scene::Blend` + lines (rebuilt with the MSAA count / HDR format), shadow (depth only, depth bias), post and overlay (swapchain format). `shaderDemoteToHelperInvocation` enabled for `discard`. |
| Pipeline cache | loaded from / saved to `$XDG_CACHE_HOME/openunderworld/vk_pipeline_cache.bin` (fallback `~/.cache/...`); drivers ignore incompatible blobs. |
| Screenshot | after `drawFrame`, a one-shot copy PRESENT_SRC → TRANSFER_SRC → buffer → PRESENT_SRC, BGRA swizzled to RGB. |
| CPU skinning | `updateVertices(meshIndex, vertices)`: scene-mesh → GPU-slot map, vertex count checked, staged copy into the device-local vertex buffer behind a write-after-read barrier, waited on a fence (one queue round-trip per call). |
| Debug lines | `FrameOptions::debugLines`: expanded to `{vec3, unorm4}` vertices in a per-frame host-coherent buffer (grown to the next power of two), `LINE_LIST` pipeline after the scene, depth test LEQUAL without depth writes, `z -= 0.5e-4·w` (the GL backend's `1e-4·w` in GL's [-w, w] clip range), unlit. |
| Ray tracing | detection only: `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_ray_tracing_pipeline` are reported in `description()` / the overlay. Nothing is enabled. **Observed** on RX 7900 XTX / RADV: all three present. |

Known limits (deliberate):
- `upload()` and swapchain recreation call `vkDeviceWaitIdle`; fine for a viewer, not for streaming.
- Readback stalls the queue; it is a debugging/testing path.
- No sRGB; no light maps, fog, wind or per-shader material tints (MATERIALS.md).

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
- Optional ray tracing on hardware exposing `VK_KHR_ray_query` + `VK_KHR_acceleration_structure`
  (detected today, unused): RT shadows / AO / reflections via ray queries in the raster
  pipeline, with the raster path as fallback. Requires BLAS/TLAS management fed from
  `scene::SceneData` and skinned-vertex updates [inferred design, not started].
- Any enhancement must be switchable per feature so a difference can be traced back to it.

The OpenGL backend stays the portability/reference path for Original Fidelity; Enhanced
features land in Vulkan first.
