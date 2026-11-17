# Original per-frame render pipeline (tru.exe) and its OpenUnderworld counterparts

This document reconstructs how GOG `tru.exe` 1.1.0.0 (SHA-256 1fdbd345…1011) draws one frame, from the main loop to
`Present`, and compares every pass with the Vulkan and OpenGL renderers in `src/render/`. It is an audit document:
no engine code was changed. It builds on [TIMING.md](TIMING.md), [STARTUP.md](STARTUP.md),
[ENGINE_ARCHITECTURE.md](ENGINE_ARCHITECTURE.md) §18, [../formats/SHADERS.md](../formats/SHADERS.md),
[../formats/MATERIALS.md](../formats/MATERIALS.md) and [../RENDERING.md](../RENDERING.md), and closes the open item
"trace PCScene_Draw 0x5150E0 pass order end to end" from ENGINE_ARCHITECTURE.md.

Snapshot: repo main a80bdb8 (2026-11-16).

## Evidence legend

| Tag | Meaning |
|---|---|
| **A** | Call order or operand checked in `objdump -d -M intel` of tru.exe during this audit. Disassembly: `objdump -d -M intel --no-show-raw-insn tru.exe` (scratch copy in `openunderworld-work/re-baseline/RenderOrder/tru.asm`, not committed) |
| **D** | Read from the Ghidra whole-program decompile (`openunderworld-work/ghidra/exports/decomp_all.c`) during this audit, not re-checked instruction by instruction |
| **C** | Taken from a cited repo document that marks it observed ([O]); not re-verified here |
| **[INFERENCE]** | Reasoned from structure; confidence given as high / medium / low |

Confidence (h/m/l) is given per row where it is not obvious from the tag. Function names that do not appear in
`tools/ghidra/names/tru_gog.csv` are descriptive labels of this document, not applied names.

Class names come from RTTI (`openunderworld-work/ghidra/exports/rtti.tsv`, `docs/re/db/rtti.tsv`): `cdc::PCRenderDevice`
(vftable 0x8492CC, 70 slots), `cdc::PCScene` (0x849A28: slot 1 = `PCScene_Draw` 0x5150E0), `cdc::PCModelDrawable`
(0x849754: Draw 0x50F4A0), `cdc::PCTerrainDrawable` (0x849944: Draw 0x510DD0), `cdc::PCPrimitive`, the four
`cdc::PC*Effect::EffectDrawable` screen filters, `cdc::PCShadowMap` (0x849AA8), `cdc::PCWaterSurface` / `cdc::IRippleMap`
(0x849BA4/0x849BAC/0x849BC4), `cdc::Scene` (0x84DECC, the portal scene graph), `UnderWaterRayDrawable` (0x85D5C4).

## 1. Architecture in one paragraph

tru.exe does not draw while it traverses. Each frame the game code opens one or more **scenes** on the render device
(`PCRenderDevice` vt 0x20 = BeginScene 0x5081A0, vt 0x34 = EndScene 0x507BF0, vt 0x38 = a scene that must draw *before*
the current one, 0x507060). While a scene is open, the portal scene graph (`cdc::Scene`) and other systems add
**drawables** to it. Each drawable is filed into up to six of the scene's eight **pass lists** by a pass bit mask.
Nothing reaches Direct3D until `PCRenderDevice` vt 0x10 = **EndFrame 0x509630**. EndFrame renders the shadow-map atlas,
then draws every finished scene in order (each through `PCScene_Draw`), copies the feedback frame and presents.

## 2. Frame driver: from the main loop to EndFrame

All rows are **A** (call order read from the disassembly of each function).

```
GAMELOOP_Frame 0x5DFFE0
  0x483810, 0x78BCC0, 0x5D4520             [not render; omitted from TIMING.md's listing]
  GAMELOOP_Process 0x5DFBE0                simulation
  STREAM_PollLoadQueue 0x5C1DA0
  GAMELOOP_DrawFrame 0x5DA1C0              ── the whole render frame, below
  0x5C1B50, 0x5B8FC0                       [not render]
  GAMELOOP_UpdateTime 0x5DA230

GAMELOOP_DrawFrame 0x5DA1C0
  0x69D740        reset per-frame water flags (0xED00C4 = 0: refraction not drawn yet; 0x8AFD79 = 1)
  0x5CF190        light-data flush, per-unit stream tracker render updates, Scene vt 0x6C(dt)
  0x5EA390        "camera under water" test (water look, also used below)
  0x5CEBC0(!underwater)                    ── frame setup + main scene + planar reflection
      0x5BF840    device vt 0x0C BeginFrame(dt = timeMult/30) 0x506F10: resets the scene stack; vt 0x88(0,0)
      0x5CAFF0    view parameters, fog / ambient / misc globals into the device (vt 0x7C, 0x64, 0x74)
      0x5CB1F0    device vt 0x20 BeginScene(main view 0xE496E0, render target or 0 = back buffer);
                  sets view byte +0xCA (0xE497AA) = 1 → the main scene clears alpha and runs the 0x800 pass (§4)
      0x5CE800    PLANAR REFLECTION (only if a plane was registered, see §7): BeginScene(mirror view, reflection
                  target), Scene::Render, EndScene, device vt 0x88(0, reflection texture)
      0x5C04C0    scene visibility masks (Scene vt 0x48..0x54)
  0x5D9F90                                 ── fill the main scene
      0x698880, 0x52E900, 0x52C470, 0x5C0380, 0x773BF0 (engine globals c140..: player/bone positions)
      0x5D4480 / 0x5C7CA0                  camera unit; neighbour-unit look blend (RENDERING.md "Neighbour units")
      0x5462D0                             effects begin (FX camera)
      instance loop (0x4D0D50 component lookups → 0x48FA40 / 0x69B700 / 0x69EA30)
      0x5C0120(underwater), 0x5CC6A0       Scene::Render of the main view (Scene vt 0x04 = 0x572320, §3)
      0x6D9DA0                             under-water full-screen pass + light rays (§7.4)
      0x6A2C70                             effect / filter accumulation (calls 0x516E50 ColorDOF)
      0x548CF0                             effect draw: FX primitives, then rain 0x5346A0 / snow 0x534DF0 if dry
      0x52C4C0                             queued screen filters → device vt 0x5C (0x5074E0) with weights
      0x696630, 0x454BC0, 0x489C60, 0x7212D0 (HUD), 0x5CDF30 (8 screen-overlay slots [INFERENCE m]),
      0x6E4DE0, 0x583E20 (menu / text), font state reset
  Font_Flush 0x5C5A30  (tail call; the name is a hook label, the function is the frame end)
      0x5BFD60    device vt 0x34 EndScene(main); vt 0x20 BeginScene(same view, no target, view +0xB0 clear = 0)
                  = the 2-D scene; 0x5BFA00 (if 0xDA61C0) and 0x476D80 (font flush) fill it; vt 0x34 EndScene
      0x5A4340    [U] (debug/profiler drawing gated by 0xCDC9A0)
      device vt 0x10 = EndFrame 0x509630    ── everything is drawn here (§5)
      0x5C1990    free unit resources scheduled for release
```

So the **submission order** of scenes in a normal frame is: planar reflection (if any) → (nested scenes such as the
water refraction, §7.3) → main 3-D scene → 2-D/text scene. EndScene appends a finished scene to the device's root list
(device +0xFAA8, **A** 0x507CF2) unless its depth target is shared with a scene still open, in which case it joins that
scene's sub-scene list (+0x1D4, **A** 0x507CB4); sub-scenes are drawn from inside the parent's depth pass (§4 step 3,
[INFERENCE m]). The root list is drawn in this order (§5 step 9).

## 3. Scene traversal, visibility and list building

`cdc::Scene::Render` = Scene vt 0x04 = 0x572320 (**D**; called from 0x5CC6A0 and 0x5CE800):

| Step | Address | What | Tag |
|---|---|---|---|
| 1 | 0x559B20 | set the view (matrices, frustum) | D |
| 2 | 0x566170 | view setup: far × 1.05, camera cell lookup (0x55F6B0), user clip plane (+0x169: reflection / refraction views) | D |
| 3 | 0x570F70 → 0x570960 | portal traversal from the camera cell: visible cells and their portal-clipped frusta | D, [INFERENCE h] |
| 4 | 0x56D5C0 | visible lights: type-10 entries, the sun (`+0x17C == 2`) picks its PSSM camera (vfunc 0x60 → 0x40) | D |
| 5 | 0x5663A0 → 0x5640C0 | per visible cell: cell drawable callbacks (frustum test, clip boxes) | D |
| 6 | 0x56CBA0 → 0x56BBC0 / 0x56BAA0 | per visible cell: terrain (0x56ABF0) and per-leaf light masks (SHADERS.md) | D, C |
| 7 | 0x55FB60 | per visible entity: bounding-volume test, light set (≤ 6 slots, device vt 0x70 = 0x5076A0), irradiance (vt 0x68 = 0x508570), then the entity's drawable (instance +0x6C) | D, C |

Drawables are filed into the **current** scene (device +0xFA50) by pass bits. The model list builder is 0x50F7C0 (**D**)
and the filing function is 0x50F720 (**A**, bit → list):

| Pass bit | List (scene offset) | Who sets it | Meaning |
|---|---|---|---|
| 0x0001 | list 0, +0x5F8 | opaque models/terrain (with 0x200), opaque primitives | depth pre-pass |
| 0x0002 | per-light caster lists (0x504EE0) | material +0x44 & 0x40000000 and blend < 4, shadow scenes only | shadow caster |
| 0x0200 | list 1, +0x608 | blend mode 0–3 and opacity 1 (models: 0x201) | opaque colour pass after the depth pre-pass |
| 0x0100 | list 2, +0x618 | models whose drawable +0x38 is 0 (no pre-pass), opaque primitives (0x502820) | opaque without pre-pass [INFERENCE m: +0x38 meaning] |
| 0x0080 | list 3, +0x628 | opacity < 1 or blend mode ≥ 4 (models: 0x80, 0x81 with material flag 0x400) | translucent, sorted |
| 0x0800 | list 4, +0x638 | material flags (+0x40) bit 1 (0x2), added to the colour bit | alpha-only mask pass |
| 0x0400 | list 5, +0x648 | material +0x44 bit 1 (0x2): the sea / refracting materials | screen readers (back-buffer copy) |
| 0x1000 | list 6, +0x658 | primitive flag 0x01000000 (0x502FF0, **A** 0x5032B0); the screen-filter drawables (0x512FA0 → 0x517DE0) | full-screen filters |
| 0x2000 | list 7, +0x668 | primitive flag 0x40000 (0x502820 / 0x502FF0); sets scene +0x678 when the primitive reads the screen | after the filters |

Sort keys (**A**, comparators read in the disassembly):

- Lists 0, 1, 2 (`0x4F63E0` → merge sort 0x4F60C0): by drawable class (vtable pointer), then the class's own comparator
  (drawable vt +0x08: model 0x50E800, terrain 0x5106E0) with the pass bit in 0xA14100. State/material order, not
  depth order.
- Lists 3 to 7 (`0x4F6410` → 0x4F6240): **descending** float key at drawable +0x08, ties by ascending int +0x0C (**A**
  0x4F6289–0x4F62AF). For primitives +0x08 = view-space z of the centre + a bias (0x502820, **D**); models fill it the
  same way in 0x50F7C0 (**D**). [INFERENCE m] with z growing away from the eye this is back to front.
- Screen-filter drawables use fixed keys (**D**, constructors 0x51B5B0, 0x51AD20, 0x51A390, 0x5172B0): Noise
  +1e7, Finish +5e6, ColorDOF = effect +0x153C, Feedback −1e7. With the descending sort, Noise draws first and Feedback
  last.

`cdc::PCModelDrawable::Draw` 0x50F4A0 (**D**) dispatches by pass: 1 → 0x5064F0 (depth program, material +0x28),
2 → 0x505D80 (shadow-caster program, +0x2C, per light type), 0x200 → 0x506880 opaque (0x506040: Z EQUAL, no writes),
0x80 / 0x400 → 0x506880 translucent (0x5066A0: material blend, Z LEQUAL, no writes), 0x800 → 0x505F40. Before the
colour passes it binds the light set (0x5042B0) and irradiance (0x5043D0). Composite program selection is in
SHADERS.md (PS slot from opacity and PSSM variant, VS slot from geometry flags). 0x100 has no material call in
0x50F4A0, only the geometry call 0x50EE60 [U: which state it inherits].

## 4. `PCScene_Draw` 0x5150E0 (cdc::PCScene vt slot 1) — **A**

Order verified instruction by instruction (0x515105–0x515415):

| # | Address | Action | D3D9 state / targets |
|---|---|---|---|
| 1 | 0x515115–0x51513B | colour target: scene +0x1C0 (own target, 0x5104C0) or the device default (vt 0xEC → 0x521D30) | SetRenderTarget |
| 2 | 0x51514D–0x51518A | clear flags from view +0xB0 (scene +0xD0): 1 → TARGET, 2 → ZBUFFER\|STENCIL, 3 → all; colour +0x220, alpha forced to 0 when scene +0xEA (main view) | |
| 3 | 0x51518C | 0x514680: fog registers (VS c22, PS c0, attenuation LUT fog row) | RENDERING.md "After geometry" |
| 4 | 0x5151F1 | cull for this scene: if the view matrix determinant is negative (mirror), swap CW/CCW (0x8A34FC / 0x8A3500 = 3/2 or 2/3) | D3DRS_CULLMODE values |
| 5 | 0x51522F / 0x51523D | PS c140 / VS c65 ← 16 registers at scene +0x49C (engine globals) | SetPixel/VertexShaderConstantF |
| 6 | 0x51526B / 0x51528A | when the scene differs from the last drawn one (+0xFA54): colour / depth surfaces from +0x1CC / +0x1D0 (0x519A50 / 0x519050), dropping the matching clear bits | |
| 7 | 0x5152AB | 0x521E80 Clear(flags, colour, z = 1.0, stencil +0xE4) | IDirect3DDevice9::Clear (vt 0xAC) |
| 8 | 0x5152B2 | **0x514F50 depth pre-pass**: list 0, pass 1, Z LEQUAL + write (0x526BB0(4,1)). When the target needs a depth texture (own target +0x54, or the default target's +0x1D == 0) the pass renders into a colour target (colour writes 0xF, cleared to 0xFFFFFFFF / 1.0 for clear modes 2 and 3) and 0x525000 StretchRects it into the DepthBufferTexture (global class 10); otherwise colour writes 0. Then the sub-scene list (+0x1D4, draw call with pass 0) [INFERENCE m] | ZFUNC 4, ZWRITE 1, COLORWRITEENABLE 0 or 0xF |
| 9 | 0x5152C4–0x5152D0 | **list 1, pass 0x200: opaque colour**, state-sorted, Z EQUAL no writes (0x506040) | per material blend (0–3), alpha test |
| 10 | 0x5152E4–0x5152F7 | **list 2, pass 0x100** (if any): opaque without pre-pass | |
| 11 | 0x51530D–0x515319 | **list 3, pass 0x80: translucent**, back to front | Z LEQUAL, no writes, material blend |
| 12 | 0x515338–0x515381 | **list 4, pass 0x800** (only with scene +0xEA, i.e. the main view): stencil off (D3DRS 0x34 = 0), colour writes = ALPHA only (0x526C40(8)), Z LEQUAL no writes | writes the alpha channel the clear zeroed [U: consumer] |
| 13 | 0x51538E | **0x512F30: list 5, pass 0x400 screen readers**, back to front, only when the scene draws to the device back buffer: 0x524F90 StretchRects the back buffer into an A8R8G8B8 texture (BackBufferTexture, global class 9) first, 0x525190 releases it after | |
| 14 | 0x5153A4–0x5153B0 | **list 6, pass 0x80: full-screen filters** (and 0x1000 primitives), only for scenes without their own target | |
| 15 | 0x5153D6–0x515401 | **list 7, pass 0x80** (0x2000 primitives); with scene +0x678 bracketed by the back-buffer copy 0x524F90 / 0x525190 | |
| 16 | 0x515410 | 0x510050: pop render-target / depth stacks (own target) | |
| 17 | 0x51541F, 0x51544F.. | reset D3DRS 0xC3 (depth bias); free the eight lists (0x512FE0) | |

## 5. EndFrame 0x509630 (PCRenderDevice vt 0x10) — **A**

| # | Address | Action |
|---|---|---|
| 1 | 0x509646 | vt 0x88(0,0) |
| 2 | 0x509672 / 0x50967A | resource manager tick (vt 0x18 of +0xFC94), 0x5249B0 (dynamic resource flush) |
| 3 | 0x509699 / 0x5096A2 | SetRenderTarget(default), 0x5228A0(1) |
| 4 | 0x5096B9 | IDirect3DDevice9::BeginScene (vt 0xA4) |
| 5 | 0x509711 / 0x509721 | `TimeVector` PS c53 / VS c23 = (ms mod 10⁶ × 0.001, dt) (SHADERS.md) |
| 6 | 0x50972E | 0x504A60: attenuation LUT (light falloff rows, fog row) |
| 7 | 0x509756 | **0x5050C0: shadow maps** (§6) |
| 8 | 0x509766–0x5097E3 | only while a queue at +0x11600 (count) is non-empty: Z EQUAL, blend 0, full-screen textured quad 0x524DF0 [U: loading / transition images] |
| 9 | 0x509816–0x50982E | SetRenderTarget(default); **root scene list (+0xFAA8)** sorted (pass 1) and drawn: every scene's `PCScene_Draw` in submission order |
| 10 | 0x509847 | 0x519360 (screen effects object +0xFDD4): 0x51A2E0 copies the back buffer, half size, into the Feedback texture (A8R8G8B8, +0x38) when a feedback effect ran this frame; with +0x64 / +0x6C a second full-size copy after drawing it back with alpha (1 − +0x6C) [U: which effect] |
| 11 | 0x509860 loop | reset sampler state on all 16 stages |
| 12 | 0x509879 | IDirect3DDevice9::EndScene (vt 0xA8) |
| 13 | 0x509888 | 0x5228A0(0) |
| 14 | 0x5098B5 | Present through +0xFC90 vt 0x18(0,0,0); or, with a capture object at +0xFC8C, set its "frame ready" byte instead |
| 15 | 0x5098CA.. | light manager end-frame (0x504C50), transient render targets released (0x507F60), deferred deletes (0x4F3900), heap reset (0x47AB90) |

Because step 9 draws the whole root list before step 10, the feedback copy contains the 2-D/text scene too
([INFERENCE m]: the HUD text is drawn in that scene).

## 6. Shadows

| Item | Evidence |
|---|---|
| 0x5050C0: sampler 14 unbound; every light whose PCRenderLight is shadow-capable is given atlas tiles by 0x504F30: tile size ≤ 2048, tiles per light = light +0xF4 (4 for the sun's PSSM splits), ≤ 16 tiles in total, at most two 2048² tiles, within a pixel budget (+0x144). The sun (type 2) is placed first. With `EnableShadows` off (device manager +0x184 = 0) only the first light whose DTP byte +0x144D is set still gets a map | D |
| Render: 0x515980 (atlas target; colour writes 0 when hardware shadow maps, +0x167C) → 0x504960 → per light 0x50DB20: type 1 (spot) 0x50C420 (perspective map), type 2 (sun) 0x50CCB0 (PSSM, 4 splits). Each clears (0x521E80 flags 3, z 1.0) and draws pass 2 (0x4F63E0/0x4F6080 with 2) with depth bias / slope bias D3DRS 0xC3 / 0xAF from the light | A (call sequence of 0x50CCB0 / 0x50C420) |
| After: Z LEQUAL write, colour 0xF; sampler 14 = the atlas, with `GET4` (0x34544547) in D3DSAMP_MIPMAPLODBIAS when Fetch4 is used (+0x167D) | D |
| Atlas layout, split distances, matrices, PS variants (Fetch4 / 4-tap / hardware PCF): SHADERS.md "PSSM" (Wine-read) | C |

Settings bytes of `g_pcDeviceManager` used by the render path, matched against `PCDisplaySettings_LoadFromRegistry`
0x51E120 (settings block copied at device manager +0x15C; **D**, consistent for every byte below, [INFERENCE h]):

| Device manager byte | Registry value | Used by |
|---|---|---|
| +0x181 | EnableFullscreenEffects | filters (0x51CD90, 0x516E70) |
| +0x182 | EnableDepthOfField | ColorDOF (0x516E70, RENDERING.md) |
| +0x184 | EnableShadows | 0x5050C0, 0x504CC0, 0x506880 |
| +0x192 | AlwaysRenderZPassFirst | [U] no reader found by text search |
| +0x198 | EnableReflection | planar reflection 0x5CE800 |
| +0x199 | EnableWaterFX | ripple map 0x515FD0 (§7.2) |
| +0x19A | EnableVolumetricFX | under-water light rays 0x6D8C20 / 0x6D9660 |

## 7. Water

### 7.1 Surface shading

The sea / pool materials are translated D3D9 programs (SHADERS.md). A material with +0x44 bit 1 is a **screen reader**:
it skips the depth pre-pass and the opaque list and draws in list 5 (pass 0x400) after the translucent list, with the
back buffer copied into BackBufferTexture (class 9) and the pre-pass depth in DepthBufferTexture (class 10) (§4 steps
8 and 13, **A**/**D**). It uses the translucent material state (0x5066A0: blend from the material, Z LEQUAL, no writes).

Per-drawable water parameters come from the water volume's draw hook **0x69DFB0** (registered by the volume type
hook 0x6AC720 through 0x774230, **D**):

- VS InstanceParams 3–6 (c60–c63): wave numbers, phases, heights of the four sea waves (RENDERING.md "Planar
  reflection", Wine-read values). The program lowers each vertex by Σ h (1 − sin(k·x + phase)) (C; verified in the l2
  sea program, RENDERING.md).
- Gameplay surface: **0x69DA80** returns Σ_{i=0..3} h_i (sin(a_i) − 1) with h at volume +0x110..+0x11C (**D**; four calls
  of 0x813680, sin per GAMEPLAY.md / memory note "SIN not cos", **V** in ENGINE_ARCHITECTURE.md). Callers 0x6ACFD0,
  0x6AD010, 0x6ADA00 (volume queries). Render and gameplay use the same four waves.
- Ripple map (§7.2) as instance texture 0, and, with component flag 8, the refraction target's two textures as
  instance textures 1 and 2 (§7.3).

### 7.2 Ripple map (`cdc::PCWaterSurface`, interface `cdc::IRippleMap`) — new finding

- Created by the volume type hook 0x6AC720 when the water component flags (+0x7C) have bit 2 (0x4) set, sized to the
  volume box, through device vt 0xBC = 0x507370 → ctor 0x515ED0 (**D**). The l2_cliffclimb sea's +0x7C = 0xF
  (RENDERING.md, Wine-read), so it has one [INFERENCE h].
- Resource create 0x515FD0 (only with EnableWaterFX): a G16R16 texture (D3DFMT 0x22 checked by 0x5215B0), one texel
  per 50 world units (extent × 0.02), at most 384 × 384, and three CPU float buffers (**D**).
- Update 0x516490 (IRippleMap vt +0x0C), called from 0x69DFB0 every time the water drawable is drawn with the list of
  disturbances from 0x5C0480; it returns early unless more than 1/30 s passed since the last step (**D**: compare with
  0.033333335). So the ripple simulation runs at most at 30 Hz on the CPU and is uploaded to the texture the water
  program samples.
- This contradicts GAMEPLAY.md "Interactive surface … TRU does not simulate ripples": tru.exe has a render-side ripple
  height map (it does not feed gameplay). Its disturbance sources (0x5C0480) and solver are not decoded yet.

### 7.3 Refraction view (water component flag 8)

0x69DBC0 (**D**), at most once per frame (flag 0xED00C4, reset by 0x69D740 at the start of GAMELOOP_DrawFrame), when
at least one such volume is visible (0xED00C0 counts them, 0x6AC720):

- creates a transient target with device vt 0xCC(50, 50, 0x5B). Target flags (0x5103D0 / 0x5101A0 / 0x50FEC0, **D**):
  0x10 = sizes in percent of the back buffer, 0x01 colour texture (A8R8G8B8), 0x02 depth as a texture, 0x08 transient.
  So a half-resolution colour + depth target.
- renders the scene into it through Scene vt 0x7C with a horizontal clip plane at water height + 30 (camera above
  water, facing down, keeps what is under the surface) or −(height − 30) facing up (camera under water).
- 0x69DFB0 then binds its textures (vt +0x24 and +0x2C [INFERENCE m: colour and depth]) as instance textures 1 and 2,
  i.e. the material texture classes 2 and 3.

SHADERS.md treats classes 2 / 3 of the l1_seatop sea (5990) as "unsupplied … [I]" and feeds full-resolution copies of
the opaque scene. tru.exe feeds a half-resolution render of only the part of the scene below (or above) the water
plane. Visible consequence in OpenUnderworld Original: objects above the surface (yacht hull, Lara's upper body,
cliffs) can show up in the refraction where tru.exe clipped them away, and the refraction is sharper than tru.exe's
half-resolution target.

### 7.4 Planar reflection

- Plane registration 0x5C5B80 / 0x5C5890 ("Multiple reflection planes visible at once!") happens while the main scene
  is filled; 0x5CE800 consumes the plane at the start of the **next** frame (it runs in 0x5CEBC0 before 0x5D9F90) and
  clears it (0xDA61C4 = 0 at exit) (**A** for the order in 0x5CEBC0, **D** for the body). So the reflection lags the
  plane by one frame. Gated by EnableReflection (+0x198).
- Target: 0x5CAE70 vt 0xCC(50, 50, 25 = 0x19): 50 % × 50 % colour target (C for the call, flags decoded above).
- The mirror view's negative determinant flips the cull mode in PCScene_Draw step 4 (**A**).

### 7.5 Under-water pass and light rays

0x6D9DA0 → 0x6D8C20 / 0x6D9660 (RENDERING.md "Under water", Wine-confirmed). New detail from this pass: the ray target
is created with vt 0xCC(0x32, 0x32, 0x19) and a second one with vt 0xCC(0x19, 0x19, 0x19) (**D**). With flag 0x10 the
sizes are **percent of the back buffer** (50 % and 25 %), not 50 × 50 pixels as RENDERING.md says. The rays need
EnableVolumetricFX (+0x19A). Which pass list the full-screen draw lands in is not decoded [U]; the cdc design (list 7 =
after the filters) fits RENDERING.md's placement after the screen filters [INFERENCE l].

## 8. Lighting limits (summary, details in SHADERS.md)

| Item | Original | Evidence |
|---|---|---|
| Lights per drawable | 6 slots: ≤ 3 special (cookie spots ≤ 2 in slots 0–1, shadow casters) + ≤ 3 plain (slots 3–5), overflow into free special slots; chosen per entity from the lights touching it (entity +0x10C, +0x12C minus the sun, receiver masks) | C (0x55FB60, 0x504CC0, 0x5042B0) |
| Terrain | two 64-bit masks per node-tree leaf over the cell's light table | C (0x5128B0 → 0x511810, 0x56ABF0) |
| Sun | separate registers c208–c212 + b6, cookie c176/c177 | C |
| Ambient / irradiance | dynamic objects: unit light-probe SH sampled at the entity centre per frame (0x55F530 → 0x52A450), PS c70–c76; static geometry: light maps (class-1 instance textures, texcoord1) and the unit ambient `g_Ambient` c15 | C |
| Shadow maps | sun PSSM 4 splits; spot lights with DTP +0x144C get atlas tiles (≤ 16 tiles total, §6) | C, D |
| Light-map scale | PS c148/c149 GlobalParams | C |

## 9. Comparison with OpenUnderworld

OU frame order (both renderers; GL `GlRenderer::drawFrame` gl/GlRenderer.cpp:1050–1247, Vulkan
`VkRenderer::drawFrame` vk/VkRenderer.cpp:855–1766):

1. unit look / dynamic lights / exterior follow, water field and ocean compute;
2. sun shadow map (`drawShadowMap` / `recordShadowPass`), Original PSSM atlas (`drawPssm` / `recordPssmPass`);
3. planar reflection (`drawReflection` / VkRenderer.cpp:1178–1267, half size);
4. AO / RT pre-pass and CORTAO / RT indirect (when enabled);
5. main pass: exterior then depth clear, opaque in mesh order, blended back to front by mesh centre;
6. screen readers after the main loop from scene copies (`drawScreenReaders` / `screenDraws_` after `recordWaterCopies`);
7. Enhanced water surfaces, splash fluid; FX batches; debug lines; under-water particles;
8. resolve, filter blur chain, shafts; post pass (tone map, FXAA, screen filters, brightness, atmosphere, under-water
   pass and rays, fade); feedback copy; overlay (HUD, menus); present.

Status: **identical** = same algorithm and data; **translated** = the original shader/data path reproduced on modern
APIs; **enhanced replacement** = an OU technique standing in for or added to the original (kept, not proposed for
removal); **reordered** = same pass, different position; **missing** = no equivalent in the Original preset.

| Original pass (§) | OU Vulkan | OU OpenGL | Status | Original preset: visible consequence |
|---|---|---|---|---|
| BeginFrame / scene stack (§2) | `drawFrame` | `drawFrame` | different design (immediate) | none |
| Scene traversal, portal culling (§3) | game-side `unitClips` → `clipTo` scissors, `drawOrder`, LOD (`lodShown`) | `scissorFor`, same helpers | reconstructed | portal clipping is a screen scissor, not a per-cell frustum: geometry can pass where tru.exe culled it (RENDERING.md "Portal clipping") |
| Light set ≤ 6 per drawable, terrain masks, irradiance (§8) | `original::lightSet`, `IrradianceSlots` (OriginalShading.hpp:67, :93) | same | translated | per mesh, not per drawable piece |
| Time / fog / attenuation LUT (§5 steps 5–6, §4 step 3) | `original::fillFrame`, `attenuationLut` | same | translated | none known |
| Shadow atlas: sun PSSM (§6) | `recordPssmPass` (FrameOptions::pssm) | `drawPssm` | translated | matches Wine-read splits (SHADERS.md) |
| Shadow atlas: spot / shadow-casting lights (§6) | RT local-light shadows (`rtLights`) when RT is on; otherwise none | none | enhanced replacement (RT) / **missing** without RT | in Original without RT, torches and the flashlight light through walls and objects |
| Sun shadow map for generic shading | `recordShadowPass` | `drawShadowMap` | enhanced (OU's own) | n/a |
| Planar reflection (§7.4) | VkRenderer.cpp:1178–1267, same-frame plane, oblique near plane | `drawReflection` | translated, **reordered in time** | OU uses this frame's plane (tru.exe: last frame's); OU picks the nearest reflecting mesh's mean z, tru.exe the drawable's registered plane. No visible lag in OU |
| Refraction view (§7.3) | scene copies (`recordWaterCopies`) bound as classes 2/3 | screen copies in `drawScreenReaders` | **missing** (approximated) | above-water objects can leak into the sea's refraction; full resolution instead of half |
| Ripple map (§7.2) | none in Original; Enhanced: GPU eWave field (`WaterField`) | same | **missing** in Original / enhanced replacement | Original water shows no ripples around Lara where tru.exe (EnableWaterFX) had a 30 Hz ripple map |
| Clear (§4 step 7) | colour (0.08, 0.09, 0.11), depth 1 | same | different value | only where nothing is drawn; tru.exe's clear colour (+0x220) not decoded |
| Depth pre-pass (§4 step 8) | only with AO / RT (`prepassPipelines_`) | AO pre-pass only | reordered (opaque draws LESS + write instead of pre-pass + EQUAL) | none for opaque; see blend 2–3 below |
| DepthBufferTexture (class 10) | depth copy (`recordWaterCopies`) | `opaqueDepthFbo` blit | translated | none known |
| Opaque list, state-sorted, Z EQUAL (§4 step 9) | opaque in mesh order | same | reordered (order irrelevant with depth test) | none |
| Blend modes 2–3 in the opaque list (§3) | drawn in the back-to-front blended part with depth writes (`SubMesh::depthWrite`) | same | **reordered** | tru.exe blends foliage edges over whatever opaque was drawn earlier in state order and lets later geometry behind fail EQUAL; OU blends over the finished opaque scene, so OU edges are cleaner than the original [INFERENCE m] |
| Opaque without pre-pass, list 2 (§4 step 10) | part of opaque | same | equivalent | none |
| Translucent list, back to front by view depth (§4 step 11) | back to front by mesh-centre distance, per mesh | same | translated (coarser key) | sorting differs inside one mesh and when centres cross; FX are not interleaved (below) |
| FX / particles / weather drops (PCPrimitives in list 3, §2) | FX batches after the water (`fxBatches`; the original rain drops are appended as additive quads, `fx/Weather.hpp`) | `drawFx` after the water | **reordered** | particles behind a translucent mesh draw over it in OU; in tru.exe they sort with translucent meshes by view depth [INFERENCE m] |
| Alpha-only mask pass 0x800 (§4 step 12) | none | none | **missing** | none until its consumer is known [U] |
| Screen readers with back-buffer copy, list 5 (§4 step 13) | `screenDraws_` after `recordWaterCopies` (second pass) | `drawScreenReaders` | translated | copy taken after all blended draws in both (same as tru.exe) |
| Enhanced water, splash fluid, FFT ocean, caustics | `water*` pipelines, `recordFluid`, `recordOcean`, `recordAgWater` | `drawWater`, `drawAgWater`, `runOcean` | enhanced replacement | Enhanced only |
| Screen filters, list 6 (§4 step 14) | post pass `filter.glsl` | same | translated, **reordered** | tru.exe order (descending keys): Noise → Finish → ColorDOF → Feedback. OU `ouScreenFilter`: ColorDOF → Finish → Feedback → tints → Noise. Noise is drawn under the bloom / DOF in tru.exe and on top in OU; whether bloom is drawn before or after DOF also differs [INFERENCE m: key semantics read from the constructors] |
| List 7 (0x2000 primitives, §4 step 15) | under-water pass / rays in post after the filters | same | translated [INFERENCE l] | none known |
| Under-water full-screen pass + rays (§7.5) | `ouWaterPass` / `ouRays` in post | same | translated | the ray target is 50 % / 25 % of the screen in tru.exe, OU rasterises 50 × 50 texels (RENDERING.md) → OU's rays are much blurrier at high resolutions [INFERENCE m] |
| 2-D/text scene (§2) | overlay after post (`drawOverlay`) | `drawOverlay` | translated | none |
| Brightness: D3D gamma ramp (0x5222D0, whole display) | post-pass gamma before the overlay | same | translated | in tru.exe the HUD is brightened too; in OU it is not |
| Feedback copy after all scenes (§5 step 10) | copy of the post output before the overlay (VkRenderer.cpp:1610) | before the overlay (GlRenderer.cpp:1240) | translated, **reordered** | tru.exe's copy includes the text/HUD scene [INFERENCE m]; in feedback effects tru.exe shows ghosted HUD trails, OU does not |
| AO (none in tru.exe) | CORTAO / RT AO | CORTAO | enhanced (added) | n/a |
| RT sun shadows, RT reflections, FSR, frame generation, Anti-Lag | VkRenderer | — | enhanced (added) | n/a |
| Present: interval ONE/TWO/IMMEDIATE (TIMING.md) | swapchain present mode | `SDL_GL_SetSwapInterval` | translated | none |

## 10. Corrections to existing documents (found by this trace)

| Document | Statement | Correction | Confidence |
|---|---|---|---|
| TIMING.md "Per-frame order" | GAMELOOP_Frame = Process, poll, DrawFrame, UpdateTime | there are also 0x483810, 0x78BCC0, 0x5D4520 before and 0x5C1B50, 0x5B8FC0 after DrawFrame (**A** 0x5DFFE1–0x5E003A); none is render code [INFERENCE m] | high (calls) |
| `Font_Flush` 0x5C5A30 (tru_gog.csv, medium) | font flush | it ends the main scene, draws the 2-D scene and calls EndFrame (vt 0x10): the frame end | high |
| GAMEPLAY.md "Interactive surface" | "TRU does not simulate ripples" | tru.exe has a render-side ripple map at ≤ 30 Hz (§7.2) | medium-high |
| RENDERING.md "Under water" light rays | "50 × 50 A8R8G8B8 target" | 50 % × 50 % of the back buffer (target flag 0x10), plus a 25 % target | medium-high |
| SHADERS.md "Screen targets" | sea instance classes 2/3 unsupplied, fed with scene copies | they are the refraction view's colour / depth (§7.3) | medium |

## 11. Open questions

1. Consumer of the alpha-only pass 0x800 (materials with flags +0x40 bit 1) and of the alpha the main scene clears to
   0: Finish (bloom) mask, DOF or something else? Needs the filter shaders' CTAB or a Wine capture of the back-buffer
   alpha.
2. The ripple map: disturbance list 0x5C0480, solver in 0x516490, how the water programs sample the G16R16 texture;
   which shipped volumes set component bit 0x4 and bit 0x8.
3. Refraction view: confirm Scene vt 0x7C's clip-plane use and that vt +0x24 / +0x2C of the target are colour and depth
   (Wine: dump instance textures 1/2 of the l1_seatop sea).
4. Which list the under-water full-screen draw and the rays use (0x1000 or 0x2000), i.e. whether they are filtered.
5. ColorDOF keys (+0x153C) for the two ColorDOF effects (+0x0C and +0x10 of the effects object), to fix the filter
   order relative to Finish.
6. Clear colour of the main view (+0x220) and the clear mode (+0xB0) for the main scene.
7. List 2 (pass 0x100) for models: which drawables have +0x38 = 0, and what state 0x50EE60 uses without a material call.
8. The queued full-screen image draw in EndFrame (§5 step 8, +0x11600).
9. Sub-scene lists (+0x1D4): which scenes share a depth target with an open scene and where exactly they draw.
10. `AlwaysRenderZPassFirst` (+0x192): no reader found by text search of the decompile; check indirect reads.
11. Does OU's mirrored reflection camera reproduce the cull flip of PCScene_Draw step 4 (both renderers use a fixed
    CCW front face)? Check by rendering a single-sided mesh in the reflection.
12. Second copy in 0x519360 (+0x64 / +0x6C): which effect uses a full-size frame copy faded by (1 − +0x6C).

## 12. How to reproduce the key reads

- Call order: `objdump -d -M intel --start-address=0x5150e0 --stop-address=0x515470 tru.exe` (PCScene_Draw), the same
  for 0x509630 (EndFrame), 0x50F720 (pass → list), 0x507BF0 (EndScene), 0x517DE0 (filter insertion), 0x4F6240 (sort
  direction), 0x5DFFE0 / 0x5DA1C0 / 0x5CEBC0 / 0x5C5A30 / 0x5BFD60 / 0x5D9F90 (frame driver).
- Bodies: `decomp_all.c` sections `/* ==== 005150e0 ... */` etc.; vtables from `rtti.tsv` (slot lists).
