# TRU PC textures (PCD9)

Code: `src/formats/Texture.cpp` (`parsePcdHeader`, `decodeTexture`, `toRgba8`).
CLI: `ou-asset textures <gameDir> <drm>`, `ou-asset dump-texture <gameDir> <drm> <id> -o out.tga|out.dds`.

Labels: **[O]** observed in game data or tru.exe, **[I]** inference, **[U]** unknown.

## Container

- [O] Textures are DRM sections of section type 5. In tru.exe the type-5 handler is a
  RenderResource handler; resource type 5 allocates a `PCTexture` (0x551F30, reported by GhidraRE).
- [O] A texture's id is the DRM section id. Materials reference textures by this id
  (see MESHES.md, "Materials").
- [O] Texture sections carry no relocations.

## Header (24 bytes, little-endian)

| off | type | meaning |
|-----|------|---------|
| 0x00 | u32 | magic `PCD9` (0x39444350) [O] |
| 0x04 | u32 | D3DFORMAT: 0x15 = `D3DFMT_A8R8G8B8`, or FourCC `DXT1` / `DXT3` / `DXT5` [O] |
| 0x08 | u32 | byte size of the pixel data that follows the header [O] |
| 0x0C | u32 | palette size; 0 in every texture surveyed [O] |
| 0x10 | u16 | width [O] |
| 0x12 | u16 | height [O] |
| 0x14 | u8  | bits per pixel: 32 for 0x15, 0 for DXT [O] |
| 0x15 | u8  | mip levels − 1 [O] (tru.exe 0x515B40 and the size check below) |
| 0x16 | u16 | flags; 0x8000 = cube map with 6 faces [O]. Other observed values: 0, 1, 3 (meaning [U]) |

- [O] Pixel data: mip levels largest-first. Each level is `max(1,w>>i) × max(1,h>>i)`. BCn levels
  are rounded up to 4×4 blocks of 8 bytes (DXT1) or 16 bytes (DXT3/DXT5).
  The sum of all levels equals the header data size exactly, which we check on every decode.
- [O] Cube maps (flags & 0x8000): the data size is 6 × (one full mip chain).
  This was observed on 349 textures. **[I]** We assume the faces are stored face-major.
  Cube maps (flags 0x8000) store six complete mip chains one face after another (dataSize = 6 × chain [O]); the decoder
  sets `TextureData::cube` and keeps the six faces of each level together, in D3D face order [I].

## Survey (C++ decoder, 2026-10-18)

[O] `ou-asset textures` was run over all 922 DRMs whose names we know (out of 1057 DRMs in the bigfile). It found 35 946 texture
sections: 17 208 DXT1 (BC1), 9 284 DXT5 (BC3) and 9 105 A8R8G8B8 decoded, and 349 rejected (all of them cube maps).
No DXT3 texture was seen, so the BC2 path is untested. No size or mip-chain errors occurred.

## Channel order (verified)

- [O] DXT1 and DXT5 decode to correct colours (the surveyed DRMs contain no DXT3). We decoded Lara's diffuse textures to TGA, converted
  them to PNG with ffmpeg and viewed them: skin, lips, hair and clothing all have natural colours.
  A textured CPU render of `pc-w\lara.drm` also looks correct.
- [O] Byte order for format 0x15 is B,G,R,A, which is standard D3D9 A8R8G8B8. In
  `pc-w\streamgroups\l2_maproom_second_maproom.drm`, texture 0xcb3 is a 1024² tangent-space
  normal map. Decoded as BGRA, its mean colour is (127,128,240), the expected blue-violet colour of a
  normal map. Decoded as RGBA it would come out orange. We viewed it as a PNG. Test `RealData.Bgra8ChannelOrder`
  checks this.
- `decodeTexture` keeps BCn data compressed (`PixelFormat::BC1/BC2/BC3`) and returns format 0x15 as
  `PixelFormat::BGRA8`, without swizzling. `toRgba8` CPU-decodes any level to RGBA8 for dumps and tests.
  BC1 uses 3-colour+transparent mode when c0 ≤ c1. BC2/BC3 colour blocks always use 4-colour mode.

## Open questions

- [U] What flags bits 0/1 mean. They are common (25 511 textures have flags = 3).
- [U] The cube face order (assumed D3D +X −X +Y −Y +Z −Z; used by the original shaders' environment reflections).
- [I] There are many small white 32bpp textures with low alpha, e.g. `co_satchel` 0x1850. They are probably
  alpha/specular masks. Material category semantics are discussed in MESHES.md.
