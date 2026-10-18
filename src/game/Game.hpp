// Gameplay: world state, Lara's controller, follow camera and input mapping. No renderer types here;
// apps turn World state into draw calls. Evidence, numbers and deviations: docs/GAMEPLAY.md.
// Frame: Z-up game units in the render-terrain frame (as ou_collision and the level intros).
#pragma once

#include "anim/Anim.hpp"
#include "collision/Collision.hpp"
#include "formats/Formats.hpp"
#include "script/Script.hpp"

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
    bool drop = false; // held; let go of a ledge
    bool interact = false; // held; switches/levers react to the press edge, push blocks while held (src/script)
    bool save = false, load = false; // held; ou-game quicksaves / quickloads on the press edge (F5 / F9)
};

// ---- animations -------------------------------------------------------------------------------
// Lara's clips the controller uses (the full mapping table is in docs/GAMEPLAY.md).
struct AnimLibrary {
    scene::Skeleton skeleton;
    std::map<std::string, anim::Clip, std::less<>> clips; // by animation-list name
    // Hands (bones 55 and 80) relative to the root in the ledgeb_idle pose, model space; places the
    // root under a grabbed ledge.
    vec3 hangHandOffset{6.3f, -141.7f, 495.6f};
    // Swim speed, units/s: mean root speed of the two one-shot clips that start swimming,
    // swim_surfaceidle_to_swim (206 units / 0.367 s) and swim_surfacedive (676 units / 1.2 s). The swim
    // loops have no root motion.
    float swimSpeed = 562.0f;
    anim::HairRig hair; // Lara's hair particles + constraints (anim::decodeHair); empty = rigid hair

    const anim::Clip* find(std::string_view name) const;
    // Mean of the hand bones (55, 80) relative to the root segment in `clip` at `t` (model space, root
    // translation removed); nullopt without a full skeleton.
    std::optional<vec3> handsAt(const anim::Clip& clip, double t) const;
};
// Names the controller requests; loadLaraAnims loads exactly these.
extern const std::vector<std::string_view> kLaraClipNames;
// How the original fades `to` in after `from`: crossfade length and start time (seconds) from lara.drm's
// anim requests. Player::play starts clips at startSeconds; World crossfades over blendSeconds.
struct ClipBlend { float blendSeconds; double startSeconds; };
ClipBlend clipBlend(std::string_view from, std::string_view to);
// Skeleton + kLaraClipNames clips from pc-w\lara.drm. Throws cdc::FormatError / std::runtime_error.
AnimLibrary loadLaraAnims(const cdc::BigfileSet& bigfile);

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
};
Tuning defaultTuning();

struct PlayerState {
    State state = State::Idle;
    vec3 pos{0};    // root = feet; swimming: the swim clips' root (water surface while treading, body centre under water)
    float yaw = 0;  // heading: forward = (cos yaw, sin yaw, 0)
    vec3 vel{0};    // air velocity, units/s
    std::string clip;
    double clipTime = 0; // unbounded for looping clips (anim time = fmod(clipTime, duration))
    float clipRate = 1;
    bool loop = true;
    // ledge (Hang / Shimmy / ClimbUp): markup polyline, segment, and position along it
    int ledge = -1, ledgeSeg = 0;
    float ledgeT = 0;     // distance from the segment start
    float ledgeSide = 1;  // which side of the polyline Lara hangs on
    bool runJump = false; // JumpPrep: take-off from a run
    float regrabCooldown = 0;
    int water = -1;  // swimming: index of the water markup (type 10) in collision().markup
    float pitch = 0; // under water: swim direction pitch, rad (+ = up)
};

class Player {
public:
    Player(const collision::CollisionMesh& col, const AnimLibrary& anims, Tuning tuning = defaultTuning());
    void spawn(vec3 feet, float yaw);
    // `move`: world-space wish direction (z ignored), length <= 1.
    void tick(vec2 move, const Actions& a, float dt);
    const PlayerState& state() const { return s_; }
    const Tuning& tuning() const { return t_; }
    void restore(const PlayerState& s) { s_ = s; jumpHeld_ = dropHeld_ = true; } // load/stream; held: no edge on the next tick
    // World-space model matrix (root at pos; model -Y = forward).
    glm::mat4 modelMatrix(vec3 pos, float yaw) const;

private:
    void play(std::string_view clip, bool loop, float rate = 1);
    bool clipDone() const;
    vec3 advanceClip(float dt); // returns the model-space root delta
    vec3 forward() const;
    vec3 toWorld(vec3 model) const; // model-space vector -> world, by yaw
    void groundMove(vec3 delta, float dt);
    void airMove(float dt, vec2 move);
    vec3 slide(vec3 pos, vec3 delta, float bottom, bool horizontalNormals) const;
    bool tryGrab(vec3 prevPos);
    void placeOnLedge();
    // water: volume overlapping the water probe box at `feet` (index into col_.markup), its surface z
    int waterAt(vec3 feet) const;
    float surfaceZ() const;
    bool enterWater(); // from the ground or the air; false if not in deep enough water
    void swim(vec2 move, const Actions& a, bool jumpPressed, bool dropPressed, float dt);
    vec3 swimSlide(vec3 pos, vec3 delta) const;
    bool tryClimbOut();
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
    std::optional<Spawn> spawn; // findSpawn
    std::shared_ptr<const cdc::Drm> drm;
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
    std::set<int32_t> collected; // intro ids of picked-up items; saved, nothing collects yet

    // Save/load helpers. Markup index <-> (unit, index in that unit); -1 / nullopt when not loaded.
    std::optional<std::pair<std::string, int>> markupOwner(int index) const;
    int markupIndex(const std::string& unit, int local) const;
    // Replaces Lara's state (world frame) and the camera orbit; no interpolation across the jump.
    void restore(const PlayerState& s, float camYaw, float camPitch);

private:
    void updatePose(float dt, bool reset); // after the player moved; reset = spawn/teleport (no blends, hair at rest)
    void updateCamera(const Actions& a, float dt);
    LoadedUnit* findUnit(const std::string& name);
    void streamAround(size_t center); // load portal neighbours of units_[center], unload the rest
    void rebuildCollision();
    void crossPortals(vec3 from, vec3 to);
    void touchCheckpoints();

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
    uint64_t scriptUnits_ = ~0ull; // unitsVersion_ the script runtime was synced to
};

// The unit's `player` intro (objectlist.txt name), else the first intro; nullopt if the unit has none.
std::optional<Spawn> findSpawn(const cdc::BigfileSet& bigfile, const cdc::Drm& unit);
// Water volumes of a unit as type-10 ("water", MUD flag 0x100000) markup boxes, as tru.exe builds them
// for each water volume instance (0x5D7130): points = {min, max}, surface = max.z. One per `watervolume`
// intro whose intro data (+0x12) names its water-surface intro; x/y and the surface come from that
// surface's model bounds (docs/GAMEPLAY.md "Water"). Missing objects are skipped.
std::vector<collision::Markup> findWater(const cdc::BigfileSet& bigfile, const cdc::Drm& unit);

// ---- input ------------------------------------------------------------------------------------
// Keyboard (SDL_GetKeyboardState) + optional gamepad -> actions. Mouse look is added by the app: it
// accumulates relative motion per frame and feeds it to the next tick as a rate (see ou-game).
// Keys: WASD/arrows move, Shift walk, Space jump, Ctrl/C drop, E interact, F5/F9 save/load. Pad: left stick move, right stick
// look, South jump, East drop, West interact, left shoulder walk.
Actions readDevices(const bool* keys, SDL_Gamepad* pad);

// Scripted input for deterministic replays. Text, one event per line: `<seconds> <action> [value]`.
// Actions: forward, right (stick axes -1..1), walk, jump, drop, interact, save, load (0/1), lookx, looky (rad/s).
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
