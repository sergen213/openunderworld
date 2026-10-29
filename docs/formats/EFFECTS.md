# TRU effects (GenericFXObject, particles)

Code: `src/fx/Fx.*` (parser, deterministic 60 Hz simulation, sprites), GL `drawFx` / Vulkan fx pipelines
(`FrameOptions::fxVertices/fxBatches`), `ou-game` `EffectsView`. Tests: `tests/test_fx.cpp` (`Fx.*`, `FxReal.*`).
Labels: **[O]** observed (data / tru.exe 1.1.0.0 decompile or disassembly), **[I]** inference, **[U]** unknown.
Evidence scripts (private work dir): `scratch/fxd.py`, `pdef.py`, `pdefsurvey.py`, `fxsurvey.py`, `fxintros.py`.

## Survey [O]

- 232 `fx_*` objects in the bigfile namelist (torches, waterfalls/splashes/spouts, steam, snow, surf, sun, sparks, …),
  plus `particle.drm` (always loaded, tru.exe 0x5D4090) and effect data on weapons, enemies, crates, Lara.
- Across all units: 424 placed objects carry particle definitions (3269 intros, 5434 definitions); heaviest units
  l6_ship_exterior 236, l5_valaskjalf 201, l2_junglehub 163 particle-object intros. `fx*` objects alone: 205 objects
  with effect data, 2011 definitions, 389 enabled birth records, 1908 particle textures (all present in their DRM).
- Other GenericFXObject classes (counts over placed objects): class 1 (0x2C) 37, class 2 (0x58) 232, class 3 (0xB8) 81,
  class 5 (0x28) 187, class 6 (0x24 lights) 37, class 7 (0x0C) 133, texture lists 1281.

## Object section [O]

Object section (type 11) +0 / +4 / +8 are DTP resource references: +0 ObjectBase (rtype 25), +4 event list (rtype 20),
+8 **GenericFXObject** (rtype 32). (`formats::objectLights` reads the light list from ObjectBase +0x3C; tru.exe
0x546710 reads it from GenericFXObject +0x3C/+0x40 — [U] whether that helper's input is right for every object.)

### GenericFXObject (tru.exe 0x547B40 command switch and the per-class creators)

| off | class | record | creator |
|-----|-------|--------|---------|
| +0x04 / +0x08 | effect map | u16 n, 4-byte `{id, class, index}` | case 0x2F |
| +0x0C / +0x10 | particle emitters | 0x17C | 0x543F70 |
| +0x14 / +0x18 | class 1 | 0x2C | 0x545810 [U: ribbon?] |
| +0x1C / +0x20 | class 2 | 0x58 | 0x536B70 [U: glow?] |
| +0x24 / +0x28 | class 3 | 0xB8 | 0x537230 [U] |
| +0x2C / +0x30 | class 4 | 0x64 | 0x53D100 [U] |
| +0x34 / +0x38 | class 5 | 0x28 | 0x535CC0 (timed colour/intensity modifier) [I] |
| +0x3C / +0x40 | lights | 0x24 | 0x546710 → 0x5C35D0 |
| +0x44 / +0x48 | class 7 | 0x0C | 0x52C920 |
| +0x54 / +0x58 | class 8 | 0x3C | 0x5310A0 |
| +0x5C / +0x60 | class 9 | 0x10 | 0x5471A0 |
| +0x64 / +0x68 | texture lists `{u32 n, ids[n]}` | | referenced by particle +0x114 |

### Event list (object +4) and commands [O]

Records `{u16 info, u16 frame, u8 event, …, command}`, size `8 + (info >> 12) * 4`, end `0xFFFF` (0x5BA000). `info & 3`:
1 effect callback; `info & 4` disabled; `info & 0x3F0` filter. Instance birth runs event 0 unless ObjectBase byte 3 bit 0
(oflags 0x01000000) is set (0x5BA480 / 0x5BA650). Animation event lists (DTP rtype 19, per clip) use the same records,
gated by key frame (0x5BA0D0) [not used yet]. Command: u16 type + 3 u16 args. Particles: 3 (idx, segment, segment 2),
0x1D, 0x20, 4 (on every segment). Other types: lights 0x11, sounds, camera shake, pad rumble, … [not implemented].

### Particle emitter definition (0x17C) — from 0x543F70 (emitter copy) and disassembly

| off | meaning |
|-----|---------|
| +0x00 | key 0 size (ftol → emitter key +4) [O]; sprite half-width [I] |
| +0x04 / +0x05 | flags; +0x04 & 0x80 model particle [O] |
| +0x06 | mode: 0 free, 1 attached, 2 one persistent particle [I from 0x540B80 `local_430`] |
| +0x0A / +0x0C | segment / second segment (shape 7 line), −127 none [O] |
| +0x10 / +0x6C | shell radius max / min (emitter +0x90 / +0x92, 0x5445B8) [O] |
| +0x14 / +0x20 | velocity / random range per axis; spawn velocity = (v + rand·range) × 0.5 [O 0x542112] |
| +0x2C | acceleration × 0.25 (emitter +0xA8) [O] → per 60 Hz tick: 30 Hz authoring [I] |
| +0x38 / +0x3C | first / last key colour, bytes R,G,B,A, 0x80 = 1.0 [I: plausible fire gradients] |
| +0x44 | emitter life (30 Hz frames [I]); −1 forever, 0 one burst |
| +0x46 / +0x7C | particle life (30 Hz frames; key times scale ×2 ticks, 0x54223F) / ± variance [O] |
| +0x48 | rate: per 60 Hz tick rate×0.5, < 0: 0.5/−rate; < 0.5 → random spawn (0x5446EB, 0x540C0B) [O] |
| +0x4C | position offset [O] |
| +0x5A / +0x5C | camera-distance fade band (particle +0xB8/+0xBA, 0x54B1A6) [O] |
| +0x60 / +0x64 | on-screen size fade ×255 (particle +0xA4/+0xA5) [O] |
| +0x68 | spawn shape: 0 sphere shell, 1 hemisphere, 2/3/4 circle XY/XZ/YZ, 7 line + sphere, 5/6 mesh, 8 camera circle, 9 ? [O switch; geometry partly I] |
| +0x74 / +0x78 | camera range for emitting (emitter +0x2C/+0x30 squared) [O] |
| +0x80 / +0x84 / +0x86 | last size (≥ 0) / its variance / key 0 variance [O] |
| +0x94 / +0x96 / +0xAA | radial-velocity / velocity-frame / offset-frame segments (−127 none) [O] |
| +0xB1 / +0x91 | texture frame rate (0 random) / start frame [O]; 255 = over life [I] |
| +0xB8 / +0xBC / +0xC0 / +0xCE | key 1 colour / size / variance / time % [O] |
| +0xC4 / +0xC8 / +0xCC / +0xD0 | key 2 likewise [O]; key present if colour ≠ 0 or size ≥ 0; colour 0 = interpolated |
| +0xE0..+0xEC | per-key height/width ×16 [O] |
| +0xF4 / +0xF6 / +0xF8 / +0xFC | rotation / range / spin / spin range, degrees (×4096/360) [O]; spin unit [I] |
| +0x110 / +0x114 | texture id / → texture list (list wins) [O] |
| +0x11C..+0x11F | per-key blend, low 3 bits: 1 alpha, 2 additive, 3 opaque, 4 rev-subtract, 5 DESTCOLOR/SRCALPHA (0x52AE60) [O] |
| +0x174..+0x177 | atlas cols, rows, frames, first frame (1-based) [O] |

Runtime: emitter 0x19x bytes, particle 0x130 bytes (`JobHarness_fxParticleUpdate` 0x54D810 → type switch on +0xB0 & 0x1F;
draw `JobHarness_fxParticleDraw` 0x54BAB0 → sprites 0x54AEC0). RNG: LCG ×0x41C64E6D + 0x3039, bits 16..30 (as tru.exe).

## Rendering

Instances of objects with oflags 0x40000000 (fx proxies, bullet hits, squibs, camera anchors, …) are not drawn:
`InstanceDrawable` 0x5B5CA0 returns no draw flags [O] (`formats::kObjectNoInstanceDraw`). Sprites are camera-facing,
back-to-front, depth-tested without writes, `tex × colour × 2`.

- Depth bias [O]: def `+0x15E` (s16) → emitter `+0x10C` → particle `+0x28`, flag 0x200; the sprite draw (0x54B0E8)
  uses `z + bias` (≥ 16) for fades and, with flag 0x400, as the projected depth. Torch flames use −158 (their origin
  sits inside the sconce). We scale the quad about the eye by `(z + bias) / z`: same screen footprint, nearer depth [I].
- Segment fields < 0 name HInfo markers (slot 2, 0x20 bytes: `+4` u16 id, `+8` float3 position, `+0x14` float3 Euler,
  segment frame) by `-id` (0x46DA20; ids ≥ 100 come from a runtime list and −125 from 0x538570 [U], both fall back to
  the root as the exe does when nothing matches). Splash edges emit along the line between markers −7 and −8 with
  velocity/offset in marker −5's frame (`physics::HMarker`, `FxReal.MarkerSegmentsResolve`).

## Status / missing

- Implemented: particle definitions, birth events, emission, shapes, velocity/accel, keys (colour/size/aspect/blend),
  texture lists/atlases, fades, depth bias, marker segments, GL + Vulkan drawing, effect proxies hidden.
- Viewed (2026-10-29, `ou-game --camera`): l1_baldrs_tomb torch 215 flame in GL + Vulkan
  (`--spawn 0,-17200,2600,-90 --camera 1500,-18200,5000,2292,-19285,5300`); the hall view hides torches 214/215
  behind the serpent heads, which was the earlier "invisible" report. l2_junglehub splash-edge foam faint but present.
  Flames look thinner than remembered from the original: no Wine reference capture yet [U].

## Animation events and combat [O unless marked]

- An object's animation list (ObjectBase `+0x54` count, `+0x58` 12-byte entries `{i16 anim, i16, u32 event-list DTP
  id, char* name}`) gives each clip an event list (DTP rtype 19): a u32 header, then records in the object-event
  layout (`8 + (info >> 12) * 4` bytes, end `0xFFFF`). `frame` = 30 Hz clip frame. `info & 0x3F0 >> 4` = surface
  material filter (0 any, else it must equal the instance material, instance `+0x186`, tru.exe 0x5BBFD0), `info & 4`
  disabled, `info & 0xC00` variant (we run variant 0 only [I]). Type-0 records with a pointer at `+0xC` are skipped.
  Parsed into `ObjectFx::clips`; `System::runEvents` (frame span) and `System::play` (one-shot instance, later frames
  fire as ticks pass).
- Material = collision face byte `+0x10` (tru.exe 0x6F06B0 copies the floor face's to Lara's `+0x186`). From
  bullet_hit's per-material lists and Lara's footstep lists: 1 stone (most faces), 2/3 water (Lara: 2 in water),
  4 dirt, 5 ice, 6 wood, 7 metal, 8 carpet, 9 snow, 11 plaster, 12 glass, 13 unbreakable glass, 14 sand, 15 mud,
  16 pipe, 17/18/19/20 stone/unpaved variants, 21 thin metal, 22 grass, 23 flesh (dark-red blood), 24 flesh?,
  25 electrical, 26 wood, 27 stone, 28 leaves, 29 goo [names I, from bullet_hit's state names + colours].
- Hitscan impacts (0x6C64D0 → 0x6B7030): the ammo object's data `+0x3C` (above water) / `+0xA8` (under water) is an
  object id (`pr_handgunbullet` 53 `bullet_hit` / 61 `bullet_hit_underwater`; shotgun 68); that object is born at
  the hit (Z on the normal) with the hit material and its `idle` clip events start the surface's particles (stone:
  87–90 + sparks 61/63; flesh: blood 20–24). Weapon data `+0xE6` names the ammo object. Bodies use material 23 [I].
- Muzzle: the weapon object's `fire` / `fire_submerged` clip events (`wp_handgun` defs 0–7, 9, 10, 22–26 at frame
  0), in the gun's frame: HInfo markers −10 muzzle (−Y = barrel), −13 ejection port, −20..−24 flash points. We place
  the gun so marker −10 sits on the shot origin facing the shot [I]. Enemies use their type's weapon object
  (`EnemyType::weapon`, e.g. 114 `wp_akrifle`).
- `ou-game` runs Lara's and every enemy's current clip events each tick on their posed bones (Lara: `lara` object,
  floor material / 2 swimming / 3 submerged [I]); `object:clip` clips (weapon, vehicle) are skipped.
- Other commands in these lists: 6 (class 2, `0x536B70`, e.g. muzzle / impact flash) [U], 9, 0xD (0x481B50), 0xE/0xF
  (0x481CC0/CF0), 0x11 light (0x546710, muzzle/metal-hit light), 0x15/0x1A — not implemented.
- Model particles (def `+4 & 0x80`, e.g. casings) are drawn as 1.5-unit sprites in their key colour [I]. Defs with no
  texture and no list name a resource at `+0x168` (wp_handgun def 25 → 2770; distortion / heat haze? [U]) and are
  not drawn — drawn white they covered the shot line.
- Viewed 2026-10-29 (GL + Vulkan, `tests/replays/poacher_firefight.txt`, l4_ballcourt `--spawn 17520,10383,2650,180`,
  frame 38 `--camera 17850,10550,3150,17620,10200,2980`): yellow muzzle flash + sparks at the pistol, blood mist and
  red streaks on the panther, smoke drifting. No Wine reference capture.

## Missing

Command classes 1/2/4/5/7–9 (ribbons, flashes/glows, lightning …), effect lights (0x11, flicker), particles that
follow their segment (mode 1), casing meshes and collision, the `+0x168` material particles, decals, lens flares,
skydomes (`*_skydome_*` dependencies not placed), UV scroll, screen effects, uber-object / door / breakable clip
events (script-driven objects' clips are not tracked), enemy textures for generator-spawned enemies before the next
scene rebuild, per-enemy hit materials (all bodies 23), Wine reference comparisons, frame-time measurements in the
heaviest units (not measured).
