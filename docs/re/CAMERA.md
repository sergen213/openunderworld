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
| 9 | Camera trigger volumes (0x5CD520 → 0x5F29A0 / 0x5F2A70 → 0x601D30 / 0x6019D0) override the camera while Lara is inside | Done (OU-CAM-005, OU-CAM-006): distance 2 / 0xD, pitch set by id 1, angle overrides 3–6 (applied when the trigger list or mode changes), look limits 7 / 8, cut 0xC (0x6017D0), inline force sets 0x17 / 0x18 with their forces, focus points 0x14 (0x5F0B30..0x5F12F0, 0x6009C0), 0x15 (no ray probe), tracker offsets 0x19 / 0x1B (pivot) and 0x1A / 0x1C (look point), level records 0x12; volumes anchored to a moving object ride it. Not modelled: 9 (no volume carries it), 0x16 (LaraCombatCamera), kind-1 blocks (AnchoredCamera), the focuspoint script type's Enable / Disable. §9 | Wine `w2/DeckCam6/`, `w2/CamZones7/`, `w2/CamRest8/`; `ScriptRealData.CameraTriggerCommandSurvey`, `CollisionRealData.FocusPointSurvey`, `StreamReal.CameraRidesTheYachtDeck`, `StreamReal.CameraTriggerVolumeSetsDistanceAndPitch`, `StreamReal.CameraTriggerForceSetEasesPitch`, `StreamReal.CameraTriggerRecordHoldsTheCamera`, `StreamReal.CameraTriggerFocusPointTurnsTheLook`, `StreamReal.CameraTriggerCutPutsTheCameraAtItsAngles`, `StreamReal.CameraTriggerPitchOverrideAndLimits` |

Camera distance on the l2_cliffclimb yacht deck (OU-CAM-005, tru.exe 948.2 in mode 1, ours was 1264.22): it is the
target distance +0x1320 (0x5FFB00 returns +0x1D4 to it), set by a **camera trigger volume**. Writers of +0x1320: the
settings' +0x60 (0x5FEF60, 1264.22; the settings resource is +0x1D0 when set, slot 0x28 via CameraManager 0x5E8180,
else global +0x144 = DTP 0x14), and commands 2 / 0xD of a command list (0x5FF080 mode list; 0x6019D0 for trigger
records). Triggers: 0x5CD520 tests the trigger instances against Lara (0x5CD1B0, her root + 200: 2 enter, 3 leave)
and calls CameraManager slot 27 0x5F29A0 / slot 28 0x5F2A70; 0x5F29A0 takes the block at the trigger's intro data
+0x18 (0x5E7BF0: kind +0 = 0 needs a command count +0xC; kinds 1 / 2 name another record) and 0x5E85B0 copies it into
CameraManager +0x8C (≤ 4, "exceeded kMaxActiveTriggers"), with the instance handle at +0x18. 0x601D30 applies the
mode-list entry, the triggers whose priority byte (+0x14 of the block = intro data +0x2C) is 0, the mode records
+0x1348 / +0x1344, the triggers with priority ≠ 0, then slot 5's record (+0x110); commands are 0x18 bytes {type, 20
bytes} (0x6019D0 jump table 0x601CC0): 1 / 0x10 pitch / yaw set by id, **2** distance, **0xD** distance (and, with
byte +0xC, a cap +0x1340), **0x17 / 0x18** an inline yaw / pitch force set {angle, speed, dir, count, forces}
(0x609C40 on +0x8F8 / +0xDA0; with count 0 its strength is 1, 0x607B90), 0x12 a level camera record, 0x1B / 0x1C
tracker records (+0x3B0 / +0x620 via 0x5FE730). Wine 2026-11-19 (`openunderworld-work/w2/DeckCam6/`, `camtrig.py`):
standing on the deck, CameraManager +0x10C = 1 trigger: triggervolume intro data with half extents (710.6, 1127.7,
478.9), priority 1, commands {0xD 948.17 (cap 790.14, flag 0)}, {0x18 5° 250°/s, dir 0, count 0}, {0x1C (0, 0,
−79.01), no blend}; the instance's intro position is ours (−34899.2, −15044.1, 322.4), Euler z 43.1° = our
l2_cliffclimb volume **3852**, and its instance position moves with the yacht (+0x20 = intro + (6, 78, 168) at the
sample). The earlier reading of DTP 0x2b54 paired the boxes with the wrong command lists: 3851 at (−36224, −16340,
295) half (1095, 557, 451) has [2 632.11, 0x18 (4, 300) with a force], 3853 at (−35813, −15914, 1708) half (1767,
1501, 781) [0xD 3160.56, 0x18 (−60, 300)], 3854 at (−38213, −18283, 810) [0xD 790.14, 0x18 (0, 10)]. The pitch set
explains the rest of the OU-CAM-001 height residual: camera − pivot z −84.2..−81.4 = −948.2 sin 5°. A second spot,
volume 3857 on the cliff path ([2 790.14], [0x18 −17° 300°/s]), with her held at its centre: one trigger, distance
790.1, camera 231.0 over the pivot (790.1 sin 17°). Ours now applies 2 / 0xD and inline sets without forces in
`World::updateOrbitCamera` (row 9 above): deck distance 948.2, camera − root 376.0 (tru.exe 369.0..374.9; the 1.1 over
is our pivot, 458.6 against 451.6..457.5); 3857: 790.1 and +231.0.

## 8. Unknowns

- Who sends requests 0x2A/0x2B/0x30..0x39 (player state properties; message 0x44E → 0x7683B0 per World.cpp:878).
- LaraCamera (+0xB90) is built but not in the table: dead code or a debug toggle [U].
- The ICamera slots 0x28 / 0x4C (set / get resource id, 0x5E8180) are read from two callers only.
- Whether the transition step is per render frame or per fixed game step under the 60 fps lock (needs a Wine trace,
  W3-9).

## 9. Camera trigger volumes: command survey (OU-CAM-006)

A triggervolume's intro data +0x18 is a camera block (0x5E7BF0 validates it): kind +0 = 0 with a count +0xC and a
pointer +0x10 to 0x18-byte commands {u32 command, 20 bytes}, priority byte +0x14 (the copy CameraManager +0x8C keeps,
≤ 4, 0x5E85B0); kind 1 = +4 a record of type 4 and +8 a DTP id: 0x5EE4A0 switches to the AnchoredCamera (type 4,
+0x7C0) with it. 0x5EE4A0 also handles commands 0x10 / 0x11 / 0x13 of a kind-0 block (AnchoredCamera, AutoSplineCamera,
FollowSplineCamera) — none occur in a volume. 0x601D30 hands every active block to 0x6019D0 (jump table 0x601CC0)
whatever its kind: priority-0 blocks before the mode records +0x1348 / +0x1344, the others after; the last writer wins.

Survey of the 82 units (`ScriptRealData.CameraTriggerCommandSurvey`; 459 kind-0 volumes carry commands, 58 of them with
priority; 9 kind-1 volumes: l1_kraken_room 49 / 291 / 327 / 328 / 353, l2_maproom 2670, l2_puzzleroom 2978, l4_pool
5514 / 5518 — 8 of them have no record at +4, so 0x5E7BF0 refuses them):

| Command | Uses | tru.exe (0x6019D0 unless noted) | OpenUnderworld |
|---|---|---|---|
| 0 | 4 | nothing | — |
| 1 | 4 | pitch set by id (0x5F98E0: global data +0x168 → 0x609C40 on +0xDA0): ids 0x1A, 6, 0 | done: sets 0 / 6 / 26 added to `kPitchSets` (global data read under Wine) |
| 2 | 129 | distance +0x1320 | done |
| 3 / 4 | 2 / 7 | yaw / pitch override by id (0x5FE7D0 / 0x5F9910: global data +0x170 / +0x174, 12-byte {angle, speed, hold} → 0x609F10 on +0x1248 / +0x1278): yaw id 7, pitch ids 9 and 1 | done (`kYawOverrides` / `kPitchOverrides`); 4 Wine-verified at 4468 |
| 5 / 6 | 1 / 15 | inline yaw / pitch override {angle, speed, hold} (0x609F10) | done (same functions as 3 / 4) |
| 7 / 8 | 1 / 4 | yaw / pitch limits {centre, range} (0x609C00 → 0x607A30 on +0x890 / +0x8C4, as the mode lists' 7 / 8; 0x608700 clamps to centre ± range) | done; 8 Wine-verified at 4349 (7: centre 0 in its one use) |
| 9 | 0 | +0x133C (as a mode list's 9, 0x5FF27D), handed by 0x602690 to the object at +0x2800 (its +0x5C) while +0x2814 is set (2 with mode 6: a spline path, 0x5F1EF0) | nothing to model: no volume carries it |
| 0xC | 2 | flag byte +0xD → 0x6017D0 at the end of 0x601D30: a cut | done (below): l1_krakenstairs_b 599, l7_arctic_sea 9226 |
| 0xD | 109 | distance +0x1320 (byte +0xC: cap +0x1340, read by 0x6000EF) | done (distance; the cap not) |
| 0x12 | 116 | level camera record (arg 0 → unit data +0x20C, 0x70 bytes) | done: the unit's record list (CollisionMesh::cameraRecords), in every mode, under a markup's record unless the priority is set |
| 0x14 | 53 | flags focus point `id` of her unit (unit data +0x200 count / +0x204 records of 0x100 bytes, the `focuspoint` script type 0x7758B0..0x775C20) with 0x20 at +0xFC (0x5ED2D0) until 0x601D30 runs again (0x5FEE10 → 0x5F1480 clears them all first) | done (below; `World::updateFocusPoints`); Wine-verified at 3859 |
| 0x15 | 33 | clears +0x1338: no on-foot ray probe (0x600591) | done |
| 0x16 | 54 | LaraCombatCamera distance cap +0x14C (0x603190 / 0x603150 copy the active triggers' 0x16 into it; 0x6019D0 skips it; 0x603C40 returns min(that camera's distance, +0x14C)) | not modelled: ours has no LaraCombatCamera (combat aim = mode 23) |
| 0x17 / 0x18 | 24 / 155 | inline yaw / pitch force set {angle, speed, dir, count, forces} (0x609C40); 23 / 66 carry forces (types 2, 3, 5, 7) | done, with their forces |
| 0x19 / 0x1A | 65 / 73 | (x, y, z) onto the focus / look tracker (+0x3B0 / +0x620 +0x20, 0x5FE730 → 0x608280) | done (every offset is (0, 0, z); world frame [I]) |
| 0x1B / 0x1C | 47 / 51 | as 0x19 / 0x1A, blending over arg 3 s when byte arg 4 is set (0x608200) | done |

The camera orbits the focus tracker's point (+0x100; 0x600B20: +0x1D4 = |camera − focus|) and looks at the look
tracker's (0x6009C0: +0x160 = look point − camera, blended toward the +0x14C0 point by +0x204), so 0x1A / 0x1C tilt the
view without moving the camera. Ours: `World::cameraPivot()` and `camera().target` = the pivot + the look offset.

Wine (2026-11-19, `openunderworld-work/w2/CamZones7/camz.py`, l2_cliffclimb; our frame = tru.exe's − (12446.2,
59357.3, −19003.8)):

- 3852 (yacht deck; {0xD 948.17}, {0x18 5° 250°/s}, {0x1C (0, 0, −79.01)}): look tracker +0x20 = (0, 0, −79.01), the
  pivot 5.00° up from the camera, the look vector 0.22° up (79.01 under the pivot). Ours: 0.22°.
- 3859 ({0x14 3}, {0x18 −6° 300°/s, force type 5 k 2}): the pitch settles at −6.00°; after a mouse nudge to +13.4° it
  returns at 19.5°/s at Wine's ~31 fps = 300 × min(1, 2 dt) per frame (10°/s at 60 Hz). Ours: −6.00°, at most 10.0°/s.
- 3861 ({0x12 → record 7}, priority 1, mode 1): +0x12D0 = (unit 9, record 7); the record (type 0, one point, 0°/s, no
  forces) replaces the forces and turns at 0°/s, so the camera holds where it was (13 s, no motion). Ours: the record
  engages, no force moves the camera while she walks.
- Riding: the yacht volumes 3851..3854 are anchored to ms_uo_yacht 2566 by their script (instance 23, `anchor()`); in
  tru.exe the volume instance links to the yacht (+0x214 → the yacht's link node) and moves with it. Volume − yacht in
  tru.exe: 3852 (1497, 1551, 811), 3851 (381, 400, 746); ours (marker frame, yacht at rest): (1547, 1530, 685),
  (408, 401, 670) — the marker, not the intro (the intros are 552 / 2216 from the yacht origin; tru.exe 928 / 2303).
  `script::Runtime` tests a volume whose body or offset is set in that frame (`World::poseAnchored`).
- Not separable at 3845 ({0x12}, {0x15}, {0x1C −158.03 0.2 s}): held mid-air she hangs (mode 11 / 37), and the mode
  records also set the look offset (0, −31.61, −158.03), the probe 0 and a record.

**Angle overrides (3..6) and limits (7 / 8)**, agent/CamRest8. 0x601D30 runs when the mode or the active trigger list
changes (enter / leave via 0x5F29A0 / 0x5F2A70, mode via 0x6020E0 / 0x6021B0), not every frame; an override no trigger
sets is switched off there (flags +0xB / +0xC → 0x608F10). Override object (+0x1248 yaw, +0x1278 pitch): +0 owner, +4
cur {angle, speed, hold}, +0x10 target {angle (deg + 90) rad, speed rad/s, hold s}, +0x1C clock, +0x20 time since set,
+0x24 / +0x28 / +0x2C own limits (no writer seen). 0x609F10: cur += (target − cur) × min(1, 4 × clock), target = the
record, clock = time = 0 (null record: 0x608F10, target 0). Active while |target speed| > 0.001 (0x5FA4A0). 0x5FFB00 runs,
per controller, a level record (0x5FA500) else an active override (0x609D40, **not** scaled by the look-idle weight:
0x5FFE47 / 0x5FFEB8 skip the fmul) else the force set; look input switches both overrides off (0x5FFD18), and so does a
level record every frame (0x5FF930 → 0x5FFA1F). 0x609D40: speed and hold = mix(cur, target, min(1, 4 × clock)); the angle
turns by sign × min(|wrapped gap|, speed × dt); once within 1° after more than `hold` seconds it switches itself off
(0x608F10 + 0x607CA0 clock 0.25); time += dt. The clock runs at 1 × dt up to 0.25 every frame, on or off (Wine: written
to 0 it reads 0.067 / 0.117 / 0.183 / 0.25 at 65 ms intervals; its writer was not found among the absolute-offset uses).
Global data (*0xE7EE50) lists read under Wine (`w2/CamRest8/gd.py`): +0x164 18 yaw sets, +0x168 36 pitch sets, +0x170 9
yaw overrides, +0x174 12 pitch overrides; the volumes use yaw override 7 (0°, 1500°/s, 0), pitch overrides 9 (−5°, 1500°/s,
0) and 1 (−35°, 80°/s, 0), pitch sets 26 (−15°, 200°/s, no forces), 6 (0, 0) and 0 (0°, 90°/s, force type 2 k 0.1).
The list +0x8C is in entry order: entering appends (0x5E85B0), leaving moves the last entry into the freed slot (0x5F2A70,
`rep movs` of 8 dwords); 0x601D30 walks it in that order (priority 0 first, then the rest), so the volume entered later
wins — ours keeps the same list (`OrbitState::triggers`, saved).

Wine (2026-11-19, `openunderworld-work/w2/CamRest8/fast.py` / `camz.py`, l3_main_bedroom after a new game, tru.exe frame):

- 4468 ({2 790.14}, {4 id 9}) inside 4476 ({2 948.17}, {0x19 / 0x1A z −126.42}, {0x18 −10° 1000°/s, one force}),
  walked in with the pitch at −10.00: distance target 790.1 (4468 is applied after 4476); +0x1278 = cur (0.198, 3.493,
  0) = id 9's target × 0.133, target 0, clock 0.25, time 0.017 — the override blended its speed in for two frames, turned
  the pitch 1.7° + 3.3° to −5, switched itself off (hold 0), and 4476's set took the pitch back to −10.00. Leaving
  switched both overrides off (clocks 0.05 one sample later). A teleport into a volume is no test: tru.exe's camera
  reset sets the pitch override itself (current angle, 300..400°/s) after the trigger's.
- 4349 (l3_bedroom_hall, {8 −10 10}): +0x8C4 = centre 1.396 (80°), range 0.175 (10°); the mouse moves the pitch no
  further than 0.00 up and −20.00 down (ours: pitch within [c − r, c + r] as the mode lists' 8).
- **Cut (0xC)**, agent/CamCut9: 0x6019D0 sets the flag byte +0xD of 0x601D30's flags (the same stack struct for the
  trigger, mode-list and record passes, objdump 601E07..601EAC), and 0x601D30 ends in 0x6017D0: both trackers jump to
  their goals with their blends done (0x6084A0(1): +0x250 = +0x260, point = goal), override clocks 0.25 (0x607CA0),
  force switch clocks done with her move history refilled (0x607C00), +0x100 = the look tracker's point, then
  +0x150 / +0x90 = the pivot + (target distance +0x1320, the angles of the active override's target (+0x1258 /
  +0x1288; 0x5FA4A0) else the force set's (0x5FA440 → 0x608980 at a full switch: base, or the signed target of a dir
  set, π/2 with side 0) else π/2), through the frame +0xD0 (0x5E0C20 → 0x435220) [I: her heading frame, as the force
  sets' angles]; with a level record (0x5FA500), its point +0x12B0. The distance state +0x1D4 and its speed +0x1D8 are
  not touched, so the same frame's 0x600200 proposes the point at +0x1D4 again, and 0x600710's step (the follow speed
  3793.1/s plus how far the proposed point moved from +0x150, which the cut set to the cut point) takes the camera
  straight to it: the cut snaps the angles, the 474.1 point exists only mid-frame. Wine 2026-11-20 (agent/WineCheck10,
  `w2/WineCheck10/b/walk1.txt`, `walk2.txt`: l1_krakenstairs_b, Forward held from the unit start, `ref rec` every
  ~16 ms frame plus mid-frame samples): she enters 599 at x −8043.5 (ours, box edge −8057.9); one sample shows +0x90 at
  474.1 behind her (yaw = hers to 0.01°, pitch −7.50°, the pivot 200.1 lower: the 0x19 offset −237.04 through the
  clearance), the next, 1..2 ms later in the same frame, +0x90 at the new +0x1D4 (1111.8 → 1097.6; 1207.8 → 1193.7 in
  the repeat) at those angles; +0x1D4 then eases toward 474.1 at ~1030 units/s and the pitch stays −7.50 while she is
  inside; she leaves at x −8058.9..−8067.2 and the distance returns to 1264.2. (`w2/CamRest8/k599tp.txt` was a teleport,
  whose own camera reset hid this; the point it counted inside was the teleport's, not the box's.)
  Ours (`World::updateOrbitCamera`): on the tick the trigger list changes with a 0xC in it, the camera and `orbit_.desired`
  (+0x150) at the cut point, so that tick's follow ends at the distance state; walked in from the start as above, ours
  enters at x −8048.0 with the camera at the distance state (1022.7), yaw = hers, −7.50°.
  `StreamReal.CameraTriggerCutPutsTheCameraAtItsAngles`. The box placement matches (entry / exit within a frame's travel
  of our box edge).
- **Focus points (0x14)**, agent/CamCut9: the 0x100-byte records at unit data +0x200 / +0x204 are the `focuspoint`
  script type's (NtFocusPoint 0x7B7A00; 1 0x7758B0 IsEnabled tests +0xFC & 0x04, 2 0x775910 Enable sets it, 3 0x775960
  Disable clears it). Record: +0 point (unit frame, + the unit's offset 0x5C1E20), +0x14 weight %, +0x18 radius, +0x1C
  view margin, +0x20 falloff distance with its 64-byte curve +0x24, fade in {+0x64 type, +0x68 length, +0x6C curve,
  +0xAC curved}, fade out {+0xB0, +0xB4, +0xB8, +0xF8}, +0xFC flags (0x01 not hidden, 0x02 radius, 0x04 enabled, 0x08
  in view, 0x10 falloff, 0x20 flagged by a trigger). Over the 82 units (`CollisionRealData.FocusPointSurvey`): 344
  records, none with 0x04, so only a trigger's 0x14 (or a script's Enable, one unit's scripts have the type [U which])
  gathers one; fades of type 0 (73) and 2 (271). Per frame from 0x6022F0, 0x5F1540 (the object at +0x14C0: point at +0,
  lists at +0x10 / +0x194):
  - 0x5F0B30 gathers, over the loaded units (stream tracker state 2), up to 32 records with +0xFC & 0x24 that pass the
    tests their flags ask for: 0x08 in the view frustum (0x5EA3D0: the view planes at DAT_00E91ED0, each distance less
    the +0x1C margin), 0x02 within the +0x18 radius of her root (instance +0x20), 0x01 not hidden from the camera
    position (0x493330, ray flags 0x302C5981);
  - 0x5F0DF0 adds each gathered one (weight 0) to the fading-in list and 0x5F0E90 moves each list entry no longer
    gathered to the fading-out list, but both loops keep only their last comparison (5F0E17..5F0E39: `dl` = match, no
    break): with one gathered point that is what it says; with two or more only the last gathered stays and it is added
    again every frame while another precedes it (up to 32 copies). Ours reproduces the loops as they are;
  - 0x5F0F70 raises each fading-in weight by its fade-in rate (type 0: stick length (0x5E9830) × dt / length, 1: her
    move this frame (0x5E97B0: |+0x20 − +0xC0|) / length, 2: dt / length, other: 1), capped at 1, curved by its curve
    when the byte is set; 0x5F1060 lowers each fading-out weight by its fade-out rate and drops it when it would reach 0;
    0x5F11C0 runs a third list (+0x330, 32 × 0x80 bytes) that nothing here fills [U];
  - 0x5F12F0: point = (look tracker point + Σ w p) / (1 + Σ w), w = +0x14 × 0.01 × the curved weight × (flag 0x10)
    curve +0x24 at clamp(|her root − p| / +0x20, 0, 1); 0x5ED260 adds the third list's;
  - 0x6009C0: +0x204 moves toward the look-idle weight (0x5FFA90, settings +0xC4 / +0xC8: 0 until 5 s after the last
    look input, 1 from 7 s) by at most 4 × dt; the look vector +0x160 = mix(look tracker point, +0x14C0, +0x204) − camera.
  Wine (w2/CamZones7 s3859.txt / s3859b.txt, l2_cliffclimb 3859 {0x14 3}: point 3 weight 65 %, radius 1295.8, margin
  695.3, Lara 1066 from it): the look vector crosses the pivot → point segment at 0.38..0.42 of the way, as
  0.65 / 1.65 = 0.394 predicts; after a mouse nudge 0.005 until the look-idle ramp (0.18, 0.36, 0.41 at the next
  samples). Ours (`World::updateFocusPoints`): 0.394, the ramp and the fade out on leaving;
  `StreamReal.CameraTriggerFocusPointTurnsTheLook`. [I]: our view test uses the last camera's frustum at the configured
  vertical FOV and 16:9 and treats the planes as facing out; the sight ray uses the camera mask; the gather order is our
  unit order (it matters only with two or more points, via the loops above). The scripts' Enable / Disable are not bound.

## 10. Traversal states: mode, mode list, pushes, matrix (agent/TraversalCam)

**Matrix rows (V-data + D).** The camera matrix (+0x10, vt 0x38 / 0x5E7850) is built at the end of 0x602690 by
0x5E0970 → 0x5E0900 → 0x49B820 from the position +0x90 and the look vector +0x160 (0x6009C0): row 0 right =
down × look, row 1 the camera's **down**, row 2 the **look** direction, row 3 the position; +0x50 its inverse (vt 0x3C /
0x5E7870). Checked on the Wine CAMDUMP of a camera pinned 1264 behind her and 300 up: row 2 = (0, −0.996, 0.092)
points from the camera at the pivot, row 1 = (0, −0.092, −0.996). The stick record's markup frame (0x6F8050, the
chart's cond 0x195) projects onto (right, down), the screen plane, and tests the look row's z (+0x196). Ours:
`ChartContext::CameraAxes{right, down, look}` (`Shadow::Impl::cameraRows`); before this it passed (right, look, up),
which put our "up" z near 1 where tru.exe's look z is 0.1..0.3.

**Which mode a traversal state asks for [O-data].** State property 1102 (0x44E) is the camera mode request
(traversal_states.json): ledge S434 / S436 / S443 / S446 / S447 / S506 (shimmy, idle hangs) 37; S254 / S257..S260 /
S452 / S505 11; S456 46; S542 42; S1197 / S1184 (pillar) / S1131 17; ladder S1051 10; poles S626 / S679 9; perch S825
16; beam S874 3, S155 / S1397 19, S1239 21, S1262 / S1265 and wall hug S1263 / S1264 22, S1043 / S1248 56, S525 57;
53 (= "the markup's mode", 0x5E8210) on beams S1003 / S1032..S1039, hurdle S512, landing S519, S276; aiming 23; and 1
on the states that leave them (S431 / S432 / S440..S442 ledge, S670 pole, S867 perch, S1064 / S1075 ladder, S1204 /
S1221 pillar, landings S1275 / S1276 / S1282, wall jump S1292 …). A state without the property keeps the last mode.

**Mode list [O-data: camera DTP 0x14 = pc-w\generalbank.drm DTP section 20, +0x144 count 25, +0x148 entries {n,
modes, n, commands (0x18 bytes)}].** {11, 15, 14} and {37, 39, 38} carry the same commands: 7 {0, 90, 70, 25, flag 1},
force set 0x10 15, look record 11 → 51, focus record 10 → 8, 8 {0, 55}, 15 (pull-in), 2 1896.33. {21, 22}: the same
with records 7 / 50 and distance 1580.28. 10 (ladder): 8 {0, 55}, 7 {0, 90, 70, 25, 1}, 2 1896.33, records 1 / 3,
15, pitch set 16. 4 (free climb): 8 {0, 55, 20, 200, flag 0}, 7 {0, 90, 70, 25, 1}, records 10 / 11, 15, yaw set 15.
17 (pillar): pitch set 16, 15, yaw set 1, 8 {0, 55}. 16 (perch): yaw set 5, pitch set 17, 15, look record 26. 9
(pole): yaw set 4, 15. 3 (beam): yaw set 6, look record 25, 15, 0x13. 47: records 53 / 54, 2 1580.28, yaw set 12, 7
{0, 90, 70, 25, 1}, pitch set 12, 15, 0x13. 5: records 56 / 57, 2 1580.28, yaw set 10, 7 {0, 70, 180, 30, 1}. 42 / 57:
inline yaw override 5 {0, 500, 0.25}, records 8 / 51. 46: records 1 / 3, pitch override id 8. 43: 0x14 158.03, pitch
override id 6. 18: pitch override id 10, pitch set 15. 7: pitch set 11, yaw set 9, records 48 / 49, 15. 6: 9 = 1.0,
yaw set 7, pitch set 8. 8: 2 948.17, records 2 / 0. 41: 0xE. 20: 8 {−40, 30}. 19: record 41, 8 {−5, 80}. 23: 2
632.11, records 22 / 35. 1 / 2: pitch set 4, records 1 / 3, 0x13.

**Soft yaw limit (D).** 0x607A30 stores command 7 as centre (c + 90°), half-range r, and with its flag byte a soft
half-range (arg 2) and speed (arg 3, deg/s), else soft = r, speed 100000. 0x600200 runs the proposed yaw through
0x608560 when the frame is not turned by look input: clamp to centre ± r, then beyond the soft range back toward it by
at most speed × dt. So hanging, shimmying, free climbing and on ladders the camera may sit up to 90° off behind her but
drifts back inside 70° at 25°/s. Ours: `Mode::yawSoft` / `yawSoftSpeed` (modes 4 / 11 / 10 and trigger command 7);
not blended over the limits' 0.25 s [I]. The pivot frame +0xD0 is her yaw (0x6022F0: Euler (0, 0, instance +0x38),
rate-limited by +0x132C when +0x1339 is set).

**Pushes (actions 2001..2005, D).** CameraManager vt 0xB0 / 0xB4 (0x5E8530 / 0x5E8570) call the Lara camera's vt
0x58 / 0x5C: 0x5FE9D0 stores the record id in +0x1F4 with a 0.1 s delay +0x1F8; 0x6025E0 then finds that id in the
settings' record table (+0x20C, 0x68-byte entries, count +0x3AC) and applies it (0x601340 → +0x134C): type 1 an offset
in the pivot frame onto a tracker (0x5E9920, with a blend), type 2 the same plus a yaw override by id (its side
mirrored to the camera's, 0x5F9860 / 0x5E8DA0) and a pitch override by id. 0x5FF3A0 (exit) clears it. States: 2001
(record 0) S434 / S435 / S1197 / S1198 ledge, S1052 ladder, S1233 beam, hurdles S486..S489 / S1166; 2002 (1) S436 /
S457, S1221 pillar, S1234 beam, S482..S485, S1168; 2003 (4) S448 / S449 / S453, S1241, S1147 / S1164; 2004 (5) S450 /
S451 / S454, S1240, S1148 / S1165; 2005 (pop) S517 / S518. Not ported (the chart's JEffect::CameraPush has no
consumer) [U: the record table's contents].

**The Wine harness's CAMFOLLOW (chart.c).** Every published frame it writes +0x90 and +0x150 = her position −
(dx, dy) × dist + (0, 0, height), not the matrix. The next update reads the angles back from there (0x600200 →
0x5FF320), keeps +0x1D4 and its speed, proposes, collides and follows (0x600710: the step includes how far the proposal
moved from the pinned +0x150, so it lands on the proposal). The corpus' CAMDUMP cameras are therefore tru.exe's update
from a pinned start, not its free camera: with the pin in front of her (chapel `ch_hang_*`, camera_follow (0, −1) while
she faces +y) the readback lands ~180° off her back, the hard ±90° clamp picks a side by the sign of the pivot's
sideways offset from her root, and the pull-in leaves the camera ~100 from the pivot. Replays reproduce it with
`World::pinCamera` (--camera-follow DX,DY,DIST,HEIGHT and a case's camera_follow), applied at the start of the orbit
update from her previous position, after the chart has read the last camera.

**Look-idle clock at a reset (D + data).** The reset (vt 0x8 0x600C10 / vt 0xC 0x600E30 → 0x5FF7F0; vt 0x6C / 0x74
0x602290) sets +0x1E4 = settings +0xC4 + +0xC8 = 5 + 2 s: the full look-idle weight, so forces and a markup's level
camera record act from the first frame after a spawn or teleport. Ours started it at 0 (5 s of no forces).

**Unpinned takes (WineChartTrace, `<family>/*_freecam`, tru.exe's own camera, CAMDUMP every tick).** Replayed with the
camera seeded at the case's `camera_at_key` (`World::seedCamera`), error after the key tick, ours tick t + 1 vs
Wine t (`openunderworld-work/tcam/camcmp.py`):

| case | camera position error mean / max | look direction error mean / max | note |
|---|---|---|---|
| ledge/hang_grab_freecam (bedroom 973) | 45.8 / 219.6 | 1.56° / 5.4° | ledge 973's record: 2 points, 100°/s, distance driven at 2528.45 /s, force 5 × 20 |
| ledge/hang_shimmy_left_freecam | 53.4 / 220.5 | 2.54° / 5.1° | |
| ledge/hang_shimmy_right_freecam | 20.6 / 225.9 | 0.88° / 5.2° | |
| ledge/hang_pullup_freecam | 175.1 / 592.0 | 6.72° / 15.5° | the climb up (S456) and standing after it |
| pole/tr_pole_grab_freecam | 415.3 / 481.3 | 2.10° / 2.8° | tru.exe's distance stays ~1900, ours returns to 1264.22 (mode 9 has no command 2) [U] |
| pole/tr_pole_swing_freecam | 422.1 / 719.9 | 3.20° / 4.8° | as above |
| pole/tr_pole_hoh_right_freecam | 302.6 / 749.2 | 8.86° / 20.5° | |
| chimney/jh_stick_jump_wall_freecam | 126.3 / 263.2 | 5.58° / 6.5° | |
| ledge/ch_hang_left_hold_freecam (chapel 1063) | 1140.8 / 1445.9 | 18.79° / 28.3° | no record on 1063; tru.exe holds ~590 from the pivot, 23° down |
| ledge/ch_hang_right_hold_freecam | 1014.5 / 1837.9 | 66.20° / 79.6° | tru.exe S451 (push 2004) where ours runs S431 |
| pillar/t1194_climb_up_t1_freecam | 574.5 / 907.3 | 26.40° / 36.3° | |

Before the reset fix the bedroom hang takes were 20° off (the record waited 5 s). The 0x195 class from each camera
(markup frame with the take's held key) agrees on every held tick of every take but tr_pole_hoh_right (224 / 260).
[U]: the chapel and pillar placements (tru.exe's camera much nearer the pivot: a collider or camera-mask difference, or
the pushes of actions 2001..2005, which ours does not port), the pole distance.
