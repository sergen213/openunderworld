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
| +0x74 | unit data (DTPDATA section) | [O] `FUN_005B3860` reads intros from `Level+0x74` |
| +0x80 | float3 offset between the collision and render frames | [O] reported by the Collision agent. Intros do **not** need it (see below) |
| +0x90 | cell group data (general section) | [O] reloc; field layout matches DXHR `CellGroupData`, reordered |
| +0x94… | stream-unit portals (`tolevelname[128]` …) | [O] e.g. "l4_ragnarok_room" in l4_pool |

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

### Cell group data (Level+0x90)

| Offset | Meaning | Evidence |
|---|---|---|
| +0x00 | header* `{numTotalCells, numToplevelCells, numBSPNodes, numStreamGroups (+0x0C), …, numImfs (+0x1C)}` | [O] counts match the arrays: ballcourt 17/17/428/10/…/3293 |
| +0x04 | BSP nodes* (planes with child links) | [O] shape only |
| +0x08 | stream groups*, 20 bytes `{name*, numCells, cells*, streamFileName*, resolveObject}` | [O] same as DXHR `CellStreamGroupData` |
| +0x0C | symbol table* (offsets + strings "unitname", "portal_cell1"…) | [O] |
| +0x10 | cells*[numTotalCells] → `CellData {header*, terrainData*, …, renderMesh* @+0x20}` | [O] |
| +0x18 | IMF records*, 0x70 bytes each | [O] |
| +0x1C | resource ref to a DTP | [O]; content [U] |

- Stream groups [O]: `streamFileName` is null for some groups ("default", "group_ballcourt_edges"). The others name
  `pc-w\streamgroups\<file>.drm` (`LOAD_StreamGroupFileName` 0x477990). Ballcourt: 10 groups, 8 DRMs. All 8 exist and
  hold the unit's terrain (res 24). The ballcourt unit DRM itself has no terrain. l4_pool has 0 stream groups, and its
  terrain is in the unit DRM.
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
