# Original engine architecture (tru.exe, cdc engine) and its Linux counterpart

This is a reconstruction of how the original Windows game is organised, one subsystem at a time. Each section
lists the key original functions, the data structures that are already established, any known state machines,
dependencies, the Linux implementation in `src/`, the fidelity status and open questions. It is assembled from
the function database ([FUNCTION_DATABASE.md](FUNCTION_DATABASE.md), `db/functions.tsv`, `db/citations.tsv`)
and from the existing documents it cites. It records no new mechanics. The closing table maps each original
system to its Linux implementation and the work still required.

Snapshot: repo v0.51.0 + audit commits (2026-11-16), GOG `tru.exe` 1.1.0.0 (build date string "Dec 16 2008
18:30:39", [STARTUP.md](STARTUP.md)).

## Evidence legend

| Tag | Meaning |
|---|---|
| **V** | Checked at instruction level against `objdump` of tru.exe during this audit ([FUNCTION_DATABASE.md](FUNCTION_DATABASE.md) "Verification sample") |
| **N-h / N-m / N-l** | Name in `tools/ghidra/names/tru_gog.csv` with confidence high / medium / low (the CSV header defines these) |
| **C** | Role taken from a repo citation: the citing doc or code comment marks it [O] (observed in the exe, data or a Wine trace). Not re-verified in this audit |
| **I** | Inference: an engine-structure reading by this document, or a citation marked [I] |

Fidelity per subsystem:

- **decoded**: the original behaviour and data are read from the exe or data, and the Linux code follows them.
- **reconstructed-approximation**: the structure is known, but the Linux code substitutes its own solver,
  middleware or heuristic, or follows only part of the original.
- **unknown**: not analysed.

All numbers are from `docs/re/db/summary.json` unless stated otherwise. 1 321 functions (615 926 bytes, 17.5 % of
non-library code) are cited from code or tests.

```
WinMain ─ Win32_GameMain ─┬─ archives (InitArchive / InitPatchArchive) ─ RESOLVE (DRM sections) ─ PCRenderDevice resources
                          ├─ window + D3D9 device (PCDeviceManager)
                          └─ MainG2 loop ─ MAIN_ProcessMainState ─ MAIN_PlayGame ─ GAMELOOP_Frame
                                 │                                   ├─ GAMELOOP_Process: INSTANCE_ProcessAll (objects, Lara, AI, scripts),
                                 │                                   │   PHYSICS_StepWorld (cdc multibody physics), camera, streaming
                                 │                                   ├─ STREAM_PollLoadQueue
                                 │                                   ├─ GAMELOOP_DrawFrame (PCScene_Draw ... Font_Flush)
                                 │                                   └─ GAMELOOP_UpdateTime
                                 └─ MainG2_PlatformUpdate: FMOD/streams, input, job sync, PumpWindowMessages
```
[I] The diagram combines the functions named in STARTUP.md and TIMING.md; only the MainG2 loop body is **V**.

---

## 1. Engine init and shutdown

Shutdown decoded 2026-11-16: [SHUTDOWN.md](SHUTDOWN.md) (MAIN_Shutdown 0x5DF930, ~76 calls in order; Win32_GameMain tail).

| Address | Name | Evidence |
|---|---|---|
| 0x818421 | entry (CRT `WinMainCRTStartup`) | N-h |
| 0x47A280 | WinMain | N-h |
| 0x479C80 | Win32_GameMain: `-setup/-config` parsing, archives, `TRUMUTEX`, window, render device, `MainG2` | N-h |
| 0x47B530 / 0x51FBB0 | ShowSetupDialogIfNeeded / PCSetupDialog_Run (launcher dialog over settings 0x890388) | N-m |
| 0x4790A0 / 0x478BC0 | CreateGameWindow (class `TOMBRAIDER8`) / MainWndProc | N-h |
| 0x478640 | Win32_InitRenderDevice (`g_pcDeviceManager` 0xAD75E4 vt+0) | N-m |
| 0x5DF390 | MainG2_InitEngine (build date log, RESOLVE_Init, section type 13 = PDAMap) | N-h |
| 0x5DEF70 | MAIN_DoMainInit | N-m |
| 0x5DF930 | called on `MainG2` exit (shutdown path) | V (call seen), role I |
| 0x596CB0 / 0x478B00 | ERROR_Fatal / MainG2_ErrorHandler | N-h / N-m |
| 0x4A21D0 / 0x4A2680 | CPUCount / cdc_JobChainImplWithThreads_StartSystem | N-m |

- Data: settings block 0x890388, language table 0x890428 ([STARTUP.md](STARTUP.md)); `mainTracker` 0xE7ED60,
  `gameTracker` 0xE7F088 (N-h/N-m).
- Linux: `src/app/game/main.cpp` (SDL3 window, Vulkan/GL device, config), `src/launcher/` (Qt launcher
  replacing the setup dialog), `src/config/`.
- Fidelity: **decoded** for the order of initialisation ([STARTUP.md](STARTUP.md)); the Win32/D3D9 specifics are
  replaced by design. Shutdown (0x5DF930 and what follows) is **unknown**.
- Open: the Media Center relaunch flag 0x9EEDCA (the docs mark its meaning as inferred from its name); the job
  system's thread layout.

## 2. Main loop and timing

| Address | Name | Evidence |
|---|---|---|
| 0x5E0190 | MainG2: loop `MAIN_ProcessMainState(0xE7ED60, 0xE7F088)`; `STREAM_PollLoadQueue`; `MainG2_PlatformUpdate`; exits when 0xE7ED6C is set or the platform update returns false | **V** |
| 0x5E0090 / 0x5E0050 | MAIN_ProcessMainState (mainState switch 1..10) / MAIN_PlayGame | N-m / N-l |
| 0x5DFFE0 | GAMELOOP_Frame: Process → PollLoadQueue → DrawFrame → UpdateTime → frame counter +0x24 | N-m |
| 0x5DFBE0 | GAMELOOP_Process | N-h |
| 0x5DA230 | GAMELOOP_UpdateTime: 6-frame moving average (ring 0xE7EE10), clamp 66 666 µs, timeMult = dt / 33 333.33 × scale | **V** (average and clamp) |
| 0x597EE0 / 0x597E90 / 0x597F10 | TIMER_Init / TIMER_SampleMicroseconds (QPC × 10⁶ / QPF) / TIMER_GetTimeMicroseconds | N-h, 0x597E90 **V** |
| 0x47AFD0 | MainG2_PlatformUpdate (sound/stream dt = gameTracker+0x10C / 30) | N-m |
| 0x5DDDF0 | MainG2_LoadingFrame (inside loading wait loops) | N-l |

- State machine: `mainTracker->mainState` (1 = play game; 2..10 other states, [STARTUP.md](STARTUP.md)). Frontend
  state 8 = "PlayNewGameMovie" 0x5DB110 (C).
- Data: gameTracker timing fields +0xC8 / +0x100 / +0x10C / +0x120 / +0x148 and labels 0xE7F150..0xE7F1D0
  ([TIMING.md](TIMING.md)).
- Linux: `src/engine/FrameClock.hpp`, the `World::tick` 30 Hz fixed step in `src/game/World.cpp`, the loop in
  `src/app/game/main.cpp`.
- Fidelity: **decoded** (the timing model); the Linux build runs gameplay at a fixed 30 Hz, independent of the
  renderer (docs/GAMEPLAY.md), where the original uses a variable timeMult. That is a deliberate
  **reconstructed-approximation**.
- Open: mainState 2..10 handlers in full; the frame-rate lock (+0x120) behaviour under variable timestep.

## 3. Resource management and asset loading (bigfile, DRM, RESOLVE)

| Address | Name | Evidence |
|---|---|---|
| 0x473840 | InitArchive: spec mask 1, `ArchiveFileSystem_Open("BIGFILE.DAT")` | **V** |
| 0x478930 | InitPatchArchive (`PATCH.000` added in front) | N-h |
| 0x4730B0 / 0x4726E0 / 0x472670 / 0x4725C0 | ArchiveFileSystem_Open / FindEntry (`(rec & mask) == mask`) / CalculateHash / CRC32 | N-h |
| 0x4737F0 / 0x472B50 / 0x472B60 | MultiFileSystem_ctor / GetFS / GetDiskFS | N-h |
| 0x49A900 | sets `1 << Language` on every archive | C |
| 0x552560 / 0x551FF0 / 0x54F1B0 | RESOLVE_Init / RESOLVE_InitSectionHandlers / RESOLVE_RegisterSectionHandler (table 0xC56FC8) | N-h / N-h / N-m |
| 0x552BF0 / 0x552FC0 | ResolveDecompressor_ParseCdrm / Receiver_ReceiveData | N-m |
| 0x54EC60 | ResolveSection_ApplyRelocations | N-h |
| 0x477970 / 0x477990 / 0x54F430 | LOAD_UnitFileName / LOAD_StreamGroupFileName / LOAD_ImageFile | N-h / N-h / N-m |
| 0x54E200 | ObjectSection construct (section +0 = ObjectBase DTP id; +0xC8 object data) | C |

- Data: [CONTAINERS.md](../formats/CONTAINERS.md) (bigfile, CDRM, DRM v19), [FILESYSTEM.md](FILESYSTEM.md).
- Linux: `src/cdc/Cdc.{hpp,cpp}` (BigfileSet with patch first, CDRM, DRM relocations), `src/formats/`,
  `src/importer/`.
- Fidelity: **decoded** (ROADMAP "Bigfile index", "DRM v19 sections/relocs": tested). The extern/resource
  relocation semantics are marked unverified in the ROADMAP.
- Open: resource-relocation kinds; asynchronous read scheduling (`ArchiveFileSystem_RequestRead` 0x472890) and its
  priorities.

## 4. Scene, streaming and level loading

| Address | Name | Evidence |
|---|---|---|
| 0x5C1DA0 / 0x5C17D0 | STREAM_PollLoadQueue / STREAM_GetObjectTrackerByName | N-m / N-h |
| 0x5D13E0 / 0x5D10D0 | portal relocation of a neighbour unit | C |
| 0x5CEC60 / 0x5CF070 | stream portals bound as `cdc::ScenePortal`s | C |
| 0x5DF8C0 / 0x5DF760 | GAMELOOP_RequestLevelChangeByName / level change | N-m / C |
| 0x5DAB40 | quit / level change path from MAIN_PlayGame | C |
| 0x5C7CA0 | unit look blended with neighbours (fog, ambient, PSSM) | C |
| 0x5B3A20 / 0x5D2AA0 | instance moved to the unit it crosses into | C |
| 0x4A4D10 | cinematic stream names of a unit (Level +0x70 → +0x1DC / +0x288) | C |

- Data: [LEVELS.md](../formats/LEVELS.md) (unit, terrain, portals at Terrain+0x04, 0x100 B, intros, lights, nav),
  `g_streamTracker` 0xDBAB40 (N-m).
- Linux: `src/game/Stream.cpp`, `src/game/World.cpp`, `src/formats/Level.cpp`, `src/scene/`.
- Fidelity: **decoded** for portals, relocation, neighbour loading and unit transitions (ROADMAP: tested). The load
  queue's timing and prioritisation are **reconstructed-approximation** (background loads in our own scheduler).
- Open: STREAM queue internals; the memory budget and eviction rules.

## 5. Entity / instance management

| Address | Name | Evidence |
|---|---|---|
| 0x5B3090 / 0x5B6DC0 | INSTANCE_ProcessAll / INSTANCE_PreProcessAll | N-m / N-l |
| 0x5BCC30 | INSTANCE_IntroduceInstance (intro rotation to instance +0x30; scale only with flags[1] & 0x10) | N-h (this audit) |
| 0x5BB160 / 0x5BB640 | INSTANCE_IntroduceSavedInstance(WithIntro) (log strings) | C (strings) |
| 0x5BA810 / 0x5B5D20 | model setup / distance LOD switch (needs oflags 0x400000) | C |
| 0x5B3750 / 0x5BC4E0 / 0x5BD0F0 | INSTANCE_Post / ReallyRemoveInstance / BirthObjectNoParent | N-m |
| 0x5B3DD0 | INSTANCE_HideUnhideDrawGroup | N-m |
| 0x5BF770 / 0x5BF0E0 | OBTABLE_GetObjectID / object family (oflags2 0x400000 enemy, 0x80000000 physical) | N-h / C |
| 0x5BA480 → 0x5BA000 | birth effects of an instance | C |
| 0x5C2360 | dormant intros not introduced at load | C |

- Data: Instance +0x30 rotation, +0x100 intro, +0x160 hit primitives, +0x294 anim component, +0x2E0 IK system;
  ObjectBase +0xC8 object data ([LEVELS.md](../formats/LEVELS.md), docs/GAMEPLAY.md). Globals
  `g_instanceList` 0xD98D54, `g_objectList` 0xDB94D0 (N-m / N-h).
- Messages: instances talk through `INSTANCE_Post(message, value)`. Message ids are catalogued in
  docs/GAMEPLAY.md and SCRIPTS.md (e.g. 0x11C hit, 0x196 checkpoint activate, 0x30 water damage) (C).
- Linux: `src/game/Objects.cpp`, `src/game/World.cpp`, `src/game/Game.hpp`, `src/formats/Scene.cpp`,
  `src/formats/Births.cpp`.
- Fidelity: **decoded** for introduction, families, births and messages used by gameplay;
  **reconstructed-approximation** for the per-frame process order (our `World::tick` order follows
  docs/GAMEPLAY.md, not a traced INSTANCE_ProcessAll).
- Open: the complete process order inside 0x5B3090; the full message table.

## 6. Player controller (Lara)

| Address | Name | Evidence |
|---|---|---|
| lara.drm state chart (player data +0x12508; conditions table 0x8B2838, actions table 0x8B0A50) | data-driven player state machine | C ([O lara.drm] docs/GAMEPLAY.md) |
| 0x760C40 | indexes the 0x30-byte game-action table (chart inputs are actions, not buttons) | C |
| 0x73EDC0 | jump-to-target solver (velocity for a root delta under gravity) | C |
| 0x70F300 / 0x70EC60 / 0x710100 | jump sweep / ledge checks / first accepted box | C |
| 0x6FCB10 / 0x725B50 | capsule test / hit holder | C |
| 0x728650 / 0x727BF0 | ledge markup classification / clearance test | C |
| 0x5D3860 / 0x5D77E0 | water depth classes (0x100 wading, 0x40/0x20/0x10 swimming) / entry from the air | C |
| 0x755230 | breath drain while the head is under water | C |
| 0x742770 | balance controller update | C |
| 0x75AA50 / 0x75AF90 | DeathState_Entry / DeathState_Process (90-frame wipe) | N-m |
| 0x5DCE20 / 0x5D3AF0 | ground check (floor rule, slide surfaces 0x0004 / 0x0040) | C |
| 0x7607C0 | input timing layer (controller input rules) | C (ROADMAP) |

- State machine: player states S### and transitions T### of the lara.drm chart, with `PlayerSC_*` condition
  classes (e.g. `PlayerSC_Grapple::OverhangDetected` 0x73AB70 N-m, `PlayerSC_PushPull::MustDetach` 0x73F900 C).
  The chart is decoded per feature in docs/GAMEPLAY.md; there is no complete dump.
- Data: lara.drm player data offsets (+0x6DC fall start z, +0x8E0 water top, +0x9C8.. grapple, +0xC7C.. prismatic
  push, …) in docs/GAMEPLAY.md; [ANIMGRAPH.md](../formats/ANIMGRAPH.md).
- Linux: `src/game/Player.cpp` (3 439 lines), `Traverse.cpp`, `Climb.cpp`, `Grapple.cpp`, `WallJump.cpp`,
  `src/game/Input.cpp`.
- Fidelity: **reconstructed-approximation**. Clip choice, thresholds and tuning are read from lara.drm and the exe
  (C); the controller is a native C++ state machine rather than an interpreter of the original chart (I, from
  the structure of Player.cpp). Self-tests compare against Wine traces (docs/SELFTEST.md).
- Open: a full chart interpreter or dump; 0x74BCF0 (MoveDir) and 0x6FC6B0 (direction test) are marked not
  decoded in the citing code.

## 7. Physics and collision

| Address | Name | Evidence |
|---|---|---|
| 0x492B80 | PHYSICS_StepWorld (dt in 30 Hz frames, clamped to 2; 1.1538 = 30/26 sub-step) | N-m, C |
| 0x4F0AB0 / 0x4F1900 | MultibodySystem_Step / step chain | N-l / C |
| 0x4EFB20 | body integration: v *= clamp(1 − dt × damping, 0, 1), damping at body +0x154 (linear) / +0x158 (angular) | **V** |
| 0x48E690 / 0x4DD820 | physics-record damping → body / template defaults 0.01 | C |
| 0x48E710 / 0x48E330 / 0x4E9100 | shape only for primitives with flag 0x100 / mass / box inertia | C |
| 0x4DEE60 | buoyancy force of a primitive in a water volume | C |
| 0x6AD070 | water-volume update: gathers primitives via 0x4DEE60, re-arms damage | **V** (call) |
| 0x4923C0 / 0x48E040 / 0x4928C0 / 0x492980 | damaging contact handler / amounts / overlap pair handlers | C |
| 0x4935F0 / 0x493330 / 0x48EF50 | collision segment queries with flag masks / rays / filters | C |
| 0x5EB9B0 / 0x5EC360 / 0x462E10 | camera collider queries (face mask 2, skip group flag 0x200) | C |
| 0x4AACA0 | NtTerrainGroup::ResolveTerrainGroup | C |
| 0x5C2440 / 0x5C2480 | terrain group disable bits | C |
| 0x4F0B40 / 0x4F1390 | ragdoll setup / 0x78-byte bone records | C |

- Data: [PHYSICS.md](../formats/PHYSICS.md), [COLLISION.md](../formats/COLLISION.md); HInfo primitives (sphere
  +0x16 buoyancy, box +0x38, capsule +0x2C; damage s16 at +0x1C / +0x3E / +0x32).
- Linux: `src/physics/Physics.{hpp,cpp}` on Jolt Physics, `src/collision/` (BVH over unit terrain and markup),
  `src/game/Hazards.cpp`, `JointSound.hpp`.
- Fidelity: **decoded** for collision geometry, query masks, materials and damping values;
  **reconstructed-approximation** for dynamics. The original solver is cdc engine code: [BINARIES.md](BINARIES.md)
  §5.3 finds no Havok, PhysX or other middleware strings [OBS]. The comment at `src/physics/Physics.cpp:1498`
  ("tru.exe's Vehicle is one Havok body") contradicts that and should say "rigid body". Linux uses Jolt with the original
  parameters converted.
- Open: the solver's contact model and joint stabilisation in 0x4F0AB0 (affects stacking and pressure plates);
  sub-step rules.

## 8. Animation and skeleton

| Address | Name | Evidence |
|---|---|---|
| 0x444BA0 → 0x4428A0 | anim request → controller (blend spaces t_i = t_0 / len_0 × len_i, mirror flag 0x80) | C |
| 0x441FB0 | clip start at (start − blend) frames; mirrored root motion (−x, −qy, −qz) | C |
| 0x44CAC0 / 0x44AEE0 / 0x432A70 | channel data decode / bit-packed segments / piece-wise linear mode | C |
| 0x438C40 | playback rate (channel +0x50, 1/4096) | C |
| 0x443D00 / 0x445160 / 0x443C60 / 0x443B50 / 0x438F60 | IK chains / GenericLimbLockController ctor, parser / surface turn / probe ray | C |
| 0x6FD080 / 0x6EFDA0 / 0x6EFD60 | head look-at / limb target callback / foot lock gate | C |
| 0x45FAB0 | hair cloth solver | C |
| 0x6A5660 / 0x6B3DD0 / 0x6B4440 / 0x6B4090 | object anim graph (Instance +0x294): run / transition pick / stimulus / Lara's use pick | C |
| 0x5B1EA0 / 0x5B1F50 / 0x5B1FC0 | G2EmulationInstanceSetAnimation / SetMode / QueryAnimation | N-m |

- Data: [ANIMATION.md](../formats/ANIMATION.md), [ANIMGRAPH.md](../formats/ANIMGRAPH.md). lara.drm has a
  116-segment skeleton + 37 hair bones and 1 744 clips decoded to their exact end offsets (ROADMAP).
- Linux: `src/anim/{Anim,Blend,Ik,Hair}.cpp`, `src/formats/AnimGraph.cpp`.
- Fidelity: **decoded** (clips, blending, mirroring, anim graph); IK and hair are
  **reconstructed-approximation** (the citing code lists deviations, e.g. Hair.cpp line 2).
- Open: the cdc AnimGraph (ObjectBase +0x64) is unused by TR8 objects (C, 0x453100), so no work is needed there.

## 9. Camera

Full decode 2026-11-16: [CAMERA.md](CAMERA.md). It covers CameraManager 0xE804F0 (59 slots, 15 camera types), the
state field +0x4C, mode requests via slot 17, TransitionCamera durations, and the platform carry (0x609830 / 0x5FF650 /
0x7027B0).

| Address | Name | Evidence |
|---|---|---|
| 0x602690 | LaraCameraRewrite (mode distances, focus, limits, collider; camera DTP 0x14) | C (ROADMAP, GAMEPLAY.md) |
| 0x5FFB00 / 0x608B60 / 0x5FFA90 | force sets per controller / look-idle weight (0 until 5 s after the last look input) | C |
| 0x601D30 / 0x5FEE10 | default force sets | C |
| 0x5F0810 | pull-in (command 15) | C |
| 0x5FE800 → 0x6019D0 / 0x606160 / 0x606310 / 0x605ED0 | level camera commands and records | C |
| 0x5E26F0 / 0x5E2D90 | AutoSplineCamera_Start / AutoSplineCamera_vf1_Update | N-m (this audit) |
| 0x60A000..0x60AF90 | CameraSpline (states 1 active, 2 paused, 3 done) | C |
| 0x5E1B60 | BaseCamera ctor (fov default π/2; gameplay takes 45° from the DTP) | C |
| 0x5F39F0 / 0x5EDF50 | CAMERA_SetMode / CameraManager_ToggleMode | N-m / N-l |
| 0x5E8750 / 0x5E86F0 | camera shake stop / free slot | C |

- State machine:
  - Camera types 1-15 come from vtable slot 57 (vftable +0xE4: the camera of a type; 8 = AutoSplineCamera).
  - The committed camera mode is the field +0x54 (manual aim 23, on foot 1, ...). Mode requests go through slot 17.
  - Vftable +0x54 = slot 21 is the blended type switch; see [CAMERA.md](CAMERA.md).
  - CameraSpline has states 1/2/3.
- Linux: `src/game/World.cpp` (follow camera), `src/game/Camera.cpp` (splines), `src/render/Camera.hpp`.
- Fidelity: **decoded** for the follow camera's tuning and the reveal splines (ROADMAP: tested). The citing code
  marks some parts as not modelled [U] (the default yaw force set 11, the look-target blend 0x6009C0), so the
  subsystem as a whole is **reconstructed-approximation**.

## 10. Cinematics

| Address | Name | Evidence |
|---|---|---|
| 0x456580 / 0x457100 | CINE header / frame decode (lerp from floor(pos)) | C |
| 0x454BD0 (light loop 0x4551B0) | cinematic instance lights, bones without a track ride on their parent | C |
| 0x455910 / 0x4545C0 / 0x4547D0 | camera selection and vertical FOV / depth of field / DOF filter | C |
| 0x455D90 | end placement (header tails) | C |
| 0x453F80 / 0x454970 / 0x453630 / 0x49C6F0 | quaternion sign fix / transform / bone matrices / Euler Rz·Ry·Rx | C |
| 0x4561C0 | cinematic introduces an instance | C |
| 0x4A12B0 | MOVIE_Play (Bink) | N-l |

- Data: [CINEMATICS.md](../formats/CINEMATICS.md), [VIDEO.md](../formats/VIDEO.md).
- Linux: `src/cine/Cine.{hpp,cpp}`, `src/app/game/Cinematic.cpp`, `src/video/` (Bink through libav).
- Fidelity: **decoded** (all 53 cinematics parse; playback tested per ROADMAP). Header +4 `alt` binding (0x4D12D0)
  is **unknown** [U].

## 11. Interaction (use / engage / push-pull / switches)

| Address | Name | Evidence |
|---|---|---|
| 0x719520 / 0x6F3C60 | engagement: the use record's cylinder and facing cone | C |
| 0x6F3BB0 → 0x6E7E90 | engagement record's named object (g_belt / g_gauntlet*) | C |
| 0x6F3A50 | switch use cone (64° about 15° off +X) | C |
| 0x74AC00 / 0x73F390 | prismatic push / pull (player data +0xC7C..+0xC88) | C |
| 0x6B5890 / 0x6B53A0 | portable objects (pick up, carry, throw; message 0x1AF) | C |
| 0x6C2530 | receptacle (posts 0x165, calls script vt 49) | C |
| 0x4D86A0 / 0x4D4A00 / 0x4D7680 | uber-object state entry actions / transitions / action switch | C |
| 0x6D5EC0 | UberObjectSection_TR8_vf2 (entry action 0x1D engagement set) | N-m (this audit) |
| 0x4D7D50 / 0x4D3110 | UberObjectComposite_ctor / UberObjectSection_ModelMarkerAnchor_ctor | N-h (this audit, vftable store checked) |

- State machines: uber-object sections (states 0x1C, transitions 0x38, entry actions 0x10 records;
  [SCRIPTS.md](../formats/SCRIPTS.md), `src/formats/Uber.hpp`) and object anim graphs.
- Linux: `src/game/Objects.cpp`, `src/formats/Uber.{hpp,cpp}`, `src/script/`.
- Fidelity: **decoded** for engagement geometry and uber state machines; puzzles verified per chapter
  (docs/matrix). The 0x4D77A0 command dispatch is marked [I] in Objects.cpp.

## 12. Climbing and traversal

| Address | Name | Evidence |
|---|---|---|
| 0x707C60 / 0x5D6F00 | free-climb attach / 3-point markup rule (MUD 0x8000) | C |
| 0x6EC360 / 0x6FC530 | hold reach / stick | C |
| 0x746F60 / 0x745FA0 / 0x73C640 | grapple swing law / rappel and wall-run / jump-offs | C |
| 0x6C8840 → 0x6C5490, 0x6B2CC0, 0x6B3570 | rope wrapping (bends, shortening, straightening) | C |
| 0x734B70 / 0x72B920 / 0x74DCD0 | ladder rung count / current rung / LadderRungAlignAnimTarget | C |
| 0x74B920 / 0x737D60 | wall-hug shelf test | C |
| 0x764AA0 → 0x733E00 | corners | C |
| 0x7594C0 / 0x758F70 / 0x5D3BA0 | wall-jump attach (flag 0x80 → 0; returns 1/2) / wall verticality / flagged faces | **V** (0x7594C0) |
| 0x742770 | balance beam controller | C |

- Data: markup types (8 rails, 9 ladders, 13 perches, 14 beams, 15 pillars, 16-20 free-climb holds) in
  [COLLISION.md](../formats/COLLISION.md) / docs/GAMEPLAY.md.
- Linux: `src/game/{Traverse,Climb,Grapple,WallJump}.cpp`, Player.cpp.
- Fidelity: **decoded** constants and rules, implemented with a native state machine
  (**reconstructed-approximation** where the citing code says "not traced").

## 13. Combat and targeting

| Address | Name | Evidence |
|---|---|---|
| 0x6F5030 / 0x6F5160 / 0x702FC0 | Lara's health / hit priority and invulnerability | C |
| 0x6F1790 | fall-damage curve (piecewise linear, 0.5 × max knee) | **V** |
| 0x6C9E50 / 0x6C64D0 | fire function / hitscan | C |
| 0x6C9670 | projectiles (class 4) | C |
| 0x713200 → 0x713090 → 0x4935F0 | targeting line-of-sight ray (flags 0xC5AC1983) | C |
| 0x70DB20 / 0x7035F0 | accurate aim / dual target hand-off | C ([I] steering) |
| 0x715020 / 0x718EA0 / 0x718FD0 / 0x7189C0 | adrenaline head shot: time scale ramp, aim open/close | C |
| 0x69F1E0 | area damage (grenades, squibs) | C |
| 0x716F30 → 0x6FFED0 | melee limb contacts (events 0xE/0xD) | C |
| 0x6E68B0 / 0x6E8370 | Mjolnir ground pound / HUD clip | C |

- Linux: `src/game/{Combat,Aim,Melee,Gadgets,Breakables}.cpp`.
- Fidelity: **decoded** values (damage tables, invulnerability, curves); targeting steering is
  **reconstructed-approximation**.

## 14. Weapons and inventory

| Address | Name | Evidence |
|---|---|---|
| 0x704930 | Change Weapon cycle (pistols → secondary → hammer) | C |
| 0x6EE930 / 0x6EEAE0 | tailoring ammo start / max tables | C |
| 0x6F5E70 / 0x6F4EF0 | ammo / health pickups | C |
| 0x6E75A0..0x6E76A0 | tailoring setters (game state +0x3EF98..+0x3EFAC) | C (ROADMAP) |
| 0x5D5F80 / 0x5DB6A0 / 0x5D6170 | reward table / collecting reward n / relics doubling health | C |

- Data: generalbank DTP 36 reward table (179 treasures, 6 relics). `docs/formats/INVENTORY.md` documents the
  `ou-import` install inventory, not in-game inventory.
- Linux: `src/game/Combat.cpp` (weapons, Tailoring), `src/game/Progress.*`, `src/ui/Hud.cpp`.
- Fidelity: **decoded** for loadouts and pickups; the in-game inventory structure (game state layout beyond the
  fields cited) is **unknown** as a whole.

## 15. AI (enemies)

| Address | Name | Evidence |
|---|---|---|
| RTTI `EnemyBehavior_*` (Patrol, PatrolMove, PatrolAttack, PatrolLookTurn, Investigate*, Search*, Flank, Sidestep, Combat, RangedCombat, Reload, Idle, Nap…) | behaviour classes, one vftable each | RTTI ([FUNCTION_DATABASE.md](FUNCTION_DATABASE.md)) |
| 0x635920 / 0x6365B0 / 0x677D70 / 0x61DA60 / 0x61DD10 | PatrolAttack_vf4 / Flank_vf3 / Sidestep_vf3 / Investigate_vf4 / Investigate_vf13_OnMessage | N-h / N-m (this audit) |
| 0x666D10 / 0x666EB0 / 0x66C500 / 0x67ED20 | EnemyState_Fire vf7 / vf9, EnemyState_Death_vf7, EnemyState_RegenerateDeath_vf7 | N-h / N-m (this audit) |
| 0x63C190 / 0x63C7A0 → 0x63AC40 → 0x62F520 | enemy data (magic 0xDAF00018) / intro data | C |
| 0x620380 / 0x613B40 / 0x642210 | hit decision / death category / attack by quadrant | C |
| 0x622670 / 0x62D930 / 0x637D10 → 0x632F80 | flank cost / side re-evaluation / position search | C |
| 0x61F990 / 0x61F5D0 / 0x622C80 | zone clamp 0.8 / guard zone | C |
| 0x48BAB0 → 0x48B770 / 0x41C7D0 / 0x41C8C0 | NavPower links / bfx_AddNavGraph / bfx_NavGraph_AddResource | C / N-h |
| 0x60E690 | MoveNav | C |

- State machines: behaviour priorities in docs/GAMEPLAY.md (e.g. PatrolAttack priority 20); EnemyState_* states.
- Linux: `src/game/ai/{Enemy,EnemyData,Nav}.cpp`.
- Fidelity: **reconstructed-approximation**. Ours "stays a native state machine following these priorities"
  (docs/GAMEPLAY.md line 3794). Navigation is our A* over the NavPower areas, not the bfx middleware (Nav.cpp
  line 2). Behaviour classes have 4–6 of 15–18 slots cited (db/rtti.tsv).
- Open: the bfx/NavPower path planner (509 bfx library functions); behaviour slot semantics.

## 16. Environment (water, weather, wind, foliage)

| Address | Name | Evidence |
|---|---|---|
| 0x69DA80 / 0x69DFB0 | sea waves Σ hᵢ(sin(…) − 1) via CRT sin 0x813680 / wave VS constants c60-c63 | **V** (0x69DA80) / C |
| 0x6D9DA0 / 0x5EA200 / 0x5C78A0 / 0x5C1F50 | under-water state / camera under water / screen filters / fog | C |
| 0x6D8C20 / 0x6D6640 / 0x6D7CB0 | under-water light rays | C |
| 0x6AC340 / 0x6C10D0 | damaging water (eitr) | C |
| 0x546FC0 / 0x545950 / 0x53A650 / 0x53A860 / 0x5346A0 | weather object per frame / lightning timer / far light / rain hits / drop quads | C |
| 0x53C3F0 / 0x52E460 / 0x53C9F0 | wind retarget / evaluation / wind at a point | C |
| 0x499B40 / 0x490900 / 0x48FD70 | bend (foliage) objects | C |

- Data: [EFFECTS.md](../formats/EFFECTS.md) "Weather"; docs/RENDERING.md "Under water".
- Linux: `src/fx/Weather.cpp`, `src/game/{Water,Weather,Bend}.cpp`, `src/render/{Water,WaterField,Ocean,Fluid}.cpp`.
- Fidelity: **decoded** for the original water/weather ("Original" preset). The Enhanced water (FFT swell, eWave
  field, PBF splashes) is our own design, not a reconstruction.

## 17. Particles and effects

| Address | Name | Evidence |
|---|---|---|
| 0x543F70 / 0x540B80 / 0x54C300 / 0x54AEC0 | particle start / emitter update / particle update / sprite draw | C |
| 0x547B40 | effect command switch (GenericFXObject) | C |
| 0x539500 / 0x52DBC0 | ribbons | C |
| 0x535FA0 / 0x536280 | glows | C |
| 0x512D60 / 0x519F20 / 0x51A530 / 0x51B1D0 | screen filters: fog block / blur / feedback / noise grid | C |
| 0x6D3160 / 0x6D2D70 / 0x6D4030 | class-30 breakables | C |

- Data: [EFFECTS.md](../formats/EFFECTS.md).
- Linux: `src/fx/Fx.{hpp,cpp}`, `src/render/shaders/filter.glsl`, `src/game/Breakables.cpp`.
- Fidelity: **decoded** (fx is the third largest cited area: 87 functions, 72 KB).

## 18. Rendering pipeline

Per-frame pass order traced 2026-11-16: [RENDER_PIPELINE.md](RENDER_PIPELINE.md).

| Address | Name | Evidence |
|---|---|---|
| 0x5239C0 / 0x522580 / 0x522060 / 0x522900 | PCDeviceManager_Create / CreateDevice / BuildPresentParams / HandleDeviceLost | N-h |
| 0x5071B0 / 0x507270 / 0x507330 | PCRenderDevice_CreateTexture / CreateRenderModel / CreateRenderTerrain | N-h |
| 0x50EA80 / 0x50EB30 / 0x511240 | PCRenderModel ctor / Finalize / PCRenderTerrain ctor | N-h |
| 0x5150E0 | PCScene_Draw | N-m |
| 0x502B70 / 0x527A80 | PCStateManager_SetRenderState / SetBlendMode (material +0x38) | N-h |
| 0x505A90 / 0x506040 / 0x5066A0 / 0x506880 | PCMaterial textures / cull / translucent pass / fog constants | C |
| 0x50F7C0 / 0x511810 / 0x511E20 | model batch LOD, depth write / terrain groups / no-shadow groups | C |
| 0x5042B0 / 0x50A5F0 / 0x50B910 / 0x504A60 / 0x504CC0 | light slots / light table upload / PCRenderLight_BuildRegisters / attenuation atlas / special lights | C / N-m |
| 0x50CCB0 | PSSM sun shadows | C |
| 0x508570 / 0x55FB60 → 0x55F530 | irradiance SH state / probe selection | C |
| 0x515530 / 0x5263A0 / 0x526440 | PCShaderLib fill / VS / PS tables | C |
| 0x514680 / 0x514D90 | distance fog for the frame | C |
| 0x509630 → 0x51A2E0 | feedback (previous frame) | C |
| 0x5CE800 / 0x5C5740 | mirror render | C |
| 0x5289D0 / 0x528DB0 | VertexFormatToD3DDeclType / PCStreamDecl_CreateD3DResource | N-h |

- Data: [MATERIALS.md](../formats/MATERIALS.md), [MESHES.md](../formats/MESHES.md),
  [SHADERS.md](../formats/SHADERS.md), [TEXTURES.md](../formats/TEXTURES.md), docs/RENDERING.md.
- Linux: `src/render/vk/VkRenderer.cpp` (Vulkan 1.3), `src/render/gl/GlRenderer.cpp`,
  `src/render/OriginalShading.cpp` and `ShaderTranslate.cpp` (D3D9 SM3 → GLSL of the original shaders),
  `src/scene/Assets.hpp`, `src/formats/{Model,Shader}.cpp`.
- Fidelity: **decoded** for materials, shaders (translated originals), lights, PSSM and filters. The Enhanced
  features (RT shadows/reflections, CORTAO AO, FSR) are additions.
- Open: the draw-list order inside PCScene_Draw (render passes are reconstructed from docs, not traced end to end).

## 19. Lighting and ambient occlusion

- Original: per-object light sets (0x5042B0), up to 3 special lights (0x504CC0), irradiance SH c70-c76 (0x508570),
  light maps from terrain/model instance textures (C). The original has no AO pass (I: none cited, and no AO
  shader in the ShaderLibs survey is mentioned in SHADERS.md).
- Linux: `src/render/OriginalShading.cpp`, `src/render/sun.glsl`, `src/render/Cortao.cpp` (CORTAO screen-space AO,
  default since 0.51.0, scaling only the translated ambient registers).
- Fidelity: original lighting **decoded**; AO is an addition (not a reconstruction).

## 20. Audio

| Address | Name | Evidence |
|---|---|---|
| FMOD Ex (statically linked: 1 232 functions, 51 `FMOD::` classes) | middleware | Library ([FUNCTION_DATABASE.md](FUNCTION_DATABASE.md)) |
| 0x5920B0.. | SoundPlex create switch | C (ROADMAP) |
| 0x590970 / 0x583490 / 0x583960 / 0x587920 / 0x590CE0 | SoundPlex nodes: assignment / selector / material list / choice list / envelope | C |
| 0x5850E0 → 0x585070 / 0x57FD50 | 3D rolloff / listener between camera and Lara | C |
| 0x587620 → 0x582EC0 / 0x582270 / 0x58DD80 | DSP type mapping / parameters / SFX reverb / category DSP | C |
| 0x5950A0 / 0x594520 / 0x580F80 / 0x595250 | music sequencer tick / track loop / meta dispatch / stream clock | C |
| 0x593EB0 / 0x594D10 / 0x593D10 | music player slots / per-slot update / stop all | C |
| 0x5957A0 / 0x595670 | `.mul` ADPCM decoder / op table | C |
| 0x4F2B00 / 0x4F3540 | joint-sound manager | C |

- Data: [AUDIO.md](../formats/AUDIO.md).
- Linux: `src/audio/{Audio,Mixer,Dsp,Mus,MusicPlayer,SoundPlex}.cpp`, `src/app/game/Sound.cpp` (SDL3 audio,
  own mixer and FMOD DSP stand-ins).
- Fidelity: **decoded** for formats, music and SoundPlex graphs; FMOD's mixer and DSPs are
  **reconstructed-approximation** (ROADMAP "Sound events": Partial).

## 21. UI, HUD and fonts

cdc UI widgets, screens and the front-end state machine: [UI.md](UI.md). The font layout is in [../formats/UI.md](../formats/UI.md).

| Address | Name | Evidence |
|---|---|---|
| 0x474C90.. 0x476BC0 / 0x5C5A30 | Font_* (cursor, scale, height, text width, printf) / Font_Flush (the name is the menu hook's; the function is the frame end: EndScene of the main scene, the 2-D scene, EndFrame vt 0x10, docs/re/RENDER_PIPELINE.md §2 / §10) | N-m |
| 0x476D80 / 0x476540 / 0x475350 | text draw (`\x1B` highlight) / glyph draw / glyph lookup | C |
| 0x5A7C20 | button prompts (U+1800 + i) | C |
| 0x789770 | load/save slot naming | C |
| 0x52E8B0 / 0x52E880 | GAMELOOP_SetScreenWipe / IsWipeDone | N-m |
| 0x583D50 / 0x583DB0 / 0x583E20 | subtitle two-line ticker | C |

- RTTI: `cdc::UIWidgetBase`, `cdc::UISlider`, `cdc::UIPDAMapWidget`, PDAMap classes — mostly uncited
  (priority list entries 37 and 82).
- Linux: `src/ui/{Ui,Hud,Art}.cpp`, `src/locale/`.
- Fidelity: **reconstructed-approximation** (menus rebuilt to match captures; the cdc UI widget system itself is
  not reimplemented).

## 22. Save, load and checkpoints

Original save buffer and `.TRUSave` file layout: [SAVEGAME.md](SAVEGAME.md). The PC writer produces `<name>.TRUSave`;
the `savegame%02d.sav` table is a generic storage registration.

| Address | Name | Evidence |
|---|---|---|
| 0x5570F0 | Save Game: buffers every live instance (0x5568C0), then 0x555BE0 | **V** (calls) |
| 0x557130 / 0x556E90 | Restore Game / checkpoint restore (clock, inventory, outfit, reverb) | C |
| 0x556310 | save buffer checks ("illegal save type", limit 0x140000) | C |
| 0x5D40C0 / 0x6B0040 | respawn intro (`checkpoint` / `invis_checkpoint`) / checkpoint birth | C |
| 0x6F55A0 | death wipe → reload checkpoint (flag 0x80000) | C |
| 0x5DE2B0 | outfit on load (l1_seatop forces the warm wetsuit) | C |
| 0x78B550 / 0x7744E0 | save file paths / slot handling | C ([SAVES.md](../SAVES.md)) |

- Data: [SAVES.md](../SAVES.md).
- Linux: `src/save/Save.cpp` (versioned JSON + CRC, atomic slots), `src/game/Stream.cpp` (checkpoints).
- Fidelity: **reconstructed-approximation** by design. The original's semantics (what is saved and when) are
  followed, but the format is our own; original save files are not read.

## 23. Scripting and events

| Address | Name | Evidence |
|---|---|---|
| 0x57CE50 | SCMD interpreter: opcode = word >> 24, `op − 1` jump table of 133 cases at 0x57E52C, per-opcode counters at 0xCC4B58 | **V** |
| 0x57B560 / 0x57AF00 / 0x57C830 / 0x574960 | StartObject state 0 / constructor call / STATE (ends other threads) / latent native | C |
| 0x57B380 | cdc_ScriptObject_CallFunction | N-l |
| `Ns*_ScriptType` RTTI classes (e.g. `NsLaraBase_ScriptType`, `NsUIScreen_ScriptType`) | native bindings of the script types; their (type, function index) → handler mapping is not decoded | RTTI / I |
| 0x4A3200..0x4A32C0, 0x795DB0 | ScriptLog* / ScriptPrint* / NsCoreBase_PrintString | N-m / N-h / N-l |
| 0x5CD520 / 0x5CD1B0 / 0x5CCC20 | trigger volume / plane tests | C |
| 0x4937A0 / 0x49A5C0 | multibody range watches | C |
| 0x5BA280 / 0x5CF260 | spline mover setup / update | C |

- Data: [SCRIPTS.md](../formats/SCRIPTS.md) (sections type 8, `Ns*_ScriptType` classes in RTTI).
- Linux: `src/script/{Vm,Script}.cpp`.
- Fidelity: **reconstructed-approximation**. A bounded VM executes the common opcodes; unsupported paths fall
  back to static links (ROADMAP "Scripts": Implemented, not Tested). 227 script functions are cited.
- Open: opcode 0x65 (docs/audit/BUGS.md line 570), collections, persistent sleeping fibers, most native bindings
  (`NsUIScreen_ScriptType` and `NsLaraBase_ScriptType` slots are in the priority list).

## 24. Input

| Address | Name | Evidence |
|---|---|---|
| 0x480E50 | InputSystem_GetAxisValue | N-m |
| 0x487710 | default virtual-pad → key table | C |
| 0x4868F0 / 0x480BC0 | action map (generalbank DTP 47) handed to the mapper / action → control binding | C |
| 0x760C40 | game-action table (0x30 bytes per action) | C |
| 0x4817B0 / 0x481160 / 0x4811E0 / 0x484940 | rumble manager tick / advance / evaluate / XInput motors | C |

- Linux: `src/platform/Input.cpp` (SDL3 keyboard/mouse/gamepad, hotplug), `src/game/Input.cpp`,
  `src/game/Feedback.cpp` (rumble), config `controls`.
- Fidelity: **decoded** (defaults, action map, rumble); the device layer is SDL3 in place of the original's
  `PCXInputDevice` / `PCMouseDevice` / `PCKeyboardDevice` classes (RTTI). The imports have no XINPUT/DINPUT DLL and
  BINARIES.md finds no DirectInput strings, so [I] XInput is loaded at runtime (`LoadLibraryW` is called from 0x4824F0,
  in the input address range; not verified).

## 25. Configuration

| Address | Name | Evidence |
|---|---|---|
| 0x4791D0 / 0x4796B0 | Registry_LoadSettings / SaveSettings | N-h |
| 0x477EA0 / 0x478010 | Registry_LoadOptions / SaveOptions | N-m |
| 0x51E120 / 0x51DA80 | PCDisplaySettings_LoadFromRegistry / SaveToRegistry | N-h |
| 0x5D3CF0 | GAMELOOP_InitTimeFields | N-m |

- Linux: `src/config/Config.{hpp,cpp}` (JSON config shared with the launcher), `config::GameConfig`.
- Fidelity: **reconstructed-approximation** (same options, own storage).

## 26. Localisation

| Address | Name | Evidence |
|---|---|---|
| 0x49A800 / 0x49A7A0 | locals.bin loader / getter (out-of-range, empty, invalid UTF-8 rules) | C |
| 0x597DC0 | UTF-8 check | C |
| 0x4A0720 / 0x4A0910 | lang_* table, subtitle cue parser | C |
| 0x4538D0 | per-frame subtitle blob | C |
| 0x4A0C70 | Bink audio track = language index | C |

- Data: [TEXT.md](../formats/TEXT.md).
- Linux: `src/locale/Locale.{hpp,cpp}`.
- Fidelity: **decoded** (ROADMAP: tested, all 6 variants).

---

## Windows → Linux mapping

| Original system | RE evidence | Existing Linux implementation | Required work |
|---|---|---|---|
| Player controller | lara.drm state chart (S/T ids, conditions 0x8B2838, actions 0x8B0A50) decoded per feature; key solvers 0x73EDC0, 0x70F300, 0x728650; wall jump 0x7594C0 **V**; fall damage 0x6F1790 **V** | `src/game/Player.cpp`, Traverse/Climb/Grapple/WallJump, a native state machine with tuning from lara.drm | A complete chart dump to check state coverage; decode the cited open functions (0x74BCF0, 0x6FC6B0); keep Wine-trace self-tests as the reference |
| Physics (Jolt vs original) | cdc multibody solver (no middleware, BINARIES.md §5.3): PHYSICS_StepWorld 0x492B80, 0x4F0AB0, integration 0x4EFB20 **V**, buoyancy 0x4DEE60, shapes 0x48E710 | `src/physics/Physics.cpp` on Jolt with converted original parameters (mass, inertia, damping 0.3/s, buoyancy) | Decode the contact/joint solver to bound Jolt's deviations (stacking, plates, bikes); document per-feature deltas; keep gameplay independent of the renderer; fix the "Havok" wording in Physics.cpp |
| Animation | anim request 0x444BA0 → 0x4428A0, 0x441FB0, channel decode 0x44CAC0; anim graph 0x6A5660 / 0x6B3DD0; IK 0x443D00 / 0x445160; hair 0x45FAB0 | `src/anim/*`, `src/formats/AnimGraph.cpp` | Close the IK/hair deviations listed in the code; nothing major open for clips |
| Camera | LaraCameraRewrite 0x602690, force sets 0x5FFB00, level camera records 0x5FE800, AutoSplineCamera 0x5E26F0/0x5E2D90 | `src/game/World.cpp` follow camera, `src/game/Camera.cpp` splines | The default yaw force set 11 and look-target blend 0x6009C0 (marked [U]) |
| Rendering | PCDeviceManager/PCRenderDevice (N-h), materials 0x527A80 / 0x5066A0, lights 0x5042B0 / 0x50B910, PSSM 0x50CCB0, filters 0x519F20.., shader libs 0x515530 | Vulkan 1.3 + GL renderers with D3D9 SM3 → GLSL translation of the original shaders | Trace PCScene_Draw 0x5150E0 pass order end to end; RT/FSR stay optional additions |
| Water | waves 0x69DA80 **V** / 0x69DFB0, under-water 0x6D9DA0 / 0x6D8C20 / 0x5C78A0, volumes 0x6AD070 **V**, buoyancy 0x4DEE60, entry S104/S105 (CHANGELOG 0.50.0) | Original preset: decoded waves/filters/rays; Enhanced: `src/render/{Water,WaterField,Ocean,Fluid}.cpp` (own design) | Keep Original as the fidelity reference; Enhanced remains a non-original option (its swell is part of gameplay only with Enhanced+FFT) |
| AO / lighting | light slots 0x5042B0, SH irradiance 0x508570, light maps; no original AO (I) | `OriginalShading.cpp`, `sun.glsl`, CORTAO AO (`src/render/Cortao.cpp`) scaling the ambient registers only | No RE work; keep AO off in fidelity comparisons (viewer parity tests run `--ao off`) |
| Input (SDL3) | defaults 0x487710, action map 0x4868F0 (generalbank DTP 47), action table 0x760C40, rumble 0x4817B0, axis 0x480E50, `PCXInputDevice` / `PCMouseDevice` / `PCKeyboardDevice` RTTI | `src/platform/Input.cpp` (SDL3), `src/game/Input.cpp`, `src/game/Feedback.cpp` | Check the runtime XInput load (0x4824F0, inferred); dead-zone and axis curve of 0x480E50 if any deltas are reported |
| Interaction | engagement 0x719520 / 0x6F3C60, records 0x6F3BB0, uber state machines 0x4D86A0 / 0x4D4A00, portables 0x6B5890 | `src/game/Objects.cpp`, `src/formats/Uber.*`, `src/script/` | Command dispatch 0x4D77A0 (marked [I]); remaining uber action types |
| UI | Font_* (N-m), text draw 0x476D80, prompts 0x5A7C20, cdc UI widgets (RTTI, mostly uncited) | `src/ui/{Ui,Hud,Art}.cpp` rebuilt from captures | Decode the cdc UI widget / PDAMap classes if menu fidelity issues come up; low priority |
| Audio | FMOD Ex statically linked (library); SoundPlex 0x5920B0.., music 0x5950A0 / 0x593EB0, `.mul` 0x5957A0, DSP mapping 0x587620 | `src/audio/*` on SDL3 with own mixer and DSP stand-ins | Remaining SoundPlex node semantics (ROADMAP: Partial); FMOD DSP parity for reverb/EQ |
| Save / load | Save Game 0x5570F0 **V** (calls), Restore 0x557130 / 0x556E90, checkpoints 0x5D40C0, death reload 0x6F55A0 | `src/save/Save.cpp` (own JSON format), `src/game/Stream.cpp` | None required for parity; importing original saves would need the 0x556310 buffer format (not decoded) |
| Scripting | SCMD interpreter 0x57CE50 **V**, state/thread functions 0x57B560 / 0x57C830, `Ns*_ScriptType` native classes (RTTI) | `src/script/Vm.cpp` bounded VM + static-link fallback, `src/script/Script.cpp` natives | Opcode 0x65, collections, persistent sleeping fibers, the native binding table (runtime capture under Wine), the `Ns*_ScriptType` slots |
| Streaming | STREAM_PollLoadQueue 0x5C1DA0, portals 0x5CEC60 / 0x5D13E0, level change 0x5DF8C0, unit look 0x5C7CA0 | `src/game/Stream.cpp`, `src/game/World.cpp`, background loading | Load-queue priorities and the memory budget (only if hitches or ordering bugs appear) |
| Resource / asset loading | InitArchive 0x473840 **V**, RESOLVE section handlers, CDRM, relocations 0x54EC60 | `src/cdc/`, `src/formats/`, `src/importer/` | Extern/resource relocation kinds (ROADMAP: unverified) |
| AI | `EnemyBehavior_*` / `EnemyState_*` RTTI, data 0x63C190, flank/patrol/investigate slots, bfx NavPower (library) | `src/game/ai/*` native state machine, own A* navigation | Decode the remaining behaviour slots (most uncited); compare planner choices with Wine traces |
| Cinematics | CINE decode 0x456580 / 0x457100, lights 0x454BD0, cameras 0x455910 | `src/cine/`, `src/app/game/Cinematic.cpp`, `src/video/` | Header +4 `alt` binding 0x4D12D0 |
| Config / localisation | Registry_* (N-h/N-m), locals.bin 0x49A800 | `src/config/`, `src/locale/`, Qt launcher | None |

## Open questions across subsystems

- 9 410 Unknown functions (1.24 MB) have no cited neighbour; entry points for them are the RTTI families (cdc UI,
  PDAMap, Screenshots, AsyncTaskManager) and the Win32 platform layer.
- Indirect and virtual calls are not resolved in the call graph, so dispatch-heavy systems (AI behaviours, UI
  widgets, script natives) look less connected than they are.
- The priority list (FUNCTION_DATABASE.md) puts large uncited functions next to animation (0x43F7F0), collision
  (0x46E460, 0x47D850), game (0x6FAEB0, 0x465780, 0x43B870) and fx (0x526EF0, 0x538930) at the top. They are
  callees of code we already follow, so they are where the next deviations are most likely to be found.
