# Audio formats: IMA ADPCM, FSB4, DRM Wave sections, `.mul` MultiplexStream, music

Labels: **[V-data]** observed in the retail GOG data; **[V-exe]** read in the tru.exe decompilation;
**[S]** from a third-party source; **[I]** inference; **[U]** unknown.
Code: `src/audio/Audio.{hpp,cpp}` (decoders), `src/audio/Mixer.{hpp,cpp}` (SDL3 mixer) and
`tools/ou-audio` (CLI).

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
    leaves them to `locale::parseCineStream` ([TEXT.md](TEXT.md)).
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
- **`.mus` (`Mus!`)** [V-data] layout:
  - `"Mus!"`, then `u32` (0x2560 in `l4_pool_mus_01`, = offset of the sequence chunk minus 8), then
    `u32 0x114` (version? The exe has "The musicfile %s has an incorrect version!") [I].
  - Then instrument/tone definitions with names such as `Sine_440`, `Brass Piccolo …`, `Atk 11`,
    `Rel 5`, `Sus 75`, `Decay 1`.
  - Then a `"QSMa"` chunk (`aMSQ` stored reversed). It holds the source path
    `e:\tr8\audio\music\L4_pool_mus_01\L4_pool_mus_01Ctl.txt`.
  - Then MIDI-like `0xFF xx len …` events (0xF1 4-byte markers, 0x31/0x33/0x3B/0x3C/0x60/0x62 …).
  - These look like Crystal Dynamics' AAD sound-font/sequence formats [I]. Event semantics are [U].
  - The sequencer is not implemented. **Deviation:** `Mixer::playMusic` plays the `.mul` with
    per-stem gains chosen by the caller, instead of the script-driven stem mix.

## 6. `ou_audio` API (implemented)

- **Decoders**
  - `decodeMul(bytes)` → `{header, Pcm (interleaved s16, loopStart, volLeft/volRight), subtitles}`.
  - `parseFsb4` / `decodeFsb4` handle IMA mono and PCM16. `decodeWave(sectionData)` returns PCM at
    the Wave rate with its loop points.
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
  - No Mus! sequencer.
  - Voice-line subtitles are exposed, but nothing displays them yet.

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
2. Mus! event semantics; how the sequencer drives `.mul` stems and loop points.
3. The Wave header byte 0x0C (10 / 100): volume or priority? `.mul` header fields 0x30/0x34.
4. Where the FSB sync points (mode 0x80004000) are stored.
5. The sound-plex layer (`cdc::SoundPlex*` RTTI, DTP data): how game objects and materials map to
   Wave ids, which footstep/ambient selection needs.
