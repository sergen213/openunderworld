# TRU PC skeletons, skin weights and animation clips

Code: `src/anim/Anim.{hpp,cpp}` (`decodeSkeletons`, `decodeClip`, `animList`, `sample`, `worldMatrices`,
`skinPalette`, `skinVertices`), skin attributes in `src/formats/Model.cpp` (`readSkin`), viewer playback in
`src/app/viewer/main.cpp` (`--anim`, `--anim-time`, `--anim-list`). Tests: `tests/test_anim.cpp`.

Labels: **[O]** observed (game data, tru.exe decompile, or a viewed render), **[I]** inference, **[U]** unknown.
Addresses are GOG tru.exe v1.1.0.0 VAs from `decomp_all.c`. Everything was checked on `pc-w\lara.drm`; other
objects are untested unless stated.

## Where the data lives (pc-w\lara.drm) [O]

| What | Section | Notes |
|------|---------|-------|
| Object | type 11 (id 8), 140 bytes | resource relocations at +0/+4/+8 name DTP sections 850, 849, 848; +0 is the object DTP |
| Object DTP | type 7 (id 850) | +0x54 u32 anim count (1818), +0x58 pointer to the anim list |
| dtp Model | type 7 (id 2267) | resource relocation at +0x64 names the render model (type 12, id 2366) |
| Render model | type 12, resource 26 (id 2366) | the 7 skinned batches; header +12 points at the hair chains |
| Clips | 1744 × type 2 | section id = animID |

`decodeSkeletons` finds dtp Models generically: every DTPData section whose resource relocation (5th relocation
table, `{offset/4 : 25, type : 7}`) at +0x64 has section type 12. Lara's DRM has two: 2267 → 2366 (Lara) and
2268 → 2367 (an empty placeholder mesh with 0 indices).

## Skeleton

dtp Model (the layout matches DXHR's `dtp::Model` from `research/cdcEngineDXHR/cdcWorld/cdcWorldTypes.h` up to
+0x64; tru.exe 0x44cac0 reads the segment list the same way) [O]:

| off | meaning |
|-----|---------|
| +0x04 | u32 segment count (Lara: 116) |
| +0x0C | pointer to the segments, 0x40 bytes each |
| +0x64 | u32 render-model section id (resource relocation) |

Segment (0x40 bytes) [O]:

| off | meaning |
|-----|---------|
| +0x00..0x1B | 7 floats, zero on bones without vertices; [I] a bounding box. [U] exact meaning |
| +0x20 | vec4 pivot: bind offset from the parent (w = 1) |
| +0x34 | u32, `0xFFFF0000` on bones without vertices; [U] |
| +0x38 | i32 parent index (−1 for segment 0; always lower than the segment's own index) |
| +0x3C | pointer (HInfo in DXHR) on some segments; [U] |

- [O] The bind pose has **no rotations**: a bone's bind position is the sum of the pivots up its chain. This is
  how DXHR's `CalcSkeletonMatrices` builds its rest pose, and the skin matrices `world · T(−bindPos)` reproduce
  Lara's T-pose exactly (screenshot below).
- [O] Lara's hierarchy: 0 root (origin) → 1 pelvis (z 325.72) → 2 → 3, legs 4–8 (x +22.86) and 9–13 (x −22.87),
  spine 14 → 15 → 16 → 17 (head, bind z 448.1), face bones 19–51 under 17, arms 52–55 / 77–80
  (shoulder x ±9.01 under 15) with fingers 56–70 / 81–95, 102–105 under 15, 106–112 extra bones at the root or
  pelvis, 113–115 hair attach points under 17.
- [O] Sanity (gated test `AnimReal.LaraSkeletonHierarchy`): legs 4..8 / 9..13 and arms 52..55 / 77..80 mirror in X
  within 0.05 units; toes at z 3.3; pelvis at 325.72; head above 420.

### Hair chains (virtual segments 116..152)

Skin palettes reference bones up to 152, past the 116 model segments. These are hair strands described by the
render model [O]:

- Resource header +12 → `{groups*, u32 count}` (Lara: 3).
- Group, 20 bytes: `{segments*, ?*, u32 attachSegment, u16 segmentCount, u16 ?, u32 ?}`. Lara: attach 113 with
  7 segments (the ponytail, back of the head) and attach 114 / 115 with 15 segments each (front strands).
- Segment record, 48 bytes: vec4 model-space bind position at +0; +0x1C u32 = `boneIndex << 16 | flags` (flags 1 on
  the first segment); +0x20 u32 index within the strand. Bone indices run 116..152 consecutively.

[O] The positions are model space: each strand starts next to its attach bone. `decodeSkeletons` appends them as
ordinary bones (parent = attach bone, then the previous strand segment; pivot = position − parent bind position).
**ponytail:** they follow the head rigidly; the game simulates them ([I] the other group fields are simulation
parameters). In the bind pose the ponytail sticks straight back horizontally, which is how it shows in the viewer.

## Skin weights [O]

| attribute hash | format | meaning |
|----------------|--------|---------|
| 0x48E691C0 | 6 (D3DCOLOR) at +16 | 4 weights, byte / 255 |
| 0x5156D8D3 | 7 (D3DCOLOR) at +20 | 4 palette indices |

- Byte k of the weights pairs with byte k of the indices; both go through the same D3DCOLOR swizzle, so the
  pairing does not depend on it. Weights sum to exactly 255 on every vertex of Lara's 7 batches (gated test).
- Indices address the batch **bone palette**: when Mesh flags bit 0 is set, batch +0x0C = palette size and
  batch +0x10 = Mesh-relative offset of `u32` bone indices (Lara: 11, 39, 42, 42, 42, 42, 11 entries; the largest
  is 42 [I] for the D3D9 vertex-shader constant budget). All indices are inside their palette.
- Mesh header +0x74 points at a `0, 1, 2, …` table [U].
- `scene::SkinInfluence::bone` stores the palette-resolved global bone index.

## Animation clip (section type 2)

Header (tru.exe `AnimFragmentNode` decompression at 0x44cac0 and mask setup at 0x44aee0) [O]:

| off | type | meaning |
|-----|------|---------|
| 0x00..0x2F | floats | [U] (first clip: −100.6, −146.5, −3.36, 121, 20.4, 473.7, …); [I] bounds or root data |
| 0x30 | i16 | animID = the section id (all 1744 clips) |
| 0x32 | u16 | key count (the update code tests `> 1`) |
| 0x34 | i16 | time per key, in anim time units (below) |
| 0x36 | u8 | segment count (Lara clips: 113, so hair attach bones 113..115 stay at bind) |
| 0x37 | u8 | section count (1 in all 1744 clips; others are rejected) |
| 0x38 | u8 | extra-channel count (0 in 1398 clips; up to 8) |
| 0x3C | u32 | self-relative offset of the extra channels = end of the bone channel data |
| 0x40 | u32 | [U] (0 in the first clip) |
| 0x44 | u32 | self-relative offset of the bone channel data (one per section) |
| 0x44 + 4·sections | bytes | component masks |

**Component masks.** First byte: bit 0 = rotation stream present, bit 1 = scale, bit 2 = translation. Starting at
bit 8, each present stream holds 3 bits per segment (x, y, z), padded to `(segments·3 + 7) & ~7` bits. Bits are
read LSB-first from 32-bit words (tru.exe reads the stream in u32s; byte-wise LSB-first reading is equivalent on
little-endian). Lara: 1716 clips rot+trans, 26 rot+scale+trans, 2 none.

**Channel data.** For each segment, for each set mask bit in the order rot x,y,z, scale x,y,z, trans x,y,z, one
channel:

| mode (i16 at +0) | layout | size |
|------------------|--------|------|
| 0 | i16 pad, `keyCount` f32 values | 4 + 4·keys |
| 1 | i16 pad, one f32 constant | 8 |
| 2 | i16 n, then `n−1` u8 segment lengths (in keys) padded to `((n+2)>>2)·4` bytes, then f32 base and `n−1` f32 deltas | 4 + 4·((n+2)>>2) + 4·n |

Mode 2 is piece-wise linear (tru.exe 0x432a70): walk the lengths subtracting from the key; at each completed piece
add its delta to the base; inside the piece, `value = base + key_in_piece · delta / length`.

- [O] Decoding all 1744 Lara clips with these rules consumes exactly up to the 0x3C offset; `decodeClip` checks
  this.
- [O] Channels without a mask bit default to rotation 0, scale 1, translation = the segment's bind pivot. Segments
  past the clip's segment count keep the bind pose (tru.exe fills them from the pivots).
- [U] Extra channels (`count` at 0x38, data at the 0x3C offset) feed indexed vec4 parameters on the instance
  (tru.exe writes `params[id−1].component`); same channel modes. Not decoded.

### Values

- [O] **Rotation is an axis-angle vector** (direction = axis, length = angle in radians). DXHR's `QuatLogToMatrix`
  takes half the vector length as the quaternion half-angle; tru.exe interpolates the vectors linearly with a 2π
  rewind (below). Check: in clip 2 the feet land at z ≈ 0 with the pelvis lowered to 300.5, and posed bone lengths
  equal bind lengths (gated test over every 50th clip).
- [O] Translation replaces the pivot (it is the full parent-relative offset). Segment 0 (root) carries root
  motion: in `ng_run` its only channel is trans Y, −42 → −849 over the 0.67 s cycle (≈ 1200 units/s; Lara is
  511 units tall, so [I] ≈ 4 m/s). The viewer resets segment 0 to bind and plays clips in place. [U] How the
  game extracts and applies it (G2-era flags `ROOT_TRANS_PRESENT` / `ROOT_ROT_PRESENT` suggest per-clip bits).
- [I] Local matrix = `T(trans) · R(rot) · S(scale)`; world = `world[parent] · local`. The scale order is not
  confirmed (26 clips have scale channels).

### Time and sampling

- [O] Anim time unit: tru.exe feeds the anim graph `gameTracker_timeMult * 100` per update (0x435070 called from
  0x448a10), i.e. **100 units per 30 Hz frame = 3000 units/s**. Clip length = `keyCount · timePerKey`. Typical
  `timePerKey`: 100 (525 clips, 30 keys/s) and 200..211 (15 keys/s). Plausibility [O]: `ng_run` is 20 keys ×
  100 = 0.67 s per cycle, `walk_in_place` 15 × 200 = 1.0 s.
- [O] Sampling (tru.exe 0x44ea20 → 0x44cac0): `k = min(floor(t / timePerKey), keyCount−1)`,
  `frac = (t − k·timePerKey) / timePerKey`; the pose is `lerp(key[k−1], key[k], frac)`. When `k−1 < 0` a
  looping clip uses the last key, a one-shot clip uses key 0 with `frac = 1`.
- [O] Rotation interpolation: if `|(B − A) / 2π|² > 0.25`, A is moved by `2π · round(|B − A| / 2π)` along its own
  axis before the lerp (shortest way).

## Anim list and names [O]

Object DTP +0x58: `count` entries of 12 bytes `{i16 animID, i16 −1, u32 ?, char* name}`. Lara: 1818 entries
(some animIDs repeat), names such as `ng_idle` (id 6), `ng_run` (id 265), `walk_in_place` (id 723). There is no
`animlist.ids` file in the bigfile. `ou-viewer --anim-list` prints the list.

## Viewer

```
ou-viewer --game-dir DIR --drm 'pc-w\lara.drm' --anim 265 [--anim-time 0.3]   # by animID
ou-viewer ... --anim '#271'      # by list index
ou-viewer ... --anim ng_run      # by name
ou-viewer ... --anim-list        # print index, id, keys, seconds, name; no window
```

The clip loops and advances on the fixed 60 Hz tick. Each tick the viewer samples the clip, builds the skin
palette and CPU-skins every mesh linked to a skeleton (`MeshData::skeleton`), then uploads the vertices with
`Renderer::updateVertices` (GL: `glNamedBufferSubData`; Vulkan: staged copy).
`formats::loadDrmScene` decodes the skeletons into `SceneData::skeletons` and links meshes by render-model id.

## Visual checks (2026-10-18, GL and Vulkan, screenshots viewed; not committed: game-derived)

- [O] Bind pose: the skinning path with `bindPose` reproduces every stored vertex of the 7 batches within
  1e-3 units (gated test `AnimReal.LaraSceneLinksSkeletonAndBindPoseIsIdentity`), and
  `ou-viewer --drm 'pc-w\lara.drm'` shows the T-pose with face, hair and clothing intact.
- [O] `ng_run` (id 265) side view (`--cam 1000,0,260,180,0 --fov 35`) at t = 0, 0.17, 0.33, 0.5 s: four
  distinct phases of a run cycle, alternating legs with knee bend, opposite arm swing with bent elbows, torso
  leaning forward; limbs attached, no stretched triangles. The Vulkan frame at t = 0.17 s shows the same pose.
- [O] `ng_idle` (id 6) at 0.3 and 1.2 s: relaxed standing stance, arms down, feet apart.
- [O] `ng_running_land_stumble` (id 260) at 0.4 s: bent-over landing pose; `Combat_walkfront` (id 13) at
  0.5 s: upright walking stride.
- Known deviations: the ponytail and front strands are rigid (no hair simulation); clips play in place
  (segment 0 root motion reset), so jumps and landings float above the ground. No side-by-side comparison
  with the running game has been made.

## Open questions

- [U] Clip header floats 0x00..0x2F and the u32 at 0x40.
- [U] Extra channels; multi-section clips (none in Lara's DRM).
- [U] Scale composition order.
- [U] Root-motion extraction (which segment, and how the game applies it).
- [U] Hair simulation parameters (group fields, segment fields other than the position).
- [U] Segment bounding data (+0x00..0x1B) and +0x34.
