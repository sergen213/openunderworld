# TRU level collision and markup

Code: `src/collision/` (`ou::collision::loadLevelCollision(const cdc::Drm&)`, `CollisionMesh` with our own BVH and
queries). Viewer: `ou-viewer --game-dir … --drm pc-w\l4_pool.drm --show-collision`.
Tests: `tests/test_collision.cpp` (synthetic BVH/query tests; `CollisionRealData.*` with `OU_GAME_DIR`).

Labels: **[O]** observed (game data, tru.exe decompile), **[I]** inference, **[U]** unknown.
tru.exe addresses are GOG v1.1.0.0 VAs from our Ghidra export (`decomp_all.c`).

## Where it lives

Unit (level) DRMs only. Section 0 is a general section holding the `Level` struct [O]. Pointers are DRM
relocations; most of the collision chain uses **external** relocations (pointer into another section):
`{u32 lo, u32 hi}`, target section = `lo & 0x3FFF`, patch offset = `((lo >> 14) | (hi & 0x3F) << 18) * 4`,
target offset = `hi >> 6` [O] (relocation loop at 0x54EC60; `cdc::sectionPointers`).

```
Level (section 0)
 +0x00 Terrain*            -> 24-byte section
 +0x08 u32 markup count    \ "markup items" (0x5C bytes each, FUN_005CE4F0): count is 0 in all 82 units [O]
 +0x0C MarkUp*             /
 +0x74 unit data*          -> DTPDATA section; +0x2C u32 markup2 count, +0x30 markup2 item pointers [O]
 +0x80 float3 levelOffset  (see "Frames")
Terrain (TR8)                                  [O], layout as TRLAU-menu-hook Level.h TR8 branch
 +0x02 u16 numPortals, +0x04 StreamUnitPortal*
 +0x08 u32 numTerrainGroups, +0x0C TerrainGroup* (0x50 bytes each)
 +0x10 TerrainGroup* signalTerrainGroup        (portal/signal quads; not collision, skipped)
TerrainGroup (0x50)
 +0x00 float3 globalOffset (== Mesh+0x20)  +0x24 ID  +0x30 TerrainGroupExtra* (u32 flags)  +0x34 Level*
 +0x38 Mesh* (null for groups without collision)
```

## Collision mesh (cdc `Mesh`, TR8 layout, 0x60 bytes) [O]

| off | type | meaning |
|-----|------|---------|
| 0x00 | 2×float4 | box min/max, local (relative to +0x20) |
| 0x20 | float4 | position: world = vertex + position (+ levelOffset) |
| 0x30 | ptr | vertices |
| 0x34 | ptr | faces |
| 0x38 | ptr | BIH root (node array) |
| 0x3C | ptr | client data = owning TerrainGroup |
| 0x40 | u32 ×5 | numNodes, numFaces, numVertices, numDegenerateFaces, numNonManifoldEdges |
| 0x54 | u16 | vertex type: 1 = float4 (x, y, z, w); 0 (int16 xyz in TRL/TRA) never occurs in TRU |
| 0x56 | u16 | tree height (max 27 over all units) |

- Vertices: 16-byte float4, w = 0 [O]. All 361 meshes in the 82 units are type 1 [O].
- Face, 20-byte stride [O]: `u32 v0, v1, v2; u8 flags (+0x0C); u8 mask (+0x0D); u16 clientFlags (+0x0E); u8 material (+0x10); pad`.
  Indices are u32 (largest mesh: 71 705 faces, 40 590 vertices).
  - tru.exe face queries (0x462E10 box query, 0x462FA0 ray query) skip a face when `flags & 0x40` or
    `(mask & queryMask) == 0`; callers pass query masks 0x0F, 0xFF or a parameter [O]. `queryable()` mirrors this.
  - l4_pool: mask = 3 on every terrain face, 0xFF on the signal quad; flags low bits vary (0x20..0x3F, plus 0x80) [O];
    clientFlags ∈ {0, 0x04, 0x80, 0xA0, 0x1A0}, material ∈ {1, 17, 27} [O]. Their meanings are [U] (TRL calls the
    fields adjacencyFlags/collisionFlags/clientFlags/materialType; adjacency for +0x0C is [I] at best since bit 0x40 is a skip flag).
  - Face and BIH sections are 3 resp. 2 bytes shorter than count × stride: the last element is stored without
    its padding [O]. Hence material is read as u8 and node bytes 0x0E–0x0F are padding [I].
- Winding: floor faces whose CCW normal points up match the render floors (see agreement below) much better than
  down-facing ones (83 % vs 47 % within 16 units in l4_pool), so [I] CCW = outward/open side. Queries are double-sided.

### Game BIH (bounding interval hierarchy) [O]

16-byte nodes, depth-first, root = node 0:

| | +0x00 | +0x04 | +0x08 | +0x0C | +0x0D |
|---|---|---|---|---|---|
| inner | f32 left max | f32 right min | u32 right child = this + value | u8 axis 0/1/2 | u8 0 |
| leaf | ? | ? | u32 first face | — | u8 face count (≠ 0) |

Left child = this + 1. The planes carry a ±1 unit margin over the child face bounds [O]. Traversal (0x462E10):
go left if `queryMin[axis] <= leftMax`, right if `rightMin < queryMax[axis]`. Checked on l4_pool mesh 0: every node
visited once, every face in exactly one leaf. The loader only verifies the leaf counts sum to numFaces; we build
our own BVH instead of using it.

## Frames [O]

- Collision vertices are stored in a frame offset from the render terrain: **render frame = vertex + Mesh.position + Level+0x80**.
  l4_pool: Level+0x80 = (−4003.8, −158.0, 6637.05). Found by 1-D histogram cross-correlation of collision vs render
  vertex coordinates per axis (peaks at −4004, ~−160..−280, 6637), then an ICP fit converged to the same value
  (−4003.77, −160.67, 6637.05).
- Markup2 points, intros (LEVELS.md) and render terrain share the render frame; they need no offset.
- [U] What tru.exe does with Level+0x80 (not traced) and how the render frame relates to the streaming world.

## Markup ("markup2") [O]

Unit data (Level+0x74) `+0x2C` count, `+0x30` pointer array of 0xB0-byte items, added by FUN_005C9DA0 → FUN_005BE650:

| off | meaning |
|-----|---------|
| 0x00 | 4×4 matrix, rows = basis, row 3 = translation; identity in l4_pool, a half turn + translation on l4_traprooms' poles [O]. Points are transformed by it (0x49BA20); row-vector convention confirmed by the poles landing on their bars [O] |
| 0x48 | u16 id (copied to MarkUp+0x2E) |
| 0x4A | u16 (copied to MarkUp+0x2C) |
| 0x4C | u8 kind (0 → flags from type; 1 → +0x98 byte copied) |
| 0x4D | u8 type |
| 0x50 | u8; bit 2 → MarkUp+0 |
| 0x98 | ptr → byte (equals type in l4_pool) |
| 0x9C | u32 point count |
| 0xA0 | ptr → points, 32 bytes each: float4 position (w = 1), float4 (0, 0, 1000, 0) in every point seen [U] |

Type → MUD flags = `switch 0x5D6F90 | switch 0x5D70A0` [O] (names from the TR8 MUD flag bits in TRLAU-menu-hook):

| type | flag | name | | type | flag | name |
|---|---|---|---|---|---|---|
| 1 | 0x40000000 | ledgegrab | | 11 | 0x80000 | rope |
| 2 | 0x20000000 | jumplanding | | 12 | 0x40000 | cover |
| 3 | 0x10000000 | enemydropdown | | 13 | 0x20000 | perch |
| 4 | 0x8000000 | pole | | 14 | 0x10000 | balancebeam |
| 5 | 0x4000000 | vertpole | | 15 | 0x10 | pillar |
| 6 | 0x2000000 | horizpole | | 16–20 | 0x4000…0x400, plus 0x8000 | 0x8000 = CONSTRAINEDGROUND (TRLAU-menu-hook TR8 names); the sub-bits are unnamed [U] |
| 7 | 0x1000000 | zipline | | 10 | 0x100000 | water (volume path: no polyline bit) |
| 8 | 0x800000 | railslide | | other | 0x80000000 | disabled |
| 9 | 0x400000 | ladder | | | | |

Types 1–9 and 11–15 also get bit 0 (POLYLINE); FUN_005BE650 then ORs 0x8, and 0x4 when the polyline has segments. [I] that
the two switches are the MarkupManager virtuals at vt+0x1C/+0x20 called there (adjacent code, matching the observed types).

l4_pool: 73 items: 61 ledgegrab, 10 balancebeam, 2 jumplanding [O]. All ledge points lie within 16 units of collision
geometry (`CollisionRealData` test), and in the `--show-collision` screenshot the yellow ledge lines run exactly along
the edges of the protruding ledge stones [O].

Markup types in all units (2026-10-18, throwaway survey over the markup2 item type bytes) [O]: 1 227 ledgegrab, 14
jumplanding, 25 pole (4), 0 vertpole, 3 horizpole, 2 railslide, 7 ladder, 34 perch, 94 balancebeam, 29 pillar, 74 / 82 /
55 / 93 / 34 of types 16–20, 6 of type 0; no zipline, rope or cover items. Units with traversal markup:

| type | units (count) |
|---|---|
| 2 jumplanding | l2_maproom 5, l7_helheim01 3, l4_pool 2, l2_junglehub 2, l2_cliffclimb 1, l5_valaskjalf 1 |
| 4 pole | l4_traprooms 15, l5_valaskjalf 6, l2_junglehub 2, l3_main_hall 1, l4_temple 1 |
| 6 horizpole | l2_upperhallway 2, l2_junglehub 1 |
| 9 ladder | l7_arctic_sea 4, l1_ship_sinking_hall 3 |
| 14 balancebeam | l3_chapel 13, l2_hubhall 10, l4_pool 10, l5_valaskjalf 8, l2_junglehub 6, l2_norsehall 6, l7_arctic_sea 6, l2_towerconnector 6, l2_towerroom 5, l4_temple 5, + 10 units with 1–3 |
| 16–20 | l2_norsehub, l7_yggdrasil_end, l2_cliffclimb, l2_puzzleroom, l4_traprooms, l1_baldrsstairs_a, l1_ship_sinking_hall, l4_temple |

The item matrix is not always identity: l4_traprooms' poles carry a translation and a half turn about Z (pole 1336:
points (11061.8, 632.1, 777.5)… with translation (25284.1, −20223.3, 6355.0) → (14222, −20855, 7133)); after the
transform every pole axis lies 11 units under the collision surface of its bar [O], which confirms the row-vector
convention. Balance beam lines lie on the beam's collision top (±0) [O]. l4_pool's ledge 1253 runs up to ~180 units
inside the collision top of its rock [O].

The 0x5C-byte "markup items" at Level+0x08/+0x0C (FUN_005CE4F0; MarkUp+0x58 → PolyLine `{int n; pad; float4 points[n]}`)
are not decoded: the count is 0 in all 82 units [O]. Object DRMs pass their own markup lists (FUN_005D0610) [O]; not decoded.

## Our implementation

- `loadLevelCollision` merges all terrain-group meshes (signal group skipped) into one `CollisionMesh` in the render
  frame, `Face::group` = terrain group index. Groups other than 0 in l4_pool are small meshes with TerrainGroupExtra
  flags (0xE005E700 …); several do not sit on render terrain [O] (median 150–1370 units off), [I] doors/platforms
  driven by instances. They are kept; filter by `group` if needed.
- BVH: median split on the longest centroid axis, ≤ 4 faces per leaf, children allocated in pairs.
- Queries: `raycast` (Möller–Trumbore, double-sided, normal faces the origin; rays within ~1e-5 rad of a face's plane
  miss it), `sweepSphere`/`sweepCapsule`
  (conservative advancement on exact segment–triangle distance; convex distance under translation ⇒ Newton step
  never overshoots; 1e-3 unit contact tolerance), `closestPoint`, `groundProbe` (ray −Z). All take the tru.exe query mask.

## Verification

`--show-collision` screenshots of l4_pool (GL; kept privately, not committed): red collision wireframe lies on the
render terrain edges (pillars, floor slabs, walls); yellow ledge markup on ledge edges [O].

Ground agreement, l4_pool (`CollisionRealData.PoolCollisionMarkupAndRenderAgreement`, 2026-10-18) [O]: 38 628 vertical
probes on a 100-unit grid, stepping down through every collision surface; 51 401 floor crossings (CCW normal z > 0.7);
88.6 % have a render-terrain surface within ±64 units on the same vertical; of those |Δz| median 8.4, p90 11.9,
p99 54.8; 9.1 % within 1, 19.4 % within 4, 92.3 % within 16 units. The offset is not a frame error: per height level
the main floors differ by −10/−11 (pool floor: collision −3477, render −3487/−3488), −8 (2844 vs 2836), −4 (−316 vs −320)
and 0 (−2212 vs −2212); [I] collision floors are flat planes on top of the render floor relief. Top-down first hits are
not comparable (render roofs are thick shells at ~11 000, collision has lids at ~9 000).

Survey of all 82 unit DRMs (throwaway tool over `loadLevelCollision` + `formats::loadLevelScene` render geometry,
200-unit grid, 2026-10-18) [O]:

- All 82 load with no warning or exception (leaf counts match numFaces everywhere): 875 749 faces, 1 779 markup
  polylines. `l1_cine_ship_sinking` has no collision; `l1_seatop` has a single 2-face plane at z = −548 304
  ([I] a kill/fallback plane).
- Per-unit median |Δz| of matched floor crossings: min 0.0, median 3.2, max 35.0. Units whose signed Δz histogram
  was inspected peak at exactly 0 (l4_pool levels at −2212, l2_maproom, l2_cliffclimb), the spread being render relief
  below flat collision; in l5_startsnow the render surface lies 24–58 units above collision ([I] snow Lara sinks into).
- Floors without render geometry within ±64 are common outdoors (l1_ship_exterior 1.9 %, l7_himalaya_end 27.6 %
  matched): [U] water surfaces and other render geometry we do not decode (skydome, water, object collision).

## Open questions

- [U] Meaning of face flags (+0x0C other than 0x40), clientFlags, material; TerrainGroupExtra flags.
- [U] Level+0x80 in tru.exe; the second float4 of markup points; markup types 16–20.
- Face flags byte (+0x0C) [O]: in l2_cliffclimb the walls carrying markup types 16-20 have flags with bits
  0x01..0x20 set (0x3f/0x3b/0x3d/0x3e/0x37/0x2b/0x1b/0x29 etc.); ordinary walls/floors have 0. We treat
  those faces as free-climbable rock (`collision::climbable`, GAMEPLAY.md "Free climbing"); the meaning of
  each direction bit is [U].
- [U] Moving terrain groups: which instance drives them, and their runtime transforms.
- [U] Object-DRM markup and per-instance collision (not decoded).
