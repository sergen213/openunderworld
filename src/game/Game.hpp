// Gameplay: world state, Lara's controller, follow camera and input mapping. No renderer types here;
// apps turn World state into draw calls. Evidence, numbers and deviations: docs/GAMEPLAY.md.
// Frame: Z-up game units in the render-terrain frame (as ou_collision and the level intros).
#pragma once

#include "anim/Anim.hpp"
#include "collision/Collision.hpp"
#include "config/Config.hpp"
#include "platform/Input.hpp"
#include "physics/Physics.hpp"
#include "formats/Formats.hpp"
#include "formats/Uber.hpp"
#include "game/Combat.hpp"
#include "game/Progress.hpp"
#include "game/Water.hpp"
#include "script/Script.hpp"
#include "game/ai/Enemy.hpp"

#include <glm/glm.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ou::cdc { class BigfileSet; }
struct SDL_Gamepad;

namespace ou::game {

using glm::vec2;
using glm::vec3;

inline constexpr double kTickSeconds = 1.0 / 60.0; // fixed simulation step (engine::FrameClock default)

// One tick of player intent, already mapped from devices or an input script.
struct Actions {
    vec2 move{0};      // camera-relative stick: x = right, y = forward, length <= 1
    vec2 look{0};      // camera orbit rate, rad/s: x = yaw (positive turns the camera left), y = pitch
    bool walk = false; // walk modifier
    bool jump = false; // held; the controller reacts to the press edge
    bool drop = false; // held; let go of a ledge / pole; on the ground: crouch (held), roll (running), drop to hang
    bool interact = false; // held; switches/levers react to the press edge, push blocks while held (src/script)
    bool save = false, load = false; // held; ou-game quicksaves / quickloads on the press edge (F5 / F9)
    // combat (Combat.cpp): draw toggles the weapon on the press edge; fire held (draws first); aim held = hard
    // lock-on; switchTarget press edge cycles the lock; weapon press edge swaps pistols <-> the secondary; heal
    // press edge uses a health pack. Adrenaline: interact while the meter is full and close to a hard lock.
    bool draw = false, fire = false, aim = false, switchTarget = false, weapon = false, heal = false;
    bool pause = false; // held; ou-game opens the pause menu on the press edge (Esc / pad Start; scripts: "pause")
    bool grapple = false; // held; the grapple reacts to the press edge (G / pad right shoulder; scripts: "grapple")
    bool sprint = false;  // held (Left Alt / pad left stick click; scripts: "sprint")
};

// ---- animations -------------------------------------------------------------------------------
// Lara's clips the controller uses (the full mapping table is in docs/GAMEPLAY.md).
struct AnimLibrary {
    scene::Skeleton skeleton;
    std::map<std::string, anim::Clip, std::less<>> clips; // by animation-list name
    // Hands (bones 55 and 80) relative to the root in the ledgeb_idle pose, model space; places the
    // root under a grabbed ledge.
    vec3 hangHandOffset{6.3f, -141.7f, 495.6f};
    // Hands relative to the root hanging from a horizontal pole (HPole_idleHangPerp at t = 0).
    vec3 poleHandOffset{-3.4f, 17.0f, 619.4f};
    // Swim speed, units/s: mean root speed of the two one-shot clips that start swimming,
    // swim_surfaceidle_to_swim (206 units / 0.367 s) and swim_surfacedive (676 units / 1.2 s). The swim
    // loops have no root motion.
    float swimSpeed = 562.0f;
    anim::HairRig hair; // Lara's hair particles + constraints (anim::decodeHair); empty = rigid hair
    // Weapons (index = Weapon): stats (loadWeapon) and Lara's clips from each weapon's DRM (weaponClipNames), kept in
    // `clips` as "<object>:<clip>" (the DRMs reuse names). Defaults keep synthetic libraries usable without them.
    std::array<WeaponDef, kWeaponCount> weapons{weaponDefaults(Weapon::Pistols), weaponDefaults(Weapon::Uzi),
                                                weaponDefaults(Weapon::Shotgun), weaponDefaults(Weapon::Speargun)};

    const anim::Clip* find(std::string_view name) const;
    const anim::Clip* weaponClip(Weapon w, std::string_view name) const; // find("<object>:<name>")
    // Mean of the hand bones (55, 80) relative to the root segment in `clip` at `t` (model space, root
    // translation removed); nullopt without a full skeleton.
    std::optional<vec3> handsAt(const anim::Clip& clip, double t) const;
};
// Names the controller requests; loadLaraAnims loads exactly these.
extern const std::vector<std::string_view> kLaraClipNames;
// Lara's clips in weapon w's DRM: aim grid, fire, reload, equip/unequip (+ shotgun pump, speargun submerged).
const std::vector<std::string_view>& weaponClipNames(Weapon w);
// How the original fades `to` in after `from`: crossfade length and start time (seconds) from lara.drm's
// anim requests. Player::play starts clips at startSeconds; World crossfades over blendSeconds.
struct ClipBlend { float blendSeconds; double startSeconds; };
ClipBlend clipBlend(std::string_view from, std::string_view to);
// Skeleton + kLaraClipNames clips from pc-w\<outfit>.drm, each weapon's weaponClipNames + stats. Throws
// cdc::FormatError / std::runtime_error.
AnimLibrary loadLaraAnims(const cdc::BigfileSet& bigfile, const std::string& outfit = "lara");
// Replaces `lib`'s skeleton, hair rig and hand offsets with those of outfit DRM `drm` (pc-w\lara_*.drm). The
// outfits' clips are byte-identical to lara.drm's; their skeletons share its topology with other pivots
// (docs/GAMEPLAY.md "Outfits"). Throws std::runtime_error without a skeleton.
void rigOutfit(AnimLibrary& lib, const cdc::Drm& drm);

// Segment-0 (root) translation of `clip` at unbounded time `t` seconds. Looping clips are unwrapped:
// each full cycle adds the cycle displacement, so root(t1) - root(t0) is the motion over [t0, t1]
// even across the loop seam. Non-looping clips clamp to [0, duration].
vec3 rootTranslation(const anim::Clip& clip, double t, bool loop);

// ---- player -----------------------------------------------------------------------------------
enum class State {
    Idle, Walk, Run, Stop, Turn, JumpPrep, Jump, Fall, Land, Hang, Shimmy, ClimbUp,
    // water (tru.exe player states SwimSplashDownState, SwimTreadState, SwimState, SwimDiveInState,
    // SwimClimbOutState; RTTI)
    SwimSplash, SwimTread, SwimSurface, SwimDive, SwimUnder, Surfacing, SwimClimbOut,
    Dead, // death clip, no control; World respawns (Combat.cpp)
    // traversal (tru.exe CrouchState, RollState, SlopeSlideState; the rest are state-chart driven):
    // Beam = balance beam (markup 14), DropToHang = drop_to_ledgeb off an edge, HangJump = targeted
    // jump from a ledge/pole to another ledge/pole, PoleHang = hanging/swinging on a horizontal pole.
    Crouch, Roll, Slide, Beam, DropToHang, HangJump, PoleHang,
    Climb, // free climbing on climbable rock faces (DICE clips; src/game/Climb.cpp)
    Ladder, Perch, Pillar, // markup 9 ladders, 13 perches, 15 pillars (src/game/Traverse.cpp)
    GrappleThrow, GrappleSwing, // grapple attached: throw/jumpon clip, then pendulum on the rope (Grapple.cpp)
    Hit, // hit reaction clip once with root motion (ground or swimming), then Idle / the swim state (World::damageLara)
    RailSlide, // markup 8 rail slide (src/game/Traverse.cpp)
    GrapplePull, // mounted target: pull the object instead of swinging (Grapple.cpp)
};
std::string_view stateName(State s); // "unknown" past the last state

// Controller constants. Provenance ([O] observed / [I] inferred) is listed in docs/GAMEPLAY.md.
struct Tuning {
    float radius = 100.0f, height = 512.0f;    // capsule; feet at the root. height: lara.drm player data +0x10
    float stepUp = 70.0f, stepDown = 70.0f;    // ground snap window
    float minFloorNormalZ = 0.7f;              // steeper = wall (~45.6 deg)
    float runSpeed = 1274.0f;                  // horizontal take-off speed of a running jump, units/s
    float gravity = 5.72443f * 900.0f;         // units/s^2 (tru.exe: 5.72443 units per 30 Hz frame^2)
    float jumpVz = 1803.0f, runJumpVz = 1803.0f, standJumpSpeed = 637.0f; // units/s
    float turnRate = 6.0f;                     // rad/s
    float grabReach = 60.0f, grabWindow = 40.0f; // hand-to-ledge horizontal / vertical tolerance
    // Grab only while -13 <= zVel <= 9 units/frame * 5.72443 (30 Hz): player data bytes +0x834 / +0x833 (0x759260)
    float grabMinVz = -13 * 5.72443f * 30.0f, grabMaxVz = 9 * 5.72443f * 30.0f; // units/s
    // Water test box (0x710A20): half extents player data shorts +0xC3E (x, y) / +0xC40 (z), centred
    // +0xC42 above the feet; overlapping a water volume puts Lara in it.
    vec3 waterProbeHalf{221.0f, 221.0f, 572.0f};
    float waterProbeCenterZ = 320.0f;
    float swimDepth = 400.0f;   // water at least this deep under the surface is swum, shallower walked
    float swimRadius = 100.0f;  // sphere around the swimming root
    float crouchHeight = 260.0f; // capsule while crouched/rolling: player data +0x14
    float slideMinNormalZ = 0.3f; // floors between this and minFloorNormalZ are slid down
    float slideAccel = 2500.0f, slideMaxSpeed = 1400.0f; // units/s^2, units/s down the slope
    float beamAttach = 30.0f;   // root within this of a balance beam (x/y, and z) gets on it
    float poleReach = 60.0f;    // hands to pole axis, horizontal
    float assistRadius = 300.0f; // jump-landing / beam markup this close to the natural landing pulls the jump onto it
    float hangJumpApex = 60.0f;  // targeted jumps rise this far above the higher of start and target
    // Reach of targeted jumps from a hang (hands to target hands): up, sideways, back (away from the wall).
    float hangJumpUp = 700.0f, hangJumpSide = 800.0f, hangJumpBack = 1200.0f, poleFlingReach = 1800.0f;
};
Tuning defaultTuning();

enum class GrappleKind : uint8_t { Ceiling, Wall, Post, Mounted };

struct PlayerState {
    State state = State::Idle;
    vec3 pos{0};    // root = feet; swimming: the swim clips' root (water surface while treading, body centre under water)
    float yaw = 0;  // heading: forward = (cos yaw, sin yaw, 0)
    vec3 vel{0};    // air velocity, units/s
    std::string clip;
    double clipTime = 0; // unbounded for looping clips (anim time = fmod(clipTime, duration))
    float clipRate = 1;
    bool loop = true;
    // attached markup (index into collision().markup): ledge while hanging, balance beam, pole; while a
    // HangJump flies, the target. Segment, position along it, and side: ledges/poles = which side of the
    // polyline Lara hangs on, beams = walking direction (+1 along the points).
    int ledge = -1, ledgeSeg = 0;
    float ledgeT = 0;     // distance from the segment start
    float ledgeSide = 1;
    bool runJump = false; // JumpPrep: take-off from a run
    float regrabCooldown = 0;
    int water = -1;  // swimming: index of the water markup (type 10) in collision().markup
    float pitch = 0; // under water: swim direction pitch, rad (+ = up); the model is pitched by it
    float flight = 0; // HangJump: seconds left until the target is reached
    float fallStartZ = 0; // z when Lara last became airborne (tru.exe player +0x6DC, set on FallState entry 0x75C550)
    float fallHeight = 0; // set by land(): fallStartZ - pos.z; 0 on every other tick
    // Free climbing (State::Climb): terrain face index + outward normal of the wall she is on.
    uint32_t climbFace = UINT32_MAX;
    vec3 climbNormal{0};
    // Grapple (State::GrappleThrow/Swing): anchor intro unique id, world anchor, rope length at hand (root + 400).
    int32_t grappleUid = 0;
    vec3 grappleAnchor{0};
    float ropeLength = 0;
    GrappleKind grappleKind = GrappleKind::Ceiling;
};

class Player {
public:
    Player(const collision::CollisionMesh& col, const AnimLibrary& anims, Tuning tuning = defaultTuning());
    void spawn(vec3 feet, float yaw);
    // `move`: world-space wish direction (z ignored), length <= 1.
    void tick(vec2 move, const Actions& a, float dt);
    const PlayerState& state() const { return s_; }
    const Tuning& tuning() const { return t_; }
    void carry(vec3 d) { s_.pos += d; } // moved by what she stands on (platforms, physics doors)
    void restore(const PlayerState& s) { s_ = s; jumpHeld_ = dropHeld_ = true; } // load/stream; held: no edge on the next tick
    // World-space model matrix (root at pos; model -Y = forward; pitch tilts the head up about the root).
    glm::mat4 modelMatrix(vec3 pos, float yaw, float pitch = 0) const;
    void face(float yaw) { s_.yaw = yaw; } // combat: turn toward a locked target
    void attachGrapple(vec3 anchor, int32_t uid, GrappleKind kind = GrappleKind::Ceiling); // Grapple.cpp: target attachment
    void releaseGrapple(bool jump);               // let go, keeping the swing's momentum
    void die(std::string_view clip);       // State::Dead playing `clip` once
    void hitReact(std::string_view clip);  // State::Hit playing `clip` once (restarts it)

private:
    void play(std::string_view clip, bool loop, float rate = 1);
    bool clipDone() const;
    vec3 advanceClip(float dt); // returns the model-space root delta
    vec3 advanceRoot(float dt); // world root delta; the clip's root Z rotation turns the yaw
    vec3 forward() const;
    vec3 toWorld(vec3 model) const; // model-space vector -> world, by yaw
    float capHeight() const;        // capsule height (crouched or standing)
    bool standable(vec3 p, const collision::Hit& g) const; // floor hit under p supports standing
    void groundMove(vec3 delta, float dt);
    void airMove(float dt, vec2 move);
    vec3 slide(vec3 pos, vec3 delta, float bottom, bool horizontalNormals) const;
    void startFall(vec3 vel);
    void startSlide(vec3 vel);
    bool tryGrab(vec3 prevPos);
    bool tryGrabPole(vec3 prevPos);
    bool tryBeam(std::string_view clip, bool anyHeading);
    bool tryDropToHang();
    bool canClimbUp() const;
    bool startHangJump(int kind); // 0 up, 1 right, -1 left, 2 back (ledge); 3 pole fling
    void assistJump();
    void placeOnLedge(); // ledge (type 1) or pole (4, 6) attachment from ledge/ledgeSeg/ledgeT/ledgeSide
    std::pair<vec3, float> attachPose(int markup, int seg, float t, float side) const; // root, yaw
    void beam(vec2 move, bool jumpPressed, bool dropPressed, float dt);
    // water: volume overlapping the water probe box at `feet` (index into col_.markup), its surface z
    int waterAt(vec3 feet) const;
    float surfaceZ() const;
    bool enterWater(); // from the ground or the air; false if not in deep enough water
    void swim(vec2 move, const Actions& a, bool jumpPressed, bool dropPressed, float dt);
    vec3 swimSlide(vec3 pos, vec3 delta) const;
    bool tryClimbOut();
    // Free climbing (src/game/Climb.cpp): attach from the ground (jump) or the air, then climb().
    bool tryClimb(bool fromAir);
    void climb(vec2 move, bool jumpPressed, bool dropPressed, float dt);
    // Ladders (markup 9), perches (13), pillars (15), src/game/Traverse.cpp. climbNormal = outward from the wall.
    bool tryLadder(bool fromAir);
    void ladder(vec2 move, bool jumpPressed, bool dropPressed, float dt);
    bool tryPerch(bool fromAir);
    void perch(bool jumpPressed, bool dropPressed, float dt);
    bool tryRail(bool fromAir);
    void rail(bool jumpPressed, bool dropPressed, float dt);
    void grapple(vec2 move, const Actions& a, bool jumpPressed, bool dropPressed, float dt); // Grapple.cpp
    void land(vec2 move);

    const collision::CollisionMesh& col_;
    const AnimLibrary& anims_;
    Tuning t_;
    PlayerState s_;
    bool jumpHeld_ = false, dropHeld_ = false;
};

// ---- camera -----------------------------------------------------------------------------------
struct CameraState {
    vec3 position{0}, target{0};
    float yaw = 0, pitch = -0.25f; // orbit angles; camera looks along yaw/pitch
};

// ---- world ------------------------------------------------------------------------------------
struct Spawn {
    vec3 pos{0};
    float yaw = 0;
    bool fromIntro = false;
};

// ---- units, streaming, checkpoints (docs/formats/LEVELS.md "Streaming", docs/SAVES.md) -------
// Grapple-target intro (`*_ei_grapple_ceiling/_wall/_post` etc.): position in its unit's frame.
struct GrappleTarget {
    int32_t uid = 0;
    vec3 pos{0};
    GrappleKind kind = GrappleKind::Ceiling;
};
// A `checkpoint` / `invis_checkpoint` intro: respawn transform in its unit's frame (tru.exe 0x5D40C0).
struct Checkpoint {
    int32_t id = 0; // intro unique id
    vec3 pos{0};
    float yaw = 0;
};
// One unit's gameplay data in its own (render-terrain) frame.
struct UnitData {
    std::string name; // base name, "l4_pool" (pc-w\<name>.drm)
    collision::CollisionMesh col; // BVH not built; World merges the loaded units
    std::vector<formats::Portal> portals;
    std::vector<Checkpoint> checkpoints;
    std::vector<GrappleTarget> grapples; // *_ei_grapple_* intros, unit frame
    std::optional<Spawn> spawn; // findSpawn
    std::shared_ptr<const cdc::Drm> drm;
    // Progression (src/game/Progress.hpp): reward intros; cinematic intros whose script names chapter start
    // units (level ends); intros whose script names a music track (pc-w\music\<name>\<name>.mus exists),
    // first one; the unit's own track "<name>_mus_01" when it exists, else empty [I].
    std::vector<Pickup> pickups;
    std::map<int32_t, std::vector<std::string>> chapterLinks;
    std::map<int32_t, std::string> musicByIntro;
    std::string music;
    std::map<int32_t, std::string> cinematics; // cinematic_object intro -> the stream it plays (cine::introCinematic)
    std::string playerObject; // Lara outfit object of this unit (formats::Unit::playerObjectId name); empty if none
};
// Throws cdc::FormatError if pc-w\<name>.drm is missing or not a unit.
UnitData loadUnitData(const cdc::BigfileSet& bigfile, const std::string& name);
using UnitLoader = std::function<UnitData(const std::string& name)>;
struct LoadedUnit {
    UnitData data;
    vec3 offset{0};        // world = unit frame + offset (portal relocation, tru.exe 0x5D13E0)
    size_t markupBase = 0; // first index of this unit's markup in World::collision().markup
};
struct CheckpointRef {
    std::string unit;
    int32_t id = 0;
    bool operator==(const CheckpointRef&) const = default;
};

class World {
public:
    World(collision::CollisionMesh col, AnimLibrary anims, Spawn spawn, Tuning tuning = defaultTuning());
    // Streaming world: `unit` at offset 0 plus its portal neighbours; Lara at `spawn` (unit frame), else
    // at the unit's player intro. Throws what `loader` throws for `unit`; failing neighbours are warnings.
    World(UnitLoader loader, const std::string& unit, AnimLibrary anims, std::optional<Spawn> spawn = {},
          Tuning tuning = defaultTuning());
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    void tick(const Actions& a); // one kTickSeconds step; keeps the previous state for interpolation

    const collision::CollisionMesh& collision() const { return col_; }
    const AnimLibrary& anims() const { return anims_; }
    const Player& player() const { return player_; }
    const PlayerState& previous() const { return prev_; }
    const CameraState& camera() const { return cam_; }
    const CameraState& previousCamera() const { return prevCam_; }
    uint64_t ticks() const { return ticks_; }
    // Switches, doors, trigger volumes/planes, push blocks (src/script). Set its bigfile before ticking;
    // the single-mesh constructor has no units: give it them with script().setUnits.
    script::Runtime& script() { return script_; }
    const script::Runtime& script() const { return script_; }
    // Object collision and physics doors (src/physics, docs/formats/PHYSICS.md): the solid primitives of script()'s
    // doors and push blocks are collision().dynamics (following their pose); rigid-body doors swing on a hinge.
    const physics::Scene& physics() const { return physics_; }
    bool sendUberCommand(int32_t uid, uint32_t commandIndex);
    // Enemies of the loaded units (src/game/ai), ticked after Lara; registered in `targets`. Set its bigfile
    // before ticking (as script()).
    ai::Enemies& enemies() { return enemies_; }
    // Save games (src/save): ai::Enemies::restore, then the loaded units' enemies come back from it at once.
    void restoreEnemies(const nlohmann::json& j);
    const ai::Enemies& enemies() const { return enemies_; }
    float fovDeg = 60.0f; // vertical FOV, from config

    // State blended between the last two ticks (alpha in [0,1]).
    PlayerState interpolated(double alpha) const;
    CameraState interpolatedCamera(double alpha) const;
    // Lara's model-space bone matrices (crossfaded clips + simulated hair), blended between the last two
    // ticks; skin with anim::skinPalette and place with Player::modelMatrix(interpolated(alpha)).
    std::vector<glm::mat4> pose(double alpha) const;

    // Streaming (empty units() for the single-mesh constructor).
    const std::vector<LoadedUnit>& units() const { return units_; }
    const LoadedUnit& currentUnit() const { return units_.at(current_); } // the unit Lara is in
    uint64_t unitsVersion() const { return unitsVersion_; } // bumped whenever units() changes
    std::vector<std::string> warnings;                      // streaming problems (missing neighbours, ...)
    // Drops every unit, loads `unit` at offset 0 + neighbours, spawns Lara (unit frame) at `spawn`, else
    // at checkpoint `checkpointId` of that unit, else at its player intro.
    void enterUnit(const std::string& unit, std::optional<Spawn> spawn, int32_t checkpointId = 0);
    // Respawn after death: last checkpoint, else the start unit's spawn (tru.exe restores the checkpoint save).
    void respawn();
    const std::optional<CheckpointRef>& checkpoint() const { return checkpoint_; }
    void setCheckpoint(std::optional<CheckpointRef> c) { checkpoint_ = std::move(c); }
    std::set<int32_t> collected; // intro ids of picked-up items (Pickup::id); saved
    std::array<int, 3> found{};  // pickups collected per PickupKind (PDA counts); saved
    int chapter = 0;             // kChapters index being played; saved
    // Lara's outfit object (objectlist.txt name, pc-w\<outfit>.drm): the level-load unit's (UnitData::playerObject,
    // set by enterUnit), or a save's. Rendering follows it; setOutfitRig re-rigs the pose skeleton. Saved.
    std::string outfit = "lara";
    void setOutfitRig(const cdc::Drm& drm); // rigOutfit on the pose library, then a pose reset
    // Starts chapter `n` (kChapters) at its start unit's player intro; respawns fall back there. Throws as enterUnit.
    void startChapter(int n);
    // Called when a script activates a cinematic_object that has a stream (UnitData::cinematics), before a level
    // end's chapter change (`levelEnd`). ou-game plays the cutscene here (src/app/game/Cinematic.*); unset = skipped.
    std::function<void(const std::string& cinematic, bool levelEnd)> onCinematic;
    const std::vector<Pickup>& pickedUp() const { return pickedUp_; } // last tick (world-frame positions)

    // Save/load helpers. Markup index <-> (unit, index in that unit); -1 / nullopt when not loaded.
    std::optional<std::pair<std::string, int>> markupOwner(int index) const;
    int markupIndex(const std::string& unit, int local) const;
    // Replaces Lara's state (world frame) and the camera orbit; no interpolation across the jump.
    void restore(const PlayerState& s, float camYaw, float camPitch);

    // Interactive water (src/game/Water): ripple field + splash droplets around Lara, ticked with the world.
    const WaterSim& water() const { return water_; }
    bool lightEnabled(std::string_view unit, uint32_t lightId) const;
    uint64_t lightsVersion() const { return lightsVersion_; }
    // One-off splash on the active surface (shots, hazards, scripted impacts).
    void splashWater(vec3 pos, float strength = 1.0f) { water_.splash(pos, strength); }
    // Grapple: best target within range, aim cone and line of sight (World.cpp), for attach + the HUD marker.
    std::optional<GrappleTarget> grappleTarget() const;
    void attachGrapple(vec3 anchor, int32_t uid, GrappleKind kind = GrappleKind::Ceiling) { player_.attachGrapple(anchor, uid, kind); }
    void releaseGrapple(bool jump) { player_.releaseGrapple(jump); }
    // Breath: 1 = full, 0 = drowning (kAirSeconds while the head is under water). [I] 45 s, refill 2 s.
    float air() const { return air_; }
    bool submerged() const { return submerged_; } // head under water this tick

    // ---- combat (Combat.cpp; docs/GAMEPLAY.md "Combat") ----
    std::vector<Damageable*> targets; // lock-on / hitscan targets, non-owning (src/game/ai registers its enemies)
    void damageLara(float amount, vec3 from, int type = kDamageLight); // enemy blows, hazards (tru.exe 0x6F5030)
    void kill();                                                      // message 4: damage = current health
    const Health& health() const { return health_; }
    void setHealth(float v) { health_.set(v); } // saves (after restore(), which heals fully)
    const CombatState& combat() const { return combat_; }
    TargetingTuning targeting;
    // Weapon w's model `hand` (0 right / the two-handed gun, 1 left): Lara bone it rides on (hands while drawn,
    // thigh / back holster otherwise) and its matrix in that bone's frame; nullopt when it is not shown (not
    // carried, a two-handed weapon's hand 1, the uzi out of hand). World matrix = modelMatrix * pose[bone] * offset.
    std::optional<std::pair<int, glm::mat4>> gunMount(Weapon w, int hand) const;
    vec3 muzzle(int hand) const; // current weapon, world, this tick's pose
    // Carries `w` as the secondary (replacing the previous one; Pistols: none), full clip, `reserve` spare rounds
    // capped at its max reserve (ou-game --weapon; the original gets them from pickups).
    void giveWeapon(Weapon w, int reserve);

private:
    void updatePose(float dt, bool reset); // after the player moved; reset = spawn/teleport (no blends, hair at rest)
    void updateCamera(const Actions& a, float dt);
    LoadedUnit* findUnit(const std::string& name);
    void streamAround(size_t center); // load portal neighbours of units_[center], unload the rest
    void rebuildCollision();
    void crossPortals(vec3 from, vec3 to);
    void syncObjects();          // script objects -> collision dynamics + physics bodies (after units change)
    void tickObjects(float dt);  // physics step, dynamics follow their objects, Lara rides what she stands on
    void touchCheckpoints();
    void tickCombat(const Actions& a, float dt);
    void aimLayer(std::vector<anim::LocalTransform>& local, float dt); // weapon upper body over `local`
    std::vector<Damageable*> candidates(bool hard) const; // valid targets in selection order
    void fire(int hand);
    void reloadWeapon(); // refill the current weapon's clips from its reserve
    void collectPickups(bool pressed);
    void tickAir(float dt); // oxygen / drowning (World.cpp)
    static constexpr float kAirSeconds = 45.0f;   // [I] no tru.exe number decoded yet
    static constexpr float kAirRefillSeconds = 2.0f; // [I]
    void checkLevelEnd(); // script Activate of a cinematic_object -> onCinematic; a level end's -> startChapter(next)
    void registerCinematicUnits();
    struct CinematicEnd { std::string stream; std::vector<std::string> chapters; };
    std::vector<script::UnitRef> cinematicRefs_;
    std::map<int32_t, CinematicEnd> cinematicEnds_;

    collision::CollisionMesh col_;
    AnimLibrary anims_;
    Player player_;
    PlayerState prev_;
    anim::AnimController anim_{anims_.skeleton};
    anim::HairSim hair_;
    std::vector<glm::mat4> bones_, prevBones_; // model space, this tick and the previous one
    CameraState cam_, prevCam_;
    uint64_t ticks_ = 0;
    UnitLoader loader_;
    std::vector<LoadedUnit> units_;
    size_t current_ = 0;
    uint64_t unitsVersion_ = 0;
    std::string startUnit_;
    std::optional<Spawn> startSpawn_;
    std::optional<CheckpointRef> checkpoint_;
    script::Runtime script_;
    ai::Enemies enemies_{&targets};
    uint64_t scriptUnits_ = ~0ull; // unitsVersion_ the script runtime was synced to
    std::vector<script::UnitRef> unitRefs() const; // loaded units + cinematic objects, for script_/enemies_ setUnits
    size_t syncedObjects_ = ~size_t(0); // script object count syncObjects ran for
    void bindMultibody();
    void bindUber();
    void bindTerrain();
    void bindLightHealth();
    const formats::UberObjectData* prepareUber(const script::Object& o);
    std::map<int32_t, float> multibodyLo_, multibodyHi_; // native limits issued before hinges exist
    std::map<std::string, std::shared_ptr<const cdc::Drm>> objectDrms_;
    std::map<int32_t, std::optional<formats::UberObjectData>> uberData_;
    std::map<int32_t, std::vector<uint32_t>> uberStates_;
    struct UberMotion { vec3 delta{0}, rotation{0}; float duration = 1, t = 0; bool active = false; };
    std::map<int32_t, UberMotion> uberMotion_;
    std::map<std::pair<std::string, uint32_t>, bool> lightStates_;
    uint64_t lightsVersion_ = 0;
    // One per collision().dynamics entry: owner, model segment and its bind frame, hinge (-1 = posed by script()).
    struct ObjectCollider { int32_t uid; int segment; int hinge; glm::mat4 intro, frame; };
    std::vector<ObjectCollider> colliders_;
    std::map<std::string, std::shared_ptr<const physics::ObjectPhysics>> objectPhysics_; // by object name; null = none
    physics::Scene physics_;
    Health health_;
    WaterSim water_;
    float air_ = 1.0f;
    bool submerged_ = false;
    CombatState combat_ = fullClips(anims_.weapons);
    double aimTime_ = 0; // pose clips' clock
    bool interactHeld_ = true;     // no press edge on the first tick
    bool grappleHeld_ = true;      // no press edge on the first tick
    std::vector<Pickup> pickedUp_;
};

// The unit's `player` intro (objectlist.txt name), else the first intro; nullopt if the unit has none.
std::optional<Spawn> findSpawn(const cdc::BigfileSet& bigfile, const cdc::Drm& unit);
// Water volumes of a unit as type-10 ("water", MUD flag 0x100000) markup boxes, as tru.exe builds them
// for each water volume instance (0x5D7130): points = {min, max}, surface = max.z. One per `watervolume`
// intro whose intro data (+0x12) names its water-surface intro; x/y and the surface come from that
// surface's model bounds (docs/GAMEPLAY.md "Water"). Missing objects are skipped.
std::vector<collision::Markup> findWater(const cdc::BigfileSet& bigfile, const cdc::Drm& unit);

// ---- input ------------------------------------------------------------------------------------
// Keyboard (SDL_GetKeyboardState) + mouse buttons (SDL_GetMouseState mask) + optional gamepad -> actions, through
// the user's bindings (config::InputConfig; defaults: WASD/arrows move, Shift walk, Alt sprint, Space jump, Ctrl/C
// drop, E interact, F draw/holster, Q switch target, X switch weapon, H health pack, G grapple, F5/F9 save/load,
// mouse left fire, right aim/lock; pad South jump, East drop, West interact, North draw, left shoulder walk, right
// shoulder grapple, triggers fire/aim, right stick click switch, d-pad right weapon, d-pad up health pack — locals.bin
// 1298 / 1291 glyphs U+180A / U+1808 [I button order]). Sticks are fixed: left moves, right looks.
// Mouse look is added by the app: it accumulates relative motion per frame and feeds it to the next tick as a rate
// (see ou-game), scaled by mouseScale and flipped by invertY.
struct Controls {
    std::vector<std::vector<platform::InputRef>> actions; // per config::bindableActions() entry
    float mouseScale = 1.0f;
    bool invertY = false;
    Controls() : Controls(config::InputConfig{}) {}
    explicit Controls(const config::InputConfig& c); // throws std::runtime_error naming an unknown input
};
Actions readDevices(const Controls& controls, const bool* keys, SDL_Gamepad* pad, uint32_t mouseButtons = 0);

// Scripted input for deterministic replays. Text, one event per line: `<seconds> <action> [value]`.
// Actions: forward, right (stick axes -1..1), walk, jump, drop, interact, save, load, draw, fire, aim, switch,
// weapon, heal, grapple, sprint (0/1), lookx, looky (rad/s).
// A value persists until changed; times map to tick round(seconds * 60). '#' starts a comment.
class InputScript {
public:
    static InputScript parse(std::string_view text); // throws std::runtime_error with the line number
    static InputScript load(const std::filesystem::path& path);
    Actions at(uint64_t tick) const; // state after applying every event with tick <= `tick`

private:
    struct Event { uint64_t tick; std::string action; float value; };
    std::vector<Event> events_; // sorted by tick (stable)
};

} // namespace ou::game
