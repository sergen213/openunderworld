# Building and running

## Dependencies (Arch / CachyOS package names)

`cmake ninja gcc` (or clang), `zlib-ng-compat`, `sdl3`, `libepoxy`, `glm`, `gtest`.
A GPU/driver with OpenGL 4.5+ core (4.6 requested; developed on Mesa radeonsi).

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
