# TRU shaders: ShaderLib sections (type 9), D3D9 SM3 bytecode, translation

Code: `src/formats/Shader.{hpp,cpp}` (container + bytecode parser/disassembler, our own implementation of the public
D3D9 token format), `src/render/ShaderTranslate.*` (SM3 → GLSL 4.50, libshaderc → SPIR-V), `src/render/OriginalShading.*`
(engine register files shared by both backends). Tools: `ou-asset shaders <gameDir> all|<drm>` (survey),
`ou-asset shader-dis <gameDir> <drm> <libId> [slot]`. Tests: `tests/test_shaders.cpp`.

Labels: **[O]** observed (data, tru.exe decompile/disassembly, our renders), **[I]** inferred, **[U]** unknown. VAs: GOG tru.exe 1.1.0.0.
Engine RE notes behind the tables below were collected in the private work dir (decomp_all.c + objdump).

## ShaderLib section [O]

| off | type | meaning |
|---|---|---|
| +0x00 | u32 | kind: 0 vertex (`PCVertexShaderTable` 0x5263A0), 1 pixel (`PCPixelShaderTable` 0x526440); PCShaderLib fill 0x515530 |
| +0x04 | u32 | table header bytes; slot count = value / 4 |
| +0x08 | u32 | [U] |
| +0x0C | u32[slots] | entry offset relative to +0x04, `~0` = empty slot |

Entry: `{u32 libHash, u16 size (including this 16-byte header), u16 flags (0 or 2, [U]), u64 shaderHash, bytecode}`.
Identical programs are shared across libs and DRMs by `shaderHash` (0 hash collisions with different bytecode in the install).
The pixel table XORs bit 0 of each entry's first dword with the slot parity at load (0x526440) — that touches `libHash`, not the bytecode.

Material blob (MATERIALS.md) → libs: +0x28 depth-pass PS (4 slots), +0x2C shadow-caster PS (2), **+0x30 composite PS (8)**,
**+0x34 VS (256)**. Composite pass 0x506980: PS slot = (opacity < 1 && blend < 4 && flags & 1 ? 1 : 0) | device+0x11484
(bits 1–2: PSSM variants [I]); VS slot = geometry flags | 0x20 | (0x80 when the decl's normal is FLOAT3 [I]).
VS slot bits [O by comparing variants]: 0–1 skinning (0 none, 1 weighted `SkinMatrices`, 2 rigid index), 3 depth output,
5 composite (fog, `WorldToPSSM0`, world position), 6 material colour inputs, 7 no `NormalScaleOffset`.
We use VS slot 0x20 (unskinned: CPU skinning feeds bind-pose-free vertices) else 0xA0, and PS slot 0 (no PSSM receive).
Where the weighted-skin slot (0x21 / 0xA1) writes COLOR0 from the palette's 4th rows (`c[a0 + 88]`: per-bone wetness and
dirt, GAMEPLAY.md "Wetness") the unskinned slot writes 0; such materials (`Material::skinParams`) are translated with
COLOR0 = the vertex's color0, which ou-game's CPU skinning fills with the same weighted sum.

## Survey of the install (`ou-asset shaders <gameDir> all`, 2026-10-18) [O]

| | count |
|---|---|
| DRMs (every bigfile/patch record probed for CDRM/DRM v19) | 1060 |
| ShaderLib sections / entries | 16 882 / 327 336 |
| distinct programs | **5 594** = 1 064 `vs_3_0` + 4 530 `ps_3_0` (no SM1/SM2) |
| VS instructions min / avg / max | 4 / 39.6 / 183; ≤ 8 temps |
| PS instructions min / avg / max | 1 / 270.0 / 462; ≤ 17 temps |
| relative addressing | 698 VS (`c[a0.x + 85]` = `SkinMatrices` only), 0 PS |
| flow control | 21 VS (`ifc`), 3 674 PS (`if b#` light toggles); no predication, no loops/calls |
| opcodes outside the translator | none |

Common inputs: VS position/normal/texcoord0 (all), color0 647, texcoord1 330, tangent + binormal 174, blend weights/indices 698.
PS samplers: 49 933 2D + 1 205 cube declarations. Full opcode / declaration / CTAB tables: run the tool.

## Translation (`render/ShaderTranslate`) [O]

- Every D3D register is a `vec4` local, every instruction one masked assignment (`r0.xy = (expr).xy`); source swizzles,
  negate/abs/bias/bx2/comp/x2 modifiers and `_sat` map 1:1. Scalar ops use the replicate component. Semantics:
  `rsq`/`log` take `abs`, `nrm` of a zero vector = 0, `cmp` = `mix(c, b, a >= 0)`, `lrp` = `mix(s2, s1, s0)`,
  `texkill` tests the dst mask components, `mova` rounds, `texldp/b/l/d` = `textureProj/bias/Lod/Grad`, `def`
  constants shadow the uploaded registers (D3D9 rule).
- Interface: VS inputs by usage at fixed locations (position 0, normal 1, texcoord0 2, color0 4, texcoord1 5, tangent 6,
  binormal 7; others read (0,0,0,1)); varyings texcoord n → location n, color n → 8 + n; `vPos` from `gl_FragCoord`
  (−0.5, y flipped in GL), `vFace` = ±1 from `gl_FrontFacing`, `dsy` sign-corrected per API.
- Clip space: shaders get D3D-convention matrices (z ∈ [0, w], `perspectiveRH_ZO`); GL remaps z, Vulkan keeps it and
  flips y with the existing negative-height viewport. Fixed-function alpha test (`ALPHAREF`, GREATEREQUAL) is appended.
- Register files (std140): **frame** (`vs[256]`, `ps[224]`, int/bool registers, screen, `lights[64 × 5]` = the scene's
  point/spot light registers), **material** (VS c25–c64, PS c100–c139, `fixed` = c2 `AlphaThreshold`, c98
  `MaterialOpacity`, alpha ref, flags), **object** (VS c0–c7 = `WorldViewProject`, `World`, `uvec4 lightSet` = one
  light-table index per PS light slot 0–5, 0xFF empty; GL UBO, Vulkan push constants). PS c178–c207 (5 registers per
  slot) read `frame.lights[lightSet slot]` and b0–b5 test the slot, so each object gets its own light set as tru.exe
  binds one per drawable (0x5042B0). VS c22 is read only for fogged materials (flags 0x04, 0x5068E6), PS c0
  `FogColor` is 0 for blend modes 5/6 (0x5066A0).
- Gated test `Sm3Real.EveryInstallShaderTranslates`: all 5 594 programs parse, translate and compile to SPIR-V for both
  Vulkan 1.3 and OpenGL 4.5 (libshaderc), 0 failures [O].

## Engine inputs (`render/OriginalShading`)

| register | value we feed | evidence |
|---|---|---|
| VS c0 / c4 / c12 | WVP, World, ViewProject (matrix columns) | VS code `x*c0 + y*c1 + z*c2 + c3` [O] |
| VS c21 `NormalScaleOffset` | (1, 0): our normals/tangents are decoded to [−1, 1] (engine: (2, −1) for D3DCOLOR, (1, 0) for FLOAT3, 0x529020) | [O] |
| VS c254 `TexcoordScales` | (1, 1): the loader applies 1/2048 to SHORT texcoords (engine: per decl, 0x52924B) | [O] |
| VS c23 / PS c53 `TimeVector` | (seconds mod 1000, 1/30) (engine 0x509630: (ms % 10⁶ · 0.001, dt)) | [O] |
| VS c22 `fogScaleAndOffset` | (−n/(f−n), 1/(f−n), 0, f) from the unit render settings | 0x514680 [O] |
| PS c0 `FogColor`, c15 `g_Ambient` | unit render settings (Level+0x70 DTP: fog rgb +0x18, near +0x40, far +0x3C/+0x38; ambient rgb +0x1E4 × +0x1E8 / 255) | 0x5C7CA0, 0x5CAFF0 [O]; neighbour-unit blending not done |
| PS c54 `HeightFogParams` | (0, 0, (96 + 0.5)/128, 0): no fog volumes | 0x514680 [O] |
| s15 `AttenuationSampler` | 256×128, rebuilt per scene: rows 0–95 = the distinct light falloff curves (light DTP +0x1C, texel k = curve[k]·255 in R, texel 255 = 0; LEVELS.md "Lights"), row 96 = identity fog ramp (R, G) as the engine default curve | 0x504A60, 0x50AFB0, 0x512CF0 [O] |
| PS c148/c149 `GlobalParams` light-map scales | (1, 1, 1, 1) (engine default; scripts fade them) | 0x7731B0 [O] |
| PS c140–c146 player/bone positions | far away (no foliage interaction) | 0x773A70 [O] / ours [I] |
| PS c208–c212 `SunLight`, b6 | the unit's type-2 light record (unit data +0x04 list, record row 2 = direction, DTP +0x08 intensity, +0x18 colour); 23 units have one, e.g. l4_ballcourt dir (−0.544, −0.351, −0.762), colour (0.82, 0.96, 1.20) | 0x5C6530, 0x50B910 [O] |
| PS c176/c177 `WorldToModulationMapSun`, s11 | sun cookie (DTP +0x828 texture, 11 units): uv = (record row 0 / row 1 · (p − position)) · 0.5 / (scale · 1000) + 0.5 + scroll·t, axes rotated by rotation·t deg; white without a cookie | 0x50B910 type 2 [O]; scroll/rotation time base and sign [I] |
| PS c178–c207 `ShadowLights` / `NonShadowLights`, b0–b5 | per light (0x50B910 → 0x50A5F0 `c178 + 5·slot`): R0 position (spots: moved back to the cone apex by width / tan(outer/2)), R1 direction, R2 (1/radius, 0, −apex/radius, (falloff row + 0.5)/128), R3 (cone scale, cone bias, apex plane, 0) — points (0, 1, −radius − 1), R4 colour × intensity × flicker (w = 0.3r + 0.6g + 0.1b). PS: attenuation = LUT((d − apex)/radius) · sat(cos·scale + bias), zero behind the apex plane | [O] register fill and PS math; spot cone scale/bias from 0x50A280 with [I] half angles |
| light set per object (slots 0–5) | the 6 strongest lights (luminance × (1 − distance / radius)) whose radius reaches the object's bounding sphere, in slot order (the PS nests slot n + 1 in slot n). tru.exe puts shadow casters with cookies in slots 0–2 (s12/s13 cookie, `WorldToShadowMap`) and the rest in 3–5; we bind no spot cookies / light shadow maps, so both groups compute alike (c172/c175 = (0, 0, 0, 1), s12/s13 white) | slot rule 0x5042B0 [O]; selection and receiver masks [I] |
| PS c70–c76 `IrradianceState` (dynamic objects) | c76 `Base` = g_Ambient, other terms 0: the SH tru.exe builds from the ambient alone (0x507D60: L00 = ambient · 1.128) converted by 0x508570 (Red0/Green0/Blue0 = xy, xz, yz, zz; Red1.. = x, y, z, x²−y²; Base = 0.886·L00 − 0.248·L20). Which lights the engine adds as SH terms [U]; ours reach characters through their light set | 0x507D60, 0x508570 [O] |
| instance textures (classes 1–4) | terrain group +0x10..+0x1C / model primgroup +0x2C..+0x38 indices into the resource texture list; IMF +0x4C..+0x58 overrides. Class 1 = light map (sampled at texcoord1), class 3 = "Setup" blend masks | 0x510780, 0x50EB30, 0x5B5160 [O] |
| cube samplers | cube maps decoded (TEXTURES.md); missing ones read black | [O] |

Not fed yet [U/I]: spot-light cookies (s12/s13, 63 spots) and light shadow maps (slots 0–2), receiver masks, directional
SH terms of IrradianceState, lights of object intros and cinematic light rigs, PSSM shadows, per-instance params (zero)
except the planar reflection (global class 5 + PS InstanceParams 0–2, RENDERING.md "Planar reflection") and the sea
waves (VS InstanceParams 3–6). Opacity fades: MaterialOpacity c98 stays 1;
the instance opacity multiplies the output alpha instead (RENDERING.md "Instance opacity") [I: the composite slot-1
opacity variant is not used].

Screen targets (`original::screenSamplers`, 2026-10-29): global class 9 `BackBufferTexture` / class 10
`DepthBufferTexture` [O: CTAB names], and for water shaders reading `DepthToW` the unsupplied instance classes 2 / 3
[I] (l1_seatop sea 5990) get single-sample copies of the scene drawn so far; those submeshes draw after the opaque and
blended scene (GL: after the main loop; Vulkan: in the second pass after `recordWaterCopies`). Frame registers: c51
`ScreenExtents` (0.5/w, 0.5/h, 1/w, 1/h; GL flips v) [I], c3 `DepthFactors` (0, 1, 0, 0), c77 `DepthToW`
(−(f−n)/(fn), 1/n), c97 `CameraDirection`. Viewed: the sea at l1_seatop / l2_cliffclimb (was white) shows refraction,
reflections and depth fade in GL and Vulkan.

## Fidelity (2026-10-18, viewed screenshots) [O]

- Static, light-mapped level geometry (terrain, IMFs, foliage) renders with the original multi-layer blend shaders, light
  maps, unit ambient and fog: noticeably darker and moodier than the generic lambert, with the original's layer blending
  (ballcourt path sand/rock/moss), water swirl shading in l4_pool and wind on foliage. The ballcourt court is lit by the
  unit sun (bluish).
- Point/spot lights (2026-10-18, viewed l3_main_bedroom): the torch lights reach the walls around their holders as an
  orange, flickering falloff; dynamic objects (Lara, enemies, cutscene actors) now use their own translated programs
  (IrradianceState ambient + their light set + sun) with CPU-skinned normals/tangents.
- Missing vs the original: spot cookies, light and PSSM shadows, directional SH terms on characters.
- No original-game screenshot of the same views was available for a pixel comparison (PCGamingWiki's TRU page only has
  settings-menu images); comparison is against our previous renders.
