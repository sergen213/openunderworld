# UI art: game font (DTP 46), front-end atlas, PDA art, cdc RAW images

Code: `src/ui/Art.{hpp,cpp}` (`ui::parseFont`, `ui::loadArt`, `ui::loadRaw`, `ui::loadSonarMap`, `ui::loadRewards`,
`ui::decodeUtf8`), text layout in `src/ui/Ui.cpp` (`ui::Canvas::fontWidth`), HUD in `src/ui/Hud.cpp`. Screen layouts,
text scales and the prompt glyphs: [GAMEPLAY.md "Progression … menus"](../GAMEPLAY.md) (the "Menus" paragraphs).
Containers: [CONTAINERS.md](CONTAINERS.md) (bigfile, CDRM, DRM v19, relocations). Texture sections: [TEXTURES.md](TEXTURES.md).

Labels: **[O]** observed in the GOG 1.1 data or in our tru.exe 1.1.0.0 decompile (`decomp_all.c`, VAs below), **[I]**
inference, **[U]** unknown. Data facts marked [O-probe] come from the read-only probe
`openunderworld-work/w2/FormatDocs/uiprobe.py` (2026-11-16; parses patch.000 + bigfile with the CONTAINERS.md layouts).

**Endianness:** little-endian throughout (x86 PC data; every multi-byte field below is LE) [O].

## 1. Where the data lives

| Asset | Container | Content | Evidence |
|---|---|---|---|
| `pc-w\generalbank.drm` | CDRM-wrapped DRM v19; the **patch.000** copy overrides bigfile.000 (CONTAINERS.md §2) | 1 261 sections: 627 DTP, 398 script, 118 wave, **117 textures (ids 1..117)**, 1 object. DTP 46 = the game font | [O-probe] |
| `pc-w\g_pda.drm` | CDRM DRM v19 (also patched) | PDA picture windows / tab bars / frame icons / first-aid picture: textures **680..706** (27 sections, ids clear of generalbank's) | [O-probe] |
| `pc-w\g_binoculars.drm` | CDRM DRM v19 | texture 649 (1024 × 512), the field camera viewfinder (`ui::kViewfinder`) | [O] `MenusReal`/HUD use |
| `pc-w\image\<name>.raw` | bare bigfile record (no CDRM), magic `!WAR` | full-screen images: loading screens, PDA bezel, EXTRAS concept art | [O] 270 records [O-probe]; path built by tru.exe 0x4779F0 `"%simage\\%s.raw"` |
| `pc-w\<unit>_map.drm` | DRM v19 whose first section is PDA_MAP (type 13) | PDA sonar map (§6) | [O] |

## 2. Font DTP (generalbank.drm DTP section id 46)

Section header: type 7 (DTPDATA), `resourceType` 11, data size 2 507 bytes [O-probe]. Relocations: one internal
pointer (+0x10 → +0x30) and two resource16 relocations at +0x00 and +0x04, both of section type 5 (texture) [O-probe].

### 2.1 Header (0x30 bytes before the glyph table)

| off | type | field | generalbank value | tru.exe use (0x475260 font init, confidence) |
|---|---|---|---|---|
| 0x00 | u32 | texture (resource16 → texture section id) | 109 (1024 × 256 A8R8G8B8) | `*font+0x04`; width/height via its vtable +0x1C/+0x20 → reciprocal texel size at +0x08/+0x0C [O] |
| 0x04 | u32 | texture2 (resource16 → texture section id) | 110 (512 × 256 DXT5) | `+0x10`, reciprocals at +0x14/+0x18 [O] |
| 0x08 | u32 | cell height in texels | 46 | glyph quad height [I from the draw path; ours uses it as the cell top] |
| 0x0C | u32 | glyph count | 205 | binary-search bound `count × 12` in 0x475260 / 0x475350 [O] |
| 0x10 | ptr | glyph table (internal relocation; raw value `0xBEEBBEEB` before relocation) | → +0x30 | font `+0x20` [O] |
| 0x14 | i16 | spacing (added to every glyph width) | 4 | `Font_GetTextWidth` 0x4758C0 reads DTP+0x14 [O] |
| 0x16 | i16 | [U] | 0 | no reader found [U] |
| 0x18 | f32 | scaleX | 0.5 | copied to font `+0x424`, multiplies every advance [O] |
| 0x1C | f32 | scaleY | 0.5 | copied to font `+0x428` [O]; vertical use [I] |
| 0x20 | u8[16] | [U] | `00 00 26 00` then 12 zero bytes | not read by 0x475260 [O]; meaning [U] |

### 2.2 Glyph record (12 bytes, `count` records, sorted by code)

| off | type | field | observed |
|---|---|---|---|
| 0x00 | u32 | code point (Unicode; U+1800..U+18FF = private controller/button glyphs) | 0x20..0x221E [O-probe] |
| 0x04 | u8 | width in texels (advance before spacing/scale) | `'A'` = 29 (`MenusReal.ArtAndFontMetrics`) |
| 0x05 | u8 | [U] | 0 in all 205 [O-probe] |
| 0x06 | u16 | x of the cell in its texture | `'A'` = 1 |
| 0x08 | u16 | y of the cell | |
| 0x0A | u8 | [U] | 0xFF in all 205 [O-probe] |
| 0x0B | u8 | [U] pad; **the last record's byte is not stored** (the section ends at +0x9CB, table end +0x9CC) | 0 in the 204 stored [O-probe] |

- Indexing [O]: the table is sorted ascending by code with no duplicates (205/205 [O-probe]); tru.exe binary-searches it
  (0x4751E0, a lower-bound over 12-byte records with comparator 0x4F1BA0 [I: compares the leading u32]).
  0x475260 pre-resolves codes 0..0xFF into a 256-entry cache at font `+0x24`; 0x475350 maps 0xA0 to the cached space,
  uses the cache below 0x100 and the binary search above, and when the found record's code differs it sets font
  byte `+0x00` (a "missing glyph seen" flag) and returns U+18AF, else the first record [O].
- Counts [O-probe]: 161 codes below 0x100, 39 in U+1800..U+18FF, U+18AF present, U+00A0 absent (the remap above
  covers it).
- Texture choice [O ours, I mapping]: codes U+1800..U+18FF are cut from texture2 (110), every other code from texture
  (109) (`Glyph::second`); the tru.exe draw path that picks the texture (0x4759B0 family) was not traced field by field.

### 2.3 Runtime interpretation

- Advance per glyph [O 0x4758C0 / 0x475960]: `(spacing + width) × textScale(0x9CE6B8) × scaleX − 1`, summed over the
  decoded string; codes 1, 2 and 0x1B are control codes with no advance (0x1B toggles the key highlight,
  GAMEPLAY.md). Strings are UTF-8 decoded by 0x597BF0 / 0x597C40; ours: `ui::decodeUtf8` (invalid bytes → U+FFFD).
- Ours: `ui::Font::find` (0xA0 → space, missing → U+18AF, then nullptr), `ui::Canvas::fontWidth`; menu text scale
  0.625 × the font's 0.5, half-texel inset (GAMEPLAY.md "Menus").

## 3. Front-end atlas (`ui::loadArt`, ours)

Not an on-disk format: the loader packs every texture section of generalbank.drm (ids 1..117: 59 DXT5, 55 A8R8G8B8,
3 DXT1 [O-probe]), g_pda.drm 680..706 and g_binoculars.drm 649 (both optional) into one RGBA8 atlas, 2 048 texels wide,
shelf-packed tallest first, each texture with a 2-texel repeated-edge gutter so bilinear taps never cross textures.
`Art::textures` maps the **texture section id** to its normalised atlas rect; ids do not overlap across the three DRMs
(generalbank 1..117, binoculars 649, PDA 680..706 [O-probe]). Cube maps and textures without mips are skipped. The atlas
is `render::FrameOptions::uiImages[0]`. HUD / menu code addresses art by these ids (e.g. 17 is 1 024 wide).

g_pda 680..706 [O-probe]: 680..695 512 × 256 DXT5 (picture windows: 680 + chapter for AREA INFO, 688 sonar, 689
weapons, 690 inventory, GAMEPLAY.md "PDA"), 696 512 × 128 DXT5, 697 512 × 16 DXT1 with 5 mips (flags 3), 698..700
32 × 32 / 32 × 16 / 16 × 32 DXT5 (frame icons), 701..706 512 × 256 DXT5.

## 4. cdc RAW image (`pc-w\image\*.raw`, magic `!WAR`)

"RAW!" stored byte-reversed. 0x80-byte header, then uncompressed 32-bit pixels, top row first, no mips, no padding.

| off | type | field | observed |
|---|---|---|---|
| 0x00 | char[4] | `!WAR` | all 270 [O-probe] |
| 0x04 | u32 | header size = pixel data offset | 0x80 in all 270 [O-probe] |
| 0x08 | u32 | pixel byte count = width × height × 4 | consistent in all 270 [O-probe] |
| 0x0C | u32 | [U] | 3 in all 270 [O-probe] |
| 0x10 | u32 | [U] flags | 0x10 (267), 0x70 (3) [O-probe] |
| 0x14 | u32 | width | 16..1 280 |
| 0x18 | u32 | height | 4..720 |
| 0x1C | u32 | row pitch in bytes = width × 4 | consistent in all 270 [O-probe] |
| 0x20 | u32 | bits per pixel | 32 [O samples] |
| 0x24..0x30 | u32 × 4 | channel bit counts | 8, 8, 8, 8 [O samples] |
| 0x34..0x40 | u32 × 4 | channel masks | 0xFF000000, 0x000000FF, 0x0000FF00, 0x00FF0000 [O samples]: A, B, G, R as an LE u32 [I names] |
| 0x44..0x7F | | [U] | not read |

- Pixel byte order is B, G, R, A (`MenusReal.RawImagesAndPdaArt`: the PDA bezel is blue only that way) [O]; record size
  = 0x80 + pixel bytes exactly (270/270 [O-probe]). Sizes [O-probe]: 135 × 640 × 480, 49 × 570 × 320, 20 × 16 × 21,
  15 × 480 × 185, 11 × 1 280 × 720, … Language: 260 records have specMask 0xFFFFFFFF; 2 images exist once per voice
  language (EN, FR, DE, IT, ES: 10 records; CONTAINERS.md §1.4).
- Field semantics beyond size/width/height/pitch follow cdcengine.re `/docs/files/image/` [S]; not cross-checked in the exe.
- tru.exe [O, medium]: `LOAD_ImageFile` 0x54F430 builds the path (0x4779F0), streams the file into a
  `cdc::ImageLoader` (vtable 0x0087CF28: 0x54F390 forwards chunks, 0x54F3B0 asks the render device, `g_pcRenderDevice`
  vfunc +0xB8, for an image object of the file size) and calls `ERROR_Fatal("Cannot open image: %s")` when the file is
  missing. **The `!WAR` header parse lives in that render-device image object and was not located [U]** (no
  `0x52415721` / `0x21574152` constant in `decomp_all.c`).
- Ours: `ui::loadRaw` swaps B/R and forces alpha 255 (backgrounds are opaque), result in `uiImages[1]` (menu/PDA/loading
  background) or `uiImages[2]` (EXTRAS preview).

## 5. EXTRAS rewards (generalbank DTP 36) and costumes

Layout in `src/ui/Art.hpp` (`ui::Rewards`) and GAMEPLAY.md "EXTRAS" / "reward table": `{u32 count, record*}` lists
indexed by category (stride 8), 0x1C-byte records `{u32 locals id, i32 unlock, u32, char* path, char* image, …}`, list 9
0x18-byte records with the `Movies\` name at +8. Images name `pc-w\image\<image>.raw` (§4); names index locals.bin
([TEXT.md](TEXT.md) §1).

## 6. PDA sonar map (`pc-w\<unit>_map.drm`)

Two sections; section 0 (PDA_MAP) `+0` is an external pointer to the map header in section 1: bbox min / max (vec4 each),
`+0x30` vertices* (vec4, unit frame), `+0x34` triangles* (20-byte `{u32 a, b, c, u32 flags, u32}`), `+0x38` kd nodes*
(16 bytes), `+0x40` u32 node / `+0x44` triangle / `+0x48` vertex counts [O l1_seafloor_map, l4_trailstart]. Neighbour
unit list in section 0 not read [U].

## 7. Relationships

generalbank DTP 46 → textures 109/110 (resource16, same DRM) → atlas rects; locals.bin strings → UTF-8 → glyph codes
(U+1800 + i placeholders → bound control names, GAMEPLAY.md); DTP 36 rewards → locals ids + RAW image names + movie
names; unit name → `<unit>_map.drm`. Compression: only the DRM wrappers (CDRM zlib blocks); font table, RAW pixels and
the atlas inputs are otherwise uncompressed (textures may be BCn, TEXTURES.md).

## 8. Validation

| Loader | Rejects (returns nullopt / reason in `why`) | Enforced by |
|---|---|---|
| `parseFont` | section missing; data < 32 bytes; glyph pointer +0x10 not relocated; `count == 0`; table + (count − 1) × 12 + 11 past the section end | real data: `MenusReal.ArtAndFontMetrics` (texture 109, cell 46, `'A'` width 29 / x 1, U+1800 on texture2, missing → U+18AF, "PLAY GAME" width 66 at 1920 × 1080). Synthetic rejection test: **missing** (no test feeds a truncated / unrelocated font DTP) |
| `loadArt` | generalbank.drm absent; font does not parse; font textures 109/110 missing from the atlas. CDRM/DRM corruption throws `cdc::FormatError` (CONTAINERS.md §7) | `MenusReal.ArtAndFontMetrics`, `MenusReal.RawImagesAndPdaArt` (680..694 are 512 wide), `MenusReal.GraphicsRowsFit` |
| `loadRaw` | record missing; < 32 bytes; magic ≠ `!WAR`; width or height 0 or > 8 192; pixel bytes ≠ w × h × 4; header size + pixel bytes past the record | `MenusReal.RawImagesAndPdaArt` (sizes, B,G,R,A order, every reward image and loading image loads, unknown name → nullopt). Synthetic rejection test (bad magic / short / size mismatch): **missing** |
| `loadSonarMap` | absent; < 2 sections or section 0 not PDA_MAP; header pointer missing; vertex/triangle pointers missing; header or arrays past the section; triangles with an index ≥ vertex count are dropped; `FormatError` → nullopt | `PdaReal.SonarMapFrameRevealAndModels` (l4_trailstart map inside the unit's collision bounds) |
| `loadRewards` | generalbank absent; DTP 36 absent; any of the five read lists empty; records past the section are cut | `MenusReal.RawImagesAndPdaArt` (9 gear, 3 trailers prevontr…credits, every image exists) |
| `decodeUtf8` | invalid lead / continuation / truncated sequence → U+FFFD | indirectly through `Menus.ButtonPrompts` / `Canvas::fontWidth`; no dedicated UTF-8 edge-case test [gap] |

Not validated by our loader: the glyph table's sort order (we use a hash map, the exe needs it sorted), the resource16
relocation types at +0x00/+0x04 (we read the stored ids), RAW header fields 0x0C/0x10/0x20.. .

## 9. Open questions

- [U] Font header +0x16 and +0x20..+0x2F; glyph bytes +0x05 / +0x0A / +0x0B.
- [U] RAW header +0x0C (3) and +0x10 (0x10 vs 0x70 on 3 records), and where tru.exe parses the header.
- [I] Which tru.exe draw function selects texture2 for controller glyphs (we assume the U+1800..U+18FF range).
