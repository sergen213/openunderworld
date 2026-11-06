# Text: locals.bin, movie subtitles (.sch), cinematic subtitles (cinstream .mul)

Code: `src/locale/Locale.{hpp,cpp}` (`ou_locale`), CLI `tools/ou-text`. Player side: [VIDEO.md](VIDEO.md).
Evidence tags as in [CONTAINERS.md](CONTAINERS.md): **[V-data]** observed on the GOG install,
**[V-exe]** our Ghidra decompilation of `tru.exe`, **[V-run]** observed running our code, **[I]** inference,
**?** unknown. Function addresses are tru.exe VAs (unnamed in our Ghidra project unless given).

## 0. Languages [V-exe]

The exe's language index (`0x49A730` returns it) orders the `lang_*` table in `0x4A0720`:

| Index | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `ou::locale::Language` | English | French | German | Italian | Spanish | Japanese | Portuguese | Polish | UkEnglish | Russian | Czech | Dutch | Hungarian |
| `.sch` key | `lang_english` | `lang_french` | `lang_german` | `lang_italian` | `lang_spanish` | `lang_japanese` | `lang_portugese` | `lang_polish` | `lang_ukenglish` | `lang_russian` | `lang_czech` | `lang_dutch` | `lang_hungarian` |

The same index is the archive spec-mask bit (`1 << index`, CONTAINERS §1.4), the Bink audio track id
(VIDEO §2) and the language number in cinematic subtitles (§3). [V-exe] for all four uses; [V-data]: Dutch
text is the record without bit 11, Czech/Russian/Polish/Hungarian subtitle lines carry 10/9/7/12.
Subtitle language is a separate option (`*(short*)(DAT_00c59264+0x208)`, registry `SubtitleLanguage`).

**Setting** [V-exe]: `Registry_LoadSettings` (0x4791D0) maps the installer's `LanguageString` (English French German
Italian Spanish Dutch Polish Russian Czech Hungarian) to the index (0 1 2 3 4 11 7 9 10 12); `Registry_LoadOptions`
(0x477EA0) reads `Language` (option 0x95) and `SubtitleLanguage` (option 0xB, -1 = the game language; 0x477CA0 keeps
-1 0 1 2 3 4 7 9 10 11 12). `0x49A900(lang)`, called once at start, sets spec mask `1 << lang` on every archive,
reloads locals.bin and the font. The in-game LANGUAGE row names the same ten languages (locals.bin 110..119 ENGLISH,
French, Italian, German, Spanish, Russian, Polish, Czech, Hungarian, Dutch).
**Ours** (`locale::kInstallLanguages`, config `game.language` / `ou-game --lang` / launcher Game tab / AUDIO/VISUAL >
LANGUAGE, applied live): one setting for text, voice and subtitles [I: the exe's two options folded]. A language sets
the archives' spec mask (`BigfileSet::setSpecMask`): voice streams and cinematics read its records (FR DE IT ES their
own `0xFFFFE0xx`, the others English's `0xFFFFFFE1`) from their next stream on; subtitles (cinematic, voice line,
movie `.sch`) and the movie's Bink track are its own; locals.bin is its own for EN FR DE IT ES NL, English for PL RU
CS HU (`locale::textLanguage`) [I: the exe's mask rule would give those the French table]. Test
`LocaleRealData.InstallLanguagesMapToTheirRecords` [V-run].

## 1. `pc-w\local\locals.bin`

Layout and records: [CONTAINERS §5](CONTAINERS.md#5-pc-wlocallocalsbin-ui-text).

**Loader** `0x49A800` [V-exe]: reads `%slocal\locals.bin` through the multi-file system (so the current
spec mask picks the variant, patch.000 first); fatal error if missing. Each offset is turned into a
pointer: offset 0 → `""`; a string failing `0x597DC0` → the literal `"Invalid UTF8 encoding"`.
`0x597DC0` rejects bytes C0 C1 F5–FF anywhere, classifies lead bytes by length, and only requires the top
bit on continuation bytes (no overlong/surrogate checks). **Getter** `0x49A7A0`: id outside
`[0, count)` → `"ERROR OUT OF TABLE RANGE"`.

`ou::locale::Strings::get(id)` returns exactly these results; unlike the exe, the constructor throws
`cdc::FormatError` on a truncated header/offset table, an offset past the end or a missing NUL.

**Observed** (`ou-text langs`) [V-run]:

| Language index | Record used | Header value | Strings | Non-empty | Invalid (exe check / strict RFC 3629) |
|---|---|---|---|---|---|
| 0 English | `0xFFFFF7E1` | 0 | 5136 | 1448 | 0 / 0 |
| 1 French | `0xFFFFF7E2` | 1 | 5136 | 1448 | 0 / 0 |
| 2 German | `0xFFFFF7E4` | 3 | 5136 | 1448 | 0 / 0 |
| 3 Italian | `0xFFFFF7E8` | 2 | 5136 | 1448 | 0 / 0 |
| 4 Spanish | `0xFFFFF7F0` | 4 | 5136 | 1448 | 0 / 0 |
| 11 Dutch | `0xFFFFFFE0` | 5 | 5136 | 1448 | 0 / 0 |
| 5–10, 12 | French table (first matching record in patch.000) | 1 | 5136 | 1448 | 0 / 0 |

- The header value is its own numbering (IT = 2, DE = 3), not the language index. [V-data]
- Languages without their own text (Japanese, Portuguese, Polish, UkEnglish, Russian, Czech, Hungarian)
  resolve to the French table under the exe's spec-mask rule because every text record has bits 5–10 and
  12 set. [V-run] for our port of the rule; [I] that the shipped exe does the same (lookup order of
  equal-hash records in `0x4726E0` was ported, not run).
- Id ranges [V-data]: 1–1372 contiguous (menus, PDA/journal text, Lara's hint lines in quotes, item names
  and descriptions, control prompts), 5000–5015 (PC error dialogs), 5050–5052 (mouse buttons), 5060–5080
  (controller names), 5100–5135 (PC setup dialog). 3688 of the 5136 offsets are 0.
- Strings use U+1800–U+180D (Mongolian block) as controller-button placeholders, e.g. 1320 "Press ᠀ to fire
  the grapple"; ids 5061–5076 are the Xbox 360 pad's button names in its order (Y, X, B, A, LT, RT, LB, RB, LS,
  RS, Start, Back, d-pad up / down / left / right): icons U+1801, 1800, 1802, 1803, 1805, 1807, 1804, 1806, 180C,
  180D, "Start Button", "Back Button", 1808, 1809, 180B, 180A. Text UI elements with the prompt flag (+0x229) run
  `0x5A7C20`: U+1800 + i becomes "\x1B" + the name of the control bound to virtual button `0x8A9EF8[i]` + "\x1B"
  (U+182A..182C: the keys Return / Escape / BackSpace, codes 0x9C / 0x81 / 0x8E; unmapped placeholders stay as
  icons; no binding: "\x1B<Unknown>\x1B"). The font draws 0x1B as a highlight toggle (`0x476D80`) and the icons
  white (`0x476540`). Port: GAMEPLAY.md "Menus". [V-data] code points and names, [V-exe] the substitution.
- Printf formats occur (`2` = `%02d:%02d:%02d`, `5101` = `%d by %d`). [V-data]

**How string ids are referenced** [V-exe] (125 call sites of `0x49A7A0`):

| Source of the id | Example | Evidence |
|---|---|---|
| Constants in the exe | 5000 (error caption), 0x13BA–0x13BC (5050–5052 mouse buttons), 0x13EE–0x140F (5102–5135 setup dialog, `0x47A2C0`/`0x47B5C0`), `Win32_GameMain`, `MAIN_DoMainInit` | immediate arguments |
| UI scripts | `cdc::UIPropertyLocalStr` (RTTI; derives from `UIPropertyBase<int>`), script type `NtUILocalStrProperty_ScriptType` (`"uilocalstrproperty"`); the int at `+4` is the id (`0x59E280`, `0x5A0620`, `0x5A2E30`) | vtable + getter calls |
| Object/item data | 16-bit id at `+0x80` (name) and `+0x88` (description, else `"no description"`) of an object's data block (`0x6E7F70`, `0x6E7FC0`, inventory slots) | getter calls |
| Global data object | 16-bit ids at `+0x12C` and `+0x22C…+0x23A` of `*(DAT_00e7ee50+0x1BC)` (`0x6DA770`, `0x6DB190`, `0x77FEF0`, `0x789D30`) | getter calls |

PDA pages and level names are among ids 1–1372 [V-data]; which data block references them (g_pda.drm
DTP, UI scripts) is not traced. ?

## 2. Movie subtitles (`.sch`)

`\tr8\local\localization\movies\<movie>.sch`, a cdc SchemaFile text (cdcResearch `files.md`) [S]. Only
`prevontr.sch` exists (bigfile.003, 7927 bytes, mask `0xFFFFFFFF`) [V-data]. CRLF text:

```
Version 1
TextEntry
line0
lang_english 2008-08-25 10:59:38 "You never have to be cold, my Lara, if you don't want to be.\(6)No, get back!\(6.9)…"
lang_czech 2008-10-16 15:04:14 "…"
```

Languages present: english, czech, dutch, french, german, hungarian, italian, polish, russian, spanish
(no japanese/portugese/ukenglish). [V-data]

**Loading** `0x4A0720` [V-exe]: the first line whose start matches the subtitle language's key
(`_strnicmp`) and contains a `"`; the text runs to the last `"` before the line end.
**Playback** `0x4A0910(t)` [V-exe], once per movie frame with `t` = Bink frame / fps:
`\(seconds)` (parsed with `atof`) sets the start time of the text after it; text before the first marker
starts at 0. When `start <= t` the text up to the next `\(` is consumed: `\n` ends a line (each line is
pushed to the ticker, §4), any other `\c` stands for `c`. At most one marker is handled per call and a line
is cut at 255 bytes (not reproduced; no line is that long).

`ou::locale::parseMovieSchema` → `Cue{start, text}`; `MovieSubtitles` drives the ticker.
`ou-text movie prevontr --lang de` lists the cues. English: 21 cues, 0 … 59 s. [V-run]

## 3. Cinematic subtitles (`pc-w\cinstream\*.mul`)

In-engine cutscenes stream from `pc-w\cinstream\<nnn_name>.mul` (MultiplexStream) next to
`<nnn_name>.drm`. Subtitles are inside the cinematic stream, per frame, for all languages at once.

**MultiplexStream** [S] (Templates `tr7ae_mul.bt`), [V-data] for TRU: 2048-byte header (`u32 rate, i32
loopStart, u32 loopEnd/samples, u32 channels, …, u32 hasCinematic @0x20, u32 hasSubtitles @0x24, …`), then
packets `{u32 type (0 sound, 1 cinematic, 2 padding), u32 size, u32 pad[2], size bytes}`. Sound packets
start with their own 16-byte header (inside `size`). Concatenating the type-1 payloads gives the CINE
stream (`ou::cine::demux`).

**CINE stream** [V-exe] `0x4537A0` + `0x456580` (header), `0x457100` (frames), `0x4538D0` (subtitles). The
header, the run-length coded per-frame channels and their meaning (camera, actors, markers) are in
[CINEMATICS.md](CINEMATICS.md); every frame ends with `u32 subtitleLength; subtitleLength bytes`, then padding to 16.

Subtitle bytes: repeated `<language index>\r<text>\r`; `\n` inside text separates lines. The exe shows
entries whose number equals the subtitle language (or, with subtitle language −1, the game language);
each line loses everything up to its first `": "` (speaker names) and is pushed to the ticker (§4).
Text is cut at 255 bytes; blobs ≥ 4095 bytes print "bad subtitle data". Frames run at 30 per second
(the debug overlay prints `frame / 30` as mm:ss). [V-exe]

**Verified** [V-run] with `ou::cine::scan` (`ou-text cine <name> --lang all`) on all 53
cinstream .mul files (English-voice record where several exist): every header size and every frame `size`
field matches, frames are numbered 0…n−1 before the −1 terminator; 845 frames carry 3253 subtitle entries.
`060_med_natla` (also a test): 3181 frames (106 s), 6933 channels, 258 entries; English
starts at frame 109 "Damn, it's too small..."; all entries strict UTF-8. The 12 cinematics without
subtitle entries all have `hasSubtitles = 0`. The English lines start with the voice (CINEMATICS.md §5).
Voice variants: subtitled cinematics have 5 records (one per voice language, e.g. `0xFFFFE010` Spanish);
the English- and Spanish-voice records of `060_med_natla` both carry every language's text. [V-data]
Voice-line `.mul` files (`pc-w\audio\streams\vo\…`, e.g. `lara\sc_04_01_answer.mul`) use the same CINE
stream with a scene-less header (headerSize 0x60, all counts 0 → 0 channels); frame 0 carries the line in
every language (the same text as the matching locals.bin hint string) and the stream ends without a −1
frame. `cine::scan` accepts the end of data at a frame boundary for this. [V-run] (reported by the
Audio work, `src/audio` `decodeMul`, which extracts these too.)

## 4. Subtitle display [V-exe]

`0x583D50` (reset), `0x583DB0` (push, duration 90), `0x583E20` (draw + tick): two 256-byte slots.
Push: if the bottom slot is non-empty it moves to the top; the new line goes to the bottom; timer = 90.
Each frame both slots are drawn (virtual 640×480, y 352 and 374, font scale 0.6, centred [I] by the
third argument), then the timer drops by `gameTracker_timeMult` (1.0 per 1/30 s); at ≤ 0 the bottom
line moves up, the bottom is cleared and the timer restarts at 90. Pushes are ignored in game modes 6
and 10 (`DAT_00e7f143`). `ou::locale::SubtitleTicker` implements push/advance; `ou-movie` renders it
(VIDEO §3), ou-game renders it during cutscenes (CINEMATICS.md §6).

## 5. Tests

`tests/test_locale.cpp`: synthetic tables (exe getter semantics, truncation), exe UTF-8 check, language
parsing, `.sch` parsing, ticker scrolling, speaker stripping, cinematic subtitle blobs. Gated on
`OU_GAME_DIR`: all six locals.bin variants (5136 strings, header values, 1448 non-empty, strict UTF-8),
`prevontr.sch` cues per language, Bink decode and track selection. CINE streams (synthetic, truncation,
`060_med_natla` subtitles, all 53 streams): `tests/test_cine.cpp`.
