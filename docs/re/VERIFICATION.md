# Instruction-level verification of tru.exe claims (protected systems)

Scope: tru.exe addresses that src/ comments and docs cite for original-faithful gameplay numbers (water waves,
entry, buoyancy and breath; timing and the physics step; damping, gravity and mass; slopes and slides; fall damage;
jump, ledge and wall-jump constants; swimming; camera; animation blend and root motion; push/pull; difficulty).
Candidates were enumerated from `docs/re/db/citations.tsv` and read in the citing file. Each claim was checked
against `objdump -d -M intel` of the GOG `tru.exe` (read-only). Function bounds come from `docs/re/db/functions.tsv`.
`.rdata` constants were decoded from the file. The decompiler was not used as proof.

Data: [`db/verification.tsv`](db/verification.tsv). It has one row per claim. Columns: `id`, `address`, `function`
(the containing Ghidra entry; `-` when the address is outside every table function), `system`, `claim`, `source`
(path:line), `evidence` (the instruction addresses and constants that settle the claim; not pasted bulk), `verdict`,
`confidence`, `ou_code`, `note`, `origin`. `V…` rows are new. `P…` rows point to the earlier checks in
[FUNCTION_DATABASE.md](FUNCTION_DATABASE.md) ("Verification sample") so that this file covers every check.

Verdicts:
- `match`: the instructions show what the claim says.
- `mismatch`: the instructions contradict the claim or the OpenUnderworld behaviour built on it.
- `partially` / `not-found-in-function` (first pass only, all settled 2026-11-20): the core held but part of the claim
  sat elsewhere, or the cited function did not contain the instructions. Each such row now carries `match` or
  `mismatch` and a "Settled 2026-11-20 (batch 10)" note with the instruction addresses that decided it.

Values that live in lara.drm or other data (player-data shorts and floats, chart properties, camera settings) cannot
be checked in the exe. For these the row checks the read (offset, scale, comparison) and says the value is data.

## Counts

| | match | mismatch | total |
|---|---|---|---|
| First pass (V001–V074) | 70 | 4 (V002, V042 resolved 2026-11-16; V020, V056 open) | 74 |
| Batch 6-9 citations (V075–V097, 2026-11-20) | 23 | 0 | 23 |
| Earlier, by reference (P001–P023) | 23 | 0 | 23 |

Before the batch 10 pass the first-pass rows stood at 51 match, 17 partially, 4 not found, 2 mismatch; P023
(0x746F60) was open.

## Batch 10 (2026-11-20): settled rows and batch 6-9 citations

Same method (objdump of the GOG tru.exe, `docs/re/db/call_edges.tsv` for call structure; no decompiler as proof).

Settled:
- **Found where the first pass looked in vain.** V065 / P023: 0x746F60 reads player data `+0x9C8` / `+0x9CC` at
  0x7473FD / 0x747443 through a base `pd + 0x888` (0x746FD1), which a displacement search cannot see. V032: the
  0.5 s blend is settings `+0x78`, passed to 0x608200 by 0x5FEE10 (0x5FEE6D / 0x5FEE81, `edi` = resource + 0x10),
  which 0x601D30 calls first. V063: the `+0x82C` / `+0x838` reads sit in 0x7591E0 / 0x758B00, both called only by
  0x7594C0.
- **Data, now with the read pinned.** V005 (the fifth class 0x80 is empty because `+0xAE` = `+0xB0` = 391), V011
  (slot 10 from the 16-byte handle stride at `[Lara+0x500]` + 4 [I]), V027, V041 (record fields +0x34 / +0x54 /
  +0x58), V058, V036 (632.1 is credited to our probe, not to 0x7396E0).
- **Extra code that does not touch the claim.** V017 (clock wrap), V045 (OU's `ledgeClear` already has the 31.605
  column), V051, V054 (the rest of 0x74FDF0 builds an attach message), V057 (return codes 0 / 1 / 3 / 2 map onto
  pushWay), V069 (0x5D6170 is the relic factor, which OU's maximum already includes).
- **Citation address wrong, behaviour right** (BUGS OU-DOC-003): V048 (0x441FB0, not 0x4428A0), V049 (0x7228E0,
  not 0x622418), V050 (0x5DA230, not 0x5DA1C0), V024 (the comment calls 1.1538 a step; it is a lag threshold).
- **Mismatch.** V020: no instruction path carries the template's 0.01 / 0.01 (`+0x100` / `+0x104`) to the
  integrator's `+0x154` / `+0x158`; in the pooled objects those are list links (BUGS OU-PHY-003). V056: MustDetach
  also releases on player data `+0xCB4` (distance) and three angles `+0xCA8` / `+0xCAC` / `+0xCB0` that OU does not
  model (BUGS OU-PLY-016).

New rows V075–V097 cover the tru.exe addresses batches 6-9 cited (docs/audit/PROGRESS.md, BUGS.md): camera triggers
(0x5CD520, 0x601D30, 0x6019D0 with its jump table 0x601CC0, 0x5E85B0 / 0x5F2A70, 0x609F10, 0x607A30, the cut
0x6017D0, focus points 0x5F0B30 / 0x5F12F0, the look blend 0x6009C0, the trackers 0x6022F0 / 0x609830), the
SoundSequence latent (0x4AF230 / 0x4AF290 / 0x4ACC30 / 0x57E970 / 0x57E016), dormant intros 0x5C2360, CloseCombat
slots 0x6108A0 (0x627F00 / 0x63A4C0), the aquatic reach (0x6391F0 / 0x6298C0 → 0x660640 → 0x64A5B0), VM locals
0x57A850 → 0x579770, the one-sided ray 0x461640, the switch stand point 0x704730 → 0x6F4740, the flinch action
0x760530, the grapple point 0x6FF040, rope bend bodies 0x6A4D40 → 0x699A50 / 0x699960, the above-surface under-water
gate 0x6D9DA0 / 0x5EA200 / 0x431CA0, the fling solver 0x73EDC0 and the shelf test 0x74E290 → 0x74E160 → 0x74BB50.
All 23 match on the cited claim. Branches the claims do not mention, recorded in each row's note and in BUGS:
- 0x73EDC0 leads a moving markup (target + t × its velocity) and caps the speed (query `+0xD4` else player data
  `+0xC10`); `jumpVelocity` does neither (OU-PLY-017).
- 0x702E90 replaces the fall-damage thresholds by the active jump query's `+0xC4` triple (OU-PLY-018).
- 0x6FF040 lets queries 0xDE / 0x159 / 0x158 replace the instance + 0xDB point; 0x601D30 applies one more record
  (trigger list vt+0x14) after the priority triggers; the active trigger list holds at most 4 (0x5E85B9); the
  0x5CD520 → 0x5F29A0 / 0x5F2A70 calls go through CameraManager vtable slots 27 / 28; 0x6391F0 reaches 0x660640
  through 0x60E4C0 (all OU-DOC-003).

The first-pass sections below are kept as written; the rows' notes carry the settlement.

## Mismatches (bug candidates)

1. **V002, 0x5DCE20, slope-slide band.** Cited at src/game/Player.cpp:385 and Game.hpp:219. The ground check reads
   only player data `+0x100E` (52°), at 0x5DD3BA, 0x5DD4F4 and 0x5DD578, each as `cos(deg × π/180)` via 0x813420. Its
   slide branch, 0x5DD3E4..0x5DD405, sets Lara +0x11CF for a slide-flagged face with `cos 52° < nz < 1`. Nothing in
   0x5DCE20 reads `+0x100C` (28°); that value is read by the slide controller 0x73FF50 (0x740036, beside +0x100E at
   0x740049) and by 0x71B550 / 0x720170. `Player::floorKind` (Player.cpp:394) requires `nz <= cos 28°`, so OU treats a
   slide-flagged face flatter than 28° as standable, where the original's ground check flags it as a slide. Medium
   confidence: the operand at `[esp+0x58]` is taken to be the face normal z from its use.
2. **V042, 0x6FC6B0, ledge wall-side test (MUD 0x40000000).** The original compares a 2-D dot (0x4898E0 = x·x + y·y,
   no z) with 0.261799 (0x855314). It loosens the limit to 0.523599 (0x85DBF4) when the length of the velocity at
   `[edi+0x80]` exceeds 3.1605 per frame (0x6FCAD2..0x6FCAE4). `wallSide` (Player.cpp:1526-1530) always uses
   0.2617994, so fast arcs are refused that the original accepts. Which object `+0x80` belongs to (the query state or
   the markup) is not resolved.

   **Resolved 2026-11-16 (agent/SlideLedge).** V002 was a false positive: 0x5DCE20 does apply the 28° bound, through
   its query record. The per-frame caller 0x71B910 loads Lara +0x644 (0x71BA19) and passes it as the 7th argument of
   0x5C0CA0 (0x71BB6D..0x71BB9B), which stores it as record word 3. The record is 0x5DCE20's second argument (`edi`),
   and `[edi+0xC]` is compared with the normal z at 0x5DD3A2, 0x5DD45F, 0x5DD4E8 and 0x5DD56C (slide needs
   `cos 52° <= nz <= [edi+0xC]`, `nz < 1`). +0x644 = cos(player data +0x100C) is written at 0x71B580 (spawn) and
   0x7201A0 (a message handler; 0x72015D sets it from a message value / 4096). The controller's 0x740036 read is only
   the seed of its angle range (props 0x192 / 0x193). `floorKind` matches; test rows added, no behaviour change.

   **V042 resolved 2026-11-16 (agent/SlideLedge).** `edi` is the ledge candidate record (0xE0 bytes, built by 0x6F6C50
   / 0x6F6DB0 in 0x70F300). Its +0x80 is the velocity of the markup's owner at the attach point (0x6EC930: zero for
   static markup, else 0x4D8F70 on a physics body or 0x5D75B0 = displacement between the segment's last and current
   matrix / timeMult, per 30 Hz frame). So the loosening applies to moving ledges, not to Lara's arc speed. Fixed:
   `Markup::velocity`, filled by `World::tickObjectMarkup`; threshold 3.1605 × 30 units/s. [U] the units of the
   physics-body path 0x4D8F70.

Citation error, behaviour correct:

- **V049, 0x622418.** Cited at World.cpp:580, Combat.hpp:376, SCRIPTS.md:435 and GAMEPLAY.md:4185. 0x622418 is not an
  instruction start: it falls inside `movaps` at 0x622415, in segment-matrix code of 0x622390. The divide by the world
  time scale is in 0x7228E0: `timeMult [0xE7F188] × (1 / [0xE7F1A0])` → `[0xE7F188]` (0x7228E9..0x72290D). The cited
  address should be 0x7228E0.
- **V074, Combat.hpp:31-33.** The comment says there is "[I: no exe reader of either]" for difficulty-table `+0x08`
  and `+0x14`. Both have readers: `+0x08` in 0x6E7960 (max health) and inline at 0x702F5E (fall-damage maximum);
  `+0x14` at 0x75E600 (saving-grab timer, level from 0x6E7740 = SavingGrabAdj). OU's behaviour already matches; only
  the remark is wrong.

## Partial results worth follow-up

- **V005, 0x5D3860.** Besides 0x400, 0x200, 0x100, 0x40, 0x20 and 0x10, the code has a fifth depth class, **0x80**,
  between `+0xAE` and `+0xB0` (0x5D3922). It is empty only while both values are 391 (lara.drm, per the comment).
  0x764340 confirms the same shorts `+0xAC..+0xB2` → classes 0x323..0x325.
- **V020, 0x4DD820.** The template constructor stores 0.01 into `+0x100` / `+0x104` and 0 into `+0x154` / `+0x158`.
  The integrator 0x4EFB20 reads `+0x154` / `+0x158`, which 0x48E690 fills from the physics record (V019, match).
  No copy from `+0x100` to body `+0x154` was found. "Bodies without a record damp at 0.01" is therefore unproven; in
  0x4DD430, `+0x154` / `+0x158` are list links, so the template is probably a different class.
- **V024, 0x492B80.** The step is clamped to 2 frames, but 1.15385 is a lag threshold rather than a step size. Above
  it an overrun accumulator `[0xA0381C]` grows (capped at 90). When it trips, the step is halved and 0x4F1900 runs
  twice. Flag `[0xE7F0A4] & 0x100` scales the step by 0.05. OU models none of this.
- **V051, 0x750040.** This resolves one of the [U] air-brake time limits: it is `2 × pd[+0x4C] / 5.72443` (cached at
  0xFAE588), the frames to the apex of a `pd[+0x4C]` take-off [I]. The other limit is query `+0x70` against
  `[ctx+0x444]`.
- **V056, 0x73F900.** MustDetach also tests player data `+0xCB4` and three angular limits, `+0xCA8`, `+0xCAC` and
  `+0xCB0` (degrees, via sin). OU models only `+0xCA0` / `+0xCA4`.
- **V069, 0x702F10.** The fall-damage maximum is `table.+8 × pd[+0x44]`, the same expression as max health (0x6E7960),
  so it equals OU's `t_.max`. The result is then multiplied by `0x5D6170(height)`, and the thresholds pass through
  `0x702E90`. Neither is modelled; their meaning is [U].
- **V054, 0x74FDF0.** The bad-attach sum `+0x6E0 + +0x6DC − +0x9F8` is confirmed. The same function also uses 0.03 and
  gravity; OU's comment does not cover them.
- **V063.** Wall-jump `+0x82C` and `+0x838` are read at 0x759228 and 0x758B70, in the functions just before 0x759260
  and 0x758BB0, not in the four functions cited.
- **V048.** The mirror negation of the root quaternion's y / z, and of translation x (V047), happens in 0x441FB0, not
  0x4428A0. It applies when request flag 0x200 XOR clip flag `[+0xF4] & 0x20` is set.
- **V045, 0x727BF0.** Clearance test 0 also uses ±31.605 margins (0x727E93, 0x727F08); OU's comment does not mention
  them.
- **V017, 0x6ADB70.** The wave clock wraps at 345 600 s before the 200π `fmod`. This does not matter in practice.

## Not checkable in the exe (data-driven), recorded as such

Camera follow speed 3793.1 is settings `+0x64` (V027). The 0.5 s focus blend's duration is a caller argument
(V032). The ledge capsule offsets 219.03 / 12.64 / 316.06 are record data (V041). The obstacle step 632.1 is not a
literal (V036). Scuba slot 10 is reached through a handle (V011).

## Notes for the parent (BUGS.md candidates)

- OU-?: slope-slide upper bound. `floorKind` uses cos 28° where tru.exe's 0x5DCE20 has no upper bound (V002).
- OU-?: wall-side limit for fast arcs. 0.5236 above 3.1605 per frame is missing (V042).
- Doc: replace the cited 0x622418 with 0x7228E0 in four places (V049), and correct the Combat.hpp:32-33 remark (V074).
