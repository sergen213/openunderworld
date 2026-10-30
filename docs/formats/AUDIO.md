# Audio formats: IMA ADPCM, FSB4, DRM Wave sections, `.mul` MultiplexStream, music

Labels: **[V-data]** observed in the retail GOG data; **[V-exe]** read in the tru.exe decompilation;
**[S]** from a third-party source; **[I]** inference; **[U]** unknown.
Code: `src/audio/Audio.{hpp,cpp}` (decoders), `src/audio/Mus.cpp` (`.mus` parser + stem-gain
sequencer), `src/audio/Mixer.{hpp,cpp}` (SDL3 mixer) and `tools/ou-audio` (CLI).

## 0. Where audio lives

| Container | Count | Content |
|---|---|---|
| DRM section type 6 (Wave) | 41 243 sections in 1060 DRMs | Sound effects and short speech. Each section is a 16-byte header plus a one-sample FSB4 bank (§2, §3) |
| `.mul` bigfile records | 5835 | MultiplexStream (§4). Voice lines are 1029 names × language dubs. Also music stems, cinematic audio and long event sounds |
| `.sam` bigfile records (`FSB4` magic) | 75 | Music sample banks. In every retail record the bank holds a 64-sample placeholder, `temp.wav` (§5) |
| `.mus` bigfile records (`Mus!` magic) | 75 | Music scripts (§5) |
| `!WAR` | 270 | Not audio: cdc RAW images such as loading screens ([CONTAINERS](CONTAINERS.md)) |

`ou-audio scan` decodes every one of these without failure:

| Kind | Decoded | Failed | Decoded length |
|---|---|---|---|
| MUL | 5835 | 0 | 39 446 s |
| FSB4 | 75 | 0 | 0.1 s |
| Wave | 41 243 | 0 | 48 396 s |

Counts are [V-data], from `ou-audio scan "<game>"` (bigfile.000 + patch.000).

## 1. IMA ADPCM blocks

All TRU PC audio is mono 4-bit IMA ADPCM in 36-byte blocks of 64 samples [V-data]:

```
s16 sample      // first output sample
u8  stepIndex   // 0..88
u8  reserved    // 0
u8  nibbles[32] // low nibble first
```

The two decoders in TRU differ:

- **`.mul` streams: tru.exe's own decoder** [V-exe]. Table setup is at `0x595670` and the MMX
  decoder at `0x5957A0`.
  - Output 0 is the header sample.
  - Nibble 0 is skipped: it is masked out and does not move the step index.
  - Nibbles 1–63 produce outputs 1–63.
  - The step table is the standard 89-entry IMA table (`0x8A9588`), clamped to 0x1FFF and multiplied by 4.
  - `delta = pmulhw(step*4, op[nibble])`, i.e. `(step*4*op) >> 16`, with
    `op = (bit0 ? 0x1800 : 0x800) + (bit1 ? 0x2000 : 0) + (bit2 ? 0x4000 : 0)`, negated when bit3 is set.
  - Samples are summed with saturating adds (`paddsw`). The step index moves by the standard IMA
    index table (`0x8A9548`) and is clamped to 0..88.
  - vgmstream's `decode_cd_ima` describes the same thing [S].
- **FSB4 samples: FMOD Xbox-IMA layout** [I].
  - Output 0 is the header sample. Nibbles 0–62 then expand with the standard IMA arithmetic; nibble 63 is unused.
  - Basis: FMOD Ex is linked into the exe (`FMOD::DSPCodecADPCM` RTTI), and vgmstream decodes FSB4 IMA this way [S].
  - We did not trace the statically linked FMOD decoder itself [U].

Evidence that the decoders are right [V-data]:

- Decoded voice shows clean speech spectrograms (§7).
- Across the 64-sample block boundaries the mean |Δsample| is the same as inside blocks: 0.0076 vs
  0.0075 for `sc_04_01_answer`. The header resets the state at every block, so a wrong variant would
  show steps at the boundaries.
- There is no low-frequency drift: the energy below 20 Hz is 0.00 in the voice lines and in the music.

## 2. FSB4 bank (FMOD Ex)

```
0x00 "FSB4"
0x04 s32 numSamples
0x08 s32 sampleHeadersSize
0x0C s32 dataSize
0x10 u32 version             // 0x40000 everywhere [V-data]
0x14 u32 mode                // 0x22 everywhere: BASICHEADERS (0x2) | 0x20 (vgmstream: MPEG_PADDED; meaning here [U])
0x18 u8  zero[8]
0x20 u8  hash[16]
0x30 sample headers, then sample data (each sample's dataSize bytes follow the previous one's)
```

Sample header, 0x50 bytes in every retail bank (the u16 `size` allows more) [V-data]:

```
0x00 u16 size; char name[30]
0x20 u32 numSamples, dataSize, loopStart, loopEnd, mode
0x34 s32 defaultRate; u16 defVol, defPan, defPri, numChannels
0x40 f32 minDistance (1.0), maxDistance (10000.0); s32 varFreq; u16 varVol; s16 varPan
```

- With BASICHEADERS, samples after the first have only an 8-byte header
  `{numSamples, dataSize}` [S]. The parser supports this, but every retail bank has one sample.
- `dataSize` is rounded up to 16 bytes. For example, 49 536 samples = 774 blocks = 27 864 bytes,
  stored as 27 872 [V-data].
- Mode flags: 0x400020 (IMA ADPCM | MONO) in 41 101 Waves and all 75 `.sam`. 0x80404020 adds
  0x4000 and 0x80000000 (FSOUND sync-point flags per vgmstream [S]) in 142 Waves [V-data].
- Those 142 decode like the rest, and their data starts with a normal IMA block header. No sync-point
  data is visible in the 0x50-byte header [V-data]. Where any sync points are stored is [U].
- `defaultRate` is 44100, 48000, 32000 or 8000, and often disagrees with the Wave header rate.
  tru.exe uses the Wave header rate (§3).

## 3. DRM Wave section (type 6)

```
0x00 u32 sampleRate   // e.g. 22050, 28000, 32000, 44100, 44098 ... (11025–48000)
0x04 u32 loopStart    // frames
0x08 u32 loopEnd      // frames, exclusive [I]; 0 = no loop points
0x0C u32 unknown      // 10 or 100 in every section; tru.exe keeps only the low byte
0x10 FSB4 bank with exactly one sample
```

tru.exe evidence [V-exe]:

- The `cdc::WaveSection` loader (`0x592A50`) first collects the 16-byte header. It stores the size
  minus 16, `loopStart`, `loopEnd` and `sampleRate` in the new wave object. A rate of 0 is replaced
  by a default (`DAT_00CC9F90`).
- It copies byte 0x0C into the wave slot at +0x0C.
- `Wave_Find` (`0x584300`) indexes the slots by section id, below `CDC_MAX_WAVES` = 0x1400. A sound
  id is therefore the Wave section id.

Using `sampleRate` as the playback rate is [I]. It is consistent with the header, but we did not
trace the FMOD `setFrequency` call.

Loop points [V-data]:

- 1072 sections have `loopEnd != 0`.
- In all 1072, `loopStart < loopEnd <= numSamples`, and the values are 64-sample block multiples.
- Example: `lara.drm` 0x43 `UI_binoc_zoom_01` 11072..19840 of 27328.

## 4. `.mul` MultiplexStream

The language variant is chosen by spec mask like any other record ([CONTAINERS §1](CONTAINERS.md)).
Each language variant has its own rate, length and subtitle block [V-data].

### 4.1 Header (0x800 bytes, little-endian)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0x00 | u32 | sampleRate | 31874–48011; voice lines are odd values (e.g. 44020, 44053) [V-data] |
| 0x04 | s32 | loopStart | frame; −1 = no loop (5739 records); 0 for music stems |
| 0x08 | u32 | numSamples | frames per channel; 20 records hold 256 fewer (§4.4) |
| 0x0C | u32 | channels | 1 (5260), 2 (524), 4 (40), 6 (11) |
| 0x10–0x1C | | 0 | |
| 0x20 | u32 | hasCinematic | 1 in 205 cinstream records |
| 0x24 | u32 | hasSubtitles | 1 in 2485 voice lines + 193 cinematics |
| 0x2C | u32 | loopStartFileOffset | file offset of the packet holding loopStart (0x800 for music; 0x15000 in `naga_combat_loop_with_intro(1)`) |
| 0x30 | u32 | ? | 3 in the naga loop file, 0 elsewhere seen [U] |
| 0x34 | u32 | ? | 0 [U] |
| 0x38 | f32 | mediaLength | length in 30 Hz frames: 3923 for 5 767 168 samples at 44.1 kHz [V-data] |
| 0x3C | f32[12] | volLeft | mono: [1]; stereo: [1,0]; 4 ch: [1,0,1,0]; 6 ch: [1,0,1,0,1,0] |
| 0x6C | f32[12] | volRight | mono: [1]; stereo: [0,1]; 4 ch: [0,1,0,1] |
| 0x9C | u32[12] | loopStartSamplesToSkip? | name from the TR7 `STRHEADER` [S]; not checked |

This is TR7's `STRHEADER` (cdcEngine `game/pc/snd/MultiplexStream.h`) with one extra u32 at 0x34.
That shifts `mediaLength` to 0x38 and the volume arrays to 0x3C / 0x6C [V-data, S]. vgmstream `mul.c`
reads the same four leading fields [S].

### 4.2 Packets

From 0x800 to the end of the file:

```
u32 type   // 0 sound, 1 cinematic, 2 / 3 padding
u32 size   // payload bytes after this 16-byte header
u32 startSample   // sound packets: first frame (per channel) of this packet
u32 0
```

- **Sound (0).** The payload is `{u32 dataSize, 12 bytes 0}` followed by `dataSize` bytes of IMA
  blocks: `dataSize / channels` bytes for channel 0, then channel 1, and so on.
  - Packets alternate between 20 and 24 blocks (1280/1536 frames), about one 30 Hz frame each.
  - Packet `startSample` always equals the running frame count. `decodeMul` checks this.
  - TR7 has the same packet framing (`ProcessStream`) [S]. tru.exe streams `"%s%s.mul"` (`0x58AA40`)
    through `cdc::MultiplexStreamImpl` [V-exe].
- **Cinematic (1).** The concatenated payloads form the CINE stream. The first one starts with:
  - bytes `"ENIC"`, then `u32 version` = 9, `u32 headerSize`, then the header;
  - frames follow from offset `8 + headerSize`.
  - Frame layout: `{u32 size, u32 frameIndex, …}`.
  - Voice lines have `headerSize` = 0x60 with an all-zero header and frames
    `{u32 size, u32 frameIndex, u32 textLength, text, padding}`. Padding makes `4 + size` a multiple
    of 16.
  - Frame 0 carries the text. Every later frame is `{12, n, 0, 0}`, one per 30 Hz frame [V-data,
    all 2485].
  - cinstream files carry scene channels in their frames. `ou_audio` flags them (`sceneFrames`) and
    leaves them to `src/cine` ([CINEMATICS.md](CINEMATICS.md)).
- **Padding.** Type 3 appears once: 1456 bytes in `naga_combat_loop_with_intro(1).mul`, ending
  exactly at the 0x15000 loop packet. Type 2 is TR7's `kPaddingPacket` [S]; it is not seen in TRU.

### 4.3 Subtitles (voice lines)

The text is `"<lang>\r<line>\r<lang>\r<line>\r…"`.

- Language numbers are tru.exe language indices (`locale::Language`): 0 EN, 1 FR, 2 DE, 3 IT, 4 ES,
  7 PL, 9 RU, 10 CS, 11 NL, 12 HU [V-data].
- Every one of the 2485 blocks parses as number/text pairs [V-data].
- Cross-check: the English line of `pc-w\audio\streams\vo\lara\sc_04_01_answer.mul`,
  `"I need to find an ancient Mayan Ball Court."` (quotes included), is a NUL-terminated string in
  the English `locals.bin` [V-data]. The test `AudioReal.SubtitleMatchesLocals` checks this.

### 4.4 Length quirk

20 records end 256 frames (4 blocks) short of `numSamples`, and the file ends exactly at the last
packet [V-data]. Examples:

- `vo\enemy\merc\b_m10_gat_g_0{1,2,4,6,7}.mul`
- some language variants of `sc_02_11_answer`, `sc_03_08_answer`

`decodeMul` returns the frames that are present. A packet that runs past the end of the file still
throws.

## 5. Music: `.mus` + `.sam` + `.mul`

`0x58BD50` builds `<music dir>\<name>\<name>` + `.mus`, `.sam` and `.mul`. It requires all three,
otherwise it reports "Cannot load music files for the '%s' music! Please rebuild music." [V-exe].

- **`.mul`**: the music audio.
  - 4 or 6 channels form stereo stem pairs. volLeft/volRight route even channels left and odd channels
    right [V-data].
  - Music stems loop from frame 0 (`loopStart = 0`, `loopStartFileOffset = 0x800`).
  - `l4_pool_mus_01` has 4 channels, 5 767 168 frames (130.8 s) and two stems of similar level
    (RMS 0.30 and 0.33 over the first 30 s).
- **`.sam`**: an FSB4 bank. In all 75 retail records it holds one 64-sample silent `temp.wav` [V-data].
  It is apparently the instrument sample bank of a build where no instrument samples were used [I].
- **`.mus` (`Mus!`)**: an AAD-style (Crystal Dynamics' sequencer lineage [I]) instrument bank plus
  one MIDI-like sequence whose meta events set the `.mul` stem gains from global music variables.
  `parseMus` / `MusSequencer` (§6). Survey of all 75 retail files below.

### 5.1 `.mus` layout [V-data, V-exe]

All offsets are file offsets; slot load `0x580E20` relocates them in place.

| Offset | Type | Field |
|---|---|---|
| 0x00 | char[4] | `"Mus!"` |
| 0x04 | u32 | 0x2560 in all 75 = first sequence offset − 8 [I]; unused by `0x580E20` |
| 0x08 | u32 | version. `0x580E20` only warns "The musicfile %s has an incorrect version! (rebuild music)" if ≠ 0x114; all 75 are 0x114. `parseMus` throws on other values |
| 0x0C–0x17 | u32×3 | 0 in all 75 [U] |
| 0x18 | u32 | sequence count (1 in all 75) |
| 0x1C | u32 | stem count (1–3; 137 in total) |
| 0x20 | u32 | 120 in all 75 [U] |
| 0x24 | u32 | offset of the stem records (0x58 bytes each) |
| 0x28 / 0x2C | u32 | label count (11–20) / offset of the label table (u32 track offsets) |
| 0x30 / 0x34 / 0x38 | u32 | sample / tone / program counts (4 / 52 / 50 in all 75) |
| 0x3C | {u32 id, u32 offset}×n | sequence table (id 0 in all 75) |
| then | u32×tones, u32×programs | tone and program offsets; the 0x34-byte sample records follow |

- **Instrument bank** (used by the note-on path `0x5930A0`):
  - Sample (0x34): `name[0x14]` (NUL optional, uninitialised bytes after it), +0x14 offset and
    +0x1C size in the `.sam` bank (loaded by `0x588DF0`), +0x30 runtime pointer.
  - Tone (0x18 + 0x68·n): `name[0x14]`, u32 layer count, layers of 0x68 bytes; layer +0x40
    `{keyLo, keyHi, velLo, velHi}`, +0x44 sample index. The other layer bytes are envelope/pitch
    floats [U].
  - Program (0x20 + 0xC·n): `name[0x14]`, +0x14 bank, +0x18 program, +0x1C zone count; zone =
    `{s32 (−1 in all 75) [U], keyLo, keyHi, velLo, velHi, u32 tone index}`. Channel bank/program
    (channel bytes 0x111/0x110) select the program.
  - The bank bytes (0x3C–0x2568) are identical in all 75 files, and no file has a channel
    (note) event, so the bank is unused in retail [V-data]; this matches the placeholder `.sam`.
- **Stem** (0x58, one per `.mul` channel group): +0x00 sample rate, +0x08 channels, +0x10 frames,
  +0x14 small id (10–24) [U], +0x18 milliseconds per beat (fade time unit), +0x1C volume, +0x20
  fade gain, +0x24/+0x28/+0x2C/+0x30 fade target/rate/seconds left/active, +0x34 mute gain, +0x38
  mute ramp step, +0x3C pan, +0x40–+0x4C pan fade, +0x50 runtime channel pointer. +0x04 and +0x0C
  hold uninitialised build-tool bytes in a few files.
  - In all 137 stems: channels 2, 500 ms/beat, volume = fade = mute = 1.0, pan 0.5.
  - In 74 of 75 files Σ stem channels = `.mul` channels and frames/rate equal the `.mul`
    numSamples/rate. `l1_intro` says 2 513 664 frames against 2 514 944 in the `.mul`.
  - The `.mus` encodes no stream loop points. The `.mul` loops from frame 0 (§4); no retail script
    uses the loop meta commands.
- **Sequence** (`0x588F10`): `"QSMa"`, u32 tempo (µs per quarter note), u16 PPQN, u16 [U], u16
  track count, u16, u32 track offsets relative to the sequence. All 75: tempo 600 000 (100 BPM),
  480 PPQN, +0x0A = 0x0106, one track.
- **Track events** (`0x594520`): VLQ delta ticks, optional status byte (running status), then
  - 0x8n/0x9n/0xAn/0xBn/0xEn: 2 data bytes; 0xCn/0xDn: 1. The usual MIDI channel messages
    (note off/on → `0x580450`/`0x593C40`, program → channel byte 0x110, pitch bend, ...).
  - 0xF0: VLQ length + data, skipped.
  - 0xFF: `{u8 cmd, u8 len, data}` dispatched through the 256-entry table at `0x8A7B10`
    (`0x580F80`). Any other 0xF1–0xFE status makes the exe parse garbage; `parseMus` throws.
  - A track ends at meta 0x44. Labels and jumps address byte offsets from the track start.

### 5.2 Meta commands [V-exe; names of slot/channel commands [I]]

Operands `a0 a1 a2 …`; `var[x]` = global music variable byte array at `0xCCB560` (256 bytes).
A taken jump sets track flag 4, which ends that track's processing for the frame, so the polling
loops in the scripts re-evaluate their variables once per frame.

| Cmd | Handler | Meaning |
|---|---|---|
| 0x00 / 0x01 / 0x02 | 0x5810D0 / 0x5810F0 / 0x591E10 | select channel (a0 − 1) / select slot (0x7F = own) / assign sequence |
| 0x05 / 0x06 / 0x07 | 0x581120 / 0x581170 / 0x5811C0 | slot ms/beat = 24-bit µs / 1000 / tempo of sequence id a0 / ms/beat × 100 / a0 |
| 0x08, 0x0B / 0x09 / 0x0A | 0x581210 / 0x591E60 / 0x5811F0 | start / stop / pause slot [I] |
| 0x0E / 0x0F | 0x581250 / 0x581290 | slot volume / pan = a0 / 127 |
| 0x10 / 0x11 | 0x5812C0 / 0x5812F0 | channel volume / pan byte |
| 0x12–0x15, 0x4B–0x4D | 0x581300 … 0x5813F0, 0x581CE0 … 0x581DC0 | channel mute/unmute (single, 16-bit mask, deferred mask) |
| 0x16–0x19 | 0x581510 … 0x5815A0 | channel / slot volume or pan fade |
| 0x1A, 0x1C–0x1E | 0x5815D0 … 0x581620 | channel byte 0x80, 0x88, flag 2 set/clear |
| 0x22–0x29, 0x2C | 0x581630 … 0x581860 | copy slot/channel state into `var[a0]` |
| 0x2E / 0x2F / 0x30 | 0x5818B0 / 0x581930 / 0x581970 | loop start (a0 times, 0 = forever) / loop end / break to 24-bit offset |
| 0x31 | none | define label a0: no-op at run time; the label table holds its offset (571 uses) |
| 0x32, 0x48 | 0x5819B0 | goto label a0 |
| 0x33 / 0x34 | 0x5819E0 / 0x581A00 | `var[a1] = a0` / `var[a1] = var[a0]` |
| 0x35 / 0x36 / 0x50 / 0x51 | 0x581A20 / 0x581A40 / 0x581F70 / 0x581F90 | `var[a1]` += / −= / ×= / ÷= a0 |
| 0x37 / 0x38 | 0x581A60 / 0x581A80 | `var[a1]` \|= a0 / &= ~a0 |
| 0x39–0x3E | 0x581AA0 … 0x581BE0 | if `var[a0]` ==, ≠, <, >, ≤, ≥ a1 (unsigned): goto label a2 |
| 0x3F / 0x40 | 0x581C20 / 0x581C60 | if `var[a0] & a1` ≠ 0 / = 0: goto label a2 |
| 0x41 / 0x42 / 0x43 | 0x581CA0 … 0x581CC0 | the next meta's operand 0/1/2 is replaced by `var[operand]` |
| 0x44 | 0x581CD0 | end of track |
| 0x4E / 0x4F | 0x581E90 / 0x581F00 | per-channel flag 0xAC = 0 / 1 for a 16-bit mask |
| 0x60 | 0x5865A0 | stem a0 volume (+0x1C) = a1 / 127; cancels the fade |
| 0x61 | 0x5865F0 | stem a0 pan (+0x3C) = a1 / 127 (0.5 if a2 = 0x40) |
| 0x62 / 0x66 | 0x5866B0 / 0x586720 | stem a0 fade (+0x20) to a2 / 127 over a1 beats (0x66: a1 / 32 beats), seconds = a1 × ms/beat × 0.001. Below 1 ms the gain is set at once (`0x586650`) |
| 0x63 / 0x67 | 0x581FC0 / 0x582040 | stem a0 pan fade, same timing |
| 0x64 / 0x65 | 0x5820C0 / 0x582110 | stem a0 ms/beat = 24-bit µs / 1000, or tempo of sequence id a1 / 1000 |
| 0x68 / 0x69 | 0x582160 / 0x582180 | stem a0 mute ramp −0.2 / +0.2 per update, clamped to 0..1 (`0x5891B0`) |
| 0xF0 / 0xF1 | 0x6457A0 (`ret`) | source path text (`…\<name>Ctl.txt`) / 4-byte sequential marker: no-ops |

Used in retail (8462 events): 0xF1 4936, 0x62 1353, 0x31 571, 0x3C 495, 0x3B 494, 0x33 228,
0x60 137, 0x32 79, 0xF0 75, 0x44 75, 0x39 13, 0x3A 6 [V-data].

### 5.3 Run time [V-exe unless tagged]

- Each frame `0x5950A0` advances the slot clock and runs every track (`0x594520`). `0x5891B0`
  ramps stem fade and mute by the frame time. `0x5864E0` then sets each stem's stream volume to
  `fade × volume × mute × slot master (slot +0x38)`; pan goes to the stream only for mono stems.
- Clock: with the `.mul` stream attached (normal play: loader `0x58BD50` → `0x593B70`),
  ticks = `(stream +0x20 (u64) × PPQN) >> 8`. What stream +0x20 counts is [U]. Without a stream,
  beats += dt × 1000 / (tempo / 1000), ticks = beats × PPQN.
- The scripts are polling state machines. In 72 of 75 files every delta is 0. Example
  `l4_pool_mus_01`: reset `var[0x7F]`, `var[0x0E]`, `var[0x0F]` to 0, set both stem volumes to
  0x28, fade both to 0 at once, then labels L0–L8 = intensity level `var[0x7D]`. Each level
  branches to its neighbours with 0x3B/0x3C and re-issues its fades every frame (level 0: stem 0 →
  0x7F over 2 beats, stem 1 → 0x5F over 3; level 7: both → 5; level 8: both → 0x7F).
  - Re-issuing a fade each frame restarts it from the current gain, so the gain approaches the
    target exponentially with a time constant of the fade length [I].
- Timed scripts: `l2_waters_of_thailand`, `l4_escaping_mexico_mus_01` and `l4_laras_new_bike` fade
  stems at 2400–26 880 ticks.
- Variables read by branches: `var[0x7D]` in 51 files, `0x7C` in 2, `0x7B` in 1, `0x0D` in 2. 21
  files have no branch (one-shot pieces such as `l1_intro`). Scripts write `0x7F`, `0x0E`, `0x0F`
  (all files) and `0x0D` (3 files), always 0.
- Who writes the variables (get/set `0x583D40`/`0x583D30`):
  - `0x618910` each update: `0x32` = enemies counted, `0x33` = those with flag 0x84 bit 0,
    `0x37` = bit 6, `0x34` = distance to the nearest one (0xFF beyond 40 800 units). These are the
    "Show Monster Music Vars" set [I]; no retail script reads them.
  - Engine code writes vars 0–2 (`0x721A30`, `0x52E130`, `0x539EF0`, `0x53AB30`, …); meaning [U].
  - Data-driven writes with any index: animation command 0x66 (`0x5B69C0`), instance event data
    (`0x5D2CA0`: +0x24 index, +0x28 value), trigger actions 2/3/4 = set/add/subtract
    (`0x594870`), and get/set/add/subtract helpers `0x4A3E00`/`10`/`30`/`50` [I: script natives].
  - No engine code writes `0x7D` with a constant index, so the intensity level comes from level
    data or scripts [I]. Which objects set it, and when, is [U].
- **`MusSequencer`** models the no-stream clock, variable ops, branches, gotos, end of track and
  stem volume/fade/tempo (0x32–0x40, 0x44, 0x48, 0x50, 0x51, 0x60, 0x62, 0x64–0x66). Commands
  absent from retail data (channel/slot/loop/substitution/pan/mute ramp) are ignored. Callers feed
  `stemGains()` to `Mixer::setMusicStemGains` each frame.

## 6. `ou_audio` API (implemented)

- **Decoders**
  - `decodeMul(bytes)` → `{header, Pcm (interleaved s16, loopStart, volLeft/volRight), subtitles}`.
  - `parseFsb4` / `decodeFsb4` handle IMA mono and PCM16. `decodeWave(sectionData)` returns PCM at
    the Wave rate with its loop points.
  - `parseMus(bytes)` → `MusFile {sequences (tracks of MusEvent {offset, delta, status, meta,
    data}), stems, labels, samples, tones, programs}`.
  - `MusSequencer(file).update(seconds, vars[256])` runs the script; `stemGains()` returns
    volume × fade × mute per stem.
  - All decoders throw `cdc::FormatError` on truncated or inconsistent data.
- **`Mixer`** renders stereo float32 at 48 kHz and streams it to the default playback device through
  an SDL3 audio stream callback (`openDevice`). `mix()` also works without a device (tests).
  - Sounds: `addWaves(drm)` registers every Wave by section id; `play(soundId | pcm, PlayParams)`.
  - `PlayParams`: volume, pan, pitch, loop (`loopStart..loopEnd`), bus (Sfx/Voice/Music), and an
    optional world position with min/max distance.
  - 3D: `setListener({position, forward, up})` (Z-up). Attenuation defaults to FMOD-style inverse
    rolloff, `min / clamp(d, min, max)`; `setAttenuation(fn)` overrides it. Pan comes from
    `dot(dir, cross(forward, up))`.
  - Mono uses equal-power pan, multichannel uses balance. Resampling is linear.
  - Voice and music: `playVoice(pcm)`; `playMusic(pcm, fadeSeconds, stemGains)` crossfades with the
    current track; `setMusicStemGains`, `stopMusic(fade)`.
  - Control: `stop(id, fade)`, `setVolume`, `setPosition`, `setPitch`, `setBusVolume`.
- **Deviations from the original** [I]
  - No FMOD effects or reverb (`reverbVol` and the Wave byte 0x0C are unused).
  - Linear resampling.
  - Mus! scripts run on `MusSequencer`. It uses the tempo clock instead of the stream-synced one
    (§5.3) and ignores pan and mute ramps. The variables are the script runtime's `gamebase` values (§9.4).
  - Voice-line subtitles go to the two-line ticker in ou-game (§9.6).

## 7. Verification log

All files are in `openunderworld-work/audio/` (not in the repo).

| Asset | Result |
|---|---|
| `vo\lara\lara_badwater_01.mul` (EN) | 44020 Hz mono, 1.529 s (ffprobe), RMS −19.0 dBFS; 81 % of energy in 300–3400 Hz |
| `vo\lara\sc_04_01_answer.mul` EN / DE | 44053 Hz 2.563 s / 44153 Hz 2.429 s. Spectrogram (`spec_sc_04_01_answer_en.png`, viewed): harmonic voiced syllables, sibilant bursts at 3–6 kHz, word gaps. Median F0 187 Hz (female speech) |
| `lara.drm` Wave 0x165 `B_L_GRUNT_D_01` | 22050 Hz, 1.146 s, RMS −17.2 dBFS, 70 % in the speech band |
| `l4_pool.drm` Wave 0xE2C `human_thrall_grunt_a_01` | 22050 Hz, 2.247 s; spectrogram (`spec_wave_thrall_grunt.png`, viewed): harmonic growl, then a broadband roar, fading out |
| `music\l4_pool_mus_01\l4_pool_mus_01.mul` | 4 ch 130.8 s; spectrogram (`spec_l4_pool_mus_01.png`, viewed): continuous tonal music with sections repeating about every 42 s |
| `music\l4_pool_mus_01\l4_pool_mus_01.sam` | FSB4 `temp.wav`, 64 samples of silence |
| SDL playback (`ou-audio play`) | voice 2.57 s, grunt 1.16 s and music (3 s cut) played through SDL driver `pipewire` at 48 kHz; the voice finished in real time. `dummy` driver also OK |
| Full install (`ou-audio scan`) | §0 table: no failures |

Not compared with the running original game: absolute levels, the FMOD rolloff curve, the Mus! stem
mixing and the exact FMOD IMA rounding.

## 8. Open questions

1. FMOD's IMA expansion inside tru.exe: is it Xbox-IMA rounding, or does it share the cdc MMX decoder?
2. Mus! clock: what the attached stream's +0x20 counter measures (§5.3). Which level data sets
   music variable 0x7D (and 0x7B/0x7C/0x0D), and when.
3. The Wave header byte 0x0C (10 / 100): volume or priority? `.mul` header fields 0x30/0x34.
4. Where the FSB sync points (mode 0x80004000) are stored.
5. The sound-plex layer (`cdc::SoundPlex*` RTTI, DTP data): how game objects and materials map to
   Wave ids, which footstep/ambient selection needs. Partial, below.

### 8.1 Voice lines are referenced from SoundPlex DTPs [V-data]

A byte survey of every decompressed DRM in the bigfile filelist for `vo\` (2026-10-29, throwaway tool) finds
voice-line names in 81 DRMs; the inspected ones (generalbank, lara) hold them in DTP-data sections, each name in a
variation record:

| Offset | Type | Value |
|---|---|---|
| +0x00 | u32 | 0xBEEBBEEB (record marker, also used as an empty slot) |
| +0x04 | u16 | 0 |
| +0x06 | u16 | 0x50 / 0x5A / 0x64 (80/90/100) [I] volume percent |
| +0x08 | 8 bytes | 0 |
| +0x10 | char[] | NUL-terminated stream path without extension, e.g. `vo\lara\lara_drown_01` |

- `lara.drm` (and every `lara_*` outfit DRM) DTP 2315 holds the 8 `lara_drown_NN` lines as one sound with 8
  variations [I random choice].
- `generalbank.drm` holds 499 distinct Lara voice names in single-line DTPs (e.g. DTP 284 = `vo\lara\sc_01_05_hint`): the `sc_CC_NN_hint` /
  `sc_CC_NN_answer` pairs of the in-game hint system and the other Lara lines.
- The l1 Kraken units and `ms_ani_kraken_body` / `ms_ani_tentacle_*` hold 16–17 `vo\kraken\*` sounds; the l1/l6 ship
  units hold one `vo\enemy\merc\*` sound (mercenary chatter).
- [I] Lines play through a SoundPlex DTP id, not by path (section types of the other 79 DRMs' hits not checked). The DX:HR
  cdc reconstruction (`research/cdcEngineDXHR/cdcSound`, `eventually_generated/cdc/dtp/objectproperties/sfxmarker.h`)
  shows `sfxmarker` objects starting sound refs on enter/exit and setting/incrementing music variables; that is the
  likely writer of the Mus! variables (question 2) [I, not yet checked in tru.exe].
- Decoded since: node types, sfxmarkers, script sound natives and the hint path (§9).

## 9. SoundPlex graphs, sfxmarkers, sound natives (`src/audio/SoundPlex.*`, `src/app/game/Sound.*`)

### 9.1 Node `{i32 type, ptr data}` [V-exe `SoundPlexCollection::Create` 0x5920B0; V-data]

`0xBEEBBEEB` is the unrelocated value of every pointer field (the relocation fills it), not a record marker.

| Type | Class | Data | Played by `SoundPlayer` |
|---|---|---|---|
| 0 | TimeIn (silence) | f32 seconds | waits |
| 1 | Reference | u32 DTP id (looked up by `0x54DF80`) | resolves via `PlexBank` |
| 2 | Wave | u32 Wave section id, u32 (30 seen), 8 bytes | `Mixer::play`; Waves with loop points loop until stopped [I] |
| 3 | Stream | `char*` path (no `.mul`), u8, u8, u16 volume % [I] | `pc-w\audio\streams\<path>.mul`, Voice bus, subtitles |
| 4 | Assignment (0x590970) | [0] child, [1..2] delay, [3..4] volume, each value ± variance (`0x582D50`, clamped ≥ 0), [14] f32 distance [I], byte 0x44 max instances | volume, delay, max distance (§9.12) |
| 5 | Envelope (0x590CE0 / 0x587A90 / 0x587B80) | {Plex*, u32 n, Curve**}; Curve {u16 param, u16 release (0 = releases on stop), Keys*}; Keys {i32 loop start, i32 loop end, u32 count, {f32 time, f32 value, u32 flags (bit 0 = loop start here, bit 1 = loop end here)}[count]} | param 0 = volume from key 0 at age 0, linear to each next key over the key time difference; at loop end (not stopped) back to loop start, loop start = loop end holds (sustain); stopping jumps a release curve from loop end (else the last key) on through the following keys; past the last key a value of 0 ends the voice [I: param meaning from shapes]; other params ignored (§9.13) |
| 9 | Reaction (0x590F30 / 0x592640) | {Plex*, u8 flags, u8 n, Map*}; Map 0x1C {u16 map (low nibble input game parameter, 0xFF random; bits 8-11 output: 0 volume ×, 1 pitch semitones +; bits 12-13 sign mode; 0x4000 / 0x8000 zero below / above), u16 flags (bit 0 off), f32 in min/max, f32 out min/max, u32 n, {f32 x, f32 y}* curve} (0x587FF0 / 0x587F10) | per voice: Σ/Π of the maps on `SoundPlayer::setParam` inputs (sound instance +0x28) |
| 12 | Effect | child at data +0 | child only (effect ignored) |
| 6 | ChoiceList (0x587920) | {n, Plex**, Plex*, i8 mode, i8, u32 counter} | mode 0 random, 1 no repeat, 2 shuffle (n ≤ 32), 3 in order |
| 7 | MaterialList (0x58DFC0 / 0x583960) | {f32, u32 n, {u32 DTP, u16 material, u16}[n]} | the entry for the instance material (+0x44), else material 0 (DTP 0 = silent) |
| 8 | Selector (0x583490) | {u32 n, {u32 DTP, u32 key}[n]} | key 0 [I: the exe's key is a list index from 0x583440], else key -1 |
| 13 | Oscillator (0x587C60) | FMOD tone DSP {f32 rate, …, i8 type} [I] | not played: no reached graph uses it |
| 10 | Sequence (0x58E070) | {n, Plex**, …} | children one after the other |
| 11 | Overlap (0x58B570) | {n, Plex**, …, +0x14 has delays, +0x18 {value, variance}[n]} | all children |

Reached from all 701 sfxmarkers: 6722 Wave, 2381 Assignment, 1508 Reference, 739 ChoiceList, 717 Envelope,
273 Silence, 186 Sequence, 79 Overlap, 17 Effect, 1 Stream; every Reference resolves inside the same unit DRM.
Examples (test `AudioReal.SoundPlexVoiceDtps`): generalbank DTP 284 = Assignment → Stream `vo\lara\sc_01_05_hint`
(100 %); lara.drm DTP 2315 = Assignment → ChoiceList mode 2 of `lara_drown_01..08` (80 %).

### 9.2 sfxmarker [V-exe 0x594B90 / 0x594A80 / 0x594870 / 0x586C70; V-data]

Unit data (Level +0x74) +0xAC count, +0xB0 `sfxmarker*[]`; each loaded unit's markers update every frame
(0x5D4520). Marker 0x3C bytes = DX:HR `dtp::sfxmarker`: position, rotation, +0x20 id, +0x24 runtime handles,
+0x28 `Plex**` sounds, +0x2C count, +0x30 trigger count, +0x38 triggers (0x18 each: u32 bits {bit 0 armed (runtime),
bits 1–2 condition Init/Enter/Exit, bits 3–6 action Start/End/Set/Inc/Dec music variable}, perimeter {f32 radius,
invader, flags bit 0 always, bit 1 inside, bits 2–7 fire count}, action {u8 restart, u8, u16 refs, u32* refs} or
{u8 variable, u8 value}). Inside = 3D distance marker → Lara ≤ radius. Init fires once; Enter/Exit fire when the
inside flag changes (first update: Enter if already inside); without bAlways only once. Start skips a ref still
playing. Survey (test `AudioReal.SfxMarkersAllUnits`): 82 units, 701 markers, 1996 triggers (Enter/Start 968,
Exit/End 898, Init/Start 120); **no retail trigger uses the music-variable actions**, so sfxmarkers are not the
writers of `0x7D` (question 2).

### 9.3 Script `sound` natives [V-exe registration 0x7ACFC0, thunks 0x79C550–0x79C630]

fn 0 same-handle test (0x4AE670), 1 Play3D(anchor) + wait thread (0x4AF230), 2 Play + wait (0x4AF290), 3 Play3D(anchor)
(0x4AE380), 4 Play (0x4AE290), 5 Stop (0x4ACE30 → 0x5527F0 [I: the release request]), 6 wait thread (0x4ACDE0),
7 cueing wait (0x4AF2E0), 8 play with a float unless playing (0x4AE500), 9 stop at once (0x4ACE70 → 0x5527F0(6)). Play
and Play3D leave a sound that still plays on the member alone (0x4AE290 / 0x4AE380 skip to the end when the member's
handle is live); scripts call them every tick to keep a loop going (l3_main_bedroom's firetriggervolumes: 2439 calls in
30 s). The member's initial value is an inline ScriptSound
`{Plex, u16 endType, u16 refCount}` in the unit DRM (`Member::initAt`); 640 such members in the units reach
streams: music stingers (`music\…`), `event_triggered\…`, `vo\kraken\…` — no Lara voice lines. ou-game plays
fn 1–4 (3D at the anchor intro; a live sound only follows the anchor) and stops on 5; the wait threads are not
modelled (the call returns at once).

### 9.4 Music variables [V-exe]

`gamebase` natives 4/5/6/7 (thunks 0x797C10–0x797C70) call 0x4A3E00/10/30/50 = get / set / add / subtract on the
music-variable array 0xCCB560 (§5.3). `script::Runtime::gameValues()` is that array; ou-game's `Music` sequencer
reads it directly and logs every change (`music var 0xNN a -> b`). **Writers [V-data, V-exe]:** soundvolume scripts
(soundvolume.drm 3485: state 0 adds Lara to the volume with `triggervolumebase` 0, `instance` 15 = her intro id) get
`triggervolumebase` prototype 10 (vt 58, reason 1) when she enters and 11 (vt 59) when she leaves (0x5CD520 ->
0x5C8A40); their vt 58 sets `0x7D` (gamebase 5, 0x7D, n; 384 volumes in the 82 units, values 0–19) and calls
`reverb` 1 / `music` 2–3 (bus fades, passive in ours). `0x7B` is set by 28 scripted objects (l7 Yggdrasil vt 63/64,
cinematic helpers), `0x0D` by the shared type 6516 vt 11/14. Replay `tests/replays/pool_to_ragnarok.txt`: tick 101
enter soundvolume 5674 -> `music var 0x7D 0 -> 8` [O, log]. Unit music still switches by unit (§9.6).

### 9.5 Voice lines with subtitles = help on demand [V-data]

Only the generalbank `sc_CC_NN_hint|answer` lines carry subtitles among gameplay voice lines (lara.drm's drown lines,
kraken and merc lines have none). generalbank script 201 (a `uiscreen`, "HelpOnDemandStrLib") holds them as 518
sound members; the PDA strings are locals 168 "Hint" / 169 "Answer". Units send the current condition as a string:
trigger volumes' OnFirstIn (vt 72) call the unit script's vt 28 with `"hint_NN"`, which stores it in a string member
(l3_main_bedroom type 12059 +168) behind a gate on an `instanceref` property (bedroom: symbol 10814 of dispatcher
4086, set by l3_secretchamber). The handler's slot varies by unit (seafloor vt 26 +228, bedroom vt 28, cliffclimb vt 25
shows tutorial popups instead); only 7 units store a condition. The unit script itself is the Level+0x70 DTP +0x1C0 type
(LEVELS.md "Unit script") and starts with its unit.
ou-game: T / Y (bindable `hint`/`answer`, keys ours [I]) play `vo\lara\sc_0<level>_<NN>_hint|answer` for
`Runtime::helpCondition()` [I: CC = the unit's level digit; sc_04_01 = "Mayan Ball Court" fits l4]. Verified
(test `ScriptRealData.SeafloorVolumeSetsHelpCondition`; replay `--level l1_seafloor --spawn 7857,-805,-3402,0`, T at
1 s): tick 1 `help condition hint_05`, T -> `voice vo\lara\sc_01_05_hint "The other two ornate disks are missing
axles."` with the subtitle on screen, Y -> sc_01_05_answer [O]. The bedroom gate stays closed until its property is
set (properties are per caller member in ours, global per target uid + symbol in the exe, 0x4B0690 [U: cross-object]).

### 9.6 ou-game

`GameSound`: PlexBank of lara.drm + generalbank.drm + every loaded unit (its Waves join the mixer); sfxmarkers
per unit (logged `sfxmarker <unit> #i (id) start sound r`); script sounds; drowning plays DTP 2315 when the air
reaches 0 and again while it stays 0 [I: lara.drm DTPs 1014/1020 hold 2315 in records whose state is not decoded];
stream subtitles (English) go to `locale::SubtitleTicker`, drawn by `ui::Hud::subtitles` (also used by cutscenes).
Replays: l2_cliffclimb starts 12 ambient sounds at tick 1, l7_arctic_sea 12, l3_main_bedroom marker 1 at tick 171
after a 2 s walk [O, logs; not listened to].

### 9.7 Animation-event sounds [V-exe 0x5BA000 / 0x5BA0D0 -> 0x5B9F70]

Event records (EFFECTS.md) dispatch on `info & 3`: 0 sound (0x5B99C0), 1 effect, 2 camera / pad (0x5B69C0), 3 weighted
sound pick (0x5B2820). Sound command (record +8): kind 0 `{u8 0, u8 flags (bit 0 follow segment), i16 segment, Plex*}` — a
SoundPlex inside the list's own DTP (lara.drm: 10 666, almost all Assignment -> Reference); kind 1 `{u8 1, u8 segment,
i16, u8 volume, u8 variance, u32 DTP, f32 pitch?, f32 var, f32 delay, f32 var}` [U: the volume byte / pitch floats]; type 3
`{i16, u8 weight[3], volumes, u32 DTP[3] at +0xC, …}` (rand() % Σweights). Position: segment > 0 bone, < 0 HInfo marker,
0 the instance (0x5875A0); material = the instance's surface material (sound instance +0x44, MaterialList input).
Lists run in ou-game (`fx::System::sound` -> `GameSound::animSound`, logged `anim sound <object> frame f DTP d material m`):
Lara's clips (footsteps / efforts by MaterialList), the combat layer clip (`World::combatLayerClip`: reload / equip /
unequip run Lara's and the weapon object's lists, e.g. wp_handgun `reload`), vehicle clips (`motorcycle:<clip>` use
motorcycle.drm), enemy clips (cat_panther, poacher, panther_thrall vocals), weapon `fire` and ammo hit objects
(`bullet_hit` idle at the impact material), script objects' clips (`World::objectClips`). The old hard-coded
footstep / shot / reload waves are gone. Replays [O, logs]: panther_fight 254 wp_handgun, 89 cat_panther, 42 bullet_hit,
40 lara, 24 poacher sounds; pool_to_ragnarok lara footsteps DTP 953/954/956 at material 17, panther_thrall DTP 12660.

### 9.8 Bike engine [V-data; I: inputs]

motorcycle.drm DTP 13589 = Overlap of five layers (SV650 low 05, mid 03, a Sequence, mid 16, mid 15), each
Assignment -> Reaction -> Envelope -> Wave; the Reactions map parameter 2 (rpm, 1500..8000/10000: volume crossfade band,
pitch -1..+5 semitones), 5 (0..700) and 0 (0..50). The Bike class's feeding of the parameters is not traced; ours: rpm
1500 + 8500 · speed / max speed, parameter 5 = acceleration as throttle (0..700), 0 = 0, started while `Ride`, moving with
Lara. Replay bike_ride: tick 66 `bike engine start`, rpm 4252 / 7224 / 8822 at ticks 120-240, stop at 379 [O, logs].

### 9.9 Music requests, cinematics [V-exe]

`music` natives (bind 0x7ACB40) work on a value `{char* track, i32 slot}`: 1 Play(fade s) (0x5942F0), 2 Play(fade s,
fade-out s) (0x594320), 3 Stop(fade s) — 0x593EB0 with a null name fades that slot out. Slot = player 0xCC5260 + slot ×
0x528. `music` locals start at their initial value (Vm), `script::Runtime::takeMusicRequests` hands them to ou-game's
Music (one voice: a request crossfades, a stop fades only the slot holding the playing track). Unit music follows the
scripts; `<unit>_mus_01` only plays while no request has started a track [I]. Replay pool_to_ragnarok [O]: tick 1
l4_pool_mus_01; tick 101 soundvolume 5674 vt 58 -> unit script 14147 vt 25 -> `l4_staircase_upper_mus_01` (slot 1), stop
slot 0 (3 s), 0x7D 0 -> 8 — the original switches there, not on the unit change. l1_ship_lower_hallway: tick 1
`l1_ship_engine_room_mus_01` (slot 1). 0x7B / 0x0D writers were not reached in a replay. Natives on a null receiver are
skipped as the exe does (soundvolume 1002 stickybool 2), messagechannel Send without subscribers is a no-op.
Cinematics mute the Music and Sfx buses while their stream plays and restore the menu volumes after (logged) [I].

### 9.10 Enemy chatter [V-data; I: categories]

The l1 / l6 ship units (exterior, upper hallways, engine halls) hold the merc lines as DTPs, each one graph over 5–10
streams `vo\enemy\merc\b_<voice>_<CAT>_<g|s>_NN` (voices M02 / M10 / M11; categories EVG, EAR, FDY, ADV, FLK, CBR,
INV, SWP, GAT; `s` = FDY). tru.exe paces them through the `bark` native script type and a BarkTimer (not decoded).
Ours: `GameSound::syncUnits` files each unit's chatter DTPs by category; `ai::Enemies::barks` (EAR spotted, CBR cover,
FDY squad mate down, SWP lost her; GAMEPLAY.md "Mercs") start a random DTP of that category at the soldier.

### 9.11 Main menu, menu sounds, pause, leaving play [V-exe, V-data; I where tagged]

- **Title music.** ScriptMainMenu (generalbank script 8, vt 2) sends GameGlobals (script 5) the message `startTitleMusic`;
  GameGlobals vt 0 plays its sound member +380 = generalbank DTP 78: Assignment → Envelope (param 0: 1 → 0 over 1.05 s)
  → stream `music\tomb_raider_underworld_main_theme` (`pc-w\audio\streams\…mul`, 213.6 s stereo, loop start 0).
  `stopTitleMusic` comes from scripts 6 (constructor), 60 (attract-mode timer), 110 ("Unmuting audio during a movie")
  and 121 (intro movie). Ours: `Music::title` on the Music bus whenever the main menu opens; it stops with a 1.05 s fade
  [I: the envelope as the release curve] when a game starts or a movie plays.
- **Menu movie sound.** The frontend (0x5DAE20) loops `medsea_loop.bik` through the movie player 0x4A12B0, Bink audio on
  (track 0: 44.1 kHz stereo sea ambience, about −25 dBFS). Its volume (0x4A0F20 `BinkSetVolume`) is sound category 25 of
  globalsoundinfo (below). Ours: decoded once (`video::Movie::decodeAudio`), cut to the video's 30.03 s and looped with
  the picture on Bus::Sfx [I: category 25 lies under the SfxVolume category 4].
- **Sound categories** (globalsoundinfo.drm DTP 792, struct at +0xE0): +0x04 count 77, +0x00 → 0x84-byte records
  (first word = parent); +0x24 → {music 1, sfx 4, voice 7, …, +0x14 movie 25} — the option sliders MusicVolume /
  SfxVolume / VoVolume set categories 1 / 4 / 7 (0x477CA0 → 0x589520 / 0x589590 / 0x587140); +0x1C/+0x20 the pause
  list {6, 25, 3, 9}: 3 = the only child of music, 6 and 9 = children of sfx, 25 = movie. The voice category and sfx's
  third child 54 are not in it.
- **Pause.** Opening the pause menu (0x5D4780) calls 0x5874B0 → 0x5873A0(1): the paused bit of every listed category;
  0x5874D0 clears it. Ours: `Mixer::setWorldPaused` holds every world voice of the Sfx and Music buses while the pause
  menu or the PDA is open (`world sound held` / `resumed` in the log); Bus::Voice (help lines) plays on, and menu sounds
  pass `PlayParams::world = false` [I: menu sounds are category 54].
- **Leaving play.** Level change (0x5DF760, 0x5DF8C0) and quitting to the frontend (0x5DAB40) call 0x593D10, which stops
  every sound and the music and resets the category volumes (0x5895F0). Ours (`audio reset` in the log): New Game,
  Continue, Load, Treasure Hunt, Revisit, quick load and Quit to Menu stop the music (`Music` state cleared; the unit's
  music starts afresh), `GameSound::reset` (every SoundPlayer instance incl. pending parts, sfxmarker triggers re-armed,
  script / drowning / bike engine / help handles, subtitles) and `Mixer::stopWorld`. Movies hold the world sound while they
  play and stop the main menu sound.
- **Menu sounds.** The uiscreen scripts play their `sound` members (Play, type 71 +0xD4) on input. Script 130: vt 36/37
  member 1582 = DTP 214 (0.04 s, volume 0.2) when the selection moves, vt 33/34/38/39 member 2748 = DTP 243 (0.22 s)
  before opening a screen, vt 40 member 2749 = DTP 217 (0.39 s) going back, vt 35 members 2750 / 2751 = DTPs 247 / 248
  (0.15 / 0.06 s, 0.2) changing a value; the same members recur in scripts 104, 110–117, 122, 127–134. Ours
  (`GameSound::menuSound`, logged `menu sound DTP d wave w`): every menu screen, from the menu's reaction to the key —
  accept / opened screen 243, Back 217, a value changed 247 (Right) / 248 (Left) [I: direction], selection moved 214
  [I: one mapping for all screens; the main menu script 8 has no sound members of its own]. DTPs 100 / 101 (members
  1589 / 467, scripts 39–45, 121, 128–134) and 140–143, 209–211 (PDA scripts 7, 37) are not mapped [U: their events].

### 9.12 3D rolloff and listener [V-exe 0x5850E0 / 0x585070 / 0x57FD50; V-data globalsoundinfo]

globalsoundinfo.drm object data (ObjectBase DTP 792 +0xC8) +0x28 → {f32 max distance 3160.56, f32 min distance 158.03,
f32 curve 1.0, …, +0x20 0.5, +0x24 0}. A 3D sound's gain (0x5850E0 → 0x585070(−d, −max, −min, curve)): 1 within min,
0 beyond max, else f = (max − d) / (max − min), gain = (f² − f)·curve + f = f². The emitter's own max (its +0, set when
> 0) replaces the global one: ours = the innermost Assignment's [14] distance (lara.drm footsteps 5000) [I: which field
fills the emitter]. The listener (0x8A7F40, updated each frame by 0x57FD50 from the camera position) sits at
camera + 0.5 · (Lara − camera) for distances (+0x20) and at the camera for panning (+0x24 = 0); ours puts the one mixer
listener at that midpoint (`ou::audio::rolloff`, `kSound*`, `GameSound` → `Mixer::setAttenuation`, main.cpp
`setListener`). Before this the Mixer's FMOD default `1 / clamp(d, 1, 10000)` applied with the listener at the camera:
Lara's own footsteps ~300 units away played at about −50 dB, every unit ambience further down — the "most chapter
sounds do not play" report.

### 9.13 Envelope sustain and release [V-exe 0x590CE0 / 0x587A90 / 0x587B80; V-data]

0x590CE0 marks key flags bit 0 / bit 1 as the curve's loop start / end; 0x587A90 steps key to key, and on reaching the
loop end while not stopped jumps to loop start + 1 (loop start = loop end: holds there). A loop end without a loop
start only requests the release (vfunc 4). Stop (0x587B80, states 1/4/5) sets every curve whose second u16 is 0 to its
loop end (or the last key) so the following keys play out; an envelope with no sustain and no such curve stops at once.
Survey of the 1120 sfxmarker sounds (82 units): 717 volume curves, 514 hold a sustain key, 53 end at 0 without a loop,
150 hold a non-zero last key; all 717 carry the release u16 = 0. Ours ignored the loops: every sustained ambience
loop (824 of the 1120 marker sounds loop a Wave) was cut when its curve reached its last key (≈1 s). Now
`Plex::Curve::value` follows the loop / release, `SoundPlayer::stop` releases (a second stop cuts) [I: a released
curve past its last key ends the voice even when that key is not 0]. generalbank DTP 78 (title music) holds key 0 =
1.0 until stopped, then fades over 1.05 s. Tests `SoundPlex.EnvelopeSustainHoldsUntilStopThenReleases`,
`SoundPlex.RolloffFollowsGlobalSoundInfo`.

### 9.14 SoundComponent slots (object loops) [V-exe 0x58D6A0 / 0x593D50 / 0x5B69C0; V-data]

ObjectBase +0xA8 count, +0xAC {Plex*, i16 segment, i16}[count] (0x58D6A0 builds instance +0x248 from it). Event
records of kind 2 with command 0x60 (0x5B69C0: a1 & 0xFF ≠ 0xFF, slot = a2 & 0xFF, action = a2 >> 8) run 0x593D50:
action 0 releases the slot's sound, 1 starts it unless it plays, others restart it, at the slot segment with the
instance material; the component's sounds end with the instance (0x58D740). 129 of 794 objects have slots, 114 hold
2619 such records: waterfalls and gutters (birth list: start), sa_uo_flush_gate / ar_uo_reward_door / grinders / gears
(clip start / stop), ignitables, motorcycle (accelerate / donut), bats, thralls, tigers. Ours: `fx::ObjectFx::soundSlots`,
`Event::slotSound`, `fx::System::slotSound` from birth lists and every clip span (Lara, enemies, script objects),
`GameSound::slotSound` (logged `slot sound <object> owner o slot s`); `fx::System::ownerStopped` (streamed-out intros)
and departed enemies stop them. fx_fire / fx_water_spout slots without records are played by scripts (§9.3).

Measured (ou-game at 160×120 under Xvfb, `--music-wav` with the Music bus at 0, 30 s, a walk / turn / jump / shoot
script; RMS of the Sfx + Voice mix) [O]:

| run | before | after | starts in 30 s after (silent) |
|---|---|---|---|
| `--chapter 0` (l3_main_bedroom) | −99.4 dB | −29.7 dB | 503 anim (75: material 1 has DTP 0 in lara.drm DTP 2305 = silent by data), 20 sfxmarker, 1 script fire loop (was 2439 restarts), 3 slot |
| `--load chapter-1` (l1_seatop) | −76.8 dB | −20.2 dB | 142 anim (0) |
| `--load chapter-2` (l2_cliffclimb) | −71.0 dB | −15.2 dB | 871 anim (3), 13 sfxmarker, 7 slot (waterfalls) |
