// Parsers for TRU PC texture (PCD9) and render-model ("Mesh") sections into the
// renderer-neutral ou::scene types. Evidence and open questions: docs/formats/TEXTURES.md,
// docs/formats/MESHES.md.
#pragma once

#include "cdc/Cdc.hpp"
#include "scene/Assets.hpp"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ou::formats {

// ---- Textures (section type 5, "PCD9") ------------------------------------------------
struct PcdHeader {
    uint32_t format = 0;      // 0x15 = A8R8G8B8 (bytes B,G,R,A), or FourCC DXT1/DXT3/DXT5
    uint32_t dataSize = 0;    // bytes of pixel data following the 24-byte header
    uint32_t paletteSize = 0; // 0 in every PC texture
    uint16_t width = 0, height = 0;
    uint8_t depth = 0;        // 32 for 0x15, 0 for DXT
    uint8_t mipCount = 0;     // number of levels - 1
    uint16_t flags = 0;       // 0x8000 = cube map (6 faces)
    bool cube() const { return flags & 0x8000; }
};
std::optional<PcdHeader> parsePcdHeader(std::span<const uint8_t> sectionData);

// Decodes a type-5 section. BCn stays compressed; 0x15 is returned as BGRA8; cube maps (flags 0x8000) set
// TextureData::cube. Returns nullopt for non-PCD9 data, unknown formats and inconsistent sizes (see `why`).
std::optional<scene::TextureData> decodeTexture(const cdc::Section& s, std::string* why = nullptr);

// CPU decode of one mip level to RGBA8 (for dumps/tests). Supports all scene formats.
std::vector<uint8_t> toRgba8(scene::PixelFormat f, uint32_t width, uint32_t height,
                             std::span<const uint8_t> data);

// ---- Models (section type 12, resource type 26, "Mesh") -------------------------------
// Decodes every render model and terrain of the DRM. Vertices are converted to scene::Vertex
// (skinning data dropped; skinned meshes are returned in bind pose; the cdc colour attribute is a
// baked-lighting input, not albedo, and is not copied). One MeshData per model batch / terrain
// vertex buffer. SubMesh::textureId = the material's category-1 (diffuse) texture, else its
// category-0 slot-0 texture (heuristic), else 0; blend/alphaRef/doubleSided = materialState().
// Batches without texcoords and material textures (occlusion/fog volumes, hypothesis) are
// skipped with a warning.
std::vector<scene::MeshData> decodeModels(const cdc::Drm& drm, std::vector<std::string>& warnings,
                                          std::string_view origin = {});
// Same meshes keyed by render-resource section id (failed/skipped resources map to no meshes). Terrain submeshes are
// split by node-tree leaf and the leaves merged by the set of `lights` (the unit's, same frame) reaching them
// (scene::MeshData::lightBoxes).
std::map<uint32_t, std::vector<scene::MeshData>> decodeRenderResources(const cdc::Drm& drm, std::vector<std::string>& warnings,
                                                                       std::string_view origin = {},
                                                                       std::span<const scene::Light> lights = {});

// Texture ids of a material section (type 10), in reference order, with their category.
struct MaterialTexture { uint32_t textureId, category, slot; };
std::vector<MaterialTexture> materialTextures(const cdc::Section& material);
// Pass state of a material section: blob +0x38 blend mode, +0x40 flags (0x80 = two-sided),
// +0x4C alpha threshold, applied as PCStateManager_SetBlendMode (tru.exe 0x527A80) does; castsShadow =
// +0x44 bit 0x40000000 with blend mode < 4 (tru.exe's shadow-pass condition, 0x50F7C0 / 0x511E20).
struct MaterialState { scene::Blend blend = scene::Blend::Opaque; uint8_t alphaRef = 0; bool doubleSided = false; bool castsShadow = true; bool depthWrite = false; };
MaterialState materialState(const cdc::Section& material);
// Shader-relevant material data (docs/formats/SHADERS.md): constants, sampler entries, blend mode. The composite
// pass programs are in the ShaderLib sections `libs` names (vertex = blob +0x34, pixel = blob +0x30); the caller
// resolves them (loadDrmScene / loadLevelScene fill Material::{vertex,pixel}Shader and SceneData::shaders).
struct MaterialShaderLibs { uint32_t vertex = 0, pixel = 0; };
scene::Material decodeMaterial(const cdc::Section& material, MaterialShaderLibs* libs = nullptr);

// ---- Scene ----------------------------------------------------------------------------
// Loads `drmPath` (e.g. "pc-w\\lara.drm", "pc-w\\l4_pool.drm"), its object dependencies
// ("pc-w\\<name>.drm") when `withDependencies` (all placed at the origin), decodes every model and every texture of
// the loaded DRMs that a submesh references. Missing/unsupported data → warnings.
// Throws cdc::FormatError if `drmPath` itself is not in the bigfile.
scene::SceneData loadDrmScene(const cdc::BigfileSet& bigfile, std::string_view drmPath,
                              bool withDependencies = false);

// ---- Levels (unit DRMs). Layouts and evidence: docs/formats/LEVELS.md ----------------
// Intro +0x68 MultiSpline (tru.exe 0x5D1290 binds it to instance +0x284; LEVELS.md "Spline movers"): +0x40 position
// track, +0x44 rotation track (+0x48 a third vec4 track written to instance +0x80; no shipped intro has one).
// Track: u16 key count +0, u8 flags +3 (8 = Bezier, else Hermite), f32 end time +4 (30 Hz frames), keys from +0x10.
// Position key 0x40 bytes: value[0..3], Hermite out-tangent [4..7], Bezier control [8..11], t0 [12], t1 [13],
// 1/(t1-t0) [15]; segment i ends at the next key (0x47CD40). Rotation key 0x20 bytes: quaternion xyzw [0..3], t0
// [4], t1 [5], 1/(t1-t0) [7], slerped (0x47D180).
struct SplineTrack {
    uint8_t flags = 0;
    float end = 0;
    std::vector<float> keys; // 16 (position) or 8 (rotation) floats per key
};
struct MultiSpline {
    std::optional<SplineTrack> position, rotation;
    float origin[3] = {}; // Level+0x80: position values are stored relative to it, like collision (intro = value + origin)
    float end() const { return position ? position->end : rotation ? rotation->end : 0.0f; } // 0x5C0870
};
// Track value at time t (clamped to [0, end]): position xyz w, or quaternion xyzw.
std::array<float, 4> evalSpline(const SplineTrack& track, float t, bool rotation);

// One intro (placed object instance) of a unit: 0x90-byte record.
struct Intro {
    float rotation[3]; // Euler radians; matrix = Rx(x) * Ry(y) * Rz(z) (cdc_Matrix_Build_XYZOrder 0x49D500)
    float position[3];
    float scale[3];    // only applied when (flags[1] & 0x10), else 1 (INSTANCE_IntroduceInstance 0x5BCC30)
    uint16_t objectId; // objectlist.txt id; 0xFFFA (-6) = marker, no object
    int32_t uniqueId;
    uint16_t marker = 0; // +0x52: the id water volumes name their flow / level markers by (0x69D690, message 0x1B4 mode 3)
    uint8_t flags[4];  // bytes +0x70..+0x73
    std::optional<cdc::Pointer> data; // +0x78 intro data (trigger shapes etc., docs/formats/SCRIPTS.md); none if null
    uint32_t scriptId = 0;             // +0x7C intro script: type-8 section id in the unit DRM, 0 = none
    // Spline mover setup (0x5BA280): +0x62 flags (1 = play to key +0x6E, 2 = clip [+0x6C, +0x6E]), +0x64 low nibble
    // 1/2/4/8 = play once/loop/loop count/ping-pong from the start (other: stopped), flags[0] bit 0 = backwards.
    std::shared_ptr<const MultiSpline> spline; // +0x68, null = none
    uint16_t splineFlags = 0;
    uint32_t splineMode = 0;
    int16_t splineFrom = 0, splineTo = 0;
    bool scaled() const { return flags[1] & 0x10; }
    // Enemy intro data (data +0x00 == 13; tru.exe 0x63C7A0 -> 0x63AC40 -> 0x62F520 / 0x62F2C0, LEVELS.md "Enemy
    // intro data"): +0x34 -> planner record (index into Unit::plannerNodes, -1 none); paths of node indices into it:
    // [0] the +0x38 path {byte +2 point mode, u32 +4 count, +8 -> u32 nodes} or, with +0x6C bit 31, every node of the
    // record; [n] the +0x3C byte count of alternatives at +0x40 + 4 (n - 1) (enemybase 22 n). +0x88 order mode.
    // Point mode (0x62F520 copies it to the AI's path +0x48): the default of a point whose node +0x40 byte is < 1;
    // 2 / 3 select the patrol record's +0x10 / +0x14 attack (EnemyBehavior_PatrolAttack 0x635920), <= 3 also sets
    // AI flag 0x20000000 (0x61B220, [U]).
    struct PatrolPath { std::vector<uint32_t> nodes; uint8_t pointMode = 0; };
    int32_t patrolRecord = -1;
    std::vector<PatrolPath> patrolPaths;
    uint32_t patrolMode = 0; // 0 loop, 1 once, 2 ping-pong, 3 random free node, 4 random unvisited (0x637870)
};
// Dormant intro (+0x72 & 0x82; only 0x80 occurs): not introduced when its unit loads (tru.exe 0x5C2360); a script's
// instanceref Birth (fn 3, 0x4AC0D0 -> 0x5BD080) or a cinematic (0x4561C0) introduces it later.
inline bool dormant(const Intro& in) { return in.flags[2] & 0x82; }
// Intro +0x72 & 0x10: the game notifier's intro check (vtable +0x3C) runs and both notifiers hide the instance (instance
// flags 0x800 / 0x20000000): DefaultGameNotifier 0x54DC20 refuses it, TRGameNotifier 0x5D4B90 hides it (0x615F70)
// even when it accepts it (ObjectBase +4 & 0x400000) [O 0x5BCC30]. Survey: generators, natla_end_mover, a trigger
// volume, all already effect-only / hidden at birth (oflags).
inline bool introHidden(const Intro& in) { return in.flags[2] & 0x10; }
inline constexpr uint16_t kMarkerObjectId = 0xFFFA;

// Objects born by an uber intro's state action 0x13 (formats::UberBirth, tru.exe 0x4D3B50: the treasure inside a reward
// urn `*_pt_weightbreak_reward`, born when the urn breaks). Each is described as a dormant intro of the born object at
// the parent's birth marker, with the parent's rotation (the markers in the data are not rotated) and no scale; its
// unique id is ours: the parent's with kBornUidBit and the birth's index (the original numbers birthed instances
// itself; intro ids stay below 20000).
inline constexpr int32_t kBornUidBit = 0x20000000;
inline constexpr int32_t bornUid(int32_t parent, size_t birth = 0) { return parent | kBornUidBit | int32_t(birth << 24); }
inline constexpr int32_t bornParent(int32_t uid) { return uid & 0x00FFFFFF; }
inline constexpr bool isBornUid(int32_t uid) { return uid > 0 && (uid & kBornUidBit); }
// Objects whose births are followed [O survey of every object DRM's uber data: the reward urns are the only objects
// that give birth to a pickup; the others (formats::UberBirth) leave debris or effects].
inline bool birthParent(std::string_view objectName) { return objectName.ends_with("_pt_weightbreak_reward"); }
struct BirthIntro {
    int32_t parent = 0; // the urn's intro id
    Intro intro;
};

// Instanced static mesh ("IMF") placement from the unit's cell group data: 0x70-byte record.
struct ImfPlacement {
    float matrix[16];  // column-major model matrix (stored D3D row-major, row 3 = translation)
    uint32_t dtpId;    // DTPDATA section {?, renderModel id @+4}
    std::string name;
    uint32_t textures[4] = {}; // +0x4C..+0x58 instance textures (classes 1-4; light map) overriding the model's, 0 = none
};

// Stream-unit portal: 0x100-byte record at Terrain +0x04 (count: i16 at Terrain +0x02). Coordinates are stored
// relative to Level+0x80 like the collision meshes; parseUnit adds it (render-terrain frame).
// Fields per tru.exe 0x5D13E0 (signal hook-up) and 0x5D2AA0 (portal hit); LEVELS.md "Streaming".
struct Portal {
    std::string toUnit;     // +0x00 char[128], unit base name ("l4_ragnarok_room")
    int16_t toSignal = 0;   // +0x80 signal id of the matching portal in toUnit
    int16_t signal = 0;     // +0x82 this portal's signal id
    int16_t toStreamId = 0; // +0x84 stream unit id of toUnit
    float radius = 0;       // +0x88 environment blend radius (tru.exe 0x5C7CA0; RENDERING.md "Neighbour units")
    float min[3], max[3];   // +0x90, +0xA0 bounds; min is the relocation anchor
    float quad[4][3];       // +0xB0 corners (float4 each)
    float normal[3];        // +0xF0
};

// Unit camera spline (reveal / scripted cameras): unit data +0x68 count, +0x6C -> 0x60-byte records, played by
// tru.exe AutoSplineCamera (0x5E26F0 start, 0x5E2D90 update) through two CameraSpline players (0x60AC00 camera
// track, 0x60ADA0 look-at target track). LEVELS.md "Camera splines".
struct CameraSplineEvent {
    int32_t type = 0; // camera: 1 switch to spline `arg`, 2 end, 3 pause `value` s; target: 1 FOV `value` deg over
                      // `value2` s, 2 FOV back over `value` s, 3 pause; others [U]
    uint32_t arg = 0;
    float value = 0, value2 = 0; // arg as float; target events: third word
};
struct CameraSplineKey {
    float pos[3] = {};
    float tangent[3] = {};  // Hermite tangent (units per segment)
    float rotation[4] = {0, 0, 0, 1}; // camera track: quaternion xyzw (local x right, y down, z forward)
    float time = 0;         // seconds
    uint16_t ease[64] = {}; // easing curve over the segment, 0..65535 (eased time mode)
    bool followPlayer = false; // target track: Lara's position instead of `pos` (0x60A100)
    std::vector<CameraSplineEvent> events; // run when the key is reached
};
struct CameraSplineTrack {
    // 1 = by key times, 2 = constant speed over `duration`, 3 = by key times through the ease curves
    int mode = 1;
    float duration = 0;
    std::vector<CameraSplineKey> keys;
};
struct CameraSpline {
    float offset[3] = {}; // +0x30, added to every camera key (unit frame)
    float fovDeg = 50;    // +0x4C
    CameraSplineTrack camera;
    std::optional<CameraSplineTrack> target; // +0x58 when +0x5C; else the camera keys' rotations aim it
    bool targetFitsCamera = false; // +0x5D: target key times scaled to the camera track's length
};

// Planner node (unit data +0x11C records, +0x50 -> 0x60-byte nodes; tru.exe 0x5C8EA0 / 0x669D00). `frame` = record
// matrix x node matrix (unit frame); cdc model -Y is its facing. +0x44 angle (|.|, at most 85 degrees), +0x48 depth,
// +0x4C height: the cover cone 0x623610 tests the threat against (Lara in front beyond depth + 632.1, within the angle,
// below the height line). +0x40 point mode (< 1: the path's), +0x50 bit 0 copied to the planner, bit 1 PatrolLookTurn.
struct PlannerNode {
    glm::mat4 frame{1};
    float angle = 0, depth = 0, height = 0;
    int8_t mode = -1;
    uint32_t flags = 0;
    glm::vec3 pos() const { return glm::vec3(frame[3]); }
};

// NavPower nav mesh (Level +0x60 -> resource at +0x1C: u8 endian, u32 version 8, u32 graph count, graphs {u32, u32
// layer, u32 area bytes, u32 size, f[4] params, bbox, ...}, convex areas from graph +0x138; LEVELS.md "Nav graphs").
// Area: +0x00 centre, +0x0C normal, +0x18 radius, +0x30 low 7 bits edge count, then 0x14-byte edges {vertex, u32
// adjacent area (graph offset, 0 none), u32 flags}. Unit frame.
struct NavGraph {
    uint32_t layer = 0;
    float radius = 0, height = 0; // params [1] / [3]: the agent the graph was built for
    struct Area {
        glm::vec3 centre{0}, normal{0, 0, 1};
        std::vector<glm::vec3> verts; // edge k runs verts[k] -> verts[k + 1]
        std::vector<int32_t> adj;     // area index across edge k, -1 none
    };
    std::vector<Area> areas;
};

// NavPower link (unit data +0x44 count, +0x48 -> record pointers; tru.exe 0x48BAB0 -> 0x48B770 creates them with
// bfx CreateLink 0x417D50 / CreateLinkStrip 0x417F30; LEVELS.md "Nav links"). Record: +0x00 matrix, +0x40 u8 link
// type, +0x41..+0x50 u8[16] usage flags (bool per bit), +0x58 maxSnapDist, +0x5C mayUseDist, +0x60 mustUseDist,
// +0x64 penalty multiplier, +0x6C point count (2 link, 4 strip), +0x70 0x24-byte points {position, +0x20 u8 may start
// here}. A strip spans the edge a[0]-a[1] to b[0]-b[1] (b[k] faces a[k]); a link has a[0] == a[1], b[0] == b[1].
// Unit frame (record matrix applied).
struct NavLink {
    std::array<glm::vec3, 2> a{}, b{};
    bool fromA = false, fromB = false; // traversable a -> b / b -> a
    bool strip = false;
    uint8_t type = 0;
    uint16_t usage = 0; // agents whose PathSpec linkUsageFlags share a bit may use it
    float maxSnap = 0, mayUse = 0, mustUse = 0, penalty = 1;
};

struct Unit {
    std::vector<Intro> intros;
    std::vector<std::string> streamGroups; // stream DRM base names ("pc-w\\streamgroups\\<name>.drm")
    std::vector<uint32_t> exteriorTerrains; // cell group +0x14: render terrain ids drawn behind everything (SubMesh::exterior)
    std::vector<ImfPlacement> imfs;
    std::vector<Portal> portals;
    std::vector<CameraSpline> cameraSplines; // index = the cameraspline script value's first word
    std::vector<std::vector<PlannerNode>> plannerNodes; // [record][node] (noderef {record, node})
    std::vector<NavGraph> navGraphs;
    std::vector<NavLink> navLinks;
    scene::Environment environment; // Level+0x70 render settings (fog, ambient)
    // Level+0x70 DTP +0x294 u16: objectlist.txt id of the Lara outfit object the unit loads (tru.exe 0x5DE2B0,
    // 0x5D4340); 0 without a settings DTP. docs/GAMEPLAY.md "Outfits".
    uint16_t playerObjectId = 0;
    // Level+0x70 DTP +0x2B4 -> byte +0x0A: the personal light also turns on its spot ("Both" mode) in this unit, else
    // only its omni light (tru.exe 0x5D3FF0 -> 0x698890 flag 0xED00AA, read by 0x6B17C0). docs/GAMEPLAY.md "Flashlight".
    bool flashlightSpot = false;
    // Level+0x70 DTP +0x1C0: the unit script's type id (a type-8 section of the unit DRM), 0 = none. docs/formats/LEVELS.md "Unit script".
    uint32_t scriptId = 0;
    // Level+0x70 DTP +0x2A4 global wind (tru.exe 0x546170 registers it as wind source type 1; game/Bend.hpp): +0x2AC
    // count, +0x2B0 -> 100-byte records. Ranges {lo, hi} draw lo + (hi - lo) sin(u pi) (0x52E750).
    struct Wind {
        bool enabled = false;     // +0x00 byte
        float retarget[2] = {};   // +0x04: seconds until the next random targets
        float strength[2] = {};   // +0x0C, x 8
        float heading[2] = {};    // +0x14: degrees about Z
        float wavelength[2] = {}; // +0x1C, x 100.60183: spatial phase |x + y| / wavelength
        float frequency = 0;      // +0x24: Hz of the rectified sine (x 2 pi / 60 per 60 Hz tick)
    };
    std::vector<Wind> wind;
    // Level+0x70 DTP +0x290 u16: objectlist.txt id of the unit's weather object (rainfx_mex in the l4 jungle units),
    // 0 = none: tru.exe 0x546FC0 then uses the always-loaded `rainfx` (0x8AA774). docs/formats/EFFECTS.md "Weather".
    uint16_t weatherObjectId = 0;
    // Level+0x2C flags read by the weather (tru.exe 0x545950, 0x539EF0): 2 = music variable 2 is 1 while it rains or snows,
    // 0x20000 = rain drops start up to 18000 deep (else 0x1FFF). 0 in every retail unit (82 units surveyed).
    uint32_t flags = 0;
    // Level+0x54 precipitation height map (tru.exe 0x5C30E0): the highest surface rain stops at, unit frame, 256-unit
    // steps over x, y in [-0x40000, 0x40000): 16 x 16 cells of 0x8000, each 16 x 16 blocks of 8 x 8 heights.
    struct RainMap {
        static constexpr int32_t kNone = -8000000;
        std::array<int32_t, 256> cells{};                // block table per cell, -1 none (filled by parseUnit)
        std::vector<std::array<int32_t, 256>> tables;    // per cell: leaf index per block, -1 none
        std::vector<std::array<int32_t, 64>> leaves;     // heights, kNone where the leaf has none
        glm::vec3 origin{0};                             // Level+0x80: the map is in tru.exe's frame, unit frame - origin
        int32_t height(float x, float y) const;          // unit frame; kNone outside or without data
    };
    RainMap rainMap;
};
// Point/spot light from a light DTP (section `dtpIndex`): type, DTP radius, cone, near width, falloff curve, flicker,
// shadow flag, colour = DTP intensity (+0x08) x rgb (+0x18) (LEVELS.md "Lights"); position/direction 0. nullopt for
// suns and other types.
std::optional<scene::Light> lightFromDtp(const cdc::Drm& drm, uint32_t dtpIndex);
// Lights of an object's effect list (DTP at object section +8: +0x3C count, +0x40 -> 0x24-byte records, light DTP id at +0x20;
// created by tru.exe 0x546710 -> 0x5C35D0), distinct DTPs in record order. Empty without an object section.
std::vector<scene::Light> objectLights(const cdc::Drm& object);
// Parses the level struct (section 0) of a unit DRM. Throws cdc::FormatError on layout mismatch.
Unit parseUnit(const cdc::Drm& unit);

// Light probes of a unit (scene::IrradianceVolume, docs/formats/LEVELS.md "Irradiance volume"): the DTP of cell group
// +0x1C, in the unit frame. Empty volume when the unit has none.
scene::IrradianceVolume parseIrradianceVolume(const cdc::Drm& unit);
// Spherical harmonics of a dynamic object at `pos` (the volume's frame): tru.exe 0x55F530 -> 0x55D360 -> 0x52A450
// (nearest probe, its x / y / z neighbours blended in), then the probe region's tint, scale and L00 offset (0x52ABF0).
// False (sh untouched) when the volume is empty.
bool sampleIrradiance(const scene::IrradianceVolume& v, glm::vec3 pos, float sh[9][3]);

// objectlist.txt ("count\r\n" then "id,name" lines) -> id → lower-case name.
std::map<uint16_t, std::string> parseObjectList(std::string_view text);

// Render-model resource ids of an object DRM's models, in Object model-list order (0 = no render
// model). Object section (type 11) +0 → ObjectBase DTP; +0xA0 u16 numModels, +0xA4 Model**;
// Model DTP +0x64 → render model. Empty if the DRM has no object section.
std::vector<uint32_t> objectRenderModels(const cdc::Drm& object);
// ObjectBase +0 flags ("oflags") of an object DRM; 0 if it has no object section.
// INSTANCE_IntroduceInstance (0x5BCC30) sets instance flag 0x800 (not drawn) for kObjectHiddenAtBirth.
uint32_t objectFlags(const cdc::Drm& object);
// Section index of the ObjectBase DTP (object section +0 resource reference); nullopt without an object section.
// Throws cdc::FormatError when the reference does not resolve.
std::optional<uint32_t> objectBaseSection(const cdc::Drm& object);
inline constexpr uint32_t kObjectHiddenAtBirth = 0x800000;
// Effect-only objects (fx_* proxies, bullet hits, squibs, camera anchors, ...): cdc::InstanceDrawable returns no draw
// flags (tru.exe 0x5B5CA0) and no bounds (0x5B5BE0) for oflags bit 0x40000000, so their models are never drawn.
inline constexpr uint32_t kObjectNoInstanceDraw = 0x40000000;

// Column-major T * Rx * Ry * Rz * S, as tru.exe builds instance matrices.
void introMatrix(const Intro& in, float out[16]);
// The births of `unit`'s birthParent() intros, in intro order (reads each parent object's DRM once).
std::vector<BirthIntro> birthIntros(const cdc::BigfileSet& bigfile, const Unit& unit, const std::map<uint16_t, std::string>& names);

// Whole level: unit terrain, stream-group DRM terrain, IMF placements and intro instances
// (model 0 of each object, placed with the intro transform). Objects hidden at birth
// (objectFlags & kObjectHiddenAtBirth: trigger/sound volumes, markers) are skipped. Textures as
// loadDrmScene. Unresolved objects, missing DRMs and objects without a model are reported as warnings.
scene::SceneData loadLevelScene(const cdc::BigfileSet& bigfile, std::string_view unitPath);

} // namespace ou::formats
