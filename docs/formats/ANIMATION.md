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
- Known deviations (viewer only; ou-game crossfades and simulates hair, below): the ponytail and front strands are rigid; clips play in place
  (segment 0 root motion reset), so jumps and landings float above the ground. No side-by-side comparison
  with the running game has been made.

## Open questions

- [U] Clip header floats 0x00..0x2F and the u32 at 0x40.
- [U] Extra channels; multi-section clips (none in Lara's DRM).
- [U] Scale composition order.
- [U] Root-motion extraction (which segment, and how the game applies it).
- [U] Hair: group u32 at +16 (7, 5, 5), segment +0x10/+0x14/+0x18 on roots; parameter fields [5], [6], [8]; which cloth mode is active when (mode 1 is [I] underwater).
- [U] Pose-blend space of the original (axis-angle lerp vs quaternion); per-request fields +0x16..+0x18, +0x1B..+0x20 beyond those named below.
- [U] Segment bounding data (+0x00..0x1B) and +0x34.

## Transitions (code: `src/anim/Blend.cpp`, `game::clipBlend` in `src/game/World.cpp`)

**Lara has no anim state graph.** Object DTP +0x6C/+0x70 (tru.exe 0x453100 reads `hasAnimGraph` / graph at
code offsets +0x64/+0x68 of the object data) are 0 in lara.drm; a scan for dtp `AnimStateGraph` layouts (tru.exe
0x452E70: +4 states, +8 state*, +0xC edges, +0x10 edge* of 0x30 bytes, edge +0x18 = blend frames) finds none
[O]. Her clips are played through **anim requests** [O]:

| Where | Layout |
|---|---|
| object DTP +0x5C / +0x60 | count (1502) / table of 0x24-byte requests |
| request +0x0C, +0x10 | `{u16 anim-list index, u8 primary}` × count (multi-clip = blend space, e.g. ng_run with lean/stairs variants) |
| +0x16 | play mode: 0 hold, 1 once, 2 loop (tru.exe 0x4428A0) |
| +0x19 | **crossfade length in 30 Hz frames**: weight step `1/frames` per frame (0x4428A0 `param_1[0x2A] = 1/frames`; 0 = cut) |
| +0x1A | **start frame**: start time `frame·100` anim units passed to 0x441FB0 (if `blend > 0 && timePerKey > 100 && start < timePerKey`: `start = timePerKey`, blend extended — not ported) |
| +0x1B | interp override for the *next* request (−1 none) |

Interp frame histogram over the 1502 requests: 3 (365), 1 (275), 4 (195), 8 (176), 5 (157), 6 (135) … [O]. Other
clips of one request are time-synced to the first by normalised phase (0x4428A0: `t_i = t_0/len_0 · len_i`) [O].
The generic interp node (`cdc::AnimInterpNode` 0x4490F0/0x44B020) also fades linearly and weights its inputs
`(1−w)` / `w` [O]. Several requests name each clip; `clipBlend` picks the locomotion one [I], e.g. ng_idle 4/4,
ng_walk 4/1, ng_run 4/4 (2/14 after ng_run_start, request 730), ng_running_stop 4/4, ng_standing_jump 6/6,
ledgeb_idle 8/4, swim_climbout 12/6 (blend/start frames; full table in World.cpp). `ng_turn_center` has no request:
default 3/0. ledgeb_climbup_fast's start frame is held at 0 until the climb root-motion alignment lands [I].

Ours: `AnimController::play(clip, blendSeconds, loop, rate, start)` pushes a layer whose weight rises linearly over
`blendSeconds`; older layers keep playing until a newer one reaches weight 1 (max 4 layers). Pose blend: per-bone
quaternion slerp of the axis-angle rotations (shortest arc), lerp of scale/translation [I: the original's blend
space is not decoded]. `World` watches `PlayerState` (clip/loop/rate change or restart) and fades with
`clipBlend(from, to).blendSeconds`; `Player::play` starts at `startSeconds`, except loop→loop swaps inside
{ng_walk, ng_run} and {swim_sub, _up, _down}, which keep the stride phase (`anim::syncedStartTime`; ng_walk and
ng_run have the left/right foot offset in phase at phase 0, cross-correlation peak at offset 0.99 ≈ 0) [O]. Root
motion still comes from the newest clip only (the original also blends root deltas, 0x44B020) [deviation].
`World::pose(alpha)` interpolates per-bone matrices between ticks (slerp + lerp).

**Measured pops** (`GameReal.TransitionsReducePops`, flat floor, 560-tick replay: run, run→walk→run, stop, walk,
standing jump, run, running jump; per-tick change of a bone's model-space orientation; at each clip change raw =
jump on the switch tick rendering only the player's clip, blended = largest jump over the fade window) [O]:

| | all skinned body bones | major bones (pelvis, spine, head, legs, arms) |
|---|---|---|
| worst transition, raw clip switch | 179.6° | 118.9° |
| worst transition, crossfaded | 71.0° | 35.3° |

e.g. run→stop 165.8/118.9° → 23.0/17.0°, walk→run 175.1/115.9° → 31.4/20.3°, idle→walk 67.1° → 9.6°. The remaining
blended maxima are fingers/helper bones whose two poses differ by ~180° and short (2-frame) fades; some clips
also jump at their own loop seam (ng_run bone 73: 53.6° both ways).

## Hair (code: `src/anim/Hair.cpp`, `anim::decodeHair`; Lara's params in `laraHair`, World.cpp)

Render-model chain groups (above) are **cloth** [O]: group +4 → `{f32 rest, u16 a, u16 b}` distance constraints,
count = group u16 at +14 (6, 36, 36; rest = bind distance within 0.01); segment +0x1C bit 0 = pinned (7 roots:
ponytail 1, front groups 3 strands each); +0x20 = index along the strand. tru.exe simulates them in
`JobClothSimulate` (0x460200 → 0x4601C0, substeps → 0x45FAB0): particles {pos, vel, target, invMass}, velocity
damping, gravity on z, sequential-impulse distance constraints on velocities, collision shapes (sphere/capsule
kinds 0/1), integrate, position correction passes, optional drive toward the animated target (`param[0x19]`) [O].
Parameter sets (13 × 4 bytes, applied by the function at decomp line ~73478: `[0]` gravity m/s² → `−g·100·3.1605/30²`
units/frame², `[1]` → +0x6C, `[2..5]` ints, `[6]`, `[7]` target drive, `[8]`, `[9]` count + `[10]` group byte
list) live in lara.drm object DTP 0x27420..0x274EC, selected by a 2-mode table at 0x273F0 [O]:

| mode | set | groups | gravity m/s² | +0x6C (damping [I]) | ints [2..5] | [6] | drive [7] |
|---|---|---|---|---|---|---|---|
| 0 | 0x27420 | 0 (ponytail) | 12 | 0.1 | 4, 2, 5, 3 | 0.5 | 0 |
| 0 | 0x274BC | 1, 2 (front) | 12 | 0.25 | 4, 2, 5, 3 | 0.5 | 0 |
| 1 | 0x27454 | 0 | 0.1 | 0.25 | 2, 6, 5, 1 | 1.0 | 0 |
| 1 | 0x27488 | 1, 2 | 9.8 | 0.25 | 4, 2, 5, 3 | 0.5 | 0 |

[I] ints = velocity iterations, position iterations, ?, substep factor; mode 1 (near-zero ponytail gravity) is
underwater. Ours: position-based Verlet in world space on the 60 Hz tick (deterministic), the same particles,
pinned roots and constraints, mode 0 gravity (3793 units/s²) and damping (per 30 Hz frame [I] → per tick
`1−(1−d)^0.5`), drive 0, 16 projection iterations, two body capsules (spine 14→16, head 17; radii [I], clamped so
no bind particle starts inside), plus `follow` 0.8 [I, ours]: 80 % of the animated motion is carried without
inertia so 6–8-unit strand segments do not stretch under Lara's 1274 units/s root motion. Bone orientation =
animated orientation swung onto the simulated segment (to the child; tips: from the predecessor).
Checks [O]: settles under gravity, finite under teleports/spins, deterministic (`Anim.HairChain…`); in the replay
the largest constraint strain is 14 %. Screenshots (not committed): at rest the ponytail hangs down the back;
after a stop it swings out behind the head; rising in a standing jump it trails downward.
