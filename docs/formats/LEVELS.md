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
| +0x78 | pointer to intro data | [O] instance +0x104 (0x5BA810); u32 +0x00 type (13 enemies, below; others [U]) |

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
- Which model an instance draws: the drawable (`cdc::InstanceDrawable`, instance +0x244) holds one render-model
  instance per object model and draws the one at instance `+0x234` [O 0x5B5BE0 / 0x5B5CA0].
  - [O] An introduced instance starts on model 0: `INSTANCE_IntroduceInstance` 0x5BCC30 → instance init 0x5BA810 with
    model argument 0 → 0x5B9170 (sets `+0x234` / `+0x235`). Model 0 has a render model on every object of
    `objectlist.txt` that has one at all (2026-11-07 survey).
  - [O] The only distance switch, 0x5B5D20, runs for oflags `0x400000` with `+0x235 == 0`: beyond ObjectBase `+0x20` /
    `+0x24` (ints after 0x54E200) it selects model `+0x28` / `+0x29` (u8, −1 none). No placed object has the flag, and
    every object has `+0x28` / `+0x29` = −1 (survey of the 697 objects intros name), so placed objects stay on model 0.
  - [O] Other model changes are runtime: breakables (class 30) switch to their broken models when damaged (0x6AE240,
    object data `+0x39` / `+0x3A`), uber sections use their own model (below), save games restore `+0x234`.
  - `loadLevelScene` draws model 0 of every placed object.

### Uber sections

[O] An uber object (class 79) does not draw its own intro instance: `UberObjectComposite` (0x4D7D50) sets instance flags
`0x800` / `0x20000000` on it, then 0x4D3DA0 creates one instance per section record (0x54 bytes; uber data `+0x08`
count) with 0x4D1110 → 0x4D0E50: model = section record `+0x18` (u8), position / rotation / scale copied from the
intro instance, an anim component of its own (instance `+0x280` → `+0x14`, all segments, at the bind pose). Record
`+0x30` is not an animation: 0x5BA000 / 0x52B1E0 walk it as the list an object's `+0x04` is (attached effects and their
handlers [I]). The section's animations come from its state machine (SCRIPTS.md "Uber objects", "Section
animations"). Each section draws its model whether or not a joint carries it. Wine instance list (2026-11-07, instance
`+0x14` flags, `+0x234` model, `+0x4C` owner, tool
`openunderworld-work/proxies/ix.c`; the new-game start, l1_kraken_room, l1_baldrs_tomb, l2_puzzleroom, l2_shrineroom):
every uber intro has `0x800`, none of its sections (uids `0x4000xxxx`, `+0x4C` = the intro) has; new-game start
`ca_ei_hpole_a` = hidden intro + sections on models 0 and 1 (the bar); l2_shrineroom `sa_uo_naga_statue` 2794 = hidden
intro + sections on models 1..5, no instance on model 0; l1_kraken_room `ms_ani_tentacle_gearr` 32 = 13 sections on
models 0..12; `ms_ani_sunsphere_plat` 27 = sections on models 0..3.

`loadLevelScene` places, for an uber object, the first section on model 0 as the object's own mesh (" @intro <uid>",
posed by its script, its model-0 joint and that section's clip in ou-game: `script::Object::animPose`) and every other
section with a drawable model as " @intro <uid> @section <index>" (ou-game: `World::uberSectionBody` for the ones riding
a joint, the intro transform otherwise, skinned by `World::uberSectionPose`, the clip its anim component plays, in the
shared scene upload: GL, Vulkan and the RT BLAS/TLAS). A section whose model has no render model draws nothing; an
object with no section on model 0
(`sa_uo_naga_statue`) draws no model 0. Survey of the 82 units (live intros): 108 more section instances in 28 units
than when only jointed sections were drawn (82 → 190), among them the horizontal poles' bars (`*_ei_hpole_*` model 1),
the Kraken room's gears and sun-sphere platform, the l2 puzzle-room scales, the calendars and the l3 chapel pillars.
State action 1 (0x4D86A0) switches a section instance's model (+0x234) at runtime: every model a section's states
switch it to is placed too, as " @intro <uid> @section <index> @model <model>" (only l1_ship_sinking_hall's break
pipes 1189-1191: model 1, the broken pipe); ou-game shows the one `World::uberSectionModel` names and collapses the
others, the own mesh included (`World::switchedObjects`); the viewer drops them (SCRIPTS.md "Section models").
Tests `RealData.LevelUberSectionModels`, `ProgressionReal.BreakPipeSwitchesItsModel`.

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
channels). The light tru.exe has at the l3_main_bedroom start that the unit lacks, (3547, 6975, −1751), is not an
object's: it is portal neighbour l3_chamberrockhall's light 2 (Wine, SceneLight +0x04/+0x1F0 → that unit's cell 2;
`StreamReal.BedroomNeighbourLights`), which ou-game has as it merges every loaded unit's lights.
Runtime (Wine, l3_main_bedroom start) [O]: SceneLight +0x64 enabled (0x5CEC60 creates every light disabled, vfunc
0x28; 0x5CC190 switches each light every frame: off unless record +0x8A, and with +0x80 off from camera distance d ≥ D + range − min(0.3 D, range / 2), fading over `range` before that, D = +0x7C, range = +0x84, d = |camera − light| − DTP radius for points (0x5CBFA0; spots [I]); bedroom start: 8, 13, 14, 15, 17 off, `OriginalShading.BedroomLightsSwitchByCameraDistance`, render::original::lightFade), +0x154 / +0x158
light / shadow masks (record bytes above), +0x178 cells reached (1 several, 2 home only), +0x1F0 home cell, +0x2CA
seen this frame.

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
| +0x1C | resource ref to the **irradiance volume** DTP (light probes, below) | [O] |

#### Irradiance volume (cell group +0x1C DTP)

[O] tru.exe 0x55D360 / 0x52A450 / 0x529B00 / 0x529D40, values checked against a running tru.exe (Wine, entity caches
at +0x13C). `SceneCellGroup` vtable +0x44 (0x571D20) returns it; 95 of 97 units have one (8–187 probes).

| Offset | Meaning |
|---|---|
| +0x00 | u32 region count |
| +0x04 | sphere-tree root: node {centre xyz, radius, region*, probe index (−1 inner), child*[] null-ended}; leaves = probes |
| +0x08 | region*[count]: {f32[6] [U], +0x18 settings* (record: +0x4A..+0x4C tint bytes, +0x50 SH scale, +0x54 L00 offset), +0x1C tint-table slot (0xAD7C90, 1.0 in every observed case), +0x20 u32 probes, +0x24 probe*[]} |
| probe | {pos xyz, +0x0C f32 SH[9][3] (L00, L1−1, L10, L11, L2−2, L2−1, L20, L21, L22), +0x78 u32 neighbours, +0x7C probe*[]} |

- Positions are in our unit frame (the render-terrain frame): tru.exe subtracts the group origin (+0xB0 = its world
  placement of the unit frame) before sampling.
- Sampling: nearest probe P; offsets toward P's best x neighbour (weight = projection on x / |n − P|²) and, through its
  best y neighbour, that probe's best x neighbour; the best z neighbour Q blended by t = (Q−P).z (pos−P).z / |Q−P|²
  (unclamped); then × region tint × scale, + offset on L00 (0x52ABF0). No volume: the cell group's SH (+0xC4, zero at
  runtime in l1). [I] tru.exe freezes an entity's region tint at its first sample until 0x558EA0 resets the cache; we
  use the current region.

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
- Exterior terrain follows the camera [O: tru.exe memory, 2026-11-06]: the terrain's matrix (+0xC4, per frame) is a
  pure translation eye − R, and the exterior cell group's OcclusionTree (vtable 0x84DD80) +0x40 holds R, constant while
  the camera moves (checked over 1 500 units of eye height). R is the spawn of the level load: new game into
  l2_junglehub → its player intro (−23399.229, 8455.768, 7614.127), the centre of its sky dome; into l2_cliffhall_b →
  that unit's intro (−21069.605, 16639.426, 6972.023). So the dome is seen from its centre (cliffs up to the frame top,
  hazy bottom rows at the horizon) wherever Lara walks. Ours: `World::exteriorOrigin` (enterUnit's spawn: intro,
  checkpoint or save position [I for the last two]) → `FrameOptions::exteriorOrigin` → `render::followExterior` (GL +
  Vulkan). The exterior materials are unfogged (flags 0x1001: VS c22 = 0, 0x506880); the haze is the textures' own.
  Exterior terrain casts no shadows (0x511E20 skips +0xC8 terrain).
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
| +0x86 | i16 | [U] |
| +0x88 | f32 **radius** of the environment blend (2500 in the units checked) | [O] `0x5C7CA0` (RENDERING.md "Neighbour units"; Wine-read weights) |
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
- `l2_cliffhall_b` ↔ `l2_hubhall` have no such back portal: cliffhall_b's portal to hubhall has toSignal 74 /
  signal 78, hubhall's to cliffhall_b toSignal 75 / signal 73. Signal 74 is hubhall's portal to l2_junglehub and 75
  cliffhall_b's to junglehub. cliffhall_b's portal to hubhall floats ~20 000 units outside cliffhall_b's collision, at
  junglehub's doorway to hubhall (relocated by the cliffhall_b → junglehub pair): a stale copy of junglehub's.
  `0x5D13E0` pairs by signal alone (below), which puts hubhall where the chain cliffhall_b → junglehub → hubhall puts
  it, within 3 units, from either side (`StreamReal.CliffhallBHubhallPair`). So all 158 pair.
- Signal records [O 0x5D2940]: unit data (Level+0x74) +0x98 count, +0x9C → 0x40-byte records, u16 signal id at +0x08,
  flags at +0x0B. A portal's target is loaded only when the record of its signal has bit 0 set (every record checked
  has it); bit 1 is set on hubhall 73 and cliffhall_b 78 [U]. The ids match the portals' +0x82 in the units checked.

### How tru.exe streams [O]

- `STREAM_LevelLoadAdjacentUnits` (0x5D2940) keeps the portal neighbours of the player's unit loaded.
- When a unit loads, `0x5D13E0` hooks it to the loaded units, in two steps. (1) For each portal P of the new unit N
  whose `toStreamId` is a loaded unit L (the main unit, unless N's Level+0x2C has 0x100000), it finds the portal Q of
  L with `Q.signal == P.toSignal`, whatever Q's target, and relocates N by `Q.min − P.min`. (2) For each loaded L's
  portal Q to N, the portal P of N with `P.signal == Q.toSignal`, the same offset. Not finding the signal is
  `ERROR_Fatal` ("Can't find signal%d in unit %s"). The offset goes through `0x5D10D0`, which calls RelocateUnit
  0x5C6210 ("RelocateUnit (%s, (%g, %g, %g))") and shifts terrain groups, instances, markup, sounds, etc. Only the
  first match relocates.
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
  computed exactly as `0x5D13E0` does (`game::relocation`, both steps).
- Collision: the loaded units' meshes and markup are merged into one `CollisionMesh` in the world frame: each unit's
  BVH (built on the loader thread) is appended with its offset (`CollisionMesh::appendPart`) under a top level over
  the units (`finishParts`). Hits equal those of one BVH over the
  merged mesh (`Collision.PartsMatchOneBuild`; equal-distance ties still go by traversal order, so at a seam between
  units they may pick another face than before). Ledge and water markup indices in Lara's state are remapped.
- Crossing: each tick, Lara's path at mid-body height (feet + 250) is tested against the planes of the current
  unit's portals, inside their bounds ± 50. Crossing one makes its target the current unit. Then the target's
  neighbours are loaded and every unit that is neither current nor adjacent is dropped. This replaces the signal
  faces; the signal terrain group is not decoded. A crossing that loads and drops nothing only changes the current
  unit (no rebuild).
- Background loads (`World::setAsyncStreaming`, on in `ou-game`; `--sync-loads` turns it off): a worker thread reads,
  decompresses and parses the neighbour DRMs, builds their gameplay data (`loadUnitData`), their render scene
  (textures decoded) and warms the Vulkan SPIR-V of their programs (`original::warmSpirv`). A bounded queue (8). A
  neighbour joins the world `World::kStreamTicks` (60, 1 s [I]) after it was asked for, at the start of a tick,
  waiting there if its load is late; crossing into a neighbour still loading waits for it (tru.exe blocks there
  too). The join tick depends on ticks only, so replays stay deterministic. Level loads (enterUnit: respawn, saves,
  chapters) wait for their neighbours.
- A neighbour that failed to load is retried on crossing.
- Rendering (`ou-game`): each loaded unit's level scene, moved by its offset, is re-uploaded when the unit set
  changes. Every source is keyed (`scene::assignKeys`) and the renderers keep keyed meshes and textures resident
  (docs/RENDERING.md "Resident units"); a unit decoded by the worker is sent ahead as a renderer part
  (`Renderer::uploadPart`, created over frames, 8 MB per frame) and freed when it unloads (`releasePart`). Script
  objects follow the unit set (src/script; `Runtime::setUnits` adds each unit once). Music: the unit scripts request
  their tracks (AUDIO.md §9.9).
- Main-thread cost of a stream, `pool_to_ragnarok.txt` with vsync (real-time ticks), logged as `stream world / scene
  / upload`: l4_staircase_mictlan joins at tick 164 with 4 + 2 + 2.5 ms on Vulkan and 4 + 2 + 3.3 ms on OpenGL
  (before: 12 + 12 + 55 and 12 + 12.5 + 87 ms; the first fix of the crossing: 773 ms). Vulkan builds the RT
  structures in the next frame's command buffer, without waiting for the device.

### Unit script

- [O] Level+0x70 DTP +0x1C0 = the unit script's type id (type-8 section of the unit DRM, super type 4 `NsUnit` chain):
  14/14 units checked (l3_main_bedroom 12059, l1_seafloor 5277, l2_cliffclimb 9502, ...). It has a state table;
  ours creates it with the unit and runs state 0 as its own thread (`unitref.Get` returns it), ending it when the
  unit streams out [I: created after the unit's intros].
- [O] The three units ours keeps resident for their chapter-end cinematics (l1_cine_ship_sinking,
  l3_mansion_exterior_afterburn, l6_ship_natla_room; `script::UnitRef::resident`) get their unit script at once (it
  listens) but its startup waits until the stream loads the unit: l3_mansion_exterior_afterburn's (11388) runs
  `globaleffect 11` Rain 20, which had put rain and lightning flashes on every level (tru.exe reads rain 0 at the
  l5_startsnow start). Test `ScriptRealData.ResidentUnitScriptWaitsForItsUnit`.

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

## Nav graphs (Level +0x60 -> NavPower resource)

- [O] Level +0x60 → section +0 {−1, 0, 1, size, hash, 0x10000, size}, resource at +0x1C (+0x68 → its end): u8 endian
  (0 little), u32 version 8 (AddResource 0x41C8C0 refuses others), u32 graph count, then graphs {u32, u32 layer (0..31,
  plus the unit's layer base), u32 area bytes, u32 size (next graph), f[4] params ([1] agent radius, [3] height), f[6]
  bbox, ...}; convex areas from graph +0x138 for `area bytes`: 0x38-byte header {+0x00 centre, +0x0C normal, +0x18
  radius, +0x28 FLT_MAX, +0x30 low 7 bits edge count} then 0x14-byte edges {vertex, u32 adjacent area as a graph
  offset (0 none), u32 flags 0xFFFF0000}; kd-tree and link records follow [U]. Areas float ~50 above the floor, in the
  planner nodes' frame. Surveyed: l1_ship_exterior 1 graph (layer 3, radius 100, 398 areas), l4_ballcourt 2 (layers
  4 / 3, radius 250 / 100), l2_norsehub 2 (6 / 1, 250 / 70), l2_cliffclimb 1 (1, 70), l7_yggdrasil_hall01 2 (12 / 11).
  Other units point +0x60 elsewhere (no graph). `formats::Unit::navGraphs`; use: GAMEPLAY.md "Navigation".

## Enemy intro data (intro +0x78, type 13)

- [O] tru.exe birth 0x63C7A0 reads it as instance +0x104: +0x30 → 0x698250 [U], +0x34 → a planner record of the
  unit (pointer into the +0x11C array), +0x38 → patrol path {byte +2 speed 1..4, u32 +4 count, +8 → u32 node
  indices into the record}, +0x3C byte count of alternative paths at +0x40, +0x44, ... (enemybase 22), +0x60 →
  u16 [U], +0x6C flags (bit 31: the whole record is the path when +0x38 is null; bit 27 skips the +0x60 part), +0x88
  order mode (0 loop, 1 once, 2 ping-pong, 3 random free node, 4 random unvisited). A point = record matrix × node
  translation + the unit offset, as the planner nodes (0x62F520 / 0x62F2C0 → 0x621A80, at most 16).
  `formats::Intro::patrolRecord` / `patrolPaths` / `patrolMode`; behaviour: GAMEPLAY.md "Behaviour trees, patrols
  and search". 295 type-13 intros have a record, 280 of them a path (l1_ship_exterior merc 1650: nodes 128 126 108
  114 117 131, ping-pong, speed 4); l2_norsehub's nagas 2412..2419 use bit 31.

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
  planes/volumes) [O]. Effect-only objects (oflags `0x40000000`: the 24-vertex `fx_*` / `co_fx_*` proxies, bullet hits,
  squibs) are skipped too: `cdc::InstanceDrawable` returns no draw flags for them (0x5B5CA0) [O]. Intro flag `+0x72 & 0x10`
  always hides the instance [O]: 0x5BCC30 asks the game notifier (vtable `+0x3C`); `DefaultGameNotifier` (0x54DC20)
  refuses and 0x5BCC30 sets flags `0x800` / `0x20000000`, the game's `TRGameNotifier` (installed by MainG2_InitEngine,
  0x5D4B90) accepts only for ObjectBase `+4 & 0x400000` and then hides the instance and its children itself (0x615F70).
  `loadLevelScene` skips such intros (`formats::introHidden`); all 118 live ones (generators, `natla_end_mover`, a
  trigger volume) are hidden by their oflags anyway.
- `co_monitor_frame` (oflags `0x90000014`, class 6, a 24-vertex 213-unit box with marker texture 372, live intro 143 at
  the l1_baldrs_tomb origin, dormant in l4_ballcourt) is not hidden by any data rule: tru.exe introduces 143 without
  `0x800` (Wine instance flags `0x00110000`, model 0 flag word `0x90000000`, 2026-11-07) [O]; no other ObjectBase,
  intro, model or material bit tells it apart from props (survey of every `objectlist.txt` object). We keep drawing it.
  tru.exe's `MonitorDrawable` (render-to-texture camera / mirror screen) belongs to object class 18 (0x6D1AC0,
  registered in 0x5DA9DC), which no object in the install uses [O]. Whether portal / cell visibility keeps the origin
  box out of view is [U].
- Dormant intros (`formats::dormant`): the unit loader 0x5C2360 introduces only intros with `+0x72 & 0x82 == 0`
  (0x5BCC30 sets `0x80` once it has introduced one) [O]; 839 intros of all units are dormant (all `0x80`, none `0x02`;
  one, l7_helheim01 `co_empty_tank` 8972, also has `+0x73 & 0x10`: kept after its cinematic, 0x4561C0): cinematic
  props parked at the cinematic origin (`co_*`, amanda / zip / winston / alister), enemies, trigger planes and props
  scripts bring in later. Two routes introduce one [O]: a script's `instanceref` Birth (fn 3, thunk 0x799AB0 ->
  0x4AC0D0: not dead (0x555A10), not alive (0x5B3950) -> the loaded unit holding it (0x5B38F0) -> 0x5BD080 ->
  0x5BCC30; "InstanceRef::Birth FAILED" otherwise) and a cinematic (0x4561C0, its actors; removed again at its end
  unless `+0x73 & 0x10`). Ours: `script::Runtime` keeps the dormant intros of the loaded units and creates one's object
  (constructor included) at the end of the tick its Birth runs, World spawns its enemy first (`ai::Enemies::birth`),
  `loadLevelScene` tags their meshes " @dormant" and ou-game draws them once born; born intros are saved
  (`runtime.born`) and come back with their unit. Example: l4_ballcourt plane 6546 (the way in from l4_trailtube) ->
  dispatcher 6535 vt 63 -> state 1 births the dead poachers 6540..6542, whose constructors kill them (health 1);
  volume 6551 births planes 6545 / 6547 (panther patrols), planes 6543 / 6544 dispatcher 6536's poachers and panthers.
  Cinematic actors are drawn by the cinematic player only (the persistent 8972 is not introduced) [I].
- Foliage renders correctly since materials carry blend/alpha-test/two-sided state (MATERIALS.md); the black ground
  plants came from the `COLOR0` attribute (0 on them) being used as a colour.
- The unit's dependency objects (`ca_skydome_a`, the `g_*`/`wp_*` gear) are not placed.

`loadLevelScene` warnings on these two units:

- `player` has no render model.
- `checkpoint`, `cinematic_object`, `co_cinetest_marker`, `co_light_rig*` and `portable_warp_attach` have models whose
  batches are all skipped volumes.
- No unresolved object ids or IMF DTPs.

## Open questions

- [U] Runtime model switches outside breakables and uber sections (scripts' model natives, if any).
- [U] Intro `+0x52`, `+0x70..0x73` flags, the non-enemy intro data types (1, 0x11, 0x404, ...), the second intro
  list's role (l4_pool: 1 entry).
- [U] IMF `+0x40`, `+0x4C`, `+0x60`; cell BSP and portals; cell group `+0x14`/`+0x1C`.
- [U] Why `Level+0x80` exists if intros and terrain already share a frame (collision only?).
