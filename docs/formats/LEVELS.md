# TRU levels: unit DRMs, stream groups, IMF placements, intros

Code: `src/formats/Level.cpp` (`parseUnit`, `parseObjectList`, `objectRenderModels`, `introMatrix`) and
`src/formats/Scene.cpp` (`loadLevelScene`). Run it with `ou-viewer --game-dir … --drm 'pc-w\l4_pool.drm' --level`
or `ou-asset stats|render|dump-mesh <game> <unit> --level`.

Labels: **[O]** observed in game data, in a render, or in our tru.exe decompile (GOG build VAs); **[I]** inference;
**[U]** unknown. Pointers below are DRM relocations: internal ones point into the same section, external ones point
into another section of the same DRM (`cdc::sectionPointers`). Resource references are type-3/type-5 relocations;
the referenced id stays in the data (`{offset/4:25, sectionType:7}`).

## Bigfile selection (language, patch)

- [O] `ArchiveFileSystem_FindEntry` 0x4726E0 accepts a record when `(record.specMask & fs.specMask) == fs.specMask`.
  `InitArchive` 0x473840 sets the mask to 1. After the `locals.bin` check, `0x49A900` calls `0x54F2E0(1 << Language)`.
  That reaches every mounted archive through the multi-FS setter `0x472D80`. Bits 0–4 = EN FR DE IT ES (see
  CONTAINERS.md §1.4). `Bigfile::find(path, mask = kSpecEnglish)` implements this rule.
- [O] `InitPatchArchive` 0x478930 inserts `PATCH.000` at slot 0 (`0x472C50` with "add to front"). The multi-FS lookup
  `0x472B70` returns the first slot whose `FileExists` succeeds. `cdc::BigfileSet(gameDir, mask)` reads patch.000
  first, then bigfile.000. Test `RealData.PatchOverridesBigfile`: `pc-w\l2_puzzleroom.drm` is the 13 751 556-byte copy
  from the patch.
- Old behaviour: the loader took the first hash match. For the 1029 hashes that have voice variants, that is often the
  French or Spanish record.

## Unit DRM layout

Every unit DRM begins with a general section: the **Level** struct. [O] in all 82 unit DRMs present in
`unitlist.txt` (4 of the 86 listed names are not in the bigfile).

| Level offset | Meaning | Evidence |
|---|---|---|
| +0x00 | `Terrain*` (24-byte section: portals, collision terrain groups) | [O] reloc; matches TRLAU-menu-hook TR8 `Terrain` |
| +0x70 | settings DTP: fog/ambient (SHADERS.md), `+0x294` u16 Lara outfit object id | [O] 0x5C7CA0, 0x5DE2B0 (GAMEPLAY.md "Outfits") |
| +0x74 | unit data (DTPDATA section) | [O] `FUN_005B3860` reads intros from `Level+0x74` |
| +0x80 | float3 offset between the collision and render frames | [O] reported by the Collision agent. Collision meshes **and portals** are stored relative to it; intros do **not** need it (see below and "Streaming") |
| +0x90 | cell group data (general section) | [O] reloc; field layout matches DXHR `CellGroupData`, reordered |
| +0xA0 (l4_pool) | stream-unit portal array, reached through `Terrain+0x04` (see "Streaming"); its position in section 0 varies (ballcourt: +0x2260) | [O] |

### Intros (placed instances)

Unit data `+0x0C u32 count`, `+0x10 Intro*` and a second list `+0x1C count`, `+0x20 Intro*`. Records are 0x90 bytes.
[O] `FUN_005B3860` searches both lists by unique id (record `+0x58`).

| Intro offset | Meaning | Evidence |
|---|---|---|
| +0x00 | rotation, float4 Euler radians | [O] `INSTANCE_IntroduceInstance` (`0x5BCC30`) copies it to instance+0x30 |
| +0x10 | position, float4 | [O] copied to instance+0x20 |
| +0x20, +0x30 | second copy of rotation and position | [O] equal in all records examined; purpose [U] |
| +0x40 | scale, float3 | [O] used only if `byte +0x71 & 0x10`, else scale = 1. No observed intro sets the bit |
| +0x50 | u16 object id (`objectlist.txt`); 0xFFFA (−6) = marker with no object | [O] `0x5C1730(*(u16*)(intro+0x50))` object-tracker lookup; −6 matches cdcResearch |
| +0x52 | u16, copied to instance+0x40 | [O]; meaning [U] |
| +0x58 | i32 unique intro id | [O] |
| +0x60 | i16 initial animation (−1 = none) | [O] `G2EmulationInstanceSetAnimation` |
| +0x70..0x73 | flag bytes | [O] several bits are tested in 0x5BCC30; meanings [U] |
| +0x78 | pointer to intro data | [O] passed to the birth function; layout [U] |

Euler order [O]: `cdc_Matrix_Build_XYZOrder` 0x49D500 (disassembled) starts from identity and post-multiplies, in
D3D row-vector form, a Z rotation by `euler.z`, then Y by `euler.y`, then X by `euler.x`. In column-vector form this
is `R = Rx(x)·Ry(y)·Rz(z)`. `introMatrix` builds `T·R·S`. [I] Scale is applied in model space.

Frame [O]: intros are in the **render-terrain frame**. They need no `Level+0x80` offset:

- l4_pool: 162 of 173 non-marker intros lie inside the terrain AABB. The others are 10 cinematic props parked at
  (0, 0, −11542) plus one trigger volume.
- The `player` start intro lies within 300 units of a terrain vertex; the median over all intros is 488. With the
  +0x80 offset applied the median becomes 1118 and the fit is worse.

### Object id → model

- [O] `\tr8\pc-w\objectlist.txt` lives in the bigfile. It has a count line, then `id,name` lines (821 entries). The
  exe string is `\tr8\%s\objectlist.txt` (0x5BF330). The object DRM is `pc-w\<name>.drm`.
- [O] Each object DRM has exactly one OBJECT section (type 11), and its section id equals the object id (e.g.
  ca_rw_artifact = 572). Object section `+0` holds a resource reference to the ObjectBase DTP.
- [O] `ObjectSection` construct `0x54E200` copies `numModels = base+0xA0` (u16) and `modelList = base+0xA4` into the
  runtime Object. The list holds resource references to Model DTPs. Model DTP `+0x64` is a resource reference to the
  render model (section type 12, res 26), observed on every object we inspected.
  - Lara: 3 models; model 2 has no render model.
  - co_wp_handgun: 4 models.
- `loadLevelScene` draws the first non-zero render model. [I] The exe picks the model per instance (LOD,
  `modelNum` arguments). That choice is not reversed.

### Lights (unit data +0x00 / +0x04)

Code: `formats::parseUnit` → `scene::Environment::{lights, sun*}`; tests `Level.ParseUnitLights`,
`RealData.UnitLightsAllUnits`. Render use: SHADERS.md "Engine inputs".

[O] Unit data `+0x00` u32 numLights, `+0x04` → array of pointers to 0xE0-byte light records. tru.exe 0x5CEC60 creates
a scene light per record from its light DTP, 0x5C6530 applies the record's overrides.

| record off | meaning | evidence |
|---|---|---|
| +0x00 / +0x10 / +0x20 / +0x30 | matrix rows X, Y, Z (= the way a spot points), position; same frame as the intros | [O] 0x5C6530 copies 16 floats; l3_main_bedroom torch light on the torch-holder wall in our render |
| +0x48 | light DTP (resource reference, type 7, resolved in the unit DRM) | [O] |
| +0x4C / +0x50 | intensity override / flag | [O] SceneLight vfunc 0x20 → PCRenderLight +0xB0 (0x50A1F0) |
| +0x51..+0x53 / +0x54 | colour override u8 rgb / flag (bedroom torches 246,154,36) | [O] vfunc 0x24 |
| +0x58 / +0x5C | radius override / flag (0 → DTP radius) | [O] SceneLight 0x5742B0 |
| +0x5D, +0x60..+0x68 | vfunc 0x48 (PCRenderLight +0x10C..+0x114, default (1.5e-5, 2.2, 0)) | [O] write; meaning [U] |
| +0x6C, +0x70..+0x78 | vfunc 0x68: fade/LOD distances (bedroom 2000/4000/8000) [I] | [O] write |
| +0x88 / +0x89 / +0x8F / +0xA6 / +0xA7 / +0xC4..+0xD0 | render-light flag bits, shadow mode, misc | [O] writes; meaning [U] |
| +0x8C..+0xB7 | two 32-bit receiver masks built from bytes (vfuncs 0x6C / 0x74) | [O] build; receiver side [U] (not applied) |

The light DTP (0x14E4 bytes) is copied to `PCLightResource +0x42C` (0x50A000; getter 0x50A030), so DTP offset d is
resource `+0x42C + d` [O]:

| DTP off | meaning | evidence |
|---|---|---|
| +0x00 | u32 type: 0 point, 1 spot, 2 sun (directional) | [O] 0x5042B0 slot choice, 0x50B910 |
| +0x04 / +0x08 | radius / intensity | [O] defaults via 0x50A3C0 bits 4 / 1 |
| +0x0C / +0x10 | spot cone angles (full, radians; bedroom 1.14 / 1.66) | [O] 0x50A3C0 bit 8 → 0x50A280 (cos of each, scale/bias); [I] half angles enter the cos |
| +0x14 | spot width at the light: apex moved back by it / tan(outer/2) | [O] 0x50B910 |
| +0x18 | u8 rgb colour | [O] 0x50AFB0 |
| +0x1C | f32[255] falloff curve over d / radius → AttenuationSampler row (texel k = curve[k]·255 in R, texel 255 = 0) | [O] 0x50AFB0, 0x50A050 |
| +0x41C / +0x420 / +0x424 | flicker flag, period, f32[256] intensity curve | [O] 0x50AFB0, 0x50A500 |
| +0x824 / +0x828 / +0x82C..+0x838 | cookie flag, cookie texture (type-5 ref), scale, rotation, scroll | [O] 0x50AFB0, 0x50B910 |
| +0x144C | casts shadows (shadow-light slot) | [O] 0x50A080 |

Survey (`RealData.UnitLightsAllUnits`, 2026-10-18) [O]: 82 units, 218 point + 342 spot lights (106 flicker, 271 shadow
casters), 23 units with a sun (11 with a cookie), at most 28 lights per unit (l1_ship_sinking_hall). 347 of 586 records
use a non-linear falloff curve. l3_main_bedroom: 18 lights, its torches are flickering orange point lights.
Not decoded: lights owned by object intros (torch objects) or cinematic light rigs (`co_light_rig*`, CINE light
channels).

### Cell group data (Level+0x90)

| Offset | Meaning | Evidence |
|---|---|---|
| +0x00 | header* `{numTotalCells, numToplevelCells, numBSPNodes, numStreamGroups (+0x0C), …, numImfs (+0x1C)}` | [O] counts match the arrays: ballcourt 17/17/428/10/…/3293 |
| +0x04 | BSP nodes* (planes with child links) | [O] shape only |
| +0x08 | stream groups*, 20 bytes `{name*, numCells, cells*, streamFileName*, resolveObject}` | [O] same as DXHR `CellStreamGroupData` |
| +0x0C | symbol table* (offsets + strings "unitname", "portal_cell1"…) | [O] |
| +0x10 | cells*[numTotalCells] → `CellData {header*, terrainData*, …, renderMesh* @+0x20}` | [O] |
| +0x14 | exterior terrains* `{count, CellTerrainData[count] {terrain ref, flags, light mask, user data}}` (`Unit::exteriorTerrains`): panorama cylinders / sky domes added to the scene's "Exterior Cellgroup" (0x572BD0 → 0x56DE80). 18 units have them (l2_junglehub 10635: four painted quarter cylinders of radius 3072 round the start + a light card) | [O] |
| +0x18 | IMF records*, 0x70 bytes each | [O] |
| +0x1C | resource ref to a DTP | [O]; content [U] |

- Stream groups [O]: `streamFileName` is null for some groups ("default", "group_ballcourt_edges"). The others name
  `pc-w\streamgroups\<file>.drm` (`LOAD_StreamGroupFileName` 0x477990). Ballcourt: 10 groups, 8 DRMs. All 8 exist and
  hold the unit's terrain (res 24). The ballcourt unit DRM itself has no terrain. l4_pool has 0 stream groups, and its
  terrain is in the unit DRM.
- Exterior terrain is drawn in the far 2 % of the depth range: `PCRenderTerrain +0xC8` (vtable +0x38 setter 0x510B00)
  makes the terrain drawable switch the viewport (0x51D0A0) to MinZ = 0.98·MaxZ, all other geometry using
  [MinZ, 0.98·MaxZ], so the backdrop is behind everything wherever it stands [O decompile; tru.exe memory at the
  l2_junglehub start: the 3 exterior terrains drawn there have +0xC8 = 1, the 8 cell terrains 0]. Renderers draw
  `SubMesh::exterior` first and clear the depth (same result). l2_hubhall's second exterior 10533 has +0xC8 = 0 there
  (not drawn from that spot [U] why).
- Terrain group `+0x0C` bit 1: the group is skipped by the colour pass (0x511810) but not by the shadow pass
  (0x511E20, bit 2 only) [O]: `SubMesh::shadowOnly`, left out of the draw order, cast shadows (map + RT shadow
  rays), skipped by other RT rays. l2_junglehub's are the speckled rock sheets (material 10914) above the hub that
  shade the canopy floor; drawn, they showed as grey patches among the trees.
- Cell terrain data `{renderTerrain resource ref, 0, 1, TerrainGroup*}` [O]. No transform is stored; terrain is drawn
  untransformed, and the renders line up [O].
- Cell `renderMesh` (+0x20) is a model with no texcoords and only depth/fog-type materials. It is skipped as a "volume",
  see MESHES.md.
- IMF record [O]:

  | Offset | Meaning |
  |---|---|
  | +0x00 | 4×4 float matrix, D3D row-major, row 3 = translation (rows 0–2 have w = 0, row 3 has w = 1 in all 3293 ballcourt records) |
  | +0x40 | internal pointer [U] |
  | +0x44 | name* (e.g. "alowpolybush_bakedInstGroup") |
  | +0x48 | resource ref to a DTP `{?, renderModel ref @+4}` |
  | +0x4C | optional texture ref [U] (lightmap?) |
  | +0x60 | u32 1 [U] |

  Column-major memory of a D3D row-major matrix is the same transform, so the matrix is copied as-is. Ballcourt's 3293
  IMFs reference 16 DTPs, all inside the unit DRM.

## Streaming: portals, unit transitions

Code: `formats::Portal` / `parseUnit` (Level.cpp), `game::loadUnitData` and `World` streaming in
`src/game/Stream.cpp`. Tests: `Stream.*` (synthetic units) and `StreamReal.*` in `tests/test_save.cpp`. Replay:
`tests/replays/pool_to_ragnarok.txt`.

### Portal records [O]

The Terrain record (`Level+0`, 24 bytes) starts `{i16 ?, i16 numPortals, StreamUnitPortal* portals, i32
numTerrainGroups, TerrainGroup*, signalTerrainGroup*, ?}`. This matches the TR8 branch of TRLAU-menu-hook `Level.h`.
Portals are 0x100 bytes:

| Offset | Meaning | Evidence |
|---|---|---|
| +0x00 | `char[128]` target unit base name | [O] e.g. "l4_ragnarok_room" |
| +0x80 | i16 **toSignal**: signal id of the matching portal in the target unit | [O] `0x5D13E0` compares it with the other unit's +0x82 |
| +0x82 | i16 **signal**: this portal's own signal id | [O] same |
| +0x84 | i16 **toStreamId**: stream-unit id of the target | [O] `0x5D13E0` / `0x5D2AA0` compare it with `StreamUnit.id` |
| +0x86..+0x8E | i16s (0/1, 16384, 17692, 0, …) | [U] |
| +0x8C | runtime `StreamUnit*` of the target (written by `0x5D13E0`) | [O] |
| +0x90 / +0xA0 | min / max, float4 | [O] |
| +0xB0 | quad[4], float4 each | [O] |
| +0xF0 | normal, float4 | [O] axis-aligned in the units checked |

Frame [O]: portal coordinates are relative to `Level+0x80`, like the collision vertices. `parseUnit` adds the
offset. Evidence:

- With the offset, l4_pool's portal is the 3792 × 4424 doorway at x = −16434. Its floor (z 4741) is exactly the
  `player` intro's height, and it is centred on y = 0.
- l4_ragnarok_room's back portal lands next to its checkpoint 5669 (z −7850 vs −7851).
- Without the offset, both portals float in empty space: no collision anywhere near them.

Survey (82 units, `scratch/portalsurvey.py`) [O]:

- 158 portals, all targets exist.
- 156 pair with a back portal by signal (`back.signal == toSignal`); 152 of these have equal extents within 2 units,
  the other 4 differ by 30–35 units in one axis (ballcourt↔trailtube, valgrind↔valgrind_tunnel).
- `l2_cliffhall_b` ↔ `l2_hubhall` do **not** pair: cliffhall_b has to 74 / signal 78, hubhall has to 75 /
  signal 73. [I] tru.exe would hit its "Can't find signal%d in unit %s" error there. We log a warning and do not
  stream that neighbour.

### How tru.exe streams [O]

- `STREAM_LevelLoadAdjacentUnits` (0x5D2940) keeps the portal neighbours of the player's unit loaded.
- When a unit loads, `0x5D13E0` hooks it to the loaded units. For each portal P of the new unit N whose
  `toStreamId` is a loaded unit L, it finds the portal Q of L with `Q.signal == P.toSignal` and relocates N by
  `Q.min − P.min`. This goes through `0x5D10D0`, which calls RelocateUnit 0x5C6210 ("RelocateUnit (%s, (%g, %g,
  %g))") and shifts terrain groups, instances, markup, sounds, etc. Only the first match relocates.
- So every streamed unit is moved into the frame of the unit that was loaded first. Moving into a new unit
  (`0x5B3A20`) only sets the instance's unit id (`instance+0xE0`). It does **not** move coordinates.
- Crossing: when an instance's collision hits a signal of the unit's signal terrain group, `0x5D2CA0` calls
  `0x5D2AA0` ("Hit stream portal in %s to %s") with the portal index at signal `+0x14`.
  - If the target unit is loaded, the instance moves into it.
  - If not, and the instance is the player, the game loads it synchronously (a `Sleep(5)` poll loop) and then moves.
  - Other instances that reach an unloaded unit get instance flag 0x20 and `0x556680` is called. Their meaning is
    [U]; [I] they are parked until the unit loads.

### Ours [I unless marked]

- `World(UnitLoader, unit, …)` loads `unit` at offset 0 plus every portal target. Each target goes at the offset
  computed exactly as `0x5D13E0` does.
- Collision: the loaded units' meshes and markup are merged into one `CollisionMesh` in the world frame, and its BVH
  is rebuilt on every change. Ledge and water markup indices in Lara's state are remapped across the rebuild.
- Crossing: each tick, Lara's path at mid-body height (feet + 250) is tested against the planes of the current
  unit's portals, inside their bounds ± 50. Crossing one makes its target the current unit. Then the target's
  neighbours are loaded and every unit that is neither current nor adjacent is dropped. This replaces the signal
  faces; the signal terrain group is not decoded.
- A neighbour that failed to load is retried on crossing. Loads are synchronous, as in tru.exe.
- Rendering (`ou-game`): each loaded unit's level scene, moved by its offset, is re-uploaded when the unit set
  changes. Unit music stays the start unit's. Script objects follow the unit set (src/script).

### Unit script

- [O] Level+0x70 DTP +0x1C0 = the unit script's type id (type-8 section of the unit DRM, super type 4 `NsUnit` chain):
  14/14 units checked (l3_main_bedroom 12059, l1_seafloor 5277, l2_cliffclimb 9502, ...). It has a state table;
  ours creates it with the unit and runs state 0 as its own thread (`unitref.Get` returns it), ending it when the
  unit streams out [I: created after the unit's intros].

### Verification

- [O] `StreamReal.PoolToRagnarokOnFootThenSaveLoad`:
  - from l4_pool's `player` intro, Lara runs west; l4_ragnarok_room is already loaded at offset
    (−20475.4, −2872.2, 12591.4);
  - she crosses into ragnarok (tick 104 in the ou-game replay) without falling; her z stays within 80 of the start
    (floors: pool 4721, ragnarok 4741);
  - l4_staircase_mictlan streams in at (−20475, −5295, 12591).
- [O] `ou-game` replay `pool_to_ragnarok.txt`, 2026-10-18: logged `tick 104 in unit l4_ragnarok_room | loaded:
  l4_pool l4_ragnarok_room l4_staircase_mictlan`. The final screenshot (tick 180) shows Lara in the ragnarok
  corridor with l4_pool's water and statue visible through the doorway, aligned.
- [O] A ground probe across the doorway hits ragnarok's floor up to x −16500 and pool's from −16250 on (scratch
  probe). The collision is continuous across the portal.
- [O] `Stream.*`: neighbours load and old units unload, relocation offsets, no fall across the seam, a missing
  neighbour gives a warning, markup index mapping.

Open [U]: signal faces and the `Signal` records; portal +0x86..+0x8E; the unpaired l2 portals; whether tru.exe ever
re-bases coordinates when the player moves far from the first unit.

## Spline movers (intro +0x68)

- [O] `0x5BA280` binds intro `+0x68` (a MultiSpline) to instance `+0x284` (`0x5D1290`). MultiSpline `+0x40` position
  track, `+0x44` rotation track, `+0x48` a vec4 track copied to instance `+0x80` (absent in all shipped intros), cursors
  at `+0x4C/+0x54/+0x5C`. Track: u16 keys, u8 flags `+3` (bit 3 Bezier, else Hermite; bits 1/2 make the cursor wrap),
  f32 end time `+4` (keys are 30 Hz frames; shipped paths have one key per frame), keys from `+0x10`: position 0x40
  bytes {value, Hermite tangent, Bezier control, t0, t1, ?, 1/(t1-t0)} (`0x47CD40`), rotation 0x20 bytes {quaternion
  xyzw, t0, t1, ?, 1/(t1-t0)}, slerped (`0x47D180`). Position values are relative to `Level+0x80`: value + it = the
  intro position at key 0 (l7_yggdrasil_end movers, offset (-1512, -2621, -4106)). The quaternion matches the intro
  Euler angles (Rx·Ry·Rz) as glm::quat(w, x, y, z).
- [O] Setup: key 0; intro `+0x64` low nibble 1/2/4/8 = once / loop / loop-count / ping-pong and playing; intro byte
  `+0x70` bit 0 = backwards; `+0x62` bit 0 = play to key `+0x6E`, bit 1 = clip [`+0x6C`, `+0x6E`]. Per frame
  `0x5CF260` advances dir × timeMult (ours 0.5 frame per 60 Hz tick), stops at a target key, ends the clip per mode,
  then reports key ranges entered/left (`0x5CC8F0`). Natives: SCRIPTS.md.
- [O] Users: 70 `natla_end_mover` (l7_yggdrasil_end; Natla's flight paths, scripts forward to `natla_end` 8403) and 21
  `fish_medsea` (l1_seafloor), all looping. Neither object has solid collision; ours moves them through
  `Object::body` (render meshes and, for objects with solid shapes, their collision, which carries Lara like the
  uber elevator). `GameReal.SplineMoverFollowsItsPath`, `GameReal.SplineMoverCarriesLara` (real path on a real solid
  object).

## Camera splines (unit data +0x68 count, +0x6C -> 0x60-byte records)

- [O] Record: +0x30 offset (added with the unit offset to every key, 0x60A3A0), +0x40 name, +0x44 time mode (0 key
  times, 2 constant speed over +0x48 s, 3 key times + ease), +0x4C FOV degrees, +0x50/+0x54 camera keys (0xC0:
  +0x00 position, +0x10 quaternion xyzw, +0x20 tangent, +0x30 time s, +0x34 u16[64] ease, +0xB4/+0xB8 events
  {type, arg}), +0x58 target track if +0x5C (mode +0x44: 0 speed, 1 times, 2 eased; 0xB0 keys: +0x00 position, +0x10
  tangent, +0x20 time, +0x24 ease, +0xA4 follow Lara, +0xA8/+0xAC events {type, arg, arg2}), +0x5D target fitted to
  the camera track's length (0x60A270). 82 units, events seen: 3 pause (148), 0 (84), 1 switch/FOV (28), 7 (8), 2 (2).
  Player: GAMEPLAY.md "Cameras".

## Planner nodes (unit data +0x118 count, +0x11C -> 0x60-byte records)

- [O] Record (0x5C8EA0, 0x669D00): +0x00 matrix, +0x4C node count, +0x50 -> 0x60-byte nodes {+0x00 matrix, +0x40 u8
  0xFF, +0x44 f angle (≤ 85 used), +0x48 / +0x4C f distances, +0x50 bit 0 copied to the planner}. A node's point =
  record matrix × node translation + the unit offset. l1_ship_exterior: one record of 187 nodes (deck z 5706, aft
  deck 3809, lower deck ~2556); l6_ship_exterior 184; l4_ballcourt 28. tru.exe builds its planner graph from them
  (0x669D00: 14 probes of radius 200 per node) [O]; scripts name one as a noderef {record, node} (enemybase 27
  GuardNode). `formats::Unit::plannerNodes`. Their +0x44..+0x4C meaning is [U].

## Survey (Python cross-check of the C++ layouts, 2026-10-18) [O]

All 82 present units:

- section 0 is a general section with `+0x74` and `+0x90` pointers;
- unit data is DTPDATA;
- intro counts range from 1 to 530;
- intro positions and rotations are finite and below 1e6;
- 19 units have stream groups (2–10 header entries each, including entries without a stream file);
- 20 units have IMFs (2–3293).

## Verification

`ou-viewer` (GL) screenshots, viewed 2026-10-18 [O]:

- **l4_pool**, camera `-1000,-9000,7000,90,-25`:
  - without instances: an empty, dark pit;
  - with `--level`: a blue goo/water surface (`ca_goo_pool_watersurface`) fills the pit exactly to its stone edges;
  - rock pillars and a statue sit on the ledges;
  - the cinematic props are parked below the level, away from the play area.
- **l4_ballcourt**:
  - overview: terrain from 8 stream groups covered by IMF tree canopy;
  - from the `player` start (`26971,23597,3000,-100,-5`): a dirt jungle path lined with ground plants and palms whose
    bases sit on the terrain;
  - elevated view: a stepped pyramid with a temple top, stone court walls, trees around them.
- [I] This matches the Mexico jungle and Xibalba areas of the game in general terms. **No side-by-side comparison with
  in-game screenshots was made**: the local PCGamingWiki copy only has settings screenshots.

Known deviations [O]:

- Editor objects are no longer drawn: `INSTANCE_IntroduceInstance` (0x5BCC30) sets instance flag `0x800` when the
  object's ObjectBase `+0` flags ("oflags") have bit `0x800000` [O], and 0x540B80, a per-instance routine that also does
  the bounding-sphere visibility test, jumps to its end for `0x800` [O; that this is the draw path is I].
  `loadLevelScene` skips those objects (`formats::objectFlags`). The 31 such objects in `objectlist.txt` are mostly
  volumes/markers (`triggervolume`, `soundvolume`, `triggerplane`, `cameraanchor_object`, `cinematic_object`,
  `dispatcher`, `reveal_object`, `watervolume`, `particle`, `sound_source`, …) plus a few props/gear (`co_rock*`,
  `g_lighter`, `ma_dungeon_watersurfaces`, `wp_natla_fireball`) [O] that game code presumably shows later [I].
  In l4_ballcourt this removes 6 objects (camera anchors, dispatchers, reveal objects, sound volumes, trigger
  planes/volumes) [O]. Still drawn [U]:
  `co_monitor_frame` (marker texture 372, oflags 0x90000014) and the 24-vertex `fx_*` effect proxies (oflags bit
  `0x40000000`, set on 279 objects incl. many visible ones, so not used as a filter). Intro flag `+0x72 & 0x10` also
  hides an instance when a callback refuses it [O]; the callback is not reversed, so such intros are still drawn.
- Foliage renders correctly since materials carry blend/alpha-test/two-sided state (MATERIALS.md); the black ground
  plants came from the `COLOR0` attribute (0 on them) being used as a colour.
- The unit's dependency objects (`ca_skydome_a`, the `g_*`/`wp_*` gear) are not placed.

`loadLevelScene` warnings on these two units:

- `player` has no render model.
- `checkpoint`, `cinematic_object`, `co_cinetest_marker`, `co_light_rig*` and `portable_warp_attach` have models whose
  batches are all skipped volumes.
- No unresolved object ids or IMF DTPs.

## Open questions

- [U] Which model an instance uses (LOD/`modelNum`); whether `co_monitor_frame` / `fx_*` proxies are hidden by script,
  the effect system or the intro `+0x72 & 0x10` callback.
- [U] Intro `+0x52`, `+0x70..0x73` flags, intro data at `+0x78`, the second intro list's role (l4_pool: 1 entry).
- [U] IMF `+0x40`, `+0x4C`, `+0x60`; cell BSP and portals; cell group `+0x14`/`+0x1C`.
- [U] Why `Level+0x80` exists if intros and terrain already share a frame (collision only?).
