# Building and running

## Dependencies (Arch / CachyOS package names)

`cmake ninja gcc` (or clang), `zlib-ng-compat`, `sdl3`, `libepoxy`, `glm`, `gtest`,
`vulkan-headers vulkan-icd-loader shaderc` (glslc) for the Vulkan backend.
`openssl nlohmann-json` (importer), `libsodium curl` (updater, `ou-sign`), `qt6-base` (`ou-launcher`;
skipped with a CMake notice if Qt 6 is missing). Launcher and updater: `docs/LAUNCHER.md`, `docs/UPDATES.md`.
`ffmpeg` (libavformat/libavcodec/libswscale/libswresample: `ou-movie`, `ou_video`) and optionally
`sdl3_ttf` (movie subtitles in a system font, else SDL's ASCII debug font; `docs/formats/VIDEO.md`).
A GPU/driver with OpenGL 4.5+ core (4.6 requested; developed on Mesa radeonsi); Vulkan 1.3
for `--renderer vulkan` (see "Vulkan backend" below).

No vendored libraries: OpenGL entry points come from libepoxy, the overlay font is SDL3's
built-in 8x8 debug font (`SDL_RenderDebugText`) rasterised once into a texture atlas.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOU_SANITIZE=ON   # ASan+UBSan
cmake --build build
ctest --test-dir build --output-on-failure
```

Release: `-DCMAKE_BUILD_TYPE=Release -DOU_SANITIZE=OFF`. Debug builds request a GL debug
context and print `KHR_debug` messages (non-notification severities) to stderr.

`viewer_placeholder_screenshot` opens a real window for 5 frames and writes
`build/viewer_placeholder.ppm`; it is reported as *skipped* (exit 77) when SDL finds no
video device (e.g. `env -u WAYLAND_DISPLAY -u DISPLAY`).

Tests that need the original game read `OU_GAME_DIR` and skip when it is unset.

## ou-viewer

```sh
build/ou-viewer                                   # placeholder scene (labelled PLACEHOLDER)
build/ou-viewer --game-dir "/path/to/Tomb Raider Underworld" --drm 'pc-w\lara.drm'
build/ou-viewer --width 3440 --height 1440 --fov 60
build/ou-viewer --frames 5 --screenshot out.ppm   # render N frames, save PPM, exit
```

| Option | Meaning |
|---|---|
| `--game-dir DIR` | install containing `bigfile.000` (read-only access) |
| `--drm PATH` | bigfile path, e.g. `pc-w\lara.drm` (quote the backslash in the shell) |
| `--width W --height H` | initial window size in logical pixels (HiDPI scaling applies) |
| `--fov DEG` | vertical FOV; horizontal FOV follows the aspect ratio (Hor+, ultrawide-safe) |
| `--frames N` | exit after N frames |
| `--screenshot FILE` | after the last frame, save the back buffer as binary PPM (implies `--frames 1`) |
| `--no-vsync` | start with vsync off |
| `--cam X,Y,Z,YAW,PITCH` | initial camera position (game units) and yaw/pitch in degrees |
| `--renderer opengl\|vulkan` | render backend (default `opengl`); unavailable backend → exit 77 |
| `--no-overlay` | start with the diagnostics overlay hidden (used for GL/Vulkan image diffs) |
| `--fullscreen` | start fullscreen |
| `--anim ID\|#INDEX\|NAME` | play a clip of `--drm` on its skinned meshes (anim id, anim-list index or name), looping on the 60 Hz tick, in place (`docs/formats/ANIMATION.md`) |
| `--anim-time SECONDS` | start time of `--anim` (deterministic screenshots with `--frames 1`) |
| `--anim-list` | print the object's animation list (index, id, keys, seconds, name) and exit; no window |
| `--config FILE` | launcher config JSON (`docs/LAUNCHER.md`), applied first; other flags override |

Controls: hold the right mouse button for mouse look (relative mouse mode); WASD move, Q/E
down/up, Shift ×4 speed, mouse wheel scales speed; gamepad left stick move, right stick
look, triggers up/down; `V` vsync, `L` lambert/unlit, `C` backface culling, `F1` overlay,
`Esc` quit.

Without `--cam` the camera frames the bounds of all meshes from outside; use `--cam` to
start inside a level.

What is shown:
- no `--drm`, or loading fails → **PLACEHOLDER SCENE** (checker cubes; RGBA8, BGRA8 and a
  synthetic BC1 texture; BC3 is exercised by `lara.drm`, BC2 by no tested file yet). The
  overlay names the failure.
- textures decode but no meshes → **REAL TEXTURES on PLACEHOLDER quads**.
- otherwise the decoded meshes with their textures.

Convert a screenshot: `ffmpeg -i out.ppm out.png`.

## Renderer notes

- Backend interface `src/render/Renderer.hpp`; OpenGL 4.6 (DSA) in `src/render/gl/`. App and
  scene code never include GL headers, so a Vulkan backend can implement the same interface.
- World is Z-up (cdc convention, confirmed by the format parsers), units are game units.
- Simulation (camera movement) runs at a fixed 60 Hz (`src/engine/FrameClock.hpp`) and is
  interpolated by `alpha()` for rendering; mouse/stick look is applied per rendered frame.
- **sRGB decision:** textures are uploaded as UNORM (not `*_SRGB`) and the default
  framebuffer is written without `GL_FRAMEBUFFER_SRGB`, i.e. shading happens in gamma
  space, matching how a Direct3D 9 game without explicit sRGB read/write states renders
  [INFERENCE: TRU's D3D9 sampler/render states are not yet verified]. Revisit when the
  original shader pipeline is understood; switching later means `GL_COMPRESSED_SRGB_*` /
  `GL_SRGB8_ALPHA8` internal formats plus `GL_FRAMEBUFFER_SRGB`.
- BC1/BC2/BC3 use `GL_EXT_texture_compression_s3tc` (checked at runtime; missing → warning
  and the texture is skipped). DXT1 is uploaded as RGBA (1-bit alpha, as in D3D9).
- Anisotropic filtering at the driver maximum (core in 4.6, or ARB/EXT extension).
- Texture alpha is ignored (opaque draw); TRU alpha channels are often masks [INFERENCE].

## Verification status (2026-10-18, GOG build, RX 7900 XTX / Mesa 26.2.4)

Observed:
- Placeholder scene renders (checker ground + 25 tinted cubes, RGBA8/BGRA8/BC1 textures),
  overlay legible; `viewer_placeholder_screenshot` passes; exits 77 with no display.
- `--width 2560 --height 720 --fov 50` (32:9) gives hFOV 117.8°, undistorted (Hor+).
- `--drm 'pc-w\lara.drm'`: 7 meshes, 24 textures (18 BC1, 5 BC3, 1 BGRA8); Lara in bind
  pose, fully textured, correct Z-up orientation.
- `--drm 'pc-w\l4_pool.drm'`: 8 meshes, 18 BC1 textures. Default framing shows the whole
  textured level block (walls, pillars, tiled floor, lower colonnade); with
  `--cam -3595,-160,9000,53,-35` the interior shows textured stone pillars, floor tiles and
  walls. The loader skips 3 untextured batches (no texcoords, depth/fog-only material;
  likely occlusion/fog volumes [INFERENCE]) that otherwise enclosed and hid the level.
- Fixed tick: 240 vsync'd frames → 88 ticks over ~1.5 s (~60 Hz); `--no-vsync` raises FPS.
- No ASan/LSan reports and no `KHR_debug` messages in these runs.

Not exercised: interactive window resize (viewport is set from the pixel size every
frame), gamepad input (no pad attached), runtime `V` toggle.

## Vulkan backend

`build/ou-viewer --renderer vulkan [...]` selects the Vulkan 1.3 backend
(`src/render/vk/`, architecture in [RENDERING.md](RENDERING.md)). Build requirements:
`vulkan-headers`, `vulkan-icd-loader` (linked as `libvulkan`; no volk) and `glslc` (package
`shaderc`). The four GLSL shaders in `src/render/vk/shaders/` are compiled at build time into
`build/vk_shaders/*.inc` and embedded; editing a shader rebuilds only the backend.

Runtime:
- Needs a Vulkan 1.3 device with a graphics+present queue and `VK_KHR_swapchain`. If the
  loader, an ICD or such a device is missing, `ou-viewer` prints "vulkan renderer unavailable"
  and exits 77 (ctest skip); `VK_DRIVER_FILES=/nonexistent.json` reproduces it.
- Validation: `VK_LAYER_KHRONOS_validation` is enabled automatically in Debug builds when it is
  installed (`OU_VK_VALIDATION=0` disables, `=1` enables in Release). Messages of warning
  severity and above, from the layer and from the loader, go through a `VK_EXT_debug_utils`
  messenger to stderr as `[vk warn]` / `[vk error]`. If the layer is not installed, only loader
  messages appear; install `vulkan-validation-layers` and rerun the tests below once to validate.
  Loader errors about third-party implicit layers (e.g. `liblsfg-vk-layer.so`) come from the
  local layer setup, not from this project; `VK_LOADER_LAYERS_DISABLE='~implicit~'` silences them.
- Pipeline cache: `$XDG_CACHE_HOME/openunderworld/vk_pipeline_cache.bin` (default
  `~/.cache/...`), written at shutdown.
- The overlay line `RT detected: ...` reports `VK_KHR_acceleration_structure`,
  `VK_KHR_ray_query` and `VK_KHR_ray_tracing_pipeline`; ray tracing is not used yet.

Tests (all skip without a display or a usable Vulkan device):
- `viewer_vulkan_placeholder_screenshot`: 5 frames + PPM, like the GL test.
- `viewer_vulkan_matches_gl`: renders the placeholder with both backends (`--no-overlay`) and
  requires `ou-imgdiff` mean |Δ| ≤ 2/255.
- `VulkanRenderer.ResizeRecreatesSwapchain` (`ou_vk_tests`): draws, resizes the SDL window
  320x240 → 640x200, feeds `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` into `resize()` as the viewer
  does, checks the read-back size and content on the new swapchain; also checks re-upload and
  `updateVertices`, and that red `debugLines` appear only when passed.

`ou-imgdiff a.ppm b.ppm [MAX_MEAN]` prints mean absolute channel error, the share of pixels
off by more than 16 and PSNR; exit 1 above `MAX_MEAN` (default 4).

ASan note: Mesa RADV 26.2.4 leaks one 256-byte `pthread_once` allocation per process (a bare
`vkCreateInstance`/`vkDestroyInstance` program reproduces it). The Vulkan tests run with
`LSAN_OPTIONS=fast_unwind_on_malloc=0:suppressions=tests/lsan_vulkan.supp`; use the same for
manual ASan runs. With `SDL_VIDEO_DRIVER=x11` (XWayland) the Vulkan viewer additionally reports
~50 KB of leaks whose stacks all start in `SDL_Init` (none in project code); Wayland, the
default here, is clean.

### Vulkan verification status (2026-10-18, RX 7900 XTX, RADV Mesa 26.2.4, KDE Wayland)

Observed (Debug + ASan/UBSan, Khronos validation layer 1.4.363 active):
- Placeholder scene and `pc-w\lara.drm` render the same as GL. Without the overlay the
  1792x1008 frames differ from GL by mean |Δ| 0.001/255, 0.001 % pixels > 16, PSNR 61 dB
  (lara) and 0.015/255, PSNR 50 dB (`pc-w\l4_pool.drm`, 18 BC1 textures); with the overlay
  the description line differs (0.5/255). BC1, BC3 and BGRA8 (lara) and RGBA8 (placeholder)
  upload with full mip chains.
- `--width/--height` 800x600, 2560x720 (32:9), 333x777 and an XWayland 1000x500 run render
  at the matching HiDPI pixel sizes; the runtime resize test passes.
- Zero validation messages and, with the RADV suppression, zero LSan/UBSan reports in these
  runs; clean shutdown (all child objects destroyed before `vkDestroyDevice`, checked by the
  layer).
- Exit 77 with no display (`env -u WAYLAND_DISPLAY -u DISPLAY`) and with no ICD.
- `--show-collision` on `pc-w\l4_pool.drm` (6043 collision faces, 73 markup polylines as
  debug lines): GL vs Vulkan mean |Δ| 0.009/255, PSNR 49 dB. `--anim '#0' --anim-time 0.5` on
  `pc-w\lara.drm` (CPU skinning via `updateVertices`): mean |Δ| 0.001/255, PSNR 66 dB; both
  screenshots viewed (lines over the level, Lara posed).

Not exercised: interactive mouse-drag resize and minimise, the `V` vsync toggle at runtime
(both go through the same recreate path as the tested resize), non-AMD drivers, BC2 data.
