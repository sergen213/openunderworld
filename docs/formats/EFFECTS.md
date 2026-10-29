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
| +0x1C / +0x20 | class 2 glows (prim 0x82) | 0x58 | 0x536B70, update 0x535FA0, draw 0x536280 — implemented, "Glows" |
| +0x24 / +0x28 | class 3 | 0xB8 | 0x537230 [U] |
| +0x2C / +0x30 | class 4 | 0x64 | 0x53D100 [U] |
| +0x34 / +0x38 | class 5 screen filters (prim 0x85) | 0x28 | 0x535CC0, draw 0x5469A0 ("Screen filters" below) |
| +0x3C / +0x40 | effect lights (prim 6) | 0x24 | 0x546710 → 0x5C35D0 — implemented, "Lights" |
| +0x44 / +0x48 | class 7 instance fades | 0x0C | 0x52C920: instance fade (+0x1B4/+0x1B8, 4096 = 1) [O] — implemented, "Instance fades" |
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
  disabled, `info & 0xC00` variant: frame-windowed runs take variant 0 only; a run with a variant (tru.exe 0x5B26C0,
  `param >> 10 & 3`) takes that variant's records whatever their frame (`System::runVariant`; rewards' `pickup` lists
  keep their fade there, info 0x2401). Type-0 records with a pointer at `+0xC` are skipped.
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
- Other commands in these lists: 6 glows and 0x11 lights (muzzle / metal-hit light), 0x15/0x1A/0x18/0x1B stops — see
  "Glows" / "Lights"; 9 (class 3), 0xD (0x481B50), 0xE/0xF (0x481CC0/CF0) — not implemented.
- Model particles (def `+4 & 0x80`, e.g. casings) are drawn as 1.5-unit sprites in their key colour [I]. Defs with a
  material (`+0x168`, or `+0x16C` list) are material particles — see "Material particles".
- Viewed 2026-10-29 (GL + Vulkan, `tests/replays/poacher_firefight.txt`, l4_ballcourt `--spawn 17520,10383,2650,180`,
  frame 38 `--camera 17850,10550,3150,17620,10200,2980`): yellow muzzle flash + sparks at the pistol, blood mist and
  red streaks on the panther, smoke drifting. No Wine reference capture.

## Material particles [O data, I reduction]

`0x543F70` takes the material path when def `+0x168` (one material, resource relocation to a type-10 section) or
`+0x16C` (pointer to `{u32 n, material ids[n]}`, picked per particle like a texture list; 993 defs) is set; `+0x110` /
`+0x114` are then unused. Survey of every object: 1077 `+0x168` defs over ~25 material families. The material's own
shaders draw the sprite; we reduce the composite pixel shaders (disassembled) to three cases:

- **Refraction** (a material texture entry from the engine-global table class 9 `BackBufferTexture` plus a category-3
  normal map; families 19–22 Lara / weapons ×431, 173–175 Thor's hammer ×81, 161–164 / 157–160 fireballs, 485–488,
  1622–1625, 227–229 rain sheets, 165–168, 482–484): the scene copy sampled at `vPos · ScreenExtents + (n.x, n.y·n.a)·2−1
  · mask alpha · scale`; alpha = mask alpha · colour alpha, blend SRCALPHA/INVSRCALPHA. Scale = the MaterialParams
  float the shader multiplies with (`mad r.yz, n, c10x.s, screen uv`): lib 174 c100.x, 159/163 c101.z, 487/1624 c100.w,
  others c100.z (e.g. wp_handgun 2770 = 0.3). The shaders' small tint toward the vertex colour (c100.x/y overlay) is
  not reproduced. Back-buffer materials without a normal map (lib 16–18) only blend texture × colour: plain sprites.
- **Soft sprites** (global class 10 `DepthBufferTexture`: fire 651–653, steam 657–659 / 628–630): texture × colour,
  alpha × sat((scene view depth − sprite view depth − (1 − a)·c100.y) · s) with s = −(the negative MaterialParams
  entry), e.g. fx_fire 6603 (2, 0, −0.01): fades over 100 units. Blend from the material (5 additive).
- Others: the first non-normal-map texture × colour with the material's blend.

Atlas fields `+0x174..+0x177` select the frame as for textures (fire flipbooks 1792–1794 / 2122 are 8×8). Both
renderers copy the scene colour / depth once before the effects (GL `drawFx`; Vulkan through `recordWaterCopies`,
before the water surfaces), so refraction does not see the sea or earlier sprites of the same frame [I].

## FX point sets [O]

Unit data (Level `+0x74`) `+0x120` u32 count, `+0x124` → 0x70-byte records (l3_main_bedroom: 230): `+0x00` matrix
(identity seen), `+0x40` name* ("FXPointSet50069", "fx_heat_haze50001"), `+0x44` id, `+0x58` event-list DTP id,
`+0x5C` float (12642 / 9482; cull distance [U]), `+0x60` u32 point count (2 in all), `+0x64` → points, 0x50 bytes
`{D3D row-major matrix, u8 0xFF, …}`. The event list (DTP rtype 34) is `{u32 GenericFXObject DTP id, records as the
object event list}`; the GenericFXObject (here fx_fire's 6702, its materials and textures) is bundled in the unit DRM.
Records `cmd 3 (def, 1, 2)` run line emitters from point 0 (segment 1) to point 1 (segment 2); cmd 0xB starts a class-5
screen filter ("Screen filters"). This is the burning-manor fire and heat haze of the prologue corridor (ours: `ou-game` starts
every loaded unit's sets at birth, owner `0x40000000 | id` [I]; the exe's starter is not traced). fx_fire's own flame
defs are not in its birth list; it is the "Lara on fire" object (clips `fsfx_burn` / `fsfx_burst` = class-5 tints).

Compared 2026-10-29 with the original (`screens/fxref/fire_rubble_close.png`, Wine; our view `--level
'pc-w\l3_main_bedroom.drm' --spawn -4263,1109,87,-90 --camera -4258,2364,661,-4263,0,300`: the original's prologue
coordinates are ours + (9621, 0, −2977), the original relocates the unit): flames now burn on the rubble in GL and
Vulkan at the original's places, but smaller and dimmer. The strong amber light on the walls and the large glow are the
point sets' class-5 screen filter (finish bloom, "Screen filters"), drawn since.

## Glows (class 2) [O unless marked]

Command 6 (a1 index, a2 segment) or the effect map; at birth only records with `+0 & 1`. Record: `+0` flags (1 birth,
2 rotation jitter, 4 ignores the instance fade, 0x10 random colour phase, 0x20 fixed start position, `>> 6 == 1`
draw mode 2), `+1` kind (1 sprite; all 415 records), `+2` segment, `+8` → colour list `{n, rgba[n]}` cycled every
`+0x10` frames (± `+0xF`) with a prev→cur lerp (0x548D40), `+0xC` id (stop commands), `+0x12`/180 facing threshold,
`+0x14/+0x16` start size, `+0x18/+0x1A` end size (growth `(end/start − 1)/life` per frame), `+0x1C` life (≤ 0 forever),
`+0x20/+0x22` fade in/out frames, `+0x24` depth bias (z ≥ 20), `+0x26` low nibble facing mode (±1/2/3 = local ±X/Y/Z of
`+0x28`, or `+0x28 → +0x2A` vector; alpha × (dot − 1 + threshold) [I scale]), `+0x2C..+0x38` rotation / spin (degrees),
`+0x3C` texture-list rate, `+0x3D` draw condition on 0x5EA390 (a camera mode [U]; 1 = never drawn by us), `+0x40`
texture (0 → `particle.drm` texture 130, the 0x8AA77C model's first frame), `+0x44` texture list, `+0x48` offset,
`+0x4E/+0x50` camera-distance fade band. Mode 1 fades RGB (0x52AE20) → drawn additive, mode 2 fades alpha → alpha
[I: D3D states]. Sizes are taken as world half-sizes [I: the `*2/z` screen scale is not checked against a capture].
Commands 0x15/0x1A fade out the instance's glows with id a1 (−1 all), 0x18/0x1B remove them. Glows of transient
owners (shots, characters) with no life last 1 s [I]; positions are fixed at the start (no segment tracking).
Used by: muzzle flashes (`wp_*` `fire`), impact flashes (`bullet_hit` `idle` by material), lamp glows
(`fx_light_bulb_*`), sun glares (`fx_glare_*`), light rays (`fx_lightray_*`), vehicles, Lara's gear.

## Lights (class 6) [O unless marked]

Command 0x11 (a1 index, a2 segment). Record: `+0` kind, `+2` timed life, `+6` segment, `+0xA` rgb, `+0xE` rgb2,
`+0x11 & 1` birth, `+0x12` radius, `+0x14` radius2, `+0x16` duration (−1 forever), `+0x18/+0x1A` fade in/out [I],
`+0x20` light DTP id (type, cone, falloff, flicker; `formats::lightFromDtp`). Colour rgb/255 → rgb2 and radius →
radius2 over a timed life [I]. ou-game: endless lights born with placed objects are scene lights with their DTP
falloff; all others (muzzle 300 units / 2 frames, metal impacts 400) use 4 reserved slots (linear falloff, point) [I].
Most fixtures (`fx_light_chapel_fill`, `fx_light_krak_ent_*`) are switched by script-played clips (`on`/`off`,
variant-1 records), which do not run yet.

Viewed 2026-10-29 (GL + Vulkan `--rt off`, firefight replay frame 38 as above): the pistol's star flash glow
(texture 557/558, growing over 2 frames) and its warm muzzle light on the rocks. Vulkan with RT on loses the device at
that spawn (also at the parent commit; RenderPerf investigating).

## Screen filters (class 5) [O unless marked]

Command 0xB (a1 index) or the effect map; at birth only records with `+1 & 1` (none in the game data). Record (0x28):
`+0x02` fade-in, `+0x04` rgba, `+0x09` id (stop commands), `+0x0A` fade-out, `+0x0C` hold (−1 forever), `+0x0E` / 128
intensity, `+0x10` radius, `+0x12` / 4096 player mix, `+0x14` filter-resource DTP id (plain), `+0x18` screen-centre
term, `+0x1C` second effect (0x546340); 30 Hz frames. Prim type 0x85 on the 0xAE4D04 list, drawn by 0x548BF0 → 0x5469A0:
weight = intensity × fade-in / hold / fade-out × (1 − d² / radius²) when radius ≠ 0, d from mix(camera, player instance
0xE7F094, player mix) to the owner; `+0x18` ≠ 0 also scales by the owner's projected distance from the screen centre and
an occlusion fade (sun glares). With a resource the weight goes to `0x52C480(resource, w)` → render device vfunc 0x5C
(0x5074E0), which sums it into the device's effect accumulators; without one a full-screen colour quad (`0x52B860`,
mode 1 when alpha = 0x80, else 2). A stop fades an endless filter out from the end of its fade-in (0x52F580 type 0x85).

Filter resource (DTP rtype 16, 0x31C0 bytes; also 0x4547D0's cinematic tables, CINEMATICS.md): each block sums
weight × value (tables + round(t × w), saturating) and is topped up with the identity for what the weights leave below
1 (0x517570 / 0x51AE50). Curves are 9 tables of 256 bytes = a 3×3 matrix: `0x519470` packs tables 3c..3c+2 into one
256×1 texture (R, G, B) per input channel c [I which channel].

| block | effect | fields | drawn |
|---|---|---|---|
| +0x0000 | PCColorDOFEffect (sum 0x516E70, draw 0x5177E0) | `+0x130E` enable, `+0x130C` depth curve, `+0x130D` scene matrix, `+0x1300` blur (sum ≤ 4) × device +0x1330 [U, 1 here], `+0x1304` alpha (≤ 1), `+0x1308` tint RGB, `+0x1310` / `+0x1314` depth range (own with `+0x130F`, else the scene's [U]), `+0x0000` scene matrix, `+0x0900` overlay matrix, `+0x1200` depth curve | scene through the scene matrix; half-size copy through the overlay matrix [I: ours after the blur], blurred, × tint, blended over by alpha (× depth curve over the view distance [I: distance, not z]) |
| +0x1320 | second ColorDOF accumulator | as above | not enabled by the data seen [U] |
| +0x2640 | PCFinishEffect (sum 0x51AAB0, draw 0x51AEF0) | `+0x909` enable, `+0x900` blur (≤ 4), `+0x904` amount (≤ 2), `+0x90A` curves, `+0x908` all 9 tables (else the diagonal) | half-size copy blurred (0x519F20: down to level ceil(r), back up by 4-tap bilinear steps, the fraction blended), through the curves, added (SRCALPHA/ONE, alpha = amount × 127.5) |
| +0x2F74 | PCFeedbackEffect (sum 0x51A190, draw 0x51A530) | floats keep (<= 1), amount (<= 1), blur (<= 4), zoom x / y (<= 4), two angles; `+0x1C` colour, `+0x20` enable | scene x keep, + the (blurred, zoomed / turned) previous frame x colour x amount; ours feeds back the current frame's blur [I] |
| +0x2F60 | PCNoiseEffect (sum 0x51B110, draw 0x51B1D0) | `+0` colour, `+4` alpha (<= 255), `+8` cells (<= 255), `+0xC` [U], `+0x10` enable | 10 x 8 quads of a noise texture at random offsets, alpha-blended; ours: hash noise |
| +0x2FA4 | device 0x512D60 (always, fog-like [U]) | `+0` enable | not drawn |

Ours: `fx::System::screenFilter` sums the running filters (camera, Lara) into `render::ScreenFilter`; the post pass
(`shaders/filter.glsl`, GL + Vulkan) applies the scene matrix, the ColorDOF overlay and the finish bloom; blur level r =
trilinear mip r of a half-size scene copy (GL glGenerateTextureMipmap, Vulkan blit chain). Point sets run class 5 at
birth although no record has the birth bit, centred on the first point [I]; transient owners' endless filters last 1 s
[I]; tints and the `+0x18` sun-glare filters run (below). Survey: 6145 records with a resource (Lara's death / damage clips,
pickups, fire, explosions), 403 tints, 2 screen-centre filters.

The prologue corridor fire's point sets run fx_fire 6702 record 0 (resource 4190: finish blur 4 / amount 2, intensity
0.625, radius 1580 around Lara): the strong amber glow on the walls and Lara in the close capture, nothing in the near /
far captures (Lara > 1580 from every set), as there. Compared 2026-10-29 (`fire_rubble_close.png` vs GL + Vulkan,
`--fov 50`): the walls now glow as in the original; the flames stay smaller than its large white-hot tongues. Flame
defs scale their rate by segment distance / def `+0x164` (3160 here; 0x543F70 → 0x5386F0 `rate × dist / v`); applied,
the fire is ~5× sparser than the capture. Resolved: 0x53F430 / 0x53F200 build each point set as an instance of the particle object whose segments 1.. are the points (segment 0 at their mean) and run its list as an event sequence (0x535EB0, not a birth), so the factor is the line length / 3160 as applied now (`Emitter::rateScale`); the remaining density difference is not in the rate rule [U].

Tints (no resource, 0x5469A0 -> 0x52B860): the record colour over the screen, added when its alpha is 0x80 (mode 1),
else blended by alpha / 128; faded in / out; intensity and radius do not apply. Sun glares (`+0x18` = s): weight x max(0,
1 - (r / 0.65) / s), r = the owner's distance from the screen centre in half-screens (x / 512, y / 448), x an occlusion
fade moving 0.25 per frame (0x535C50; ours: a collision ray from the camera [I]).

## Commands (0x547B40) and classes [O]

Every command type the switch dispatches; "used" = records in object birth / animation lists (survey of every object
DRM, `scratch/cmdsurvey.py`). Classes by GenericFXObject list:

| cmd | target | used | ours |
|---|---|---|---|
| 1 / 2 | 0x536070 / 0x536250 glow prims from a global colour table | 0 | — |
| 3, 0x1D, 0x20, 4 | particles (0x543F70) | 32 512 | yes |
| 5, 0x16 | class 1 ribbons (0x545810) | 107 (Lara birth, tiger / merc / tentacle clips) | yes ("Ribbons") |
| 6 | class 2 glows | 563 | yes |
| 7 / 8 | 0x52E130 / 0x53AB30 (global switches) [U] | 0 | — |
| 9, 0x19 | class 3 lightning / beams (0x537230) | 100 | yes ("Lightning") |
| 0xA, 0xC | class 4 (0x53D100, 0x64 bytes; prim with own segments) [U: blast ring] | 0 (only particle.drm has records) | no |
| 0xB | class 5 screen filters | 369 | yes |
| 0xD | sound 0x481B50 | 931 | audio (AUDIO.md) |
| 0xE / 0xF / 0x10 | pad rumble 0x481CC0 / 0x481CF0, distance-scaled (0x10) | 175 / 322 / 11 | no (no rumble output) |
| 0x11 | class 6 lights | 73 | yes |
| 0x12, 0x1F | debug print | 35 | — |
| 0x15 / 0x1A, 0x18 / 0x1B | fade / remove prims by id (self, +0x218, 0x5B36F0, parent) | 467 / 13 | yes (own instance) |
| 0x17 | 0x5B3EC0 instance model switch [I] | 41 | no |
| 0x1C, 0x2F | effect map id -> class 0..8 | 220 (Lara) | yes, classes 0-3, 5, 6, 7 |
| 0x1E | clears 0xAE5144 (weather lightning timer, 0x545950) | 0 | — |
| 0x21 / 0x22 | instance fade: 0x52C890 (to a1 / 255 over a2 frames from the current fade; a3 != 0 the instance at +0x218) / class 7 record a1 (0x52C920; a2 != 0 the +0x218 instance) | 0 / 122 (rewards, keys, chunkdolls, natla_end; Lara's 17 records are model crossfades, reached only by effect-map ids) | yes ("Instance fades"; +0x218 targets and model crossfades no) |
| 0x23 | 0x52B2C0: births object list entry a1 as a child instance playing animation a2 (not a fade) | 0 | no |
| 0x25 | class 8 (0x5310A0, 0x3C bytes): per-vertex burn map on the instance mesh (instance +0x1B0) [I: BurnFX] | 0 (particle.drm records only) | no |
| 0x26 | class 9 (0x5471A0, 0x10 bytes) | 0 (no records) | no |
| 0x27 / 0x28 / 0x29 / 0x2B / 0x2C / 0x2D / 0x2E | 0x531510, 0x531430, 0x52B5C0, 0x52CE90, 0x52CF30, 0x53B780 (Lara, x a3), 0x53B650 [U] | 0x2C 18, 0x2D 250 | no |
| 0x32 | event sequence 0x535EB0 | 1 | no |
| 0x34 | global float 0xAE3C90 eased over a frames (Lara clips; read by the sound code 0x5B?) [U] | 40 | no |

Scripts: the native type `effect` (NtEffect, tru.exe 0x705871) is never referenced by any script in the data (survey of
every unit and object DRM); script objects reach effects only through their animation clips (below).

Decals: tru.exe has none — no decal / footprint / bullet-hole code, RTTI or strings, and no such textures in the data;
impacts are the hit objects' particles, glows and lights. Not implemented by design.

## Ribbons (class 1) [O unless marked]

Record 0x2C: `+0` id, `+1` flags (1 birth, `>> 6` face blend: 1 alpha, else additive [I]), `+4` material (face
`+0xC0`; spears, tentacles, Natla, Amanda), `+8 / +0xA` segments (command a2 / a3 win), `+0xC` kind (1 = chain of
segments, unused), `+0xD` owner select, `+0xE` ribbon life (-1 endless), `+0x10` face life, `+0x12` start fade (x2,
0x2000 = clear), `+0x14 / +0x18` colours, `+0x1C` subdivision (spline 0x52DA30 between frames: not done), `+0x20`
texture (0: particle.drm model 0 frame 4 = texture 131), `+0x25 / +0x26` fade in / out frames. 0x539500 per frame:
records both segment positions; with the last pair a quad 0x52DBC0 (a type-3 particle) whose edges fade from the start
fade to clear over the face life; the colour goes to `+0x18` [I]. Not added while the owner is hidden (flags 0x800 /
+0x18 & 0x4000000).

Ours: `fx::System` ribbons need their owner tracked every tick (`System::track`; ou-game tracks Lara, enemies and script
objects that have ribbons / lightning). Tiger and merc attack clips leave their claw / blade trails. Lara's birth list
starts four endless arm / leg ribbons (segments 4-6, 11-9, 87-79, 62-54, gold to black): they are not started — the
instance bit that gates them (+0x18 & 0x4000000) is [U] and drawn always they would trail every move.

## Lightning and beams (class 3) [O unless marked]

Record 0xB8 (0x537230 -> prim 0x84, update 0x5370C0, draw 0x53F8C0 -> 0x531770): `+0` kind (0 bolt, 2 colours along
the bolt, 3 beam: colours and widths along), `+1` owner select, `+2` life, `+4` wave phase (-1 random), `+0x10 / +0x16`
start offset / segment, `+0x18 / +0x20` end offset / segment (-100: the runtime marker list, the shot's end for
weapons [I]), `+0x22` steps, `+0x24 / +0x52` core width start / end, `+0x26 / +0x54` glow width, `+0x28` wave and
`+0x2A` jag amplitude, `+0x2C..+0x38` core / glow colours start / end, `+0x4C` flags (1 birth, 0xC0 == 0x40 alpha
blend), `+0x4D` id, `+0x56 / +0x58` fade in / out, `+0x64 / +0x68` core / glow textures (0: texture 131), `+0x6C /
+0x6E` random end offsets, `+0xA8 / +0xAA` moving section (speed per frame, length; handgun tracers 1300 / 1200).
0x531770 steps the line, jitters each point across the view (random jag x a sine envelope, wave) every frame and draws
two camera-facing bands; alpha fades below 896 units of view depth.

Ours: `LightningDef`, drawn as quads in the effect pass (hash-random jitter per 30 Hz frame [I: the exe's sine-table
indexing]). Users: weapon tracers (`wp_handgun` / `wp_uzi` / `wp_akrifle` `fire`), Thor's hammer (`wp_thorhammer`
melee / fire, `co_fx_val_hammer`), Valaskjalf door beam (`fx_beam_val_door_unlock`), light shafts (`sa_uo_light_beams`,
`fx_glare_*`, `fx_light_ray_*`), cinematic lightning (`fx_cine_lightning`). Mjolnir itself is not a player weapon yet.

## Script-object clip events

ou-game runs the clip events of switches / doors (their pull / open and held idle clips, `World::objectClips`) and of
uber objects while their travel clip runs, as the characters' (owner = intro uid). Light fixtures switched by script
clips (`on` / `off`) still do not run: those clips are played by natives not tracked.

## Instance fades

Class 7 records (`+0x44` count, `+0x48` records of 0x0C bytes) [O 0x52C920]: `+0x00` / `+0x02` s16 fade from / to
(4096 = 1), `+0x04` s16 frames (0: `to` at once), `+0x06` model index and `+0x08..+0x0A` model parameters, `+0x0B`
flags (0x80: crossfade to model `+0x06`, fade in instance `+0x238` and switch with 0x5B3EC0 — Lara's 17 records; not
implemented). The record sets instance `+0x1B4` (fade) = from, `+0x1B8` = to, `+0x1BC` = (to − from) / frames and
instance flag 0x40000000; 0x5B55D0 adds `+0x1BC × timeMult` every frame (0.5 per 60 Hz tick) until it passes the
target, and copies the fade to the instances chained from `+0x218`. Fade 0 = drawn, 1 = gone [O: rewards fade 0 → 1
when picked up, keys' `fx_fade_in` 1 → 0, natla_end's `poof_in` sets 1 then fades to 0]. Command 0x21 (0x52C890) moves
the fade from its current value to a1 / 255 over a2 frames.

Drawing [O 0x5066A0]: a fading instance (opacity = 1 − fade < 1) goes through the translucent pass; opaque materials
(blob mode 0) draw as mode 4 (SRCALPHA / INVSRCALPHA), the others keep their mode; MaterialOpacity (PS c98) = the
opacity; no depth writes. Ours: `FrameOptions::meshOpacity` (RENDERING.md "Instance opacity") with alpha = opacity for
opaque materials and alpha × opacity for the others [I: the opacity PS variant, composite slot 1, is not translated].
The instance's glows fade with it unless their flag 4 is set [O 0x536280]; particles also scale by it unless their
prim `+0x83 & 4` [O 0x5364A4] — not done (particles do not keep their owner).

ou-game (`EffectsView::pickedUp`): a collected reward runs its `pickup` list's variant-1 records (the 0x2401 fade,
0 → 1 over 35 frames = 70 ticks) and is hidden (meshes collapsed, its effects stopped) when the fade ends; rewards
without a fade vanish at once as before. Script objects with fade records in their clips (keys' `obj_store` /
`fx_fade_in`) fade through the same path. Viewed [O] (2026-10-29): l4_pool treasure 5489 half faded at tick 66 (pickup
tick 31) in GL and Vulkan, the floor showing through; the tablet's mean grey 3.4 → 14.8 in both.

## Missing

Classes 4, 8, 9 (above); class 7 model crossfades and +0x218 targets; instance fades of particles; ribbon subdivision;
lightning's exact jitter tables; the feedback filter
uses the current frame's blur instead of the previous frame; noise uses a hash, not the device texture; skydomes,
particles that follow their segment (mode 1), casing meshes, per-enemy hit materials, the line emitters' view-angle
density rule (def `+0x8C / +0x90`, 0x5386F0 second branch: no data sets it in the fire defs checked).
