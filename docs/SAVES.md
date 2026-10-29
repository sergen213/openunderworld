# Saves, checkpoints, respawn

Code: `src/save` (`Save.hpp`: format, slots, atomic files, `capture`/`apply`), `src/game/Stream.cpp` (checkpoints,
respawn, `World::enterUnit`), `ou-game` (F5/F9, `--load`, `--die`). Tests: `tests/test_save.cpp`.

Labels: **[O]** observed in game data, in our tru.exe decompile (GOG v1.1.0.0 VAs, `decomp_all.c`) or in a run;
**[I]** inference / our design; **[U]** unknown.

## Where saves live

- Directory: `$XDG_DATA_HOME/openunderworld/saves` (fallback `~/.local/share/openunderworld/saves`) — the same
  directory the launcher's **Saves** tab backs up and restores (LAUNCHER.md). The launcher copies the whole
  directory, so the `.bak` files below are backed up too.
- One file per slot: `<slot>.ousave`. Slot names are 1–32 characters of `[A-Za-z0-9_-]`; anything else is rejected
  (no paths). `ou-game` uses the slot `quick`.
- Backup slot: `<slot>.ousave.bak` is the previous content of the slot.

## Writing (atomic, with a backup)

`save::writeFile` (tested by `Save.AtomicWriteKeepsBackup`):

1. Write the new bytes to `<slot>.ousave.tmp` and `fsync` it.
2. If the slot exists, hard-link it as `<slot>.ousave.bak.tmp` and `rename` that over `<slot>.ousave.bak`. The
   backup is replaced atomically, so there is never a moment without one.
3. `rename` the tmp file over `<slot>.ousave` (atomic on POSIX), then `fsync` the directory.

A crash at any step leaves the slot either old or new, never partial. A leftover `.tmp` is ignored and overwritten
by the next save.

Reading (`save::readFile`) parses the slot. If the slot is missing or invalid, it falls back to the `.bak` file and
reports it: `ou-game` logs `(from .bak)`. If both are bad, the error names both failures.

## Format (version 2)

A text header line, then a JSON object:

```
OUSAVE <version> <crc32 of the body, 8 hex digits> <body length in bytes>\n
{ ...JSON body... }
```

- The CRC is zlib `crc32` of the exact body bytes.
- A file is rejected (`save::SaveError`) on any of these:
  - wrong magic, missing header;
  - body length ≠ header length (truncation or trailing bytes);
  - CRC mismatch (corruption);
  - a version above the current one, or below 1;
  - a body that is not a JSON object;
  - a missing or ill-typed required field (`unit`, `player.state` / `pos` / `yaw`);
  - an unknown player state name;
  - `script` that is not an object.

  `Save.RejectsCorruptAndTruncated` covers each case.

Body fields. Positions are in the frame of `unit`, the unit Lara is in. This is the frame that unit has when it is
loaded at offset 0, not the streamed world frame, so a load puts it back at offset 0.

| Field | Type | Meaning |
|---|---|---|
| `unit` | string | unit base name (`pc-w\<unit>.drm`) |
| `ticks` | int | simulation ticks played when saved (informational; replays keep their own clock) |
| `player.state` | string | `game::stateName` (`idle`, `run`, `hang`, `swimtread`, …) |
| `player.pos`, `vel` | [x, y, z] | feet position (unit frame), air velocity |
| `player.yaw`, `pitch` | number | heading; swim pitch |
| `player.clip`, `clipTime`, `clipRate`, `loop` | | Lara's current clip and its unbounded time |
| `player.ledge` | null or `{unit, index, seg, t, side}` | hung-on ledge: markup `index` within `unit`'s markup |
| `player.water` | null or `{unit, index}` | water volume Lara swims in, same addressing |
| `player.runJump`, `regrabCooldown` | | controller details |
| `camera.yaw`, `camera.pitch` | number | follow-camera orbit |
| `checkpoint` | null or `{unit, id}` | last checkpoint: intro unique id in `unit` |
| `collected` | [int] | intro unique ids of picked-up rewards (`World::collected`, src/game/Progress.hpp) |
| `chapter` | int | `game::kChapters` index; missing (older saves) = the unit's `l<N>_` prefix, else 0 |
| `found` | [int, int, int] | rewards collected per kind: treasures, relics, health packs (PDA counts; health packs = carried, a use takes one); missing = 0s |
| `health` | number or null | Lara's health (`World::health`); missing/null (older saves, or saved while dead) = full; must be > 0 |
| `script` | object | `script::Runtime::serialize()` (src/script): per-intro trigger/switch/door/block state, opaque here |
| `enemies` | object or null | `ai::Enemies::serialize()`: `states` (intro uid → full enemy state in its unit's frame: mode, pos, yaw, clip + time, health, AI timers, aim RNG; generator spawns under their generator's uid; loaded and streamed-out enemies), `firedGenerators`, `deadAtCheckpoint`; missing/null (older saves) = enemies left as they are |

Ledge and water indices are stored as (unit, index within that unit), not as indices into the merged collision mesh.
That index depends on which units happen to be loaded and in what order.

Numbers are written with round-trip precision. Float state is restored bit-exactly (`Save.RoundTripIsExact`).

### Versions and migration

- **1** — the development draft format. It was never released; it is kept as the migration test case.
  - Fields: `unit`, `player.{state,pos,yaw}`, and `checkpoint` as a bare intro id of the save's unit.
  - No camera, items or script.
- **2** — current.

`parse()` migrates 1 → 2:

- `checkpoint: <id>` becomes `{unit: <save unit>, id}`;
- missing fields take defaults (camera behind Lara, no items, empty script state).

Saves are always written as the current version. Newer versions are rejected, never guessed.
`Save.MigratesVersion1` loads a hand-written v1 file.

## Load

`save::apply` does the following:

1. `World::enterUnit(unit, saved position)`. This drops the stream and loads the save's unit at offset 0 plus its
   portal neighbours (LEVELS.md "Streaming").
2. Maps ledge/water back to merged-mesh indices.
3. `World::restore` sets the full player state and the camera. It does not blend from the previous frame; the
   animation pose is reset.
4. Restores the checkpoint, the collected items, the script state, Lara's health (`World::setHealth`;
   `Save.HealthRoundTrip`) and the enemies (`World::restoreEnemies`: every enemy of the loaded units is recreated
   from the saved state or, if the save never saw it, from its intro; `AiReal.EnemyStateSaveRestore`).

The simulation then continues exactly. `StreamReal.PoolToRagnarokOnFootThenSaveLoad` saves in `l4_ragnarok_room`
(streamed in from `l4_pool` at offset (−20475.4, −2872.2, 12591.4)) and runs 40 more ticks. It then loads, which puts
ragnarok at offset 0, and runs the same 40 ticks again. States and clips match on every tick; positions match within
0.5 units, which is the float rounding of the other frame.

## Checkpoints and respawn

- [O] tru.exe births Lara in `0x5D40C0`. If a checkpoint id is stored in the game state (`DAT_00c59264 +0x90` /
  `+0x114`), it places the player at the intro with that unique id: `FUN_00698180` copies its transform to the
  player intro. Otherwise it chooses among intros of the objects `checkpoint` / `invis_checkpoint`
  (`OBTABLE_GetObjectID`), then calls the save routine `0x5570F0` ("Save Game").
- [O] Death/restore goes through `0x556E90` ("MAIN checkpoint restore %s ... ID wanted = %d"). That function also
  checks the save version ("Wrong version for save").
- [O] The checkpoint object's birth (`0x6B0040`) compares its intro id with the stored checkpoint id to show the
  activated state.
- [U] What activates a checkpoint. `checkpoint.drm` carries 50+ Script sections and its render model has no drawable
  batches; the trigger logic is in those scripts, which are not decoded.
- [I] Ours: `UnitData::checkpoints` holds every `checkpoint` / `invis_checkpoint` intro: unique id, position, and
  heading = rotation z − 90° (as the player intro). When Lara's feet come within 250 units horizontally and 400
  vertically of one, `World::checkpoint()` becomes {unit, id}. l4_pool has 12 checkpoints and l4_ragnarok_room 2.
- `World::respawn()` (death) calls `enterUnit(checkpoint unit)` and places Lara at the checkpoint intro, or at the
  start unit's spawn if no checkpoint was touched, at full health. Death comes from Combat (health ≤ 0 on the
  ground); `ou-game --die SECONDS` forces it.
- [I] Enemies are reset with it (`ai::Enemies::reset`): when a new checkpoint is touched the enemies dead at that
  moment are remembered (`Enemies::checkpoint`); on respawn those stay dead and every other enemy returns to its
  placement at full health, generator spawns are removed and their generators re-armed. tru.exe restores the
  checkpoint save (`0x556E90`), which holds per-intro instance state; that path was not traced, so which enemies the
  original keeps dead is inferred. Our saves hold every enemy's state and the dead-at-checkpoint set, so a load
  restores enemies exactly; whether the original's save keeps mid-fight state (rather than checkpoint state) is
  not traced [U].

## ou-game

```
ou-game --game-dir DIR                     # F5 quicksave, F9 quickload (slot "quick")
ou-game --game-dir DIR --load quick        # start from a slot
ou-game --game-dir DIR --die 2.5           # die at 2.5 s, respawn at the last checkpoint
ou-game --game-dir DIR --input-script tests/replays/save_move_load.txt --frames 240
```

Input scripts drive saves with `save` and `load` actions (press edges, like `jump`). Every save/load/respawn logs:

```
ou-game: <saved|loaded|died, respawned> ...: unit U state S pos X Y Z (unit frame) yaw Y clip C t T checkpoint ID
```

Verification:

- [O] `tests/replays/save_move_load.txt`, 2026-10-18, `XDG_DATA_HOME=/tmp/...`:
  - the save at 2.0 s and the load at 3.5 s logged identical lines: unit `l4_ragnarok_room`, state run, pos
    3685.266 2829.039 −7850.686, yaw −3.1416, clip ng_run t 1.3333, checkpoint 5502;
  - the slot file was 528 bytes, header `OUSAVE 2 36a60efc 506`.
- [O] `tests/replays/pool_to_ragnarok.txt` with `--die 2.5 --frames 200`, 2026-10-18:
  - Lara had touched l4_pool checkpoint 5502 on her way out, then crossed into ragnarok at tick 104;
  - the respawn at tick 150 logged `unit l4_pool ... pos -15549.563 -288.284 4721.234 ... checkpoint 5502`: the
    checkpoint intro, ground-snapped, heading 0.5°;
  - the stream was back to {l4_pool, l4_ragnarok_room}, and the tick-200 screenshot shows Lara on the pool-side
    walkway by the doorway.

## The original game's saves

- [O] From tru.exe:
  - Saves go under `CSIDL_PERSONAL` (My Documents), via `SHGetFolderLocation`, with fallbacks `Personal` and
    `C:\My Documents`, in `Eidos\Tomb Raider - Underworld\` (`0x78B550`).
  - Slots are registered in `0x7744E0`:
    - `savegame%02d.sav`: 10 slots, size limit 0x140000 bytes;
    - `Screenshot%03d.ss`: 100;
    - `profile.pro`: 1.
  - The contents are the engine's SAVE block buffer (`0x556310`: "illegal save type", "save %d is too big",
    "SAVE_BufferIntro", per-intro saved instances). It also holds the PDA map and gear ("evControlSaveData").
- [U] The binary layout of `.sav` / `.pro`. No public documentation was found, and there are no original saves on
  this machine: no Wine prefix holds an `Eidos\Tomb Raider - Underworld` folder.
- Import is **not implemented**. It would need a real `.sav` to reverse the SAVE block types against `0x556310` and
  `0x556E90`. OpenUnderworld never reads or writes the original saves.
