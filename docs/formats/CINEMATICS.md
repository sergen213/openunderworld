# In-engine cutscenes: cinstream CINE streams

Code: `src/cine/Cine.{hpp,cpp}` (`ou_cine`: demux, header, frame decoder, playback, unit binding),
`src/app/game/Cinematic.{hpp,cpp}` (ou-game playback), CLI `ou-text cine <name>` (subtitles, counts).
Tests: `tests/test_cine.cpp`. Subtitle text and the ticker: [TEXT.md](TEXT.md) §3–4. Audio packets: [AUDIO.md](AUDIO.md) §4.

Labels: **[O]** observed in the GOG data, in a render, or in our tru.exe 1.1.0.0 decompile (VAs below); **[I]**
inferred; **[U]** unknown.

## 1. Files and binding

- [O] A cutscene is `pc-w\cinstream\<nnn_name>.mul` (MultiplexStream: sound packets + cinematic packets, one record per
  voice language) plus `pc-w\cinstream\<nnn_name>.drm`, an object DRM (ObjectBase DTP res 32 whose effect list holds
  e.g. 59 screen-filter records over 0x31C0 tables in 060_med_natla, light DTPs in 220_mex_exit). Who starts those
  effects is [U]; the camera filter and the light templates of the stream come from `globalaniminfo.drm` (§4). 53 streams.
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

- Instance channels 0–2 [O 0x454BD0 at 0x455096]: ch1 rounded → instance `+0xA1` when it is a valid bone index, else
  0 (values 0 or 1, constant per instance; meaning [U]); ch2 rounded → the instance's render component `+0x20`
  (values 1..65, 16 for most actors) — [I] the light receiver mask the light masks below test (the rig lights' ch6 is
  16 / 17 / 32); ch0 (−1 in 468 of 469 instances) is not read by 0x454BD0. Not used by our playback (no receiver
  masks; LEVELS.md "Lights").
- Per bone, relative to `channel + 3 + 14·b` (`Instance::boneChannel`): 0–8 transform; 9 flags; 10–13 the bone's
  skin-palette 4th row. [O] Flags: bit 0 = divide the local matrix's columns by the
  parent's scale (translation kept), bit 1 = parent is bone 0; observed values 1 (roots), 5 (16 289 bones), 0 once;
  bone 0's bit 2 moves the instance to the handler's unit (0x5B3A20). The exe reads the flags from the previous frame
  value, not interpolated.
- [O] Channels 10–13: when the instance has a render component whose `+0x14 → +0x49` is 0, 0x454BD0 lerps the four
  values of every stream bone into a zeroed 1000-entry vec4 table and hands it to 0x5B60F0 (mask 0xF), which writes the
  instance's per-bone rows, uploaded as the 4th row of each skin-palette bone (GAMEPLAY.md "Wetness": x wetness,
  y dirt). Survey: Lara's face bones 22–30 animate z / w (0..1) in 060, 070, 110, 220; 220_mex_exit's 5464 holds
  constant x / y (2.37, 6.48 / −1.35, −5.36) on 20 bones. Ours writes the weighted rows into COLOR0 of the skinned
  meshes whose materials read them (`Material::skinParams`; Lara always), as in play.
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

### Lights (10 per light)

Header entry (24 bytes, `cine::Light`): `{bone, type, flag, ?, ?, table}`. [O] all 53 streams: 6 lights per stream,
on a `co_light_rig` / `co_light_rig_two` instance, bones 1–4, 6, 7, type 2 (spot), flag 0/1, `?` 1/2,
`?` 512/1024/2048 [U], table 0/1.

[O] 0x454BD0 at 0x4551B0, per light and frame (p = the frame-floor(pos) value, i = interpolated):

| ch | use |
|---|---|
| 5 (p) | bit 0: create the scene light (else destroy it); bit 2 → PCRenderLight 0x58 (`+0xC5` bit 0) [U] |
| 0–2 (i) | colour → PCRenderLight 0x24 |
| 8 (i) | intensity → PCRenderLight 0x20 (`+0xB0`) |
| 3, 9 (i) | SceneLight 0x54 (0x574320) `(ch3, ch3 + ch9)` → PCRenderLight 0x34 (0x50A280): inner / outer cone, full angles (0.35–2.44 / 0.08–0.79 rad) |
| 6 (p) | `round | 0x83000000` → SceneLight `+0x14` (vfunc 0x6C): receiver mask (16 / 17 / 32) |
| 7 (p) | `round | 0x83000000` → SceneLight `+0x18` (0x74); `ch7 != 0` → SceneLight 0x58 → PCRenderLight `+0xC5` bit 2 [U: shadow mask] |
| 4 | not read |

- Template [O]: `globalaniminfo.drm` ObjectBase `+0xC8` (0x4533A0 keeps it at handler `+0x10C`): `+0x14` 20 light DTP
  ids (`table` picks one: 804 spot radius 40 000, 805 point radius 2741, 806 spot), `+0x64` / `+0x68` two 0x31C0
  filter ids (807, 808). The 0x14E4-byte DTP is copied, its type replaced (entry type 0 → 2 directional, 1 → 0 point,
  2 → 1 spot) and created through the render device (vfunc 0xC0). No template: radius 5000, intensity 1, falloff
  `1 − (k/255)²`.
- Matrix [O]: the instance bone's matrix × the quaternion (x, y, z, w) = (1, 0, 0, 0) (0x49CF90), i.e. Rx(180°): a spot
  shines along the bone's −Z. Test `CineReal.LightRigsAimAtTheActors`: an actor root lies inside 89 % of the lit cones
  (2179 / 2444 samples of 001, 010, 030, 220), 35 % with +Z.
- Ours: one environment light slot per entry at the front of the cinematic scene's lights (OriginalShading takes the
  first that reach an object), posed per frame through `FrameOptions::dynamicLights`; off = dark and far away.
  Masks [O Wine, TerrainLightsPssm]: a light reaches a receiver when `light mask & receiver mask != 0` (entities
  0x55FB60: entity `+0xE4`; terrain 0x56ABF0: cell terrain data `+0x08`, 1 in l3_main_bedroom); unit lights carry
  0x8300000F. Ours (original shaders, `original::lightSet`): `scene::Light::mask` = the rig light's ch6 mask, the
  cinematic actors' meshes take instance ch2 as `MeshData::lightMask`, every other mesh 1 [I: level objects as the
  bedroom terrain] and unit lights all bits, so a rig light with ch6 16 lights the actors only, 17 also the level.
  Not applied by the generic shading or the RT light shadows.

### Cameras (16 channels)

| ch | meaning | evidence |
|---|---|---|
| 0–8 | transform; world = `origin · M · Rx(−90°)` (the exe camera looks along +Y): forward = −Z of M, up = +Y of M | [O] 0x455910 |
| 9 | horizontal FOV, radians; vertical = `2 atan(tan(ch9/2) / aspect)`, aspect = device `+0x170` / 10000 | [O] 0x455910 disassembly: `x·0.5`, 0x813870, `/ aspect`, 0x814E10, `·2` [I that these CRT helpers are tan/atan]; e.g. 0.21–1.36 in 060 |
| 10 | flags: bit 0 cut (this frame starts a shot), bit 2 active camera (first such camera, else camera 0), bit 3 light-region probe under the camera (0x4540E0), bit 6 depth of field (0x4547D0) | [O] 0x453CE0, 0x455910, 0x4547D0; values 8/9, 12/13, 76/77 |
| 11–13 | depth of field: focus (60–4827), f-stop (1–14.8), focal length (25–600) → 0x4545C0 | [O] |
| 14 | time scale multiplied into `gameTracker_timeMult` when ≠ 1 | [O] 0x454550; 1.0 in all 55 cameras |
| 15 | [U] | 0 except one stream (100) |

[O] 060_med_natla: 24 cut frames; inside shots the camera moves at most 8.9 units and turns at most 1.04° per frame,
the cut frames jump 119–3970 units (test `CineReal.NatlaSubtitlesCameraAndVoiceLines`). Across a cut our playback holds
the old shot until the frame boundary instead of interpolating into the new one [I] (the exe resets per-instance
interpolation state on cuts).

Depth of field [O]: with camera flag bit 6 (045_med_kraken, 110_tha_yachtexit, 150_man_pinned: every frame; no other
stream) 0x4547D0 copies globalaniminfo's filter 808, applies 0x4545C0 (`cine::depthOfField`, focus a, f-stop N,
focal f) and hands it to the render device (vfunc 0x5C → 0x517D60, weight 1):

```
blur (+0x1300) = clamp(4 / N, 0, 4);  k = 0.1 blur;  F = k N (a − f) / f²
nr = a / (1 + clamp(F, ±0.99));  fr = a / (1 − clamp(F, ±0.8));  +0x1310 = 3.1605 nr, +0x1314 = 3.1605 fr
curve[i] (+0x1200) = clamp(trunc(255 |D − a| f² / (D N (a − f)) / k), 0, 255),  D = lerp(nr, fr, i / 256)
```

So the alpha of the blurred overlay is 0 at the focus and saturates away from it over the depth range; a < f (all the
other cameras: 5 / 5.6 / 25..600) gives an all-zero curve. [O] Filter 808 has both ColorDOF blocks off (`+0x130E`,
`+0x262E` 0; `+0x130C` DOF 1, `+0x130F` 1 = use its own range) and its fog, noise, feedback and finish blocks off, and
0x516E70 tests `+0x130E` first: the retail PC game draws no cutscene depth of field. Ours: off by default in every
preset; graphics setting `cine_dof` (launcher "Cutscene depth of field", in-game DISPLAY SETTINGS ▸ Water & Sky
"CUTSCENE DEPTH OF FIELD", with effects on) draws 808 with the ColorDOF block on (its scene matrix off) through the
screen-filter pass (`fx::System::screenFilter`). Shallow cameras blur most of the frame (150_man_pinned: f/1).

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
- Frame clock: the exe runs the frame position off the stream's playback clock. Ours (`cine::Clock`, test
  `Cine.ClockFollowsTheSoundTrack`): seconds of the track the mixer has played (`audio::Mixer::position`), carried on
  by wall time between the mixer's chunk updates for at most 0.1 s, never backwards; wall time without a track and a
  fixed step with `--render-hz` (replays, screenshots).
- [O] Subtitles: TEXT.md §3; shown with the exe ticker (TEXT.md §4) at y 352 / 374 of 640×480.

## 6. Playback in ou-game

- `ou-game --game-dir DIR --cine NAME [--render-hz 30 --frames N --screenshot out.ppm [--cine-shots N]]`: loads the stream's unit (with
  its portal neighbours), plays the cutscene, exits. Esc or gamepad Start skips.
- [O run] When a script activates a `cinematic_object` that has a stream (`UnitData::cinematics`), `World::onCinematic`
  plays it first; a level end's chapter change follows. Replay check: spawning in l4_ballcourt's volume 6407 activates
  5483 → 220_mex_exit plays (1061 frames), then chapter 5 starts in l5_startsnow. [I] each cinematic plays once per
  session.
- The game scene is re-used: Lara's skinned meshes are posed from the stream, other actors come from their object DRM
  (render model 0; rigid models follow bone 0; light rigs only for their bones), the actors' static level copies and
  the pistols are hidden, game music is muted, the camera comes from the stream (with roll), the `.mul` track plays as a
  voice started with frame 0 and clocks the frames (§5); lights, depth of field and the palette rows as in §4.
- Lara (instance −1) is the player instance, so she wears what she wears in play: the unit's outfit or the EXTRAS
  costume (GAMEPLAY.md "Outfits"). Her hair is simulated on the stream's pose (`anim::HairSim` with `game::laraHair()`,
  60 Hz steps on the frame clock), reset at the first frame and [I] when her root jumps more than 200 units in a frame
  (the exe resets at frame 1 and at instance record `+0x3C`'s frame [U]).
- Actors: `cine::actorScene` (render model 0's skinned meshes; [I] the model whose skeleton has exactly the stream's
  bones when model 0's has fewer: ms_ani_sunsphere_plat's 33-bone model 1 in 049_med_kraken_death). Meshes with Mesh
  flags bit 0x40 or primgroup `+0x1C` bit 0 are left out of the colour pass (0x50F7C0 [O]): `co_shadow_card` (38
  instances, flags 0x41) no longer draws as a grey plane (186, 230, 235, 275, 290).
- Effects: the game's running effects (`EffectsView`) tick on the frame clock and draw with the cinematic camera
  (torches, the burning manor in 170); actors that own effects (`co_fx_hell_gate`, `co_fx_shp_escape`,
  `co_fx_hel_saved`, `fx_cine_lightning`, `co_lightning`) carry their emitters on bone 0 and are moved back afterwards.
  The `cinstream\*.drm` files hold no GenericFXObject (`fx::parseObjectFx` finds none in all 53); their birth records
  that are disabled (info 0x2005, e.g. all 10 of co_fx_hel_saved) are started by nothing we run [U].
- Not done: events / vars (no retail stream has events; vars only on the light rigs).
- Sweep 2026-11-06 (`../openunderworld-work/cinesweep`, `run.sh`: Vulkan, invisible KWin, `--render-hz 30
  --cine-shots 6`, contact sheets `sheets/*.png`): all 53 play to their last frame. Triggers (test
  `CineReal.EveryStreamPlaysWithItsActors` prints them): 52 streams have a `cinematic_object` intro; 310_hel_saved has
  none (only listed by l7_yggdrasil_hall01/02, no cinematic member names it) and 091_tha_naga's intro 2937 is named by
  no script or link — a static placeholder (Lara in the bind pose at the marker, fixed camera); 343_hel_engame plays
  only when l7_himalaya_end is started directly.

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
