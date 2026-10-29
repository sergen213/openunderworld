# TRU PC materials (section type 10) and shader libraries (section type 9)

Code: `src/formats/Model.cpp` (`materialState`, `materialTextures`), consumed by both renderers through
`scene::SubMesh::{blend, alphaRef, doubleSided}`.

Labels: **[O]** observed (game data, tru.exe decompile/disassembly, or our renders), **[I]** inference, **[U]** unknown.
Addresses are GOG tru.exe v1.1.0.0 VAs. Evidence tools (private work dir, not shipped): `scratch/mat.py`,
`matparams.py`, `matsurvey.py`, `d3dbc.py` (a small D3D9 SM2/SM3 bytecode disassembler) and `shl2.py`.

## Material blob (section data, offsets from the section start)

`cdc::PCMaterial` (RTTI `.?AVPCMaterial@cdc@@`) keeps the section data as its blob (`*(this+4)`). Fields used by the
PCMaterial pass functions 0x505D80–0x506880 and `FUN_00505A90`/`FUN_00505C00` (constant upload) [O]:

| off | type | meaning |
|-----|------|---------|
| +0x04 | u32 | returned by `PCMaterial` vtable slot 1 (0x505A20, `GetId` in DXHR's `CommonMaterial`) [O]; 0 in the file, patched at load [I] |
| +0x08 … +0x24 | ptr[8] | per-pass shader input specs (vertex attribute hash lists, e.g. `0xD2F7D823` position, `0x36F5E414` normal) [O] |
| +0x28 … +0x34 | res[4] | ShaderLib (section type 9) references, resource relocations; e.g. ballcourt material 13393 → ShaderLibs 945–948 [O] |
| +0x38 | u32 | **blend mode**, passed to `PCStateManager_SetBlendMode` (0x527A80) [O], table below |
| +0x40 | u32 | **flags**: `0x80` = cull none (0x506040, 0x5061C0, 0x5066A0: `(flags & 0x80 or instance polyflags & 0x40) and !(flags & 0x800)` selects the no-cull entry of `DAT_008A34F8`) [O]; `0x800` = "render twice" in DXHR (`dword18 & 0x800`) [I]; `0x1000`, `0x04`, `0x20`, … [U] |
| +0x4C | u8 | **alpha threshold** (0–255), second argument of SetBlendMode [O] |
| +0x50 / +0x51 / +0x52 | u8 | texture-entry counts (PS instance, PS material, …) [O]; exact split [I] |
| +0x54 | ptr | 16-byte texture entries `{textureId, f32 lodBias, u32 category, u8 class, u8 sampler, u16 filter}` [O] |
| +0x58 / +0x5C | u32 / ptr | pixel-shader constants: `SetPixelShaderConstantF(100, ptr, count)` = CTAB `MaterialParams` c100+ [O] |
| +0x68 / +0x6C | u16 / ptr | vertex-shader constants: `SetVertexShaderConstantF(25, ptr, count)` = CTAB `MaterialParams` c25+ [O] |
| +0x70–0x73 | u8 | per-instance parameter registers/counts (PS c132+, VS c57+) [O]; source data [U] |

Texture entry `class` byte (+0x0C): `FUN_00505A90` substitutes `instanceTextures[class − 1]` when the instance supplies
one, and class 10 a scene-global texture table [O]. Ballcourt foliage uses category 6 / class 1 for its second sampler
[O]; [I] that slot is the per-instance light map (IMF record +0x4C, LEVELS.md).

## Blend modes (`PCStateManager_SetBlendMode`, 0x527A80) [O]

| mode | D3D9 states | alpha test (`FUN_00527940`) | ours (`scene::Blend`) |
|------|-------------|-----------------------------|------------------------|
| 0 | blend off | off (ref 0) | Opaque |
| 1 | blend off | ref = material threshold | Opaque, `alphaRef` = +0x4C |
| 2, 3, 4 | SRCALPHA / INVSRCALPHA, ADD | ref 1 | Alpha, `alphaRef` 1 |
| 5 | SRCALPHA / ONE, ADD, fog off | ref 1 | Additive, `alphaRef` 1 |
| 6 | SRCALPHA / ONE, REVSUBTRACT, fog off | ref 1 | not seen → Alpha |
| 7 | DESTCOLOR / SRCALPHA | off | not seen → Alpha |
| 8 | BLENDFACTOR 0x80808080 both sides | off | Average (constant 0.5) |
| 9 | ONE / INVSRCALPHA | off | not seen → Alpha |

- The alpha reference is also written to pixel-shader constant c2 (`ref / 255`). Every TRU pixel shader we
  disassembled declares `AlphaThreshold` at c2 and does `texkill(alpha − c2)` [O], so the test keeps texels with
  `alpha ≥ ref / 255`. The translucent pass passes the negated threshold (`neg eax` at 0x506728), which disables the
  fixed-function test and leaves only the shader test [O].
- Translucent pass (0x5066A0, pass bit 0x80): mode from the blob (forced to 4 while an instance fades),
  `FUN_00526BB0(LEQUAL or ALWAYS, write = 0)` → depth test without depth writes [O]. Opaque materials are drawn after a
  depth pre-pass with `D3DCMP_EQUAL` and no writes (0x506040, `FUN_00526BB0(3, 0)`) [O]. We draw opaque with LESS +
  writes and blended submeshes afterwards, sorted back to front per mesh, LEQUAL without writes [I: equivalent ordering;
  the original's sort key is unknown].

Survey of all 1057 DRMs in the bigfile (2248 distinct materials) [O]: mode 0 ×1720, 1 ×32, 2 ×44, 3 ×51, 4 ×339,
5 ×57, 8 ×5; flags 0x80 ×97, 0x880 ×60; mode-1 thresholds 127 (30 materials), 102, 124.

## ShaderLib sections (type 9) [O]

- Each holds several D3D9 **shader model 3.0** programs (`vs_3_0` 0xFFFE0300 / `ps_3_0` 0xFFFF0300) with standard
  `CTAB` constant tables (names such as `AlphaThreshold`, `MaterialParams`, `MaterialOpacity`, `FogColor`,
  `HeightFogParams`, `TexcoordScales`, `TimeVector`, `WorldToPSSM0`, `p_default_Material_…_Texture`). They can be
  disassembled offline; translating them is not done.
- Ballcourt foliage (material 13393, ShaderLibs 945–948), composite pass:
  - VS 948 @12452: passes `COLOR0` straight to the pixel shader, writes `texcoord0 × TexcoordScales.x` and
    `texcoord1.yx × TexcoordScales.y` (the second UV, attribute `0x8E54B6F3`), animates positions with a sine wind
    term (`TimeVector`, `MaterialParams` c25–c26), and outputs `WorldToPSSM0` coordinates (parallel-split shadow maps:
    **the original has sun shadow maps**).
  - PS 947 @60: `rgb = tex(s1, uv0) × (vFace ≥ 0 ? c100·c102.y : c101·c103.x)` (front/back tints);
    `light = lerp(tex(s0, uv1) × c102.x, COLOR0, c102.w)`; fog `lerp(rgb·light, FogColor, fogFactor)`;
    `alpha = tex.a × MaterialOpacity`.
  - Material 13393 constants: c100 (0.314, 0.478, 0, 0), c101 (0.894, 0.875, 0.875, 0), c102 (1, 1, 0.7, **0**),
    c103 (1, 0, 0, 0): with c102.w = 0 the vertex colour is **not** used; lighting comes from the uv1 light-map texture.

## Vertex colour, second UV

- Attribute `0x7E7DD623` (D3DCOLOR) feeds `COLOR0`, a baked-lighting input that the pixel shader blends with the
  light map by a per-material constant [O for the foliage shaders above]. Its values are often not a colour: 0 on the
  ballcourt ground plants (rendered black before), (99, 0, 0, 255) on `ca_pt_weightbreak_b`, (0, 0, 80, 60) on a
  ballcourt terrain buffer [O]. The loader therefore no longer copies it into `scene::Vertex::color` (stays white).
- Attribute `0x8E54B6F3` (second texcoord) is the light-map UV of the shaders above [O for those shaders];
  light maps themselves (class-1 instance textures) are not loaded yet [U which DRM/IMF field supplies them].

## What the renderers implement

With `--shaders original` (default) materials whose composite programs translate use them (docs/formats/SHADERS.md); the
list below describes the generic fallback.

- Original Fidelity: texture × fixed sun lambert (0.45 ambient), alpha test at the material threshold, per-material
  blend mode and cull mode, two-sided lighting via `gl_FrontFacing` for two-sided materials.
- Not reproduced (deviations): the per-shader colour tints/params, light maps, vertex-colour baked lighting, fog,
  wind animation, the original's PSSM shadows (our Enhanced shadow map is our own). Opacity fades: both paths
  (docs/RENDERING.md "Instance opacity").
- Verified [O]: `l4_ballcourt` from the `player` start (`--cam 26971,23597,3000,-100,-5`): ground plants and palms
  that rendered black or opaque now show textured, alpha-cut leaves in GL and Vulkan (docs/RENDERING.md).

## Open questions

- [U] Flags `0x1000`, `0x04`, `0x20`, `0x40` and the remaining blob fields; which pass list a material joins.
- [U] Light-map source for IMFs and terrain (IMF +0x4C?), and the `COLOR0` meaning for objects whose shaders use it.
- Shader translation, light maps, fog, instance textures: done, see docs/formats/SHADERS.md (PSSM and dynamic lights still open).
