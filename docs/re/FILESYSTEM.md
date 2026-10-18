# File system, DRM loader and render resources

Addresses are VAs in the GOG `tru.exe` (sha256 `1fdbd345…1011`). Names come from
`tools/ghidra/names/tru_gog.csv`. Tags: **[OBS]** seen in our decompile, **[INF]**
inference, **[?]** unknown. Our parser implementation is `src/cdc/Cdc.{hpp,cpp}`.

## File-system objects [OBS]

RTTI (from `ExportRtti`) gives the class layout:

| Class | vftable | Notes |
|---|---|---|
| `cdc::FileSystem` | 0x843F54 | abstract, 15 slots |
| `MultiFileSystem` | 0x8440EC | `MultiFileSystem_ctor` 0x4737F0; 0x23 mount slots of 0x28 bytes (fs pointer + name) |
| `ArchiveFileSystem` | 0x843FCC | bigfile reader, object size 0x20 |
| `cdc::MSFileSystem` | 0x84425C | Win32 disk files (`CreateFileA`), `GetDiskFS` 0x472B60 → `g_diskFileSystem` 0x9CE27C |
| `cdc::ResolveDecompressor` | 0x84CD74 | a `cdc::FileSystem` that wraps another and inflates CDRM |
| `cdc::FileUserBufferReceiver` | 0x844014 | read-into-caller-buffer receiver |
| `ResolveReceiver` | 0x84C654 | DRM stream receiver (slot 0 = `ResolveReceiver_ReceiveData`) |

`InitArchive` 0x473840 (called from `Win32_GameMain`):

1. allocates a `MultiFileSystem` (0x58C bytes) into `g_fileSystem` 0x9CE278;
2. allocates an `ArchiveFileSystem` over the disk FS, `specMask` (this+8) = 1;
3. `ArchiveFileSystem_Open("BIGFILE.DAT")` 0x4730B0;
4. mounts it in the next multi-FS slot under the name `"Unnamed"`;
5. on failure: `ERROR_Fatal("Unable to open bigfile BIGFILE.DAT!")`.

`InitPatchArchive("PATCH.000")` 0x478930 mounts a second archive the same way. The
patch archive overrides the base one [INF: mount order + TRLAU-menu-hook ModLoader].

## Bigfile [OBS]

`ArchiveFileSystem_Open` 0x4730B0:

* The `.DAT` extension is stripped and `.000` appended. The header is read from `BIGFILE.000`:
  * first `u32 count` into this+0x1C;
  * then `count*20 + 4` bytes into a heap block;
  * this+0x18 → `hashes[count]`, this+0x14 → records of 16 bytes:
    `{u32 size, u32 sector, u32 specMask, u32 compressedSize}`.
* Number of parts = `(max(sector)*0x800 + 0x7FEFFFFF) / 0x7FF00000`. Each part
  `BIGFILE.%03d` is opened through the disk FS into this+0xC[]. A missing part is fatal:
  "Unable to open bigfile: %s".
* "Archive %s isn't large enough!" is a size sanity check on the last part.

`ArchiveFileSystem_FindEntry` 0x4726E0:

* `ArchiveFileSystem_CalculateHash` 0x472670 copies the path and `_strlwr`s it.
* It then runs `ArchiveFileSystem_CRC32` 0x4725C0: MSB-first, poly 0x04C11DB7, init ~0, final ~.
* Binary search over the sorted `hashes`.
* Among equal hashes it returns the first record with `(record.specMask & fs.specMask) == fs.specMask`. It scans both directions, so this is how language/SKU variants of the same path are selected.
* Miss: debug "* Archived file '%s' NOT found *" and returns NULL.

Our `src/cdc` matches this: `Bigfile::find(path, specMask = kSpecEnglish)` uses the same superset
test, and `BigfileSet` puts patch.000 in front. [OBS] The language bits are set after startup:
`0x49A900(language)` calls `0x54F2E0(1 << language)`, which reaches every mounted archive through the
multi-FS setter `0x472D80` (vtable +0x10). Details are in docs/formats/LEVELS.md.

Other ArchiveFileSystem slots:

* `RequestRead` 0x472890:
  * part = `sector*0x800 / 0x7FF00000`;
  * offset inside the part = the remainder;
  * it forwards a request to the part's disk file, and a missing file is fatal: "Can't open file %s".
* `OpenFile` 0x473020 builds an `ArchiveFile` (part handle, record, part offset).
* `FileExists` 0x472920.
* `GetFileSize` 0x472940 returns `record.size`.
* Slots 0x472960…0x472A40 forward to the underlying disk FS.

## CDRM [OBS]

`cdc::ResolveDecompressor` is an FS decorator. Its receiver, `ResolveDecompressor_Receiver_ReceiveData` 0x552FC0, works like this:

* It peeks the first 16 bytes of the stream for the magic `"CDRM"`.
* If the magic is absent, it passes the bytes straight through.
* If present, `ResolveDecompressor_ParseCdrm` 0x552BF0 runs a 3-state machine:
  0. header: `"CDRM"`, `u32`, `u32 numBlocks` (fatal "CDRM file corrupt" if the count is implausible), `u32` (stored at +0x34, bytes to skip before the blocks [INF]);
  1. block table `numBlocks * 8` bytes;
  2. block data.

Inflation runs as a job (`JobHarness_JobInflate` RTTI; `FUN_004A1700`/`FUN_00552720`).
Our parser `decompressCdrm` (type 1 = stored, 2 = zlib, data 16-byte aligned) reads the same
header fields [INF: the exe's block-type handling is in the inflate job, not traced].

## DRM loader [OBS]

`ResolveReceiver_ReceiveData` 0x54F9B0 is a streaming state machine. Its state is at this+0x10.

**State 0, header:**

* `hdr[0]` must be 0x13. Otherwise it is fatal: "Wrong DRM version data %x, code %x\nrebuild of %s needed".
* `hdr[1]` = objDep bytes, `hdr[2]` = unitDep bytes, `hdr[4]` = flags, `hdr[5]` = numSections.
* It copies `numSections` 20-byte section headers.
* If `flags & 1`, the start of each section's payload is padded to 16 bytes inside the stream.

Section header (20 bytes):

| off | type | meaning |
|---|---|---|
| +0x00 | u32 | data size |
| +0x04 | u8 | section type → index into `g_sectionHandlers` 0xC56FC8 |
| +0x05 | u8 | bit0 = skip (already resident / not to be loaded) [INF] |
| +0x06 | u16 | passed to the handler |
| +0x08 | u32 | `(v>>1)&0x7F` = resource type, `v>>8` = relocation bytes |
| +0x0C | u32 | section/resource id; matched against already loaded sections of the same type for sharing [INF] |
| +0x10 | u32 | [?] (language/spec mask in other cdc games) |

**States 1..6** stream the dependency names, then for each section the relocation block and
the data. Per section the handler's `Allocate` (vtable +8, `(id, resType, …)`) or
`AllocateNoType` (+0xC) creates the resource. Data then streams into it (`HandleResourceData`,
e.g. 0x550C70). Relocations are applied by `ResolveSection_ApplyRelocations` 0x54EC60.

Relocations (5 counts at the start of each reloc block):

1. internal `{u32 off, u32 target}`: `*(base+off) = base + target`.
2. external, 8 bytes:
   * `section = lo & 0x3FFF`;
   * `patch = ((lo>>14) | (hi&0x3F)<<18) * 4`;
   * `*patch = sectionBase(section) + ((hi>>6) & 0x3FFFFFF)`.
3. resource id, u32:
   * `patch = (v & 0x1FFFFFF) * 4`;
   * `handler type = v >> 25`;
   * `*patch = handler.FindResource(*patch)`.
4. 16-bit variant: low 16 = handler type, high 16 + u16 at the patch site → FindResource.
5. like 3 but stores `handler.GetResourcePointer(...)`. On a miss it logs "Resource %d (type %d, file %s) can't find resource %d (type %d)".

Paths: `LOAD_UnitFileName` 0x477970 = `"%s%s.drm"`; `LOAD_StreamGroupFileName` 0x477990
= `"%sStreamGroups\%s.drm"`.

## Section-type dispatch [OBS]

`RESOLVE_InitSectionHandlers` 0x551FF0 fills `g_sectionHandlers` 0xC56FC8.
`MainG2_InitEngine` 0x5DF390 adds type 13 through `RESOLVE_RegisterSectionHandler` 0x54F1B0.

| type | handler (vftable) | content |
|---|---|---|
| 0 | GenericSection 0x84C754 | generic data |
| 1 | EmptySection 0x84C6F4 | |
| 2 | AnimationSection 0x84C7B4 | |
| 3, 4 | (none) | pushbuffer types, unused on PC |
| 5 | RenderResourceSection 0x84CB84 | textures (PCD9) |
| 6 | cdc::WaveSection 0x850D2C | audio |
| 7 | DTPDataSection 0x84C994 | |
| 8 | ScriptSection 0x84C934 | |
| 9 | ShaderLibSection 0x84C874 | |
| 10 | MaterialSection 0x84C8D4 | |
| 11 | ObjectSection 0x84C814 | |
| 12 | RenderResourceSection (second instance) | meshes / terrain |
| 13 | PDAMapSection 0x8552BC | |

This matches `research/Templates/tr8_section.bt` and our `SectionType` enum.

`RenderResourceSection_AllocateResource` 0x551F30 switches on the resource type (header
+0x08). It calls `g_pcRenderDevice` 0xA1407C:

| res type | device slot | factory | object |
|---|---|---|---|
| 5 | vt+0xA4 | `PCRenderDevice_CreateTexture` 0x5071B0 | `cdc::PCTexture` (0x28 B) |
| 24 | vt+0xB4 | `PCRenderDevice_CreateRenderTerrain` 0x507330 | `cdc::PCRenderTerrain` (0xE0 B) |
| 26 | vt+0xAC | `PCRenderDevice_CreateRenderModel` 0x507270 | `cdc::PCRenderModel` (0x48 B) |

## PCD9 textures [OBS]

`PCTexture_ReceiveData` 0x515B40 checks magic `PCD9`. Otherwise it fails with "Corrupt texture data passed into PCTexture constructor!".

24-byte header:

| off | type | meaning |
|---|---|---|
| +0x00 | char[4] | `PCD9` |
| +0x04 | u32 | D3DFORMAT: 0x15 A8R8G8B8, 0x16 X8R8G8B8, 0x17–0x1A/0x33 16 bpp, 0x1C 8 bpp, `DXT1`…`DXT5` FourCC |
| +0x08 | u32 | [?] (data size in other cdc games) |
| +0x0C | u32 | [?] |
| +0x10 | u16 | width |
| +0x12 | u16 | height |
| +0x14 | u8 | [?] |
| +0x15 | u8 | extra mip count (levels = n+1) |
| +0x16 | u16 | flags; 0x8000 = cube map (6 faces) |

* Mips follow largest-first (faces in order for cubes). Sizes come from `PCTexture_GetMipByteSize` 0x51FFC0: DXT1 8 bytes/block, DXT2–5 16 bytes/block, else bpp × w × h.
* Device textures are created by `PCDeviceTexture_Create` 0x523D70 or `PCDeviceCubeTexture_Create` 0x520FA0.
* [INF] The top mips can be skipped while w,h > 32, depending on the TextureDetail setting.

## Render models and vertex declarations {#render-resources}

`PCRenderModel_Finalize` 0x50EB30 [OBS]:

* The resource data is `{Mesh*, u32* textureTable(count, ids…), u32* materialTable}`.
* `Mesh` starts with magic `'Mesh'` (0x6873654D). Otherwise it fails with "Corrupt mesh data passed into PCRenderMesh constructor!".

| Mesh off | meaning |
|---|---|
| +0x04 | flags (bit0: optional extra block at +0x74) |
| +0x0C | index count; index data at `Mesh + [+0x78]` |
| +0x44 | -1 sentinel [?] |
| +0x6C | offset → primitive groups, count u16 at +0x7C, stride 0x40: +0x28 material index (via material table), +0x2C/+0x30/+0x34/+0x38 texture indices (via texture table, -1 = none) |
| +0x70 | offset → vertex buffers, count u16 at +0x7E, stride 0x34: +0x10 rel ptr (if flags&1), +0x14 rel ptr vertex data, +0x24 rel ptr `VertexDecl`, +0x28 vertex count |

* Offsets are relative to the Mesh start.
* Static buffers: `PCStaticVertexBuffer_Init` 0x528720, `PCStaticIndexBuffer_Init` 0x5280F0.

`VertexDecl` [OBS] has the same layout as DXHR (`research/cdcEngineDXHR/cdcRender/VertexDeclaration.h`):

* Header `{u32, u32, u16 numAttr @+8, u8 stride @+10, u8, u32}`.
* Then 8-byte elements `{u32 attribHash, u16 offset, u8 format, u8 bufferIndex}`.
* `VertexDecl_FindAttribute` 0x4F3FA0 does a linear search by hash.

`VertexFormatToD3DDeclType` 0x5289D0:

| format | D3DDECLTYPE |
|---|---|
| 0–3 | FLOAT1–4 |
| 4–7 | D3DCOLOR |
| 8 | UBYTE4 |
| 9 / 0x13 | SHORT2 |
| 10 / 0x14 | SHORT4 |
| 0xB | UBYTE4N |
| 0xC | SHORT2N |
| 0xD | SHORT4N |
| 0xE | USHORT2N |
| 0xF | USHORT4N |
| 0x10 | UDEC3 |
| 0x11 | DEC3N |

Formats 0x13/0x14 get a shader-side scale of 1/2048 (short texcoords).

* `PCStreamDecl_CreateFromVertexDecl` 0x529020 matches the vertex shader's input semantics against the attribute hashes. It builds a sorted `D3DVERTEXELEMENT9[]` ending with `D3DDECL_END`.
* `VertexKindToD3DDeclUsage` 0x528A90 maps the attribute kind to (usage, index).
* `PCStreamDecl_CreateD3DResource` 0x528DB0 is the **only** caller of `IDirect3DDevice9::CreateVertexDeclaration` (vt+0x158).
* Callers of 0x529020: 0x50EE60 (render model), 0x510DD0 (terrain), 0x5033E0 [?].
* [?] The DXHR attribute-hash constants do not appear as immediates in tru.exe. The TR8 hash values must be read from the mesh data.
