# Camera system (tru.exe)

Static reconstruction of tru.exe's camera layer: the `CameraManager` singleton, its camera table and state machine,
the transition camera, the gameplay-camera mode requests, and how moving platforms carry the camera
(BUGS OU-CAM-001). The tuning of the gameplay orbit camera (`LaraCameraRewrite` 0x602690: distances, focus
records, force sets, collider) is already decoded in `src/game/World.cpp` (`updateOrbitCamera`) and
docs/GAMEPLAY.md "Cameras"; this file does not repeat it.

Binary: GOG tru.exe 1.1, SHA-256 `1fdbd345…1011`. Sources: `objdump -d -M intel` of tru.exe (**V** = read at
instruction level for this document) and `openunderworld-work/ghidra/exports/decomp_all.c` (**D** = decompiler
output only). **[I]** = inference, with a confidence word. Offsets are byte offsets from the object start.

## 1. Function table

| Address | Name (tru_gog.csv) | Role | Conf. | Check |
|---|---|---|---|---|
| 0x5E7770 | CameraManager_Get | `mov eax,0xE804F0; ret` — the singleton | high | V |
| 0x5F3A70 | CameraManager_ctor | stores vftable 0x855AE4, constructs 16 embedded cameras, fills the camera table (§2) | high | V (member ctor calls 5F3AAB..5F3B6A) |
| 0x5EB0A0 | CameraManager_dtor | runs the member destructors in reverse, ends on `ICameraManager` vftable | high | D |
| 0x5EDD50 | CameraManager_scalar_dtor | slot 0 | high | D |
| 0x837000 / 0x83CEE0 | (CRT init / atexit stubs) | `mov ecx,0xE804F0; call 0x5F3A70` / calls 0x5EB0A0 | high | V (837000) |
| 0x5F3C70 | CameraManager_Update | slot 10, per game frame: platform self-motion (0x5E7EF0), forced swaps (Exploration→Lara, Combat→LaraCombat), instance list GC (0x5F2E90), mode commit (0x5E7FE0), markup tracking (0x5F2E00), vehicle (0x5F2760), anchored exit, transition end, debug key, then active camera vf1 (Update), then `+0x78 += dt` | high | D, V (`timeMult / 30.0` at 5F3C79) |
| 0x5E8210 | CameraManager_RequestMode | slot 17: the player state machine asks for a Lara-camera **mode** (§4) | high | V (5E8210..5E8232) |
| 0x5E7FE0 | CameraManager_CommitMode | per frame: combat overrides of the requested mode, then if `+0x58 != +0x54` calls the Lara camera's vf 0x60 SetMode (or LaraCombatCamera 0x6033F0), `+0x54 = +0x58`, `+0x5C = 0` (time in mode) | high | D |
| 0x5EDFC0 | CameraManager_SwitchType | slot 20: switch to camera **type** n, cut (no transition) | high | D |
| 0x5EE190 | CameraManager_SwitchTypeBlended | slot 21: as slot 20 but through the transition camera | high | D |
| 0x5EDD70 | CameraManager_EnterGameplay | state 0, type n (11 Lara, 10, 13 vehicle); cut via 0x5E8630 or blended via 0x5EB310 | high | D |
| 0x5EDEB0 | CameraManager_EnterCombat | state 1, type 2→12 (LaraCombat); remembers it in +0x68 | high | D |
| 0x5EDE20 | CameraManager_EnterScripted | state 3, types 4 / 7 / 8 / 15 (anchored, cinematic, autospline, follow-spline); +0x6C | high | D |
| 0x5E8630 | CameraManager_SetActive | `old->vf 0x20(new)` (deactivate), `+0x44 = new`, `new->vf 0x1C(old)` (activate) | high | D |
| 0x5EB310 | CameraManager_StartTransition | activates the TransitionCamera (+0x670) towards `target`; `+0x70 = state`, `+0x4C = 4` | high | V (5EB310..5EB337) |
| 0x5EE3C0 | CameraManager_EndTransition | when the transition's elapsed ≥ duration − 0.01: re-enters the state saved in +0x70 with the target's type, cut | high | D |
| 0x5EDF50 | CameraManager_ToggleMode (existing name) | slot 8: reset flags, back to the Lara camera (state 0, type 11) and its vf 8 | medium | D |
| 0x5EE460 / 0x5EE450 | CameraManager_ResetToLara / _ReenterGameplay | slot 32 / slots 30, 34 | high | D |
| 0x5EB300 | CameraManager_GetCamera | slot 57: `return [this + 4 + 4·type]` | high | V |
| 0x433AB0 | CameraManager_GetActive | slot 56: `return [this+0x44]` | high | D |
| 0x5EB160 / 0x5EB170 / 0x5EB180 / 0x5EB190 | CameraManager_IsFrozen / _MarkupMode / _GetMarkup / _GetMode | slots 1, 6, 7, 2: bytes +0x7E, +0x7D, dwords +0x74, +0x54 | high | D |
| 0x5EB3C0 / 0x5EB420 | CameraManager_EnterModelView / _EnterObjectDebug | slots 29, 33: state 5 | medium | D |
| 0x5F2BA0 | CameraManager_PlayCinematic | slot 31: state 3, type 7 | medium | D |
| 0x5F2860 | CameraManager_ScriptSetType | slot 26 (script camera request with the vehicle check) | medium | D |
| 0x60B320 | TransitionCamera_Retarget | snapshot current frame, new target, duration from the table (§5) | high | D |
| 0x60B4F0 | TransitionCamera_Activate | vf 7: snapshot the previous camera's matrix (vf 0x38), fov (vf 0x40) and type (vf 0x2C); elapsed = 0; duration (0x60B210) | high | D |
| 0x60B3C0 | TransitionCamera_Update | vf 1: update target, `t = min(elapsed/duration, 1)`, lerp position, slerp rotation, lerp fov, `elapsed += 1/30` | high | V (constant 0x843CB0 = 0.0333) |
| 0x60B210 | TransitionCamera_GetDuration | (from type, to type) lookup in the transition resource; anchored→anchored uses the anchor record's +0x58 | high | D |
| 0x60B0A0 / 0x60B130 | TransitionCamera_LerpPosition / _SlerpRotation | linear position, quaternion rotation | high | D |
| 0x6020E0 | LaraCameraRewrite_SetMode | vf 24: `+0x1E0 = mode`, mode-list records (0x5FE800 / 0x5FE8C0) into +0x1344 / +0x1348, defaults 0x601D30 | high | D |
| 0x600710 | LaraCameraRewrite_Follow | carry (0x5FF650), forces (0x5FFB00), proposal (0x600200), then position += min(1, (speed·dt + proposal move)/dist)·(proposed − position) | high | D |
| 0x5FF650 | LaraCamera_CarryWithPlatform | moves the camera position (+0x90) and proposed point (+0x150) with the carrier's frame delta (§6) | high | V (tail 5FF7B8..5FF7E0) |
| 0x609830 | CameraFocus_Update | focus tracker: record blend, then carry by the carrier velocity, then the dead-zone lag 0x609440 (§6) | high | V (609AFD..609B55) |
| 0x7027B0 | Player_GetCarrierVelocity | the platform Lara rides and its velocity at her root per 30 Hz frame (§6) | high | V (702A03..702A71) |
| 0x5D75B0 / 0x5DC120 | Carrier_PointVelocity / Carrier_RootVelocity | segment matrix delta (old +0x94 → new +0x90) applied to her root, / min(timeMult, 2) | high | D |
| 0x5E7EF0 | Camera_PlayerSelfMotion | `0xE91F20 = (pos +0x20 − oldPos +0xC0) − timeMult·30·carrierVelocity`: her move relative to the platform | high | D, V (constant 0x845258 = 30.0) |
| 0x5E7D70 / 0x5E7D40 / 0x5E7DB0 | Camera_HangMarkup / Camera_StandMarkup / Camera_MarkupMode | markup Lara holds (player +0xA64, state +0xA80) / stands on (+0xC94); markup type → mode (§4) | high | D |
| 0x5F2E00 | CameraManager_TrackMarkup | keeps +0x74 = the markup Lara is on (0x5F2D60 enter, 0x5F2DB0 leave) | high | D |

Existing names kept: 0x5F39F0 `CAMERA_SetMode` (it calls 0x5F2680 = "enter gameplay type n unless scripted",
then the active camera's vf 8), 0x5E26F0 / 0x5E2D90 / 0x5E2950 / 0x5E2CA0 AutoSplineCamera.

## 2. CameraManager (0xE804F0, vftable 0x855AE4, 59 slots)

Embedded cameras (constructor 0x5F3A70, V) and the type table `+4 + 4·type` (slot 57):

| Type | Table slot | Object offset | Class (RTTI vftable) |
|---|---|---|---|
| 1 | +0x08 | +0x120 | ExplorationCamera (0x855D04) |
| 2 | +0x0C | +0x430 | CombatCamera (0x85581C) |
| 3 | +0x10 | +0x670 | TransitionCamera (0x8564A4) |
| 4 | +0x14 | +0x7C0 | AnchoredCamera (0x8555CC) |
| 5 | +0x18 | +0xE4B0 | FreeCamera (0x855E54) |
| 6 | +0x1C | +0xE5F0 | ModelViewCamera (0x85614C) |
| 7 | +0x20 | +0x8F0 | CinematicCamera (0x855C54; BaseCamera ctor, vftable patched in place) |
| 8 | +0x24 | +0x9D0 | AutoSplineCamera (0x8556B4) |
| 9 | +0x28 | +0xE6D0 | ObjectDebugCamera (0x8561F4) |
| 10 | +0x2C | +0xE820 | CheatCamera (0x85575C) |
| 11 | +0x30 | +0x3370 (via +0xB80) | LaraCameraRewrite (0x85600C) — the gameplay camera |
| 12 | +0x34 | +0x5B90 | LaraCombatCamera (0x8560C4) |
| 13 | +0x38 | +0x9F30 | VehicleCamera (0x856544) |
| 14 | +0x3C | +0xDFD0 | ScopeCamera (0x856304) |
| 15 | +0x40 | +0xE360 | FollowSplineCamera (0x855DCC) |
| — | — | +0xB90 | LaraCamera (0x855F54): constructed, never put in the table (the ctor points +0x30/+0xB80 at +0x3370) |

The ICamera virtuals the manager uses: vf 1 Update, vf 2 Activate-request, vf 0x1C Activate(previous), vf 0x20
Deactivate(next), vf 0x24 SetSubject(instance), vf 0x2C GetType (the table index), vf 0x30 GetPosition (+0xC0),
vf 0x38 GetMatrix (+0x10, 4×4), vf 0x40 GetFov (+0xB0), vf 0x0C CopyFrom(camera). BaseCamera fields: +0x10 matrix,
+0x50 its inverse, +0x90 position, +0xB0 fov, +0xC0 position copy (D: 0x5E1C70, 0x5E1C90, 0x5E1E50, 0x60B3C0).

CameraManager fields (D unless marked):

| Offset | Meaning | Evidence |
|---|---|---|
| +0x04..+0x40 | camera table, types 0..15 (type 0 unused) | 0x5F3A70 (V), 0x5EB300 (V) |
| +0x44 | active camera | slot 56, 0x5E8630 |
| +0x48 | slot 58 arg masked by +0x7C | 0x5E8660 |
| +0x4C | **state**: 0 gameplay, 1 combat, 2 scope, 3 scripted, 4 transition, 5 debug/model view | 0x5EDD70 / 0x5EDEB0 / 0x5EE190 / 0x5EDE20 / 0x5EB310 / 0x5EB3C0 |
| +0x50 | state before scope | 0x5EE190 |
| +0x54 | **current Lara-camera mode** (1 on foot, 4 free climb, 11 hang …) — a field; slot 2 returns it | 0x5E7FE0 writes it, 0x5EB190 reads it |
| +0x58 | requested mode | 0x5E8210 |
| +0x5C | seconds in the current mode | 0x5E7FE0 |
| +0x60 | gameplay type to (re)enter (default 11) | 0x5EDD70, ctor |
| +0x64 / +0x68 / +0x6C | scope / combat / scripted type | 0x5EE190, 0x5EDEB0, 0x5EDE20 |
| +0x70 | state saved by a transition | 0x5EB310 |
| +0x74 | markup Lara is on (camera records hang off it, World.cpp:1059) | 0x5F2E00 |
| +0x78 | seconds since the last mode request (−1 = stopped) | 0x5E8210, 0x5F3C70 |
| +0x7C | byte: scripted camera holds (gameplay requests are queued in +0x60) | 0x5EDD70 |
| +0x7D | byte: mode comes from markup | 0x5E8210 |
| +0x7E | byte: frozen — no platform carry, no self-motion (mode request 0x37) | 0x5E8210, 0x5F3C70, 0x609830 |
| +0x80 | flags (bits 0..2, slots 35..43) | 0x5EB1A0.. |
| +0x84 / +0x88 | types pending while anchored / in combat | 0x5EDFC0 |
| +0x8C + 0x20·i, count +0x10C | registered camera instances (records with an instance handle at +0x18) | 0x5E84A0, 0x5F2A70 |
| +0x110 | current registered camera instance | 0x5E84C0 |
| +0xB80 | the Lara camera in use (= +0x3370) | 0x5EDF50 |

State machine (+0x4C):

```
            SwitchType(1|11|10|13)            SwitchType(2|12)
  ┌───────────────► 0 gameplay ◄──────────────────────► 1 combat
  │                   │  ▲  SwitchType(14)        ▲
  │                   ▼  │                        │
  │                2 scope  (+0x50 = state before) │
  │   SwitchType(4|7|8|15) ──► 3 scripted ─────────┘
  │                   any blended switch ──► 4 transition ──(elapsed ≥ duration−0.01, 0x5EE3C0)──► state +0x70
  └── debug/model-view keys ──► 5 debug
```

Slot 20 (cut) and slot 21 (blended) differ only in the last argument (0 / 1) passed to 0x5EDD70 / 0x5EDE20 /
0x5EDEB0. Script callers: `gamebase` natives 0x7767A0 (slot 20), 0x775F60 / 0x7767C0 (slot 21) (vcall_edges.tsv).

## 3. Per-frame order (0x5F3C70)

1. `dt = timeMult / 30` (V). Unless frozen, `0x5E7EF0(dt)` stores Lara's platform-relative move at 0xE91F20.
2. Forced swaps: an active ExplorationCamera becomes the Lara camera; an active CombatCamera becomes
   LaraCombatCamera.
3. Drop dead registered camera instances (0x5F2E90); commit the requested mode (0x5E7FE0); track the markup she is
   on (0x5F2E00); enter or leave the vehicle camera (0x5F2760: type 13 via a transition while player +0xF00 is set).
4. Anchored camera finished (0x5E0DA0) → back to gameplay (blended).
5. State 4 and transition elapsed ≥ duration − 0.01 → 0x5EE3C0.
6. Debug key (input +0x450 bit 4): Lara camera vf 0x54 or exploration 0x5F6430.
7. Active camera vf 1 (Update). 8. `+0x78 += dt` when ≥ 0.

## 4. Mode requests (slot 17, 0x5E8210) and markup modes

`RequestMode(m)` (V for the compares): no-op if `m == +0x58`. Clears +0x7E. Then:

- `m == 0x37` (55): sets +0x7E (freeze: no carry, no self-motion) and returns; the mode is unchanged.
- `m == 0x35` (53), `0x38`, `0x39` (56, 57): if Lara hangs on a markup (0x5E7D70) or, for 56/57, stands on one
  (0x5E7D40), the mode becomes the markup's (0x5E7DB0) and +0x7D is set. Otherwise, if not markup-driven and
  more than 0.5 s passed since the last request (+0x78) and the current mode is not 8, the mode becomes 1.
- otherwise `+0x58 = m`, +0x78 = −1, +0x7D = (m is 0x2A or 0x2B).

Markup type → mode (0x5E7DB0, D): 1 → 11 (21 when the previous mode was 0x29 / 0x38), 9 → 10, 15 → 17, 13 → 16,
14 → 3, 16..20 → 4, 4 / 6 → 9, other → 1; Lara's hang markup in hang state 4 → 21. This matches the markup-type
table World.cpp:883 quotes from 0x5F9960 for ladders 9→10, perches 13→16, pillars 15→17, beams 14→3, poles 4→9.

Commit overrides (0x5E7FE0, D): a requested 1 / 0x30 / 0x33 / 0x34 becomes 0x33 when Lara has a combat target
(player +0x504 → +0x50), else with the combat camera active 0x34 (target flag bit 27) or 0x30 (bit 10 clear);
19 / 0x36 becomes 0x36 under the combat camera with bit 10 clear.

## 5. Transition camera

- Start (0x5EB310 → 0x60B010 sets +0x140 = target, then Activate 0x60B4F0): snapshot of the previous camera's
  matrix, position (+0x100), fov (+0x110) and type (+0x114); `elapsed +0x144 = 0`; `duration +0x148 =
  0x60B210(fromType = +0x114, toType = target vf 0x2C)` from the transition resource (id +0x14C, else global data
  +0x124): a table of `{from, to, seconds}` (12 bytes; count +4, entries +8) with a default (+0); anchored→anchored
  takes the anchor record's +0x58.
- Update (0x60B3C0): updates the target first, `t = min(elapsed/duration, 1)`, linear position, quaternion slerp of
  the rotation, linear fov; **`elapsed += 1/30` per call, a fixed step** (V: `fadd QWORD [0x843CB0]` = 0.0333).
  [I, medium] The game loop calls the camera once per game frame, so a transition's wall-clock length scales with
  the frame-rate lock (gameTracker +0x120).
- End: CameraManager_Update (step 5) → 0x5EE3C0 re-enters the saved state with the target's type, cut.

## 6. Moving platforms (OU-CAM-001)

**Carrier selection (0x7027B0, V for the velocity branch).** The player data at `*0xFADB58` names what she rides,
first match wins: (1) handle +0x610 with segment +0x618; (2) the markup she is on (+0xC94: its instance +0xC, segment
from its +0x20 → +0x30); (3) handle +0x420, segment +0x428; (4) the player instance's (`*0xE7F094`) +0x228 handle,
segment +0x226. A candidate needs current and previous segment matrices (instance +0x90, +0x94). [I, low] (1) and
(3) are the grabbed / swung object.

**Velocity:** a physics carrier (instance +0x274 ≠ 0) gives the rigid-body point velocity at her position
(0x4D8F70: `linear +0xA0 + angular +0xB0 × (p − com +0x80)`); otherwise 0x5D75B0 moves her root (instance +0x94 →
+0x30) from the segment's old matrix to its new one and divides by `min(timeMult, 2)` — a velocity **per 30 Hz
frame**.

**Focus (0x609830, V):** when the focus subject (+0x30) is the player instance and the manager is not frozen,
`v = carrierVelocity · (dt · 30)` (V: `fmul QWORD [0x8439B8]` = 30.0, then 0x435470 scales the vector) is added to
the previous focus **before** the dead-zone lag 0x609440: the focus rides the platform and only her motion relative
to it is eased. LaraCameraRewrite has two trackers, +0x3B0 and +0x620 (0x6022F0, V) [I, medium: focus record and
look target].

**Position (0x5FF650, V tail):** first thing in 0x600710: if the carrier handle and segment equal last frame's
(+0x1C0/+0x1C4 handle, +0x1C8 segment), the camera position (+0x90) and proposed point (+0x150) are transformed by
`inverse(previous carrier matrix +0x180)` then the current matrix; the current matrix is stored at +0x180
(5FF7BD `lea ecx,[ebx+0x180]; call 0x431E10`) and the handle at +0x1C0..+0x1C8. Skipped when frozen.

**Her move (0x5E7EF0):** the move the camera force sets read is her displacement minus `timeMult · 30 · v`, i.e.
relative to the platform.

## 7. Differences vs OpenUnderworld

| # | tru.exe | OpenUnderworld | Evidence |
|---|---|---|---|
| 1 | Focus pre-moved by the carrier velocity × dt·30 before the dead-zone lag (0x609830) | Done (OU-CAM-001): `*focus_ += carrierMove` (her root's displacement by the markup she is on / the dynamic she stands on, `World::tickObjects`) before the lag | `StreamReal.CameraRidesTheYachtDeck`: lag 0 ticks (was 0.75 s) |
| 2 | Camera position and proposed point transformed by the carrier's frame delta (0x5FF650) | Moved by the same displacement, without the carrier's rotation: tru.exe's deck camera keeps camera − pivot z within −84.2..−81.4 while the hull tilts up to 3.7° (a rigid tilt carry would swing it ~±55 at d 948); whether a loose body's segment matrix carries the tilt, or +0xD0 / the limits remove it, is [U]. A platform that turns about the vertical does not turn ours | Wine `deck-*.txt`; ours full-frame 439..508 vs translated 486.4..490.1 camera − root (tru.exe 369.0..374.9) |
| 3 | Force-set "her move" is platform-relative (0x5E7EF0) | Done: the move subtracts `carrierMove` | World.cpp `move` |
| 4 | Mode requests go through slot 17 (0x5E8210) with markup overrides 53/56/57, freeze 55, 0.5 s fallback to 1; field +0x54 = committed mode | Mode chosen per player state in `updateOrbitCamera` (no freeze 55: ours always carries) | Comments corrected: field +0x54 = committed mode (0x5E7FE0 / 0x5EB190), vtable offset 0x54 = slot 21 SwitchTypeBlended |
| 5 | Combat target overrides the mode to 0x33 / 0x34 / 0x30 / 0x36 (0x5E7FE0) | Only mode 23 (manual aim) is modelled | World.cpp:910-914 |
| 6 | Camera types switch through a TransitionCamera with per-(from, to) durations from a resource and a fixed 1/30 s step | Reveal splines blend in/out with our own times (OU-CAM-004); no general transition camera | src/game/Camera.cpp; BUGS OU-CAM-004 |
| 7 | Vehicle: VehicleCamera (type 13) entered via a transition while player +0xF00 is set | `kDistance = 1711` on the bike, our ratio | World.cpp:986 "[I] VehicleCamera"; OU-CAM-003 |
| 8 | Scope (type 14) and LaraCombatCamera (type 12) are separate cameras | One orbit camera with modes | World.cpp |

Camera distance on the l2_cliffclimb yacht deck (OU-CAM-005, tru.exe 948.2 in mode 1, ours 1264.22): it is the target
distance +0x1320 (0x5FFB00 returns +0x1D4 to it), held at 948.2 for 15 s of idling and 5.5 s of running across the deck,
so it is a set value, not a pull-in. Writers of +0x1320: the settings' +0x60 (0x5FEF60, 1264.22; the settings resource
is +0x1D0 when set, slot 0x28 via CameraManager 0x5E8180, else global +0x144 = DTP 0x14, the only camera-settings DTP
in generalbank / l2_cliffclimb / l1_seatop / every object DRM), and commands 2 / 0xD of a command list (0x5FF080
mode list; 0x6019D0 for camera-trigger records). Camera triggers: 0x5CD520 tests trigger volumes against Lara
(0x5CD1B0: 2 enter, 3 leave) and calls CameraManager slot 27 0x5F29A0 / slot 28 0x5F2A70 with the trigger instance;
0x5E85B0 copies its intro data +0x78 record into +0x8C (≤ 4, "exceeded kMaxActiveTriggers"); 0x601D30 applies the
mode-list entry, the triggers with +0x14 = 0, the mode records +0x1348 / +0x1344, the triggers with +0x14 ≠ 0, then
slot 5's record (+0x110); the last command 2 / 0xD wins. l2_cliffclimb's level DTP 0x2b54 holds four such
volumes by the yacht (box, half extents, Euler z 43.1°): 0x2b6d at (−34899, −15044, 322), half (1095, 557, 451),
commands [2 → 632.11, 0x18 (4, 300)]; 0x2b6e at (−35813, −15914, 1708), half (711, 1128, 479), [0xD → 948.17 (cap
790.14 unused), 0x18 (5, 250), 0x1C]; one at (−36224, −16340, 295) [0x12 …, 0x15]; 0x2b6f at (−38213, −18283, 810),
[0xD → 3160.56, 0x18 (−60, 300)]. Mode 8's list entry also has command 2 → 948.17. With the Wine deck position (ours
(−34816, −14652, 211), test point +200) she is inside 0x2b6d (632.11) and outside 0x2b6e, which contradicts the measured
948.2, so which record sets 948.2 is not established [U]; ours has no camera triggers. Not fixed.

## 8. Unknowns

- Who sends requests 0x2A/0x2B/0x30..0x39 (player state properties; message 0x44E → 0x7683B0 per World.cpp:878).
- LaraCamera (+0xB90) is built but not in the table: dead code or a debug toggle [U].
- The ICamera slots 0x28 / 0x4C (set / get resource id, 0x5E8180) are read from two callers only.
- Whether the transition step is per render frame or per fixed game step under the 60 fps lock (needs a Wine trace,
  W3-9).
