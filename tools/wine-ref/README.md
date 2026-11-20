# wine-ref: measuring the original tru.exe under Wine

The harness behind the `[O-runtime]` rows of `docs/REFERENCE_COMPARISON.md` and `docs/GAMEPLAY.md`: tru.exe (GOG
1.1.0.0) runs under Wine in a private nested compositor and a private Wine prefix; small Win32 helpers read and write
its memory, send it keys and print per-frame traces; Python scripts reduce the traces to the numbers in the docs.

Nothing here contains game data. The game install, the Wine prefix, traces and screenshots stay in the private work
directory (`openunderworld-work/`, default `OU_WORK`), never in the repository.

These sources were collected from the work directory, where they grew session by session (2026-10 .. 11):
`scratch/climbcam/ouk/` (wmem.c, wkey.c, session.sh), `poleattach/` (pole.c, start-kwin.sh, newgame.sh, tru.sh,
w.sh, cg.sh), `waterentry-wine/` (weref.c = `src/ref.c`, a superset of `gameplaygoal/reference/ref2.c`, itself
`climbq/pt.c` + `tpv` + `CAMFOLLOW`; model.py, validate.py, fitwave.py), `gameplaygoal/reference/` (tr.py, tl.py,
swing.py, session7a.sh / session7b.sh as the session template), `camfollow-wine/` (turn.py), `scratch/climbcam/ouk/
hook.sh` + `log.sh` (= `camhook.sh`). The per-session copies (`camfollow-wine/`, `musicwine/`, `yachtwine/`, ...)
differed only in their hard-coded paths and socket names, which `env.sh` now parameterises. Changes made while
collecting: paths and unit names come from `env.sh`; the compositor runs as a systemd service and writes its DISPLAY to
`$OU_WREF_DIR/display.env`; `newgame.sh` / `stop.sh` stop only this harness's units and its own prefix's wineserver
(the old scripts ran `pkill -f tru.exe`, which would kill another agent's session); `ref.c` gained `rec` (breath,
health, camera and water columns); helpers link statically.

## Prerequisites

- Wine (tested: wine-11.22, WoW64) and mingw-w64 (`i686-w64-mingw32-gcc`; tru.exe is a 32-bit process), KWin 6
  (`kwin_wayland --virtual --xwayland`), `dbus-run-session`, `xdotool` and ImageMagick `import` (screenshots),
  Python 3 (numpy for the fit scripts), systemd user session (`systemd-run --user`).
- A copy of the game install whose `tru.exe` directory is `OU_TRU_DIR` (default `$OU_WORK/wine-drive/game`, a
  hard-link copy of the GOG install). In it, GOG's compatibility layer is switched off: `D3D9.dll` (dxwrapper) and
  `DEPHook.asi` are renamed to `*.bak`, and `tru.sh` sets `WINEDLLOVERRIDES="d3d9=n;dxwrapper=d"`.
- A Wine prefix with DXVK's 32-bit `d3d9.dll` in `drive_c/windows/syswow64` and the game's settings already chosen
  once in its setup dialog (1280x720 windowed; the dialog still opens at every start and `newgame.sh` presses Ok).
  `$OU_WORK/wine` is such a prefix. Each concurrent harness needs its own copy (`cp -a $OU_WORK/wine $DIR/prefix`):
  helpers find the game by its window class (`TOMBRAIDER8`), so two sessions in one prefix would see each other.
- **The prefix's Documents must be private.** `winecfg` links `drive_c/users/<user>/Documents` (and Desktop, Music,
  ...) to the Linux home directory, and `$OU_WORK/wine` (so every `cp -a` copy of it) has those links: tru.exe then
  writes `Autosave.TRUSave` and menu saves into the user's own `~/Documents/Eidos/Tomb Raider - Underworld/` (seen
  2026-11-20: agent saves of several chapters there, the autosave rewritten by a running session). `menu.sh setup`
  replaces the links with empty directories; every `menu.sh` command refuses to run while any `drive_c/users/*/` entry is a link (also `Default`).

## Configuration (`env.sh`)

| variable | default | meaning |
|---|---|---|
| `OU_WREF_DIR` | `$OU_WORK/wine-ref` | session directory: `bin/` (helpers), `display.env`, `kwin.log`, `xdg/`, traces |
| `WINEPREFIX` | `$OU_WREF_DIR/prefix` | the private prefix |
| `OU_TRU_DIR` | `$OU_WORK/wine-drive/game` | tru.exe's directory |
| `OU_WREF_NAME` | `wineref` | compositor socket `ou-<name>`, units `ou-<name>-kwin.service` / `ou-<name>-tru.service` |
| `OU_WREF_CPUS` | `0-3` | `taskset` CPU list of tru.exe (its CPUCount 0x4A21D0 overflows with 32 CPUs; parallel harnesses each take their own four) |

The compositor runs as a transient systemd service with `KillMode=control-group` (`start-kwin.sh`): stopping it also
ends what its private D-Bus session activated (xdg-desktop-portal, ksecretd), which a scope left running. Harnesses
with their own prefix and game copy on NVMe may run side by side; the shared `$OU_WORK/wine` prefix still needs
`/usr/bin/flock $OU_WORK/wine.lock`.

## A session

```sh
export OU_WREF_DIR=$OU_WORK/w2/me OU_WREF_NAME=me            # WINEPREFIX defaults to $OU_WREF_DIR/prefix
cp -a $OU_WORK/wine $OU_WREF_DIR/prefix                         # once
tools/wine-ref/build.sh                                         # helpers -> $OU_WREF_DIR/bin
tools/wine-ref/start-kwin.sh                                    # private compositor (prints its DISPLAY)
tools/wine-ref/newgame.sh                                       # tru.exe + new game into the prologue (~2.5 min)
tools/wine-ref/w.sh wmem goto l2_cliffclimb; sleep 100          # load a unit (tru.exe 0x5DF8C0)
tools/wine-ref/w.sh pole dump -1                                # where she is (uid -1 = the player)
tools/wine-ref/w.sh ref drop -20777 44668 -18595.8 1500 4000 0 0 0 > h300.txt
tools/wine-ref/stop.sh                                          # tru.exe, wineserver, compositor
```

`sessions/l2_cliffclimb_sea.sh` is a complete scripted session (water entry, treading, swimming, breath, the camera
on the yacht's deck); `sessions/l2_cliffclimb_sharks.sh` watches the l2_cliffclimb sharks around a treading, diving or
pinned Lara (OU-CMB-003); `$OU_WORK/gameplaygoal/reference/session7a.sh` / `session7b.sh` are the older templates for
ledge / chimney / grapple placements (they still use the pre-`env.sh` paths).

### Starting from a tru.exe save: `menu.sh`

A `wmem goto` is a mid-game jump: the unit loads, but with the prologue's chapter state, and nothing the chapter's
scripts or earlier units would have done. `menu.sh` drives the front end and pause menu by key presses instead, so a
session can start from a tru.exe save (`.TRUSave`, from any earlier session or a player) and write new saves:

```sh
tools/wine-ref/menu.sh setup                                   # once per prefix: private Documents
tools/wine-ref/menu.sh install --only "$OU_WORK/.../SOUTHERN MEXICO - THE UNNAMED DAYS.TRUSave"
tools/wine-ref/menu.sh boot                                    # tru.exe to the main menu (~1 min on an idle machine)
tools/wine-ref/menu.sh load 1                                  # LOAD GAME slot 1, YES; returns once she stands still
tools/wine-ref/w.sh wmem state                                 # main 1 ... unit l4_trailstart ... pos ...
tools/wine-ref/menu.sh save mylabel                            # pause, SAVE GAME, first empty slot -> saves/mylabel.TRUSave
tools/wine-ref/menu.sh resume                                  # (after a restart) RESUME GAME = the autosave
```

Each step reads the menu back before the next key (`menu.sh sel`: the selected item is the one drawn with a white
icon, the open slot ring's item is circled; at 1280x720 only) and the game state from memory (`wmem state`: main
state 10 / front-end sub-state 3 = main menu, main state 1 = in play, gameTracker flag 0x10000000 = pause menu open),
because presses are lost when a loaded machine runs the menu at a few frames a second. Learnt on the way (2026-11-20):

- A fresh private Documents has no `Autosave.TRUSave`: the front end then asks "Create autosave file?" (`boot`
  answers YES). The centre item is PLAY GAME, RESUME GAME once an autosave exists. With a save installed the setup
  dialog of `newgame.sh` did not open in this prefix copy; `boot` handles both.
- The ring remembers its last item (Left from the centre goes there), the pause menu reopens where it was left (even
  inside a slot ring), and Escape inside the pause menu's slot ring closes the whole menu: navigate by reading back.
- LOAD GAME lists the saves except the autosave; their order is not the file names' ASCII order (with "SOUTHERN
  MEXICO - ..." and "Savegame 1" slot 1 was "Savegame 1"): check `wmem state`'s unit after a load.
- SAVE GAME on an empty slot asks "Enter savegame name" ("Savegame <n>", OK) and writes `Savegame <n>.TRUSave`, then
  "Save Successful". A save holds tru.exe's committed checkpoint buffer, not the moment: loading the l4_ballcourt
  save written after the combat-4 births put her back at the checkpoint with nothing born (docs/re/SAVEGAME.md: the
  file is written from the committed buffer). Save right after a checkpoint, or replay the steps after the load.
- A death reloads the checkpoint (= the autosave): births and kills since are undone.

`sessions/l4_ballcourt_combat.sh` is the complete example: a chapter-4 save (l4_trailstart) loaded through the menus,
`goto l4_ballcourt` (whose checkpoint becomes the autosave, so `again` restarts from it with `menu.sh resume`), Lara
placed with `ref tpv` on the near side of trigger plane 6543 and walked over it, which births l4_ballcourt's "combat 4"
(panther 6532, poachers 6537..6539) 1.1 s later, then `enemy fight` logs of the encounter and of the pistols
(docs/REFERENCE_COMPARISON.md M31 / M44 / M45, BUGS OU-CMB-006..008). The "no enemy after a goto" of earlier sessions
was not the goto: l4_ballcourt's enemies are dormant intros that only its trigger planes birth, and teleports do not
cross a one-sided plane.

`sessions/render_capture.sh` records every D3D9 call of a new game with apitrace's `d3d9.dll` wrapper (apitrace 14.0
win32 release, `lib/wrappers/d3d9.dll`, copied next to tru.exe in a private game copy; it forwards to the prefix's DXVK
`d3d9.dll`; `tru.sh` passes `TRACE_FILE`). `SCENE=bedroom` (the prologue corridor), `cliffclimb` (the yacht deck after
`goto l2_cliffclimb`) or `sharks` (Lara treading near the sharks); the view is held still at the end, so the trace's
last frames are the scene. A session writes 16..22 GB. `py/d3d9_frames.py` reduces it with the Linux `apitrace dump`
(`APITRACE=…/apitrace-14.0-Linux/bin/apitrace`): `index` once per trace (resources, shader disassembly, Present call
numbers; ~3 min), then `frame` (pass list of one frame), `sort` (translucent order), `shader`, `lod` (batch LODs by
primitive totals). docs/re/RENDER_PIPELINE.md §13 is its result.

Rules learnt the hard way (details in docs/REFERENCE_COMPARISON.md "Method"):

- Only the first `wmem goto` of a session is reliable: one unit per session.
- After `goto`, tru.exe's world is our unit frame plus a per-unit offset (her start position minus our `player`
  intro); headings: tru yaw = ours + 90°. A death reloads the chapter's checkpoint in the chapter's frame, after which
  every `goto`-frame coordinate is wrong: keep `GOD=1` (or `ref god &`) and raise the fall-damage heights
  (`session7a.sh` `nofall`).
- The game renders at ~60 Hz with a variable step; velocities (+0x2A0) are units per 30 Hz frame. A key sent with
  SendInput reaches the game up to 35 ms later.
- Hold her with `tp` (fall state keeps running) or `tpv` (velocity zeroed too); `CAMFOLLOW=dx,dy,dist,h` pins the
  camera so that Forward means world direction (dx, dy).
- A `goto` can fail silently (she stays in the prologue; seen after deaths before the `goto`, and once on a clean
  start for l5_valaskjalf): check her position after the load and restart the session (`stop.sh`, `newgame.sh`).
- Fall damage counts from where the fall state began: a pinned `ref drop` gives none at any height; lift her once
  with `pole lara x y z` instead.
- A `goto` unit may lack its enemies: dormant intros (+0x72 & 0x80) are born by the unit's trigger planes / scripts
  (l4_ballcourt: planes 6543 / 6544 / 6546), so walk her over them; the sharks of l2_cliffclimb / l1_seafloor are
  there at once.
- Teleport with `ref tpv` (velocity zeroed for its duration). A `pole lara` jump of tens of thousands of units keeps her velocity:
  twice she was flung across l4_ballcourt, once to her death (2026-11-20).

## Helpers (`src/`, run as `w.sh <tool> args`)

| tool | commands |
|---|---|
| `wmem` | `goto <unit>` (load a unit), `unit <name>` (redirect the next load), `inst` (instance list), `state` (main state 0xE7ED60, front-end sub-state 0xFAE8DA, gameTracker flags +0x2C, unit name +0x38, player, Lara's position: what `menu.sh` polls), `cam [n] [ms]` (Lara + camera position / pivot / distance / mode), `rd` / `w32` / `wf` / `w8` / `wb` / `wp` (memory), `wbs` (`wb` with the game's threads suspended and none inside the patched bytes: detours of hot functions), `find`, `alloc [bytes]` (code cave, default one page), `call2` (remote cdecl call), `mask` |
| `wkey` | `click <title> <button>` (dialogs), `key <vk> [ms]`, `multi <ms> vk...`, `km <hold> <total> vk...` (keys + camera mode samples), `mouse dx dy steps`, `combo`, `post` |
| `pole` | `dump uid...` / `raw uid...` (instance position / rotation / words; -1 = player), `watch n ms uid...`, `yaw <rad>`, `lara x y z`, `post` / `postv` (INSTANCE_Post 0x5B3750) |
| `ref` | `trace <ms> [vk:down:up ...]` (keys + a line per change: pos +0x20, rot +0x30, vel +0x2A0, state pointer, anim +0xC18), `drop x y z pinms ms vx vy vz [keys]` (pin, release with a velocity, trace with chart request / parent, volume top wt and wave surface sf), `rec <ms> [keys]` (trace + water, breath +0x920, health +0x628, camera +0x90 / pivot +0x100 / distance +0x1D4 / mode +0x1E0, look vector `lk` +0x160 (unit, look point − camera), matrix rows `mx` +0x10; `PDX=<hex>` adds a `px` line with the player-data dword at that offset, e.g. `PDX=EDC` the clearance result), `tp` / `tpv x y z ms`, `god`, `mouse dx dy [steps]`, and the `wmem` memory commands |
| `enemy` | `list` (instances with an AI: uid, position, object), `watch <ms> <period> uid...` (Lara, health with bites as `hurt` and a heal under 600, per enemy position, distance, Lara-sense flags and the EnemyBehavior / EnemyState objects its AI points at; map the vftables with docs/re/db/rtti.tsv), `fight <ms> <period> uid...` (on change: Lara's health drops as `hurt`, each enemy's health AI + 0x2C64 as `hit <uid> <amount> -> <health>`, the ammo words +0x51C, her anim word; positions and distances every 100 ms; `HEAL=h`), `zones uid...` (guard zones and region as 0x622C80 tests them), `hp <ms> <period> [set]` (Lara's position and health on every change and once a second, drops as `hurt`; `set` writes the health first; env `HEAL=h` refills to 1000 under h: the hazard measurements of docs/REFERENCE_COMPARISON.md, Lara held with `ref tpv`) |
| `section` | `<n> <ms>`: Lara's anim section 1 (weight / step / target, request playing, bone-mask runs; changed dwords per sample) and her upper-body chart (state, pattern +0x488); docs/GAMEPLAY.md "Script flinch overlay" (l3_main_bedroom fire volume 4529) |

`camhook.sh install` detours CameraManager 0x5E8210 (camera mode requests) into a ring buffer; `camhook.sh log`
prints the requests as `mode@caller`.

Keys are Windows virtual-key codes in hex with press / release times in ms from the start of the command, e.g.
`26:0:3000` = Up held for 3 s; in `ref` schedules 1 / 2 are the left / right mouse buttons. Defaults of the GOG build
(docs/GAMEPLAY.md "Controls"): arrows move (Up 26, Left 25, Right 27, Down 28; W 57 also walks forward in the old
traces), Space 20 jump / swim up, Ctrl 11 crouch / dive, LShift a0 sprint, Q 51 grapple, G 47 (or right mouse) target
lock, which draws, H 48 (or left mouse) shoot, R 52 change weapon, Esc 1B pause.

## Reading traces

Every line is `ms` (since the command started) followed by named fields. The scripts in `py/`:

- `tr.py FILE...`: ground speeds (least-squares slope of path length over 0.5 s windows) and flights (runs of non-zero
  velocity: take-off, apex, landing, length).
- `tl.py FILE LABEL [OX OY OZ]`: timeline of one `== LABEL keys` segment (state / animation changes, starts and ends
  of motion) in our unit frame.
- `swing.py`: pendulum turning points and periods about a pivot (grapple).
- `turn.py`: heading, motion direction and camera look per sample of `wmem cam` output (surface swim turn).
- `drop_summary.py TRACE...`: water entry (depth under the waves, request sequence) and treading offsets of
  `drop` / `rec` traces.
- `fitwave.py`, `waterentry_model.py`, `waterentry_validate.py`: the water-entry law fits of docs/GAMEPLAY.md
  "Water entry".
- `swim_summary.py TRACE...`: speeds, turn / pitch rates, breath and damage times, camera lag of `rec` traces.
- `regcap.py install | find P0 P1 ... | read`: the pixel-shader constants (c15, c70–c76, c96, c100–c104, c140–c149,
  c176–c212), bound textures and PS bool mask of one material's model draws (detours of 0x526920 and 0x50EFED; the
  material is picked by the first floats of its MaterialParams). Used for SHADERS.md "PSSM on the l2_cliffclimb yacht
  deck".
- `d3d9_frames.py index | frame | sort | shader | lod TRACE ...`: reduce a `sessions/render_capture.sh` D3D9 trace
  (above; docs/re/RENDER_PIPELINE.md §13).
