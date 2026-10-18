# In-engine cutscenes: cinstream CINE streams

Code: `src/cine/Cine.{hpp,cpp}` (`ou_cine`: demux, header, frame decoder, playback, unit binding),
`src/app/game/Cinematic.{hpp,cpp}` (ou-game playback), CLI `ou-text cine <name>` (subtitles, counts).
Tests: `tests/test_cine.cpp`. Subtitle text and the ticker: [TEXT.md](TEXT.md) §3–4. Audio packets: [AUDIO.md](AUDIO.md) §4.

Labels: **[O]** observed in the GOG data, in a render, or in our tru.exe 1.1.0.0 decompile (VAs below); **[I]**
inferred; **[U]** unknown.

## 1. Files and binding

- [O] A cutscene is `pc-w\cinstream\<nnn_name>.mul` (MultiplexStream: sound packets + cinematic packets, one record per
  voice language) plus `pc-w\cinstream\<nnn_name>.drm` (DTP sections only; 12 736-byte = 0x31C0 tables that 0x4547D0
  hands to the render device as lighting / depth-of-field state [I], not decoded). 53 streams.
- [O] Concatenating the type-1 packet payloads gives the CINE stream (`cine::demux`).
- [O] Each unit lists its cinematics: `Level+0x70` → level data, `+0x1DC` u32 count, `+0x288` `char*` table
  (`cine::unitCinematics`; 0x4A4D10 opens `table[index]` through 0x5B2460 / 0x58ACE0 "%s%s.mul"). l4_ballcourt:
  185_mex_calendar, 190_mex_mayan, 220_mex_exit, 186_mex_calendar_mirror.
- [O] A `cinematic_object` intro script has one member of native type `cinematic` (script section 3521 in
  cinematic_object.drm, "game/script/game/cinematic") at instance offset 4 with the initial value
  `{u32 index into the unit's table, u32 stream unit id}` (0x4A4D60 reads the index as u16) → `cine::introCinematic`.
  Chapter ends (test `CineReal.ChapterEndCinematics`):

  | unit | cinematic_object | stream |
  |---|---|---|
  | l3_main_hall | 4106 | 010_pro_surprise |
  | l1_cine_ship_sinking | 1005 | 070_med_dive |
  | l2_cliffclimb | 2565 | 110_tha_yachtexit |
  | l3_mansion_exterior_afterburn | 4070 | 170_man_exit |
  | l4_ballcourt | 5483 | 220_mex_exit |
  | l5_valaskjalf | 6846 | 250_val_hammer |
  | l6_ship_natla_room | 1387 | 270_shp_escape |

  Each of these streams' header unit name is that unit (case-insensitively).

## 2. CINE header (tru.exe 0x4537A0 + 0x456580, `cine::parseHeader`)

All little-endian u32 / f32. The channel count is the running total kept at handler `+0xCC`; channels are numbered in
header order and every channel holds one float per frame.

```
char  magic[4] = "ENIC"; u32 version (9); u32 headerSize      // bytes after magic + version
char  unitName[64]
u32   unitId                                                  // stream unit id (handler +0x20)
u32   nMarkers; {u32 unit, i32 intro, u32 channel} x n        // 9 channels each
u32   nInstances; per instance (handler record 0xB0 bytes):
        i32 intro            // unit intro unique id; -1 = the player
        i32 alt              // != -1: bind 0x4D12D0(alt) instead [U]; -1 in all 53
        u32 bones, u32 channel
        f32[16] x bones      // per-bone fix matrix; the exe zeroes its 4th column -> rotation/scale only
        u32 nLights, u32 lightChannel; {u32 bone, type, flag, ?, ?, table} x nLights   // 10 channels each
        u32 nVars, u32 varChannel                              // nVars channels
        // channels: 3 + 14 x bones
u32   nCameras; u32 channel x n                               // 16 channels each
u32   eventTarget; u32 nEvents; {u32 id, u32 channel} x n     // 1 channel each
u32   0
if (pos < headerSize + 8): per instance {f32 pos[3], f32 rot[3], u32 unit}  // placement after the cinematic
```

[O] Every one of the 53 English streams parses with `pos == headerSize + 8` and has the tail. Survey (all 53): 1 marker
(230_val_intro: 2, units 74 and 21), 1 camera (040_med_ruin, 245_val_yeti: 2), 0 events, 1–21 instances.

## 3. Frames (0x457100, `cine::Decoder`)

```
u32 size (bytes after this field); i32 frame (0, 1, ... ; -1 ends)
per channel: when its run is exhausted: u32 (bits 28-31 animated, 0-27 run length) + f32 first value;
             otherwise an f32 only if the run is animated (a constant run repeats its value)
u32 subtitleLength; subtitleLength bytes (TEXT.md §3); pad to 16
```

- [O] After the −1 frame the muxer keeps emitting `{12, -1, 0, 0}` frames in the remaining cinematic packets; all 53
  streams end with nothing else (`Summary::cleanEnd`, test `CineReal.AllStreamsDecodeToTheEnd`).
- [O] Voice-line `.mul` streams use the same format with 0 channels and end at a frame boundary without −1.
- [O] 30 frames per second (debug overlay `frame / 30` as mm:ss).

## 4. Channel semantics

### Transforms (9 channels)

[O] `scale xyz, rotation xyz (Euler radians), translation xyz` (0x454970 / 0x454A90). [O] 0x454970 builds the
rotation matrix from a quaternion, then multiplies its columns by the scale and stores the translation in row 3:
column-vector `M = T · R · S`. [O] tru.exe matrices are column-major for column vectors (0x49CF90 puts
`2(xy + wz)` at element 1), so `glm::make_mat4` reads them directly.

Euler order: [O] 0x49C6F0 converts the three angles to a quaternion; [O data] `R = Rz(z) · Ry(y) · Rx(x)`: with that
order the camera's forward axis points at Lara's pelvis (median 13°, 34°, 29° in 001/060/010) while `Rx · Ry · Rz`
gives 14°, 124°, 94°, and 060's 180°-yaw shots are only consistent with Z applied last.

Interpolation: [O] between the previous and the current frame by the fraction of the frame position (0x453F30 lerp;
rotations: 0x453F80 converts both Euler triples to quaternions, flips each to w ≥ 0, then slerps). The handler decodes
until the current frame is `floor(pos) + 1`, so subtitles of frame n appear at position n − 1 (as the exe).

### Area markers → scene origin

[O] 0x457100: for the marker whose unit is the player's unit, `origin = markerIntro · inverse(M(marker channels))`
(handler `+0x80`); `markerIntro` = the `co_cinetest_marker` intro matrix. Bones without a parent and the camera are
relative to it. [O] 51 streams have identity marker channels; 230_val_intro's second marker carries the offset
between its two units (translation −15 980, −40 809, −37 156). We use the first marker found among the loaded units,
the stream's unit first [I].

### Instances (3 + 14 per bone)

- Instance channels 0–2 [O values]: ch0 −1 in 468 of 469 instances (0x454BD0 stores a clamped value at instance
  `+0xA1` [I]); ch1 0 or 1, constant per instance [U]; ch2 small integers (0..65) written to the instance's render
  component `+0x20` [O] (meaning [U]). Not used by our playback.
- Per bone, relative to `channel + 3 + 14·b` (`Instance::boneChannel`): 0–8 transform; 9 flags; 10–13 four values per
  bone passed to 0x5B60F0 [U] (non-zero in 792 bones). [O] Flags: bit 0 = divide the local matrix's columns by the
  parent's scale (translation kept), bit 1 = parent is bone 0; observed values 1 (roots), 5 (16 289 bones), 0 once.
  The exe reads the flags from the previous frame value, not interpolated.
- [O] 0x454BD0 (per instance, per frame): `world[b] = (parent < 0 ? origin : world[parent]) · local[b]`, the parent
  index from the instance's model skeleton; the instance matrix = `world[b] · fix[b]` with world's translation. Model
  bones beyond the stream's count take their parent's matrix with the translation moved to the bind pivot. For the
  player (−1) the count is clamped to the model's; for others a stream with more bones than the model is skipped with
  "CONTENT MISMATCH". The instance position becomes bone 0's.
- [O] Lara (−1) has 113 animated bones; their offsets match lara.drm's bind pivots (test
  `CineReal.LaraBonesMatchHerSkeleton`: > 90 % of the compared offsets within 5 %); fix matrices are non-identity for
  248 instances (Lara included). Scales ≠ 1 occur on 834 bones.
- Instances are unit intros of `co_*` objects (co_natla, co_amanda, co_wp_handgun, co_recorder, …), most parked at
  z ≈ −10 723 in the level until the cinematic moves them.

### Lights (10 per light) [U]

0x454BD0 creates a light per entry while `round(ch5)` has bit 0 set (type from the entry, colour/intensity from ch0–3,
ranges from ch6/ch7, attached to the entry's bone) [I from the call sequence]. Not rendered by us.

### Cameras (16 channels)

| ch | meaning | evidence |
|---|---|---|
| 0–8 | transform; world = `origin · M · Rx(−90°)` (the exe camera looks along +Y): forward = −Z of M, up = +Y of M | [O] 0x455910 |
| 9 | horizontal FOV, radians; vertical = `2 atan(tan(ch9/2) / aspect)`, aspect = device `+0x170` / 10000 | [O] 0x455910 disassembly: `x·0.5`, 0x813870, `/ aspect`, 0x814E10, `·2` [I that these CRT helpers are tan/atan]; e.g. 0.21–1.36 in 060 |
| 10 | flags: bit 0 cut (this frame starts a shot), bit 2 active camera (first such camera, else camera 0), bit 3 light-region probe under the camera (0x4540E0), bit 6 [U] | [O] 0x453CE0, 0x455910; values 8/9, 12/13, 76/77 |
| 11–13 | depth of field (focus distance, f-stop 5.6, …) → 0x4545C0 | [O] call; [U] units |
| 14 | time scale multiplied into `gameTracker_timeMult` when ≠ 1 | [O] 0x454550; 1.0 in all 55 cameras |
| 15 | [U] | 0 except one stream (100) |

[O] 060_med_natla: 24 cut frames; inside shots the camera moves at most 8.9 units and turns at most 1.04° per frame,
the cut frames jump 119–3970 units (test `CineReal.NatlaSubtitlesCameraAndVoiceLines`). Across a cut our playback holds
the old shot until the frame boundary instead of interpolating into the new one [I] (the exe resets per-instance
interpolation state on cuts).

### Events

[O] `{id, channel}` pairs: on a rising channel the handler sends `id` to the event target (0x5B2120). No retail stream
has any.

### Placement after the cinematic

[O] 0x455D90 puts every instance back at the header tail: position + the marker intro position of the tail's unit,
rotation as stored. ou-game moves Lara there after cinematics that are not level ends (heading = rot.z − 90°, as the
player intro).

## 5. Audio and subtitles

- [O] The sound packets of the same `.mul` hold the whole sound track (voice, music, effects; 44.1 kHz stereo IMA,
  AUDIO.md §4), one record per voice language (`0xFFFFFFE1` English). [O] Audio sample 0 = frame 0: in 060_med_natla
  the speech-band (300–3400 Hz) energy rises a median 5 frames (0.17 s) after each English subtitle's frame (27 lines
  between frames 109 and 1500; envelope plot viewed). The English track outlasts the frames (060: 110.3 s of audio, 106.0 s of frames).
- [O] Subtitles: TEXT.md §3; shown with the exe ticker (TEXT.md §4) at y 352 / 374 of 640×480.

## 6. Playback in ou-game

- `ou-game --game-dir DIR --cine NAME [--render-hz 30 --frames N --screenshot out.ppm]`: loads the stream's unit (with
  its portal neighbours), plays the cutscene, exits. Esc or gamepad Start skips.
- [O run] When a script activates a `cinematic_object` that has a stream (`UnitData::cinematics`), `World::onCinematic`
  plays it first; a level end's chapter change follows. Replay check: spawning in l4_ballcourt's volume 6407 activates
  5483 → 220_mex_exit plays (1061 frames), then chapter 5 starts in l5_startsnow. [I] each cinematic plays once per
  session.
- The game scene is re-used: Lara's skinned meshes are posed from the stream, other actors come from their object DRM
  (render model 0; rigid models follow bone 0), the actors' static level copies and the pistols are hidden, game music
  is muted, the camera comes from the stream (with roll), the `.mul` track plays as a voice started with frame 0.
- Not done: lights, depth of field, outfit variants (Lara always wears lara.drm's model), hair simulation (hair bones
  ride rigidly), events/vars, instance channels 0–2. [I] Frame position follows wall time; the exe follows the stream's
  playback clock.

Viewed renders (`../openunderworld-work/screens/cine`): 001_pro_corridor frames 40 and 150 (Lara walking into the
corridor, the rubble actor flying; "My god!"), 060_med_natla frames 130 ("Damn, it's too small..." close-up), 420
(Amanda on the walkway), 700 (Lara close-up, two-line ticker), 850 (Natla in the cage, wings folded); 220_mex_exit frame
300 (Lara with the headset in l4_ballcourt, "Zip, I have everything I need to get Thor's hammer."). Faces of co_*
characters render green (material path, not the poses).

## 7. Tests (`tests/test_cine.cpp`)

Synthetic: header fields, run-length decode of every value, out-of-sequence frames, every truncation (throws, or a
frame-boundary end), interpolation, camera basis and FOV, bone FK with a fix matrix and a rigid extra bone, cut
holding, origin, end of stream. Gated on `OU_GAME_DIR`: all 53 streams decode to a clean end (3253 subtitle entries on
845 frames), 060_med_natla subtitles + camera continuity + a voice-line stream, the chapter-end bindings, Lara's bones
against lara.drm.
