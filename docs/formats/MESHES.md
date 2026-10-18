# TRU PC static geometry: render models, terrain, materials

Code: `src/formats/Model.cpp` (`decodeModels`, `materialTextures`) and `src/formats/Scene.cpp` (`loadDrmScene`).
CLI: `ou-asset stats|render|dump-mesh <gameDir> <drm>`.

Labels: **[O]** observed (game data, CPU render, or tru.exe decompile), **[I]** inference, **[U]** unknown.
The tru.exe addresses below are GOG build VAs. They come from our own Ghidra decompiles and from GhidraRE's notes.

## Where the geometry lives

- [O] DRM section type 12 is a RenderResource. The low byte of its packed header word, shifted right by 1 and masked to 7 bits (`(packed >> 1) & 0x7F`), is the resource type:
  - 26 = `PCRenderModel`, with a "Mesh" header.
  - 24 = `PCRenderTerrain`.
  - 5 = `PCTexture`.
  The constructors are dispatched by `RenderResourceSection_AllocateResource` at 0x551F30.
- [O] Object DRMs such as `pc-w\lara.drm` contain models.
- [O] Unit (level) DRMs such as `pc-w\l4_pool.drm` contain terrain plus a few models.
- Pointers inside a section are DRM internal relocations. `cdc::internalPointer` resolves them.

## Render model (resource type 26)

Resource header (relocated pointers) [O]: `{Mesh*, u32* textureList, u32* materialList, ?}`.
Both lists have the form `{u32 count, u32 ids[count]}`. The material ids are material section ids.

`Mesh` header. Offsets are relative to the Mesh start, and all offsets stored inside it are relative to the Mesh start too [O]:

| off | type | meaning |
|-----|------|---------|
| 0x00 | u32 | magic `Mesh` (0x6873654D) [O] |
| 0x04 | u32 | flags. When bit 0 is set, the batch +0x10 field and the header +0x74 field are used (tru.exe 0x50EB30) [O]. Meaning [U] |
| 0x0C | u32 | total index count [O] |
| 0x44 | u32 | −1 sentinel in tru.exe [O]; meaning [U] |
| 0x6C | u32 | offset of the primgroups, 0x40 bytes each [O] |
| 0x70 | u32 | offset of the batches (vertex buffers), 0x34 bytes each [O] |
| 0x78 | u32 | offset of the u16 index buffer [O] |
| 0x7C | u16 | primgroup count [O] |
| 0x7E | u16 | batch count [O] |

- [O] Models with an index count of 0 or a batch count of 0 also have −1 offsets. They are skipped silently as placeholders.
- [I] Lara's header bounding box max.z is 510.97. It matches the decoded geometry (`RealData.LaraTexturesAndModel`). We have not mapped the exact offset of the box.

Batch (0x34 bytes) [O]:

| off | meaning |
|-----|---------|
| +0x08 | number of consecutive primgroups belonging to this batch |
| +0x14 | vertex data offset |
| +0x24 | VertexDecl offset |
| +0x28 | vertex count |
| +0x10 | used only when flags bit 0 is set; [U] |

Primgroup (0x40 bytes) [O]:

| off | meaning |
|-----|---------|
| +0x10 | first index |
| +0x14 | triangle count; the primitive is a triangle list |
| +0x28 | index into the model's material list |
| +0x2C..0x38 | texture indices via textureList, or −1 (tru.exe). We don't use these [U] |

The indices are local to their batch (u16), and every one is below that batch's vertex count. We check this [O].

## Terrain (resource type 24)

Resource header [O]: `{?, Header*, textureList*, materialList*}`. The lists have the same form as for models.

Header fields used (u32, offsets relative to Header) [O]:

| off | meaning |
|-----|---------|
| +0x24 | node count |
| +0x28, +0x2C | group array offset and count (0x20 bytes each) |
| +0x30, +0x34 | VertexDecl array offset and count. Decls are packed back to back, `16 + 8·numAttr` bytes each |
| +0x3C, +0x40 | vertex-buffer array offset and count (16 bytes each: `{dataOff, ?, vertexCount, declIndex}`) |
| +0x44 | per-node u32 offset table: 0 means the node has no geometry, otherwise it points to a chunk list |
| +0x50, +0x54 | u16 index buffer offset and index count |

- [O] Group: +0 is the material-list index and +4 is the vertex-buffer index.
- [O] Chunk list: `{u16 count, u16 ?, count × {u16 group, u16 indexCount, u32 firstIndex}}`.
  A chunk is a triangle list drawn from the group's vertex buffer.
- [I] The nodes form a spatial tree (each node record has a centre and extents). We read only the count.
- [U] The first header bytes (two vec4s) and the rest of the node records.
- `decodeTerrain` emits one `MeshData` per vertex buffer, with one submesh per group texture.

## VertexDecl and attributes

- [O] VertexDecl layout: `{u64 hash, u16 numAttr, u8 stride, u8, u32, numAttr × {u32 attribHash, u16 offset, u8 format, u8 stream}}`.
  This is the layout of `research/cdcEngineDXHR/cdcRender/VertexDeclaration.h`. tru.exe builds the D3D declaration at 0x529020.
- [O] Format → D3DDECLTYPE mapping (switch at 0x5289D0):

  | format | D3DDECLTYPE |
  |--------|-------------|
  | 0–3 | FLOAT1–4 |
  | 4–7 | D3DCOLOR |
  | 8 | UBYTE4 |
  | 9, 0x13 | SHORT2 |
  | 10, 0x14 | SHORT4 |
  | 0xB | UBYTE4N |
  | 0xC | SHORT2N |
  | 0xD | SHORT4N |
  | 0xE | USHORT2N |
  | 0xF | USHORT4N |
  | 0x10 | UDEC3 |
  | 0x11 | DEC3N |

  Formats 0x13 and 0x14 (texcoords) are multiplied by 1/2048 (the constant at 0x84BA10).
  The decoder does not implement 0x10 or 0x11; a vertex using them produces a warning. None has been seen in the survey.
- Attribute hashes:

  | hash | attribute | evidence |
  |------|-----------|----------|
  | 0xD2F7D823 | position | [O] float3. Bounding boxes and renders are plausible |
  | 0x36F5E414 | model normal | [O] D3DCOLOR (B,G,R,A); the shader applies `*2−1` |
  | 0x09B1D4EA | terrain normal | [O] SHORT4, normalized by us |
  | 0x8317902A | texcoord 0 | [O] SHORT2 ÷ 2048. v = 0 is texture row 0 (D3D convention, no flip). Verified by textured CPU renders of Lara (face and clothing land exactly) and of l4_pool |
  | 0x7E7DD623 | colour | [I] colour, copied to `Vertex::color` |
  | 0x8E54B6F3 | second texcoord | [O] SHORT2, present on Lara (offset 28) and on terrain. [U] Its purpose (lightmap or blend UV?); ignored |
  | 0x48E691C0 / 0x5156D8D3 | skin weights / bone-palette indices, formats 6 and 7 (D3DCOLOR) at +16 and +20 | [O] decoded into `MeshData::skin`; see ANIMATION.md |
  | 0x64A86F01 / 0xF1ED11C3 | D3DCOLOR at +36 and +40 | [I] tangent and binormal; ignored |

  Lara batch 0's decl is observed as stride 44 with 9 attributes in the order: pos, normal, 6, 7, uv, uv2, colour, tangent?, binormal?.
  The names come from DXHR's VertexElem constants [I]. The hash values are not immediates in tru.exe.
- [O] Vertices are stored in the bind pose (Lara's T-pose). Skin influences, the batch bone palette (Mesh flags bit 0:
  batch +0x0C count, +0x10 offset) and the skeleton are documented in [ANIMATION.md](ANIMATION.md).
- [O] Axes: Z-up and game units (Lara is about 511 tall). Front faces wind CCW: 99–100 % of vertex normals agree with
  `(b−a)×(c−a)` on Lara and on terrain.

## Materials and texture selection

- [O] Material sections are section type 10. Their texture references are relocations in the material's 5th relocation
  table (resource-id relocations: `offset/4 : 25 bits, section type : 7 bits`). Type 5 means a texture.
- [O] Each relocated reference points at a 16-byte `TextureEntry` `{u32 textureId (patched to a pointer at load), f32 mipLodBias,
  u32 category, u8 type|class<<5, u8 slot, u16 filter}`. The layout matches TRLAU-menu-hook / DXHR `MaterialTexRef`.
- [O] Category 1 = diffuse. We decoded every texture of Lara's materials and viewed them as PNGs:
  - category 1 holds the colour maps (skin, lips, clothes);
  - category 3 holds normal maps;
  - [I] categories 2, 5, 7 and 0 are grey detail, AO-like, mask and noise maps respectively.
- [I] Fallback: a material with no category-1 entry (e.g. Lara's hair, where every entry is category 0) uses its category-0 slot-0
  texture. This is visually right for the hair; it is a heuristic.
- `SubMesh::textureId` = the selected texture id, or 0 when there is none.
- [O] Skipped batches: a model batch that has no texcoord attribute **and** no material texture references is not emitted. It produces the
  warning `…: skipped (no texcoords, no material textures)`. All 259 such batches in the survey are unit models
  that use material 0xd78 (e.g. `l4_pool` sections 13938/13941/13944). They are boxes enclosing whole terrain blocks.
  Their shaders contain only AlphaThreshold/ViewportDepthScaleOffset, ShadowDepthBias and
  FogColor/HeightFogParams/MaterialOpacity constants, with no diffuse sampler.
  [I] They are occlusion, depth or fog volumes rather than visible surfaces. [U] Their exact role.

## loadDrmScene

`ou::formats::loadDrmScene(const cdc::Bigfile&, std::string_view drmPath, bool withDependencies = false)`:

- It decodes every model and terrain in the DRM, plus the textures that submeshes reference.
- With `withDependencies`, it also loads `pc-w\<dep>.drm` for each dependency name, without placement.
- Anything unsupported becomes a `SceneData::warnings` entry; the call does not throw for it. Warnings cover:
  - unused section kinds (counts per kind);
  - unsupported render resource types;
  - cube maps;
  - textures not present in the loaded DRMs;
  - skipped volumes.
- It throws `cdc::FormatError` only when `drmPath` is missing from the bigfile.

Verified paths: `pc-w\lara.drm` (7 meshes, 24 textures), `pc-w\l4_pool.drm` (8 terrain vertex buffers, 3 skipped volumes).

## Survey (all 922 named DRMs, `ou-asset stats`, 2026-10-18) [O]

- No DRM failed to load.
- Output: 2 465 meshes, made up of 1 711 model batches and 754 terrain vertex buffers. 259 model batches were skipped as volumes.
- 0 out-of-range indices.
- All normals have unit length.
- Normal and winding agreement per mesh:
  - ≥ 99 %: 2 100 meshes;
  - 90–99 %: 184 meshes;
  - 50–90 %: 181 meshes;
  - under 50 %: none.
  [I] The lower values come from double-sided or smoothed geometry.
- UV extent: −47.6 to 48.6 (tiling terrain).
- 120 "texture not found in loaded DRMs" warnings: those textures live in other DRMs (dependencies or global DRMs). [U] Which global DRM supplies them.

## Visual checks [O]

- `ou-asset render <game> pc-w\lara.drm --textured --view front|side`: Lara is in her T-pose with correct face, hair, clothing, holsters and boots.
- `ou-asset render <game> pc-w\l4_pool.drm --textured --view top|front`: coherent stone-block level geometry with tiled textures.
- `ou-asset dump-mesh … -o lara.obj --textures` writes OBJ, MTL and TGA files. OBJ `vt` = (u, 1−v).

## Open questions

- Placement of objects in units is solved in [LEVELS.md](LEVELS.md) (`loadLevelScene`). `loadDrmScene(…, withDependencies)` still stacks the models at the origin.
- [U] Primgroup texture indices +0x2C..0x38; the second terrain UV.
- [U] Mesh header +0x74 (an identity `0, 1, 2, …` table on Lara); batch +0x10 is the bone palette (ANIMATION.md).
- [U] The terrain node tree.
- [U] Cube-map texture use.
