# WaterFluid status (agent/WaterFluid, continuation notes; delete before merge)

## Done
- `game::SplashBurst` replaces the ballistic `SplashDrop`s (src/game/Water.*): `entryBursts` (scaled by entry speed, swan
  dive = previous clip `ng_inair_swan*`), `impactBursts` (shots, bodies, dynamics), climb-out and stroke sprays; delayed
  Worthington jets wait in `pending_`; `bursts()` = last 64 with serials. Entry also fires on any plunge into a volume
  (`ps.water` from -1 to >= 0 with vz < -300): in l4_pool the eitr kills Lara on contact (fall -> dead, no SwimSplash).
- ou-game (src/app/game/main.cpp) forwards new bursts as `FrameOptions::fluidBursts` (render::FluidBurst).
- render/Fluid.{hpp,cpp}: constants, GPU structs, `Sim` (fixed 1/120 s steps, max 3/frame, ring slots, range culling,
  lifetime / clear logic), `computeSource()`.
- shaders/fluid.comp + fluid.glsl: PBF (Macklin & Mueller 2013), hashed linked-list grid (65536 buckets), 2 iterations,
  XSPH, Akinci cohesion, surface merge + C3 kicks, screen-space depth collision.
- splash.vert (vertex pulling from the pool), splash_splat (RGBA32F: depth, thickness, foam), blur (vec4), composite
  (refraction of the colour copy refreshed after the water surfaces, absorption, Fresnel, foam).
- VK: createFluid/destroyFluid/recordFluid, splash set bindings 2 (pool) / 3 (colour), RGBA32F targets, FSR splash
  reactivity now fullscreen over the splat target, `fluidKickBuffer()`; GL: same (`simulateFluid`, `fluidKickBuffer()`).
- Timing: OU_VK_PRESENT_STATS prints "splash fluid GPU ms per drawing frame" (VK + GL).
- Tests: ou_shader_tests `SplashFluid.*` (4), ou_game_tests `Game.Water*` (entry crown, delayed jet, scaling). Pass.
- Docs: GAMEPLAY.md water section, RESEARCH.md citations, RENDERING.md bullet (the "### Splash fluid" section itself is
  NOT written yet).

## Measured (VK, RX 7900 XTX RADV, 1920x1080, l4_pool plunge from 3500)
- 0.55-0.65 ms per drawing frame (sim 0.28-0.38, draw 0.27) for ~3500 particles. Idle: 0 (passes skipped).

## Open / remaining
1. VISUAL BUG: side view (openunderworld-work/waterfluid-shots/side-vk-seq.png, camera -3000,-1400,2950 -> -3000,0,2700)
   shows the splash as a white block extending BELOW the pool surface and reading as foam, not liquid. Check: burst
   surfaceZ vs the drawn pool surface (WaterSim::waterAt for l4_pool), splash_splat's `vSurface` discard, fluid.comp's
   kill test (`surfaceZ()`), and whether particles rise at all (crown up 650+420e). Then tune foam (`ouFluidFoam`
   thresholds 0.15..0.45 are too high; try 0.05..0.2) and the composite's white mix (0.85).
2. GL capture not yet run (need --screenshot at a splash frame, no OU_VK_CAPTURE_DIR on GL).
3. Write docs/RENDERING.md "### Splash fluid" (passes, bindings, costs, C3 kick contract) before "### Water colour".
4. C3 integration (parent): wire `fluidKickBuffer()` into WaterField's `setExternalKicks`; point fluid.comp `surfaceZ()`
   at water_surface.glsl `ouSurfaceHeight` once it lands.
5. Full ctest not run.

## Verify
- Build: `systemd-run --user --scope -q -p MemoryMax=10G -- ninja -C build-WaterFluid -j8`
- Tests: `build-WaterFluid/ou_shader_tests --gtest_filter='SplashFluid.*'`,
  `build-WaterFluid/ou_game_tests --gtest_filter='Game.Water*:WaveField.*'`
- Captures: `CAP=1 systemd-run --user --scope -q --unit=ou-wfluid-N -p MemoryMax=10G -- \
  ../waterfluid-shots/run.sh NAME cfg-vk.json 110 --level 'pc-w\l4_pool.drm' --spawn -3000,0,3500,0 --render-hz 60
  [--camera ...]` then `systemctl --user stop ou-wfluid-N.scope`; entry is around frame 36.
