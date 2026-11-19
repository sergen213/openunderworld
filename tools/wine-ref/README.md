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
collecting: paths and unit names come from `env.sh`; the compositor runs in a systemd scope and writes its DISPLAY to
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

## Configuration (`env.sh`)

| variable | default | meaning |
|---|---|---|
| `OU_WREF_DIR` | `$OU_WORK/wine-ref` | session directory: `bin/` (helpers), `display.env`, `kwin.log`, `xdg/`, traces |
| `WINEPREFIX` | `$OU_WREF_DIR/prefix` | the private prefix |
| `OU_TRU_DIR` | `$OU_WORK/wine-drive/game` | tru.exe's directory |
| `OU_WREF_NAME` | `wineref` | compositor socket `ou-<name>`, units `ou-<name>-kwin.scope` / `ou-<name>-tru.service` |

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

## Helpers (`src/`, run as `w.sh <tool> args`)

| tool | commands |
|---|---|
| `wmem` | `goto <unit>` (load a unit), `unit <name>` (redirect the next load), `inst` (instance list), `cam [n] [ms]` (Lara + camera position / pivot / distance / mode), `rd` / `w32` / `wf` / `w8` / `wb` / `wp` (memory), `wbs` (`wb` with the game's threads suspended and none inside the patched bytes: detours of hot functions), `find`, `alloc [bytes]` (code cave, default one page), `call2` (remote cdecl call), `mask` |
| `wkey` | `click <title> <button>` (dialogs), `key <vk> [ms]`, `multi <ms> vk...`, `km <hold> <total> vk...` (keys + camera mode samples), `mouse dx dy steps`, `combo`, `post` |
| `pole` | `dump uid...` / `raw uid...` (instance position / rotation / words; -1 = player), `watch n ms uid...`, `yaw <rad>`, `lara x y z`, `post` / `postv` (INSTANCE_Post 0x5B3750) |
| `ref` | `trace <ms> [vk:down:up ...]` (keys + a line per change: pos +0x20, rot +0x30, vel +0x2A0, state pointer, anim +0xC18), `drop x y z pinms ms vx vy vz [keys]` (pin, release with a velocity, trace with chart request / parent, volume top wt and wave surface sf), `rec <ms> [keys]` (trace + water, breath +0x920, health +0x628, camera +0x90 / pivot +0x100 / distance +0x1D4 / mode +0x1E0), `tp` / `tpv x y z ms`, `god`, `mouse dx dy [steps]`, and the `wmem` memory commands |
| `enemy` | `list` (instances with an AI: uid, position, object), `watch <ms> <period> uid...` (Lara, health with bites as `hurt` and a heal under 600, per enemy position, distance, Lara-sense flags and the EnemyBehavior / EnemyState objects its AI points at; map the vftables with docs/re/db/rtti.tsv), `zones uid...` (guard zones and region as 0x622C80 tests them) |

`camhook.sh install` detours CameraManager 0x5E8210 (camera mode requests) into a ring buffer; `camhook.sh log`
prints the requests as `mode@caller`.

Keys are Windows virtual-key codes in hex with press / release times in ms from the start of the command, e.g.
`26:0:3000` = Up held for 3 s. Defaults of the GOG build: arrows move (Up 26, Left 25, Right 27, Down 28; W 57 also
walks forward in the old traces), Space 20 jump / swim up, Ctrl 11 crouch / dive, LShift a0 sprint, Q 51 grapple,
X 58 draw / holster.

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
