# Movies (Bink): inventory, original playback, our player

Code: `src/video/Movie.{hpp,cpp}` (libav decode), `src/video/Player.{hpp,cpp}` (SDL3 window + audio),
`tools/ou-movie`. Subtitle text: [TEXT.md](TEXT.md#2-movie-subtitles-sch).

Evidence tags as in [CONTAINERS.md](CONTAINERS.md): **[V-data]** observed on the GOG install,
**[V-exe]** read in our Ghidra decompilation of `tru.exe`, **[V-run]** observed running our code,
**[I]** inference, **?** unknown.

## 1. Inventory (`ou-movie --info Movies/`) [V-data]

All five are Bink 1 (`BIKi`), demuxed by libavformat `bink`, video `binkvideo`, audio `binkaudio_dct`.

| File | Video | fps | Duration | Audio tracks (Bink track id: rate, channels) |
|---|---|---|---|---|
| `intro.bik` | 1280×720 | 29.97009 | 120.120 s | 0: 48000 Hz 2 ch |
| `prevontr.bik` | 1280×720 | 29.97 | 64.464 s | 0: 44100 Hz 2 ch; 1, 2, 3, 4: 48000 Hz 2 ch |
| `explode.bik` | 1280×720 | 29.97 | 17.784 s | 0: 44100 Hz 2 ch |
| `medsea_loop.bik` | 1280×720 | 29.97 | 30.030 s | 0: 44100 Hz 2 ch |
| `credits.bik` | 960×720 | 30.00003 | 405.433 s | 0: 44100 Hz 2 ch |

- The Bink track ids are the bytes after the per-track rate/flags words in the header (`prevontr.bik`
  offset 0x54: `0 1 2 3 4`); libav exposes them as `AVStream::id`. [V-data]
- `prevontr.bik` ("Previously on Tomb Raider") is the only movie with several audio tracks: ids 0–4 =
  English, French, German, Italian, Spanish by the exe's language order (§2). [V-exe] for the selection
  rule, [I] for the per-track language (not listened to per track).
- No `<name>_nw.bik` (4:3, §2) files exist in this install. [V-data]

## 2. How tru.exe plays a movie [V-exe]

Unnamed functions `0x4A0C70` (open) / `0x4A12B0` (loop) / `0x4A0F70` (per frame) / `0x4A1100` (draw):

- File: `0x4A0B00` builds `movies\<name>.bik` and checks it with `fopen`. When the display aspect
  ratio ×10000 (`g_pcDeviceManager+0x170`) is below 13334, `<name>_nw.bik` is tried first.
- Audio track: `BinkSetSoundTrack(1, &lang)` with `lang = 0x49A730()` (the language index, 0..12,
  else 0), then `BinkOpen(..., 0x4000)`. If no track has `BinkGetTrackID == lang`, the movie is closed and
  reopened with track 0. Our `Movie(file, audioTrackId)` does the same selection on libav streams.
- Frame loop: `BinkWait` / `BinkDoFrame` / `BinkCopyToBuffer` into a texture, `BinkNextFrame`; Bink's own
  clock paces it. Volume via `BinkSetVolume` from the movie volume setting.
- Subtitles: `0x4A0720` loads `\tr8\local\localization\movies\<name>.sch`; each frame `0x4A0910` is called
  with `t = FrameNum / (FrameRate / FrameRateDiv)` seconds. See [TEXT.md §2](TEXT.md#2-movie-subtitles-sch).
- The quad is letterboxed by the movie/display aspect ratio (`0x4A1100`).
- Skippable when the caller's flag allows it (`0x4A12B0` second argument); the input check itself was not
  traced. ?

## 3. Our player

- **Decode** (`ou::video::Movie`): `avformat_open_input` → `binkvideo` frames → `sws_scale` to RGBA
  (bilinear, same size); selected audio track → `swr_convert` to interleaved float32 at the track's rate.
  Unselected streams are discarded at the demuxer. EOF flushes both decoders.
- **Present** (`ou::video::playMovie`): `SDL_Renderer` with a streaming `RGBA32` texture, logical
  presentation = movie size, letterboxed; vsync on. Audio: `SDL_OpenAudioDeviceStream` with the track's
  spec (F32); SDL converts to the device.
- **Clock**: audio clock = frames handed to SDL − frames still queued in the stream − one device buffer
  (`SDL_GetAudioDeviceFormat` sample frames). A frame is shown when `pts <= clock`; frames already late
  when a newer one is due are dropped. When the audio has drained (or there is no device) the clock
  continues on `SDL_GetTicksNS`. Up to 16 frames are decoded ahead while the audio queue holds < 0.25 s.
- **Skip**: any key, mouse button or gamepad button, or closing the window.
- **Subtitles**: the exe's two-line ticker (see TEXT.md §4) at the exe's place (y 352 and 374 of 480,
  centred), SDL3_ttf with a system sans font (Noto Sans → DejaVu Sans → Liberation Sans, or `--font`);
  without SDL3_ttf, SDL's 8×8 debug font (ASCII only). The original game font is not used.

`ou-movie <file.bik> [--frames N --screenshot out.ppm] [--lang xx] [--track ID] [--no-audio] [--no-subs] [--stats]`

## 4. Verification

| Check | Result | Tag |
|---|---|---|
| `ou-movie intro.bik --frames 300 --screenshot` (≈10 s, audio clock, PipeWire) | Frame 300 viewed: hammer on the sea floor, 1280×720 in a 1280×720 window. 300 frames, 0 dropped. Drift (pts − audio clock): mean \|d\| 10.6 ms, max \|d\| 21.3 ms, first second −9.7 ms, last second −10.5 ms → no growth over 10 s (the ~10 ms is vsync quantisation) | [V-run] |
| `ou-movie prevontr.bik --lang en/ru --frames 215` | Viewed: "No, get back!" / "What's happening, Mother?" (Russian: Cyrillic via Noto Sans) on two lines at the bottom; drift mean 11.7 ms, max 23.2 ms | [V-run] |
| ASan/UBSan (`-DOU_SANITIZE=ON`) on both runs and on full decodes | Clean apart from Mesa RADV's known 256-byte `pthread_once` leak (SDL_Renderer picked Vulkan; `tests/lsan_vulkan.supp`) | [V-run] |
| Audio decode | `prevontr.bik` German track through `Movie` is bit-identical to `ffmpeg -map 0:a:2 -f f32le`; English 2 843 520 frames @44.1 kHz, German 3 095 040 @48 kHz (64.48 s each, video 64.46 s); RMS −14.8 / −14.4 dBFS; peaks +0.8 / +2.8 dBFS (Bink DCT overshoot above 1.0, SDL clips); spectrum plausible (content up to ~16 kHz) | [V-run] |
| `explode.bik` full decode (test) | 533 frames, monotonic pts, audio length within 0.25 s of video length | [V-run] |
| Skip by key press | Code path not exercised by an automated test (needs real input) | ? |

Not compared against the original game's playback (timing, letterbox colours, subtitle font/position
on screen). Bink 1 decode correctness rests on libavcodec.
