# Lighting data: where each input lives, how it is read, what consumes it

Index document. It owns no byte layout: every table below links to the one document that does, so a layout is
described exactly once. Scope: the original game's lighting inputs (unit lights, light masks, light probes / SH, light
maps, sun and cookies, PSSM shadow inputs, per-drawable light limits) and where OpenUnderworld reads them.
Enhanced-only lighting (our shadow maps, RT shadows / AO / GI, CORTAO) is ours, not data: [RENDERING.md](../RENDERING.md)
"Sun shadows", "Ray tracing", "Ambient occlusion".

Labels: **[O]** observed in the GOG 1.1 data, in a running tru.exe under Wine, or in our tru.exe 1.1.0.0 decompile;
**[I]** inference; **[U]** unknown. VAs are tru.exe 1.1.0.0.

**Endianness:** little-endian for every field listed (PC data); light records and DTPs are read with relocations
resolved per [CONTAINERS.md](CONTAINERS.md) §4.5 [O].

## 1. Inventory

| Input | Stored in | Layout (single source) | Loader (ours) | Consumer (ours) | tru.exe |
|---|---|---|---|---|---|
| Unit lights (point / spot / sun) | unit DRM: unit data `+0x00` count, `+0x04` → 0xE0-byte light records; each names a 0x14E4-byte **light DTP** (type-7 resource reference) | [LEVELS.md "Lights"](LEVELS.md#lights-unit-data-0x00--0x04) | `formats::parseUnit` → `scene::Environment::lights` / `sun*` (`scene::Light`, `src/scene/Assets.hpp`) | `render::original::lightSet`, `LightTable` (`src/render/OriginalShading.*`) | 0x5CEC60 (create), 0x5C6530 (record overrides), 0x50AFB0 / 0x50B910 (DTP → render light) |
| Light falloff curves | light DTP `+0x1C` f32[255] | LEVELS.md "Lights" (DTP table) | `scene::Light::falloff` | AttenuationSampler s15 rows 0–95 ([SHADERS.md "Engine inputs"](SHADERS.md#engine-inputs-renderoriginalshading)) | 0x504A60, 0x50A050 |
| Flicker | light DTP `+0x41C` flag, `+0x420` period, `+0x424` f32[256] | LEVELS.md "Lights" | `scene::Light::flicker`, `flickerPeriod` | light colour × curve | 0x50A500 |
| Light switching by camera distance | light record `+0x7C`, `+0x80`, `+0x84`, `+0x8A` | LEVELS.md "Lights" (runtime paragraph) | `scene::Light::{lit, fadeDistance, fadeRange, fadeRadius}` | `render::original::lightFade` | 0x5CC190, 0x5CBFA0 |
| Light masks | light record bytes `+0x8C..+0xB7` (two 32-bit masks: light / shadow, SceneLight `+0x154` / `+0x158`); receiver mask on entities (`+0xE4`) and terrain cell data (`+0x08`); cinematic rig lights: CINE channel 6 | LEVELS.md "Lights"; [CINEMATICS.md §4 "Lights"](CINEMATICS.md#lights-10-per-light) | `scene::Light::mask` (unit lights: all bits, see §3), `scene::MeshData::lightMask` (1; cinematic actors: instance channel 2) | `original::lightSet(…, receiver)` | 0x55FB60 (entities), 0x56ABF0 (terrain) |
| Spot cookies | light DTP `+0x824` flag, `+0x828` texture ref, `+0x82C` scale, `+0x830` rotation, `+0x834/+0x838` scroll | LEVELS.md "Lights"; projection in SHADERS.md (`WorldToModulationMapSpot`) | `scene::Light::cookie*` | s12/s13, one 16-layer 256² array | 0x50B910 spot branch, 0x50A5F0 |
| Sun (directional light) | the unit's type-2 light record (same list) | LEVELS.md "Lights"; registers in SHADERS.md (`SunLight` c208–c212) | `scene::Environment::{sun, sunDirection, sunColor}` | PS c208–c212, b6 | 0x5C6530, 0x50B910 |
| Sun cookie ("modulation map") | sun's light DTP `+0x828` (11 units) | SHADERS.md (`WorldToModulationMapSun`) | `Environment::{sunCookie, sunAxes, sunOrigin, cookie*}` | s11, c176/c177 | 0x50B910 type 2 |
| Unit ambient `g_Ambient` | unit render-settings DTP (Level `+0x70`) `+0x1E4` rgb × `+0x1E8` / 255 | SHADERS.md (PS c15) | `Environment::ambient` | PS c15, blended with neighbour units by portal distance | 0x5C7CA0, 0x5CAFF0 |
| Light probes (irradiance volume, SH L2) | cell group `+0x1C` → irradiance DTP (regions, sphere tree, probes with f32 SH[9][3]) | [LEVELS.md "Irradiance volume"](LEVELS.md#irradiance-volume-cell-group-0x1c-dtp) | `formats::parseIrradianceVolume` → `Environment::irradiance`; `formats::sampleIrradiance` | `original::IrradianceSlots` → PS c70–c76 `IrradianceState` | 0x55D360, 0x52A450, 0x52ABF0 (tint/scale/offset), 0x508570 (SH → registers) |
| Light maps | class-1 **instance textures**: terrain group `+0x10..+0x1C` / model primgroup `+0x2C..+0x38` indices into the resource texture list, IMF `+0x4C..+0x58` overrides; sampled at **TEXCOORD1** (vertex attribute `0x8E54B6F3`) | SHADERS.md ("instance textures"); [MESHES.md "VertexDecl and attributes"](MESHES.md#vertexdecl-and-attributes); IMF record in LEVELS.md "Cell group data" | `formats::loadLevelScene` / `loadDrmScene` → `SubMesh::instanceTextures[0]`, then `meanLightMaps` (§2) | original composite shaders (light map × albedo, lerp with COLOR0 by a material constant, [MATERIALS.md "Vertex colour, second UV"](MATERIALS.md#vertex-colour-second-uv)) | 0x510780, 0x50EB30, 0x5B5160 |
| Light-map scale | PS c148/c149 `GlobalParams` | SHADERS.md | constant (1, 1, 1, 1) | — | 0x7731B0 (scripts fade them [O]; not fed [I]) |
| Baked vertex lighting | vertex attribute `0x7E7DD623` (D3DCOLOR, `COLOR0`) | MATERIALS.md "Vertex colour, second UV"; MESHES.md | translated shaders read it; not copied to `Vertex::color` | original shaders | — |
| PSSM sun shadows | sun SceneLight `+0xF4` = 4 splits; render settings `+0x28..+0x34` split fractions and `+0x38` far plane; sun record `+0x5D`, `+0x60` / `+0x64` depth / slope bias | [SHADERS.md "Fidelity" (PSSM)](SHADERS.md#fidelity-2026-10-18-viewed-screenshots-o); atlas allocation [docs/re/RENDER_PIPELINE.md §6](../re/RENDER_PIPELINE.md#6-shadows) | `Environment::{pssmSplits, farPlane, shadowBias}` | `original::pssm` / `fillPssm` (Original preset shadows), PS slot 6, s14 | 0x50CCB0, 0x504F30, 0x508350 |
| Light shadow maps | light DTP `+0x144C` (shadow caster), `+0x144D` (keeps a map with shadows off) | LEVELS.md "Lights"; RENDER_PIPELINE.md §6 | `scene::Light::shadow` | slot choice only; maps not rendered (RT / our maps instead, SHADERS.md "Not fed yet") | 0x50A080, 0x50C420 |
| Exterior / sky terrain | cell group `+0x14` CellTerrainData `{terrain ref, flags, light mask, user data}` | LEVELS.md "Cell group data" | `Unit::exteriorTerrains` | unfogged, casts no shadows | 0x572BD0, 0x511E20 |
| Object-intro lights, effect lights | FX class 6 lights; gadget lights (personal light) | [EFFECTS.md "Lights (class 6)"](EFFECTS.md#lights-class-6-o-unless-marked); [GAMEPLAY.md "Flashlight"](../GAMEPLAY.md) | `fx::` / `game::Gadgets` | added to the frame's lights | — |
| Cinematic light rigs | CINE light channels (10 per light) | CINEMATICS.md §4 "Lights" | `cine::` | lights with ch6 masks | — |

## 2. Light maps without TEXCOORD1 (mean-light-map rule)

Deviation from tru.exe, by design; full text in [SHADERS.md "Engine inputs"](SHADERS.md#engine-inputs-renderoriginalshading)
("Light maps without coordinates"), code `meanLightMaps` in `src/formats/Scene.cpp`:

- Trigger: a mesh whose vertices all have uv1 = (0, 0) (its declaration has no TEXCOORD1) and a submesh whose material
  samples a class-1 light map (sampler source 2, class 1). Light-map source = the IMF/primgroup instance texture, else
  the material's own [O code].
- tru.exe reads the undeclared input (undefined in D3D9, zero under DXVK [I]), i.e. the blend of the map's four corner
  texels for the whole mesh. If any corner texel is pure black and the map has lit texels, ours substitutes a 1 × 1 map
  of the mean of the lit (non-black) texels, id = source | `0x40000000`; maps with four lit corners keep the corner
  read; all-black maps stay black.
- Affected [O]: l1_seafloor palm kelp / fan coral, l2_flushtunnel sea fans and seaweed, jungle grass, l2_cliffclimb's
  reused l1 kelp (atlas 1340). Test: `RealData.LightMapsWithoutCoordinatesReadTheMean` (24 palm-kelp submeshes get
  1340 | 0x40000000 with a lit texel; seaweed bushes and straight kelp keep their maps).

## 3. Per-drawable light limits

Single source: [SHADERS.md "Engine inputs"](SHADERS.md#engine-inputs-renderoriginalshading) ("light set per object") and the
summary table in [docs/re/RENDER_PIPELINE.md §8](../re/RENDER_PIPELINE.md#8-lighting-limits-summary-details-in-shadersmd).
In short [O 0x55FB60, 0x504CC0, 0x5042B0]: 6 light slots per drawn entity, ≤ 3 special (cookie spots ≤ 2 in slots 0–1,
shadow casters) + ≤ 3 plain (slots 3–5), overflow into free special slots, chosen in list order from the lights touching
the entity minus the sun, filtered by receiver mask; the sun has its own registers; terrain uses two 64-bit light masks
per node-tree leaf over its cell's light table (0x5128B0 → 0x511810, 0x56ABF0), ours `MeshData::lightBoxes` /
`SubMesh::lightGroup` ([MESHES.md "Terrain"](MESHES.md#terrain-resource-type-24)). Survey: at most 28 lights in one unit
(l1_ship_sinking_hall) [O]. Ours merges the lights of every loaded unit (`StreamReal.BedroomNeighbourLights`).

Not applied [O code]: the unit light records' own mask bytes (unit lights get all bits, as their observed `0x8300000F`
never excludes a receiver with mask 1 [I]); directional SH terms of `IrradianceState`; light shadow maps.

## 4. Validation

| Data | Loader rejects | Enforced by |
|---|---|---|
| Light records / DTPs | `parseUnit` throws `cdc::FormatError` on reads past a section end (record or DTP shorter than the offsets read, e.g. a DTP under 0x144D bytes), a missing relocated pointer ("no relocated pointer for …"), a truncated resource relocation table. Skipped silently [O code, Level.cpp]: list entries without a relocation, records whose `+0x48` DTP is absent from the unit DRM, DTP types > 2, every sun after the first | `Level.ParseUnitLights` (synthetic records + DTPs: point radius / colour override / falloff texels, spot cone / near width / flicker, sun cookie, scale and origin Level+0x80), `RealData.UnitLightsAllUnits` (all 82 units). Synthetic rejection test (short DTP, missing DTP): **missing** |
| Irradiance volume | regions or probes pointing outside the irradiance DTP ("irradiance region/probe outside its DTP"); missing pointers | real data: `OriginalShading.KrakenHeadIrradianceMatchesOriginal` (sampled SH vs a Wine read). Synthetic rejection test: **missing** |
| Light set / masks / switching | — (runtime) | `OriginalShading.LightTableAndSets`, `LightSetGroups`, `BedroomLightSetMatchesOriginal`, `BedroomLightsSwitchByCameraDistance`, `TerrainLeafLightsMatchOriginal`, `SpotCookieProjection`; `CineReal.LightRigsAimAtTheActors` (rig masks) |
| PSSM inputs | — | `OriginalShading.PssmSplitsMatchOriginal` (split distances / matrices vs Wine-read registers) |
| Light maps | missing texture → submesh keeps its source id (renderers bind a fallback) [I]; mean rule above | `RealData.LightMapsWithoutCoordinatesReadTheMean` |
| Effect / gadget lights | see EFFECTS.md / GAMEPLAY.md | `FxReal.GlowsAndEffectLights`, `GadgetsReal.TuningLightsAndSpotUnits` |

## 5. Open questions

- [U] Light record `+0x5D/+0x60..+0x68` beyond the shadow bias use, `+0x88/+0x89/+0x8F/+0xA6/+0xA7/+0xC4..+0xD0`.
- [U] Irradiance region `f32[6]` and the per-entity region-tint freeze (LEVELS.md).
- [U] IMF `+0x4C` texture override beyond class-1 light maps; who drives `GlobalParams` c148/c149 per level.
- [I] Spot cone half-angle convention and cookie rotation sign (SHADERS.md).
