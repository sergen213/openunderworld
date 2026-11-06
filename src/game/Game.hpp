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
#include "game/Bend.hpp"
#include "game/Combat.hpp"
#include "game/Progress.hpp"
#include "game/Water.hpp"
#include "game/Feedback.hpp"
#include "game/Vehicle.hpp"
#include "script/Script.hpp"
#include "game/ai/Enemy.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ou::cdc { class BigfileSet; }
namespace ou::locale { class Strings; }

namespace ou::game {

using glm::vec2;
using glm::vec3;
using glm::vec4;

inline constexpr double kTickSeconds = 1.0 / 60.0; // fixed simulation step (engine::FrameClock default)

// One tick of player intent, already mapped from devices or an input script.
struct Actions {
    vec2 move{0};      // camera-relative stick: x = right, y = forward, length <= 1
    vec2 look{0};      // camera orbit rate, rad/s: x = yaw (positive turns the camera left), y = pitch
    bool walk = false; // walk modifier (Left Alt; a light stick walks too)
    bool jump = false; // held; the controller reacts to the press edge
    bool drop = false; // held; let go of a ledge / pole; on the ground: crouch (held), roll (running), drop to hang
    bool interact = false; // held; switches/levers react to the press edge, push blocks while held (src/script)
    bool save = false, load = false; // held; ou-game quicksaves / quickloads on the press edge (F5 / F9)
    // combat (Combat.cpp): draw toggles the weapon on the press edge; fire / aim held draw first; aim held = hard
    // lock-on (target lock); switchTarget press edge cycles the lock; weapon press edge swaps pistols <-> the
    // secondary; heal press edge uses a health pack. Adrenaline: interact while the meter is full and close to a hard lock.
    bool draw = false, fire = false, aim = false, switchTarget = false, weapon = false, heal = false;
    bool pause = false; // held; ou-game opens the pause menu on the press edge (Esc / pad Start; scripts: "pause")
    bool grapple = false; // held; the grapple reacts to the press edge (Q / pad X; scripts: "grapple")
    bool sprint = false;  // held (Left Shift / pad left shoulder; scripts: "sprint")
    bool pda = false;  // held; ou-game opens the PDA on the press edge (Tab / pad Back; scripts: "pda")
    bool hint = false, answer = false; // held; ou-game plays the help-on-demand hint / answer line on the press edge (F2 / F3)
    bool light = false;   // held; the flashlight toggles on the press edge (F / pad d-pad left; scripts: "light")
    bool grenade = false; // held; a sticky grenade is thrown on the press edge (Caps Lock / pad RB; scripts: "grenade")
    bool recenter = false;    // held; the orbit camera turns behind Lara on the press edge (Q / pad LS click; "recenter")
    bool fieldCamera = false; // held; the field camera view toggles on the press edge (T / pad d-pad down; "camera")
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
    vec3 poleParaHandOffset{-0.8f, -16.0f, 609.6f}; // hanging along the pole (hpole_IdleHangPara at t = 0)
    anim::HairRig hair; // Lara's hair particles + constraints (anim::decodeHair); empty = rigid hair
    // Weapons (index = Weapon): stats (loadWeapon) and Lara's clips from each weapon's DRM (weaponClipNames), kept in
    // `clips` as "<object>:<clip>" (the DRMs reuse names). Defaults keep synthetic libraries usable without them.
    std::array<WeaponDef, kWeaponCount> weapons{weaponDefaults(Weapon::Pistols), weaponDefaults(Weapon::Uzi),
                                                weaponDefaults(Weapon::Shotgun), weaponDefaults(Weapon::Speargun),
                                                weaponDefaults(Weapon::ThorHammer), weaponDefaults(Weapon::AssaultRifle),
                                                weaponDefaults(Weapon::Tranquilizer)};
    BikeTuning bike; // motorcycle.drm (loadBikeTuning); its riding clips are in `clips` as "motorcycle:<name>"
    GrenadeTuning grenade; // g_grenade_thrower + pr_grenade (loadGrenadeTuning); throw clip "g_grenade_thrower:combat_grenade_Throw1"
    GrappleTuning grapple; // g_grapple data (loadGrappleTuning): throw range, rope limits, bend pool
    AdrenalineTuning adrenaline; // Lara data +0x2C8..+0x31C (loadAdrenalineTuning): meter, drains, time scales
    DifficultyTable difficulty; // generalbank's (loadDifficulty): PLAYER TAILORING health / saving grab levels
    // Lara's HInfo markers (lara.drm model): id -> (segment, frame in it, R = Rx * Ry * Rz); weapons mount there.
    std::map<int, std::pair<int, glm::mat4>> markers;
    // Her death ragdoll (the outfit's ObjectBase +0x7C, physics::loadRagdoll: 15 bones, 55 kg) and each segment's bind
    // position (model space).
    std::optional<physics::RagdollDef> ragdoll;
    std::vector<vec3> bind;

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
// Lara's hair simulation parameters (per 60 Hz tick; World's HairSim, cutscenes).
anim::HairParams laraHair();

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
    Vehicle,     // on the motorcycle: mounting, riding, dismounting; World drives her from the bike (Vehicle.cpp)
    GrappleRappel, // feet on a wall on the rope: rappel up/down, run along it (Grapple.cpp); climbNormal = the wall's
    // Interactions (World, docs/GAMEPLAY.md "Interactions"): Use = one in-place clip with the input locked (a switch's
    // Lara clip, a pickup, letting go of a block), then Idle; Block = gripping a push block (obj_block_* clips; the
    // block's motion carries her).
    Use, Block,
    WallHug, // back to the wall on a narrow ledge shelf (Wall_Hug_* clips; src/game/Traverse.cpp)
};
std::string_view stateName(State s); // "unknown" past the last state

// Controller constants. Provenance ([O] observed / [I] inferred) is listed in docs/GAMEPLAY.md.
struct Tuning {
    // Lara's height: lara.drm player data +0x10. Collision capsule as tru.exe's standing Havok proxy (Wine, instance
    // +0x288: radius +0x65C 79.01, cylinder +0x660 189.63, centre 315.4 above the feet), so its top is 489.2; on the
    // ground our capsule's bottom is stepUp (the proxy's own bottom, 141.6, is above the 121-unit hearth it stops at).
    float radius = 79.01f, height = 512.0f, capsuleTop = 489.2f;
    float wallOffset = 100.0f; // root distance from a wall climbed / hung on (climb, ladders, grapple walls) [I]
    // Ground snap window. tru.exe (Wine, l3_main_bedroom): Lara runs up the 97..100-unit stair risers with the
    // root snapping to each step top; the 121-unit hearth edge blocks her. 110 lies between (GAMEPLAY.md Numbers).
    float stepUp = 110.0f, stepDown = 110.0f;
    float minFloorNormalZ = 0.7f;              // steeper = wall (~45.6 deg)
    float gravity = 5.72443f * 900.0f;         // units/s^2 (tru.exe: 5.72443 units per 30 Hz frame^2)
    // Jump take-offs measured in tru.exe under Wine (Instance +0x2A0 velocity, units per 30 Hz frame x 30):
    // standing 48.04 up; standing with the stick 49.97 up + 40.00 along it; running (or the stick behind her:
    // she turns round) 42.53 up + 65.83 forward. Delays from the press: 359 / 290 / 109 ms (GAMEPLAY.md Numbers).
    float jumpVz = 1441.2f, standMoveJumpVz = 1499.1f, standJumpSpeed = 1200.0f; // units/s
    float runJumpVz = 1275.9f, runJumpSpeed = 1974.9f;                          // units/s
    float standMoveTakeoff = 0.26f; // s into ng_standing_jump a stick jump leaves the ground (0.33 s clip: 359 vs 290 ms)
    float turnRate = 6.0f;                     // rad/s
    float grabReach = 60.0f, grabWindow = 40.0f; // hand-to-ledge horizontal / vertical tolerance
    // Grab only while -13 <= zVel <= 9 units/frame * 5.72443 (30 Hz): player data bytes +0x834 / +0x833 (0x759260)
    float grabMinVz = -13 * 5.72443f * 30.0f, grabMaxVz = 9 * 5.72443f * 30.0f; // units/s
    // Saving grab ("TIME FOR SAVING GRABS"): a ledge caught while falling up to twice as fast as grabMinVz [I] is held one
    // handed (ledgeb_bad_attach_M) for this long; jump pulls her up (ledgeb_bad_attach_recover), else she falls.
    // DifficultyTable::savingGrabSeconds by Tailoring::savingGrab (World::setTailoring).
    float savingGrabSeconds = 50 / 30.0f;
    // Water test box (0x710A20): half extents player data shorts +0xC3E (x, y) / +0xC40 (z), centred
    // +0xC42 above the feet; overlapping a water volume puts Lara in it.
    vec3 waterProbeHalf{221.0f, 221.0f, 572.0f};
    float waterProbeCenterZ = 320.0f;
    // Swimming, surface and under water (tru.exe, Wine, l2_cliffclimb sea): 44.95 units per 30 Hz frame at full
    // stick; from rest it ramps up in ~0.7 s, after release it slows ~twice as fast (fitted slopes, units/s^2).
    float swimSpeed = 1348.6f, swimAccel = 2475.0f, swimDecel = 3760.0f;
    // Sprint (Shift held) swims 1.2x as fast, surface and under water: 53.94 vs 44.95 units per 30 Hz frame (tru.exe,
    // Wine, l2_cliffclimb sea; player +0x2A0 velocity), on the run sprint's stamina (sprintSeconds, sprintResume).
    float swimSprintFactor = 1.2f;
    // Water at least this deep (surface - floor) is swum, shallower walked: lara.drm player data +0xAE / +0xB0 = 391, the
    // classes of tru.exe 0x5D3860 (< +0xAC 154: 0x200, < 391: 0x100 wading, then 0x40 surface / 0x20 / 0x10 swimming).
    float swimDepth = 391.0f;
    float wadeDepth = 154.0f;   // +0xAC: water this deep up to swimDepth is waded (class 0x100), shallower walked (0x200)
    float swimRadius = 100.0f;  // sphere around the swimming root
    // Root under the wave surface treading / swimming at the surface (tru.exe, Wine, l2_cliffclimb sea: z fits
    // player+0x8E0 top + the 0x69DA80 waves - 44.1 at rms < 1 in both states).
    float swimSurfaceDepth = 44.1f;
    float crouchHeight = 260.0f; // capsule while crouched/rolling: player data +0x14
    float slideMinNormalZ = 0.3f; // floors between this and minFloorNormalZ are slid down
    float beamAttach = 30.0f;   // root within this of a balance beam (x/y, and z) gets on it
    float poleReach = 60.0f;    // hands to pole axis, horizontal
    // Pole hands (pole pose) may be this far above the axis when caught: Hpole_perp_attach_M starts with the hands
    // 138 below the pole pose (481 vs 619 above the root), so the root sinks 138 onto the hang.
    float poleGrabAbove = 138.0f;
    float assistRadius = 300.0f; // jump-landing / beam markup this close to the natural landing pulls the jump onto it
    float hangJumpApex = 60.0f;  // hang jumps above their take-off arc rise this far above the higher end
    // Reach of targeted jumps from a hang (hands to target hands): up, sideways, back (away from the wall).
    float hangJumpUp = 700.0f, hangJumpSide = 800.0f, hangJumpBack = 1200.0f, poleFlingReach = 1800.0f;
    // Take-off vertical speed of targeted hang jumps: lara.drm queries q10 (up) / q7, q8 (sides) / q9 (back), units
    // per 30 Hz frame x 30 (docs/GAMEPLAY.md "Hang jumps").
    float hangJumpUpVz = 75.85f * 30, hangJumpSideVz = 34.77f * 30, hangJumpBackVz = 42.54f * 30;
    // Free leaps off a hang (no target), launched when the take-off clip ends (tru.exe under Wine, l3_main_bedroom
    // ledge 973): back 53.73 out + 34.77 up, sideways 70.80 + 34.77 (units per 30 Hz frame x 30).
    float hangLeapBack = 53.73f * 30, hangLeapSide = 70.80f * 30, hangLeapVz = 34.77f * 30;
    // Sprint (Shift / LB held while running; tru.exe under Wine, l4_trailstart): sprint_loop for 9.5..9.9 s, then
    // ng_sprint_to_exhaust + ng_run_exhaust (no refill while they play); a sprint resumes once 0.9 s is refilled
    // (Shift held throughout: ~2.2 s running, 0.9 s sprinting). Refill = 1 s per second: 4 s off -> 4.1 s sprint.
    float sprintSeconds = 9.7f, sprintResume = 0.9f;
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
    // Blend space: `clip` is the first clip of a multi-clip lara.drm anim request (hurdles: obstacle height / depth);
    // `clipMix` is the other, weighted `mix`, phase-synced; root motion is blended the same way. Empty = single clip.
    std::string clipMix;
    float mix = 0;
    // `clip` reflected left / right (lara.drm anim request +0x20 bit 0x80, e.g. request 979 = ledgeb_jump_right mirrored:
    // the jump left; 1009 is the unmirrored one). The pose and the root motion's X are mirrored (tru.exe 0x4428A0 /
    // 0x441FB0).
    bool mirror = false;
    // Obstacle move in progress (State::ClimbUp hurdle_* clips, Player::tryHurdle / tryHurdleDown): what follows the
    // attach clip, its blend weight and pair (over: the long clips; onto the far side: 65 / 100 cm deep), the
    // obstacle's top edge on her side and outward normal (toward where she came from), its height above her start,
    // the floor z she ends on, and for onto-the-far-side the ledge she ends hanging from.
    struct Hurdle {
        enum Next : uint8_t { Onto, Over, ToLedge, Down } next = Onto;
        float mix = 0;
        bool alt = false;
        bool draw = false; // low (no attach clip): drawn onto the ledge point + 86.94 over the clip (anim target 0x273)
        vec2 drawOff{0};   // that correction, spread over the clip
        vec3 edge{0};
        vec2 out{0};
        float h = 0, endZ = 0;
        int farLedge = -1, farSeg = 0;
        float farT = 0, farSide = 1;
    } hurdle;
    // attached markup (index into collision().markup): ledge while hanging, balance beam, pole; while a
    // HangJump flies, the target. Segment, position along it, and side: ledges/poles = which side of the
    // polyline Lara hangs on, beams = walking direction (+1 along the points).
    int ledge = -1, ledgeSeg = 0;
    float ledgeT = 0;     // distance from the segment start
    float ledgeSide = 1;
    bool polePara = false; // PoleHang: hanging along the pole (facing polyline direction * ledgeSide), else across it
    bool runJump = false; // JumpPrep: take-off from a run
    float regrabCooldown = 0;
    float savingGrab = 0; // Hang: seconds left of a saving grab (one hand); 0 = a firm hang
    int water = -1;  // swimming: index of the water markup (type 10) in collision().markup
    float pitch = 0; // under water: swim direction pitch, rad (+ = up); the model is pitched by it
    float roll = 0;  // on the motorcycle: its roll, rad (+ = left side down); the model is rolled by it
    float flight = 0; // HangJump: seconds left until the target is reached
    float fallStartZ = 0; // z when Lara last became airborne (tru.exe player +0x6DC, set on FallState entry 0x75C550)
    float fallHeight = 0; // set by land(): fallStartZ - pos.z; 0 on every other tick
    // Free climbing (State::Climb): the hold she is at (CollisionMesh::markup index, -1 = not on a wall), its hand stance
    // (0..4 = DICE B / L / LW / R / RW, markup type - 16) and the rock's outward normal there. climbWarp: an attach /
    // hold-to-hold / step-off clip is carrying her from climbFrom (heading climbYawFrom) to climbTo (climbYawTo); the
    // ladder climb-off clips use climbFrom / climbTo too.
    int32_t climbHold = -1;
    uint8_t climbStance = 0;
    bool climbWarp = false;
    vec3 climbNormal{0};
    vec3 climbFrom{0}, climbTo{0};
    float climbYawFrom = 0, climbYawTo = 0;
    // Grapple (State::GrappleThrow/Swing/Pull/Rappel): anchor intro unique id, world anchor, rope length at hand (root + 400)
    // along the rope: anchor -> bends -> hand. The rope wraps round geometry as the original's (Grapple.cpp
    // wrapRope): ropeBend[0..ropeBends) are its bends from the anchor side (the last one is the pivot she swings on),
    // ropeBendClear[i] the ticks bend i's straight-through path has been clear (it slides off 15 more per tick).
    int32_t grappleUid = 0;
    vec3 grappleAnchor{0};
    float ropeLength = 0;
    GrappleKind grappleKind = GrappleKind::Ceiling;
    uint8_t ropeBends = 0;
    std::array<vec3, kRopeBends> ropeBend{};
    std::array<uint8_t, kRopeBends> ropeBendClear{};
    float sprintUsed = 0; // sprint stamina spent, seconds (Tuning::sprintSeconds)
};

// The rope's pivot (its last bend, else the anchor) and its length from the anchor to the pivot along the bends.
vec3 ropePivot(const PlayerState& s);
float ropeBentLength(const PlayerState& s);

class Player {
public:
    Player(const collision::CollisionMesh& col, const AnimLibrary& anims, Tuning tuning = defaultTuning());
    void spawn(vec3 feet, float yaw);
    // `move`: world-space wish direction (z ignored), length <= 1.
    void tick(vec2 move, const Actions& a, float dt);
    const PlayerState& state() const { return s_; }
    const Tuning& tuning() const { return t_; }
    void carry(vec3 d) { s_.pos += d; } // moved by what she stands on (platforms, physics doors)
    void setSavingGrabSeconds(float s) { t_.savingGrabSeconds = s; }
    void setWaveSeconds(double s) { waveSeconds_ = s; } // the sea waves' clock (waterSurface), set by World each tick
    void restore(const PlayerState& s) { s_ = s; jumpHeld_ = dropHeld_ = true; } // load/stream; held: no edge on the next tick
    // World-space model matrix (root at pos; model -Y = forward; pitch tilts the head up about the root; roll lowers
    // the model's left (+X) side about its forward axis).
    glm::mat4 modelMatrix(vec3 pos, float yaw, float pitch = 0, float roll = 0) const;
    void face(float yaw) { s_.yaw = yaw; } // combat: turn toward a locked target
    void attachGrapple(vec3 anchor, int32_t uid, GrappleKind kind = GrappleKind::Ceiling); // Grapple.cpp: target attachment
    void releaseGrapple(bool jump);               // let go, keeping the swing's momentum
    void die(std::string_view clip);       // State::Dead playing `clip` once
    void hitReact(std::string_view clip, bool mirror = false); // State::Hit playing `clip` once (restarts it)
    // Interactions (World): where they can start: on her feet (standing, walking, running, landing) or crouched, out of
    // the water. use() plays `clip` once in State::Use at `pos` facing `yaw` with the input locked, then Idle; false
    // (nothing changes) when she cannot interact or without the clip.
    bool canInteract() const;
    bool use(std::string_view clip, vec3 pos, float yaw);
    // Push block grip, every tick: `axis` = the gripped block's push axis (0 = none), `moving` +1 push / -1 pull / 0.
    // Grips (State::Block, obj_block_grab, facing the axis) from the states use() starts in; lets go (obj_block_release).
    void grip(vec3 axis, int moving);
    bool gripping() const { return s_.state == State::Block && s_.clip != "obj_block_grab"; } // grab clip done
    // Water: the volume overlapping the water probe box at `feet` (index into the collision markup), else -1.
    int waterAt(vec3 feet) const;

private:
    void play(std::string_view clip, bool loop, float rate = 1, bool mirror = false);
    void playMix(std::string_view clip, std::string_view other, float mix); // blend-space request, played once
    vec3 rootLeft() const; // model-space root motion from now to the clip end (blend space included)
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
    bool tryPoleLeap(); // a jump with a pole in reach: the straight, gravity-free leap onto its hang (HangJump)
    bool tryBeam(std::string_view clip, bool anyHeading);
    bool tryDropToHang();
    bool canClimbUp() const;
    bool tryHurdle(bool jumped); // onto / over a ledge from the ground (State::ClimbUp, hurdle_* clips)
    void startHurdleFollowUp(bool low); // the onto / over / far-side hang clip after the attach (or S958)
    bool tryHurdleDown();                      // down off an obstacle's ledge (hurdle_down_*)
    bool hurdleTick(vec2 move, float dt);      // State::ClimbUp hurdle_* clip; true when it ended on the ground
    bool startHangJump(int kind); // 0 up, 1 right, -1 left, 2 back (ledge)
    void launchHangJump();        // HangJump velocity / flight time from here onto the target (after the take-off clip)
    void assistJump();
    void placeOnLedge(); // ledge (type 1) or pole (4, 6) attachment from ledge/ledgeSeg/ledgeT/ledgeSide
    // root, yaw; poles: `para` = the parallel hang
    std::pair<vec3, float> attachPose(int markup, int seg, float t, float side, bool para = false) const;
    void pole(vec2 move, bool jumpPressed, bool dropPressed, float dt); // State::PoleHang
    void poleFling(float dirSign = 1); // the leap off a pole once the fling clip has played; -1: backwards (pole-top jump back)
    void poleTop(vec2 move, bool jumpPressed, bool dropPressed, float dt); // State::Perch on a pole (markup 4 / 6)
    void beam(vec2 move, bool jumpPressed, bool dropPressed, float dt);
    float surfaceZ() const;
    bool enterWater(); // from the ground or the air; false if not in deep enough water
    bool wading() const; // on her feet in water of class 0x100 (wadeDepth..swimDepth deep, tru.exe 0x5D3860)
    void swim(vec2 move, const Actions& a, bool jumpPressed, bool dropPressed, float dt);
    vec3 swimSlide(vec3 pos, vec3 delta) const;
    bool tryClimbOut();
    // Free climbing (src/game/Climb.cpp): attach from the ground (jump) or the air, then climb().
    bool tryClimb(bool fromAir);
    bool warpStep(float dt); // the clip carries her climbFrom -> climbTo (climbing, ladder climb-off); true when done
    void climb(vec2 stick, bool jumpPressed, bool dropPressed, float dt);
    // Ladders (markup 9), perches (13), pillars (15), src/game/Traverse.cpp. climbNormal = outward from the wall.
    bool tryLadder(bool fromAir);
    void ladder(vec2 move, bool jumpPressed, bool dropPressed, float dt);
    void pillar(vec2 move, vec2 stick, bool jumpPressed, bool dropPressed, float dt); // stick: camera-relative
    bool tryPerch(bool fromAir);
    void perch(vec2 move, bool jumpPressed, bool dropPressed, float dt);
    bool shelfAt(int markup, int seg, float t) const; // a ledge point whose top is a wall-hug shelf (tru.exe 0x74BB50 == 1)
    bool tryWallHug(vec2 move);                       // walking onto such a shelf along it
    void wallHug(vec2 move, bool jumpPressed, float dt);
    bool tryRail(bool fromAir);
    void rail(bool jumpPressed, bool dropPressed, float dt);
    void grapple(vec2 move, const Actions& a, bool jumpPressed, bool dropPressed, float dt); // Grapple.cpp
    void rappel(vec2 move, bool jumpPressed, bool dropPressed, float dt);
    void rappelOnto(const collision::Hit& wall, vec3 vel, std::string_view clip);
    void standUp(); // let go of the rope, standing
    void wrapRope(vec3 oldHand, vec3 hand); // bends the rope round geometry / straightens it (Grapple.cpp)
    void land(vec2 move);

    const collision::CollisionMesh& col_;
    const AnimLibrary& anims_;
    Tuning t_;
    PlayerState s_;
    bool jumpHeld_ = false, dropHeld_ = false;
    double waveSeconds_ = 0;
};

// ---- camera -----------------------------------------------------------------------------------
struct CameraState {
    vec3 position{0}, target{0};
    float yaw = 0, pitch = -0.0225f; // orbit angles; camera looks along yaw/pitch (tru.exe's rest pitch)
    vec3 up{0, 0, 1};              // spline cameras roll
    float fovDeg = 0;              // vertical; 0 = World::fovDeg (spline cameras use their own)
};
struct SplineCamera; // a unit camera spline playing (Camera.cpp)

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
    std::vector<formats::CameraSpline> cameraSplines; // formats::Unit::cameraSplines (reveal cameras)
    std::shared_ptr<const cdc::Drm> drm;
    // Progression (src/game/Progress.hpp): reward intros; cinematic intros whose script names chapter start
    // units (level ends); the unit's own track "<name>_mus_01" when it exists, else empty [I: played only while no
    // script `music` request has started a track; AUDIO.md §9.4].
    std::vector<Pickup> pickups;
    std::vector<WeaponPickup> weaponPickups; // intros of ammoWeapon() objects
    std::map<int32_t, std::vector<std::string>> chapterLinks;
    std::string music;
    std::map<int32_t, std::string> cinematics; // cinematic_object intro -> the stream it plays (cine::introCinematic)
    std::string playerObject; // Lara outfit object of this unit (formats::Unit::playerObjectId name); empty if none
    std::vector<VehicleIntro> vehicles; // `motorcycle` intros (unit frame)
    bool flashlightSpot = false; // formats::Unit::flashlightSpot: the flashlight's spot is on here, else only its omni
    std::vector<BendIntro> bends;          // bend_* intros (game/Bend.hpp), unit frame
    std::vector<formats::Unit::Wind> wind; // formats::Unit::wind
    std::string weatherObject;             // formats::Unit::weatherObjectId name; empty = none
    uint32_t levelFlags = 0;               // formats::Unit::flags
    formats::Unit::RainMap rainMap;        // formats::Unit::rainMap (unit frame)
    // Every intro's +0x52 marker id and position (unit frame), in intro order: water volumes resolve their flow and level
    // markers through them (tru.exe 0x69D690 / 0x7781D0 / 0x6AC340 mode 3; World::bindWater).
    struct IntroMarker { int32_t uid = 0; uint16_t marker = 0; vec3 pos{0}; };
    std::vector<IntroMarker> markers;
    // Render payload a UnitLoader may decode off the main thread (ou-game: formats::loadLevelScene); null = none.
    std::shared_ptr<const scene::SceneData> scene;
};
// Throws cdc::FormatError if pc-w\<name>.drm is missing or not a unit.
UnitData loadUnitData(const cdc::BigfileSet& bigfile, const std::string& name);
using UnitLoader = std::function<UnitData(const std::string& name)>;
class UnitStreamer; // Stream.cpp: the background loader thread
struct LoadedUnit {
    UnitData data;
    vec3 offset{0};        // world = unit frame + offset (portal relocation, tru.exe 0x5D13E0)
    size_t markupBase = 0; // first index of this unit's markup in World::collision().markup
};
// tru.exe 0x5D13E0 (LEVELS.md "Streaming"): offset that places `unit` (its portal to `from`) at `from`'s matching portal.
std::optional<vec3> relocation(const UnitData& unit, const LoadedUnit& from);
// tru.exe 0x5C7CA0 (docs/RENDERING.md "Neighbour units"): the weight a unit's look takes from the neighbour behind portal
// `p` (unit frame, moved by `offset`) at `pos` (the camera's focus: Lara). Inside the portal box grown by its radius r:
// (r - d) / r, d = distance to the portal plane, or to the quad's nearest edge when `pos` projects outside the quad
// (0x5C7690 / 0x4A03B0); 0 elsewhere.
float portalBlendWeight(const formats::Portal& p, vec3 offset, vec3 pos);
// The camera unit's look blended with its neighbours' (`others`: environment, portalBlendWeight): fog and water fog
// sets (colour, near, far), far plane, ambient and PSSM split fractions = (own + sum w x other) / (1 + sum w).
// Writes only those fields (and valid) of `out`.
void blendUnitLook(scene::Environment& out, const scene::Environment& own,
                   std::span<const std::pair<const scene::Environment*, float>> others);
class World;
// The look of World::currentUnit() at Lara (0x5C7CA0 over its loaded portal neighbours). `environment` returns a
// loaded unit's render settings by name (null: skipped).
void unitLook(const World& w, const std::function<const scene::Environment*(const std::string&)>& environment,
              scene::Environment& out);
struct CheckpointRef {
    std::string unit;
    int32_t id = 0;
    bool operator==(const CheckpointRef&) const = default;
};

// Lara's wetness (docs/GAMEPLAY.md "Wetness"; tru.exe object 0xFADF98, ticked by 0x709D10 -> 0x6FF200 each game frame):
// seven body regions soak in water and dry slowly; the per-bone values follow their region along bone chains (0x6ED130)
// and reach her shaders as the 4th skin-palette row (0x5B60F0, mask 1 = x) = COLOR0.x: darker diffuse, wet sheen.
// Dirt (the y channel, the 0x3C region mask, PS InstanceParams c132 = the unit's dirt colour) and rain soaking (0x44,
// 0x52E180) are not modelled: nothing of ours posts them.
struct Wetness {
    static constexpr int kRegions = 7, kBones = 160;
    // lara.drm object data (also the lara_wetsuit_* outfits) [O]: +0xB70 max, +0xB78 drying, +0xB84 soaking, per 30 Hz frame.
    static constexpr float kMax = 1.0f, kDry = 0.0002f, kSoak = 1.0f;
    std::array<float, kRegions> region{}; // +0x00
    std::array<float, kBones> bone{};     // +0x60 + 16 bone (.x)
    uint8_t water = 0;                    // +0x38: regions in water (message 0x641 -> 0x6ED050; swimming 0x7F [O Wine])
    void tick(float frames);              // frames = timeMult (dt / 33.3 ms)
    void soak() { region.fill(kMax); }    // larabase 67 (0x778140 -> 0x6ED510)
    void reset() { *this = {}; }          // larabase 68 (0x778150 -> 0x6ED460)
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
    // CPU time accumulated by tick() in the script runtime and the physics step (ou-game --frames reports the means).
    struct TickTimes { double scriptMs = 0, physicsMs = 0; } tickTimes;

    const collision::CollisionMesh& collision() const { return col_; }
    const AnimLibrary& anims() const { return anims_; }
    const Player& player() const { return player_; }
    const PlayerState& previous() const { return prev_; }
    const CameraState& camera() const { return cam_; }
    const CameraState& previousCamera() const { return prevCam_; }
    // Field camera view (World::tick): on, and its zoom 0..1 (the app draws the g_binoculars viewfinder).
    bool fieldCamera() const { return fieldCamera_; }
    float fieldZoom() const { return fieldZoom_; }
    uint64_t ticks() const { return ticks_; }
    void setTicks(uint64_t t) { ticks_ = t; } // a load (save::apply): the clock the waves and stream timers run on
    // Switches, doors, trigger volumes/planes, push blocks (src/script). Set its bigfile before ticking;
    // the single-mesh constructor has no units: give it them with script().setUnits.
    script::Runtime& script() { return script_; }
    const script::Runtime& script() const { return script_; }
    // Object collision and physics doors (src/physics, docs/formats/PHYSICS.md): the solid primitives of script()'s
    // doors and push blocks are collision().dynamics (following their pose); rigid-body doors swing on a hinge.
    const physics::Scene& physics() const { return physics_; }
    bool sendUberCommand(int32_t uid, uint32_t commandIndex);
    // A bullet in direction `dir` struck uber object `uid`'s collision: an impulse on each of its simulated joints that
    // has a range watch (tru.exe: the bullet's Havok impulse; l1 column 32 section 12 slides out of its range and takes
    // trigger 15). Zero `dir`: along each joint's axis.
    void uberShot(int32_t uid, vec3 dir = vec3(0));
    // Simulated uber joint `joint` (UberObjectData::joints index) of object `uid`, null when it does not exist (yet).
    const physics::Joint1* uberJoint(int32_t uid, int32_t joint) const {
        const auto rt = uberRuntime_.find(uid);
        if (rt == uberRuntime_.end()) return nullptr;
        const auto it = rt->second.joints.find(joint);
        return it == rt->second.joints.end() ? nullptr : &it->second;
    }
    // Section states of uber object `uid` (UberObjectData::sections order; 0xFFFFFFFF = in a transition), null before its
    // first use.
    const std::vector<uint32_t>* uberState(int32_t uid) const {
        const auto it = uberStates_.find(uid);
        return it == uberStates_.end() ? nullptr : &it->second;
    }
    // Script objects playing an animation-list clip this tick (switches / doors: their pull / open and held idle
    // clips; uber objects: their travel clip while it runs forward; anim-graph objects: their state / transition clip,
    // e.g. light fixtures' on / off): for their clip effect events (EFFECTS.md).
    struct ObjectClip {
        int32_t uid = 0;
        std::string object, clip; // objectlist name, animation-list name
        double t = 0, duration = 0; // seconds into the clip, its length
        bool loop = false;
        glm::mat4 model{1};       // intro frame
    };
    std::vector<ObjectClip> objectClips() const;
    // Posed segment frames (object frame) of a switch / door at its clip time; empty = bind pose.
    std::vector<glm::mat4> objectBones(int32_t uid) const;
    // Uber objects whose model-0 section was removed (action 28: broken urns, exploded barrels); drawn no more.
    std::set<int32_t> removedObjects() const;
    // Hit message 0x11C on an uber object's sections (0x6D4690): shots, Mjolnir's bolts, the ground pound.
    void damageObject(int32_t uid, float damage, int damageClass);
    // Enemies of the loaded units (src/game/ai), ticked after Lara; registered in `targets`. Set its bigfile
    // before ticking (as script()).
    ai::Enemies& enemies() { return enemies_; }
    // FNV-1a digest of the simulated state (Lara, camera, health/air, rigid bodies, object collision, script objects
    // and their fields, enemies): equal digests at a tick = the same simulation (render-rate tests, ou-game log).
    uint64_t stateHash() const;
    // stateHash's digest per subsystem part ("player", "camera", "bones", "body <i>", "object <uid> <name>",
    // "enemy <uid>", "uber <uid>", ...), in a fixed order: where two runs differ (save tests).
    std::vector<std::pair<std::string, uint64_t>> stateParts() const;
    // Save games (src/save): ai::Enemies::restore, then the loaded units' enemies come back from it at once.
    void restoreEnemies(const nlohmann::json& j);
    // The World's own running state for saves: the orbit camera (focus lag, desired point, mode blend, look-idle clock),
    // Lara's hair particles, the unit the scripts were last told she is in. restoreRuntime after restore() (a load
    // continues them bit for bit); bad / null json: left as restore() seeded them.
    nlohmann::json serializeRuntime() const;
    void restoreRuntime(const nlohmann::json& j);
    const ai::Enemies& enemies() const { return enemies_; }
    float fovDeg = 60.0f; // vertical FOV, from config
    // Scripted reveal camera (Camera.cpp, docs/GAMEPLAY.md "Cameras"): a unit camera spline replaces the orbit
    // camera until it ends; Lara's stick is zeroed meanwhile when a script disabled her control (larabase 56).
    bool splineCameraActive() const { return bool(splineCam_); }
    // The unit whose camera spline is playing (its tracks are in that unit's frame), else the current unit: the unit the
    // renderer's portal walk starts from (a l2_puzzleroom spline seen from l2_hubhall must not clip to hubhall's portals).
    const std::string& cameraUnit() const;

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
    // Where the last level load put Lara (its spawn: the intro / checkpoint / save position), in the world frame.
    // tru.exe keeps the exterior cell group's view there: exterior terrain draws translated by (eye - this)
    // (render::FrameOptions::exteriorOrigin; docs/formats/LEVELS.md).
    vec3 exteriorOrigin() const { return exteriorOrigin_; }
    std::vector<std::string> warnings;                      // streaming problems (missing neighbours, ...)
    // Background streaming (ou-game): portal neighbours load on a worker thread through `background` (empty = load
    // synchronously, the default) and join the world kStreamTicks after they were asked for, waiting then for a late
    // load, or at once when Lara crosses into one (tru.exe blocks there too). Level loads still wait for them. Same
    // units, offsets and collision as synchronous loads, at fixed ticks: replays stay deterministic.
    static constexpr uint64_t kStreamTicks = 60; // [I] one second
    void setAsyncStreaming(UnitLoader background);
    bool streaming() const { return !pending_.empty(); } // neighbour loads asked for, not yet joined
    void finishStreaming() { joinStreams(true); }         // join them now (tests)
    // Drops every unit, loads `unit` at offset 0 + neighbours, spawns Lara (unit frame) at `spawn`, else
    // at checkpoint `checkpointId` of that unit, else at its player intro.
    void enterUnit(const std::string& unit, std::optional<Spawn> spawn, int32_t checkpointId = 0);
    // Respawn after death: last checkpoint, else the start unit's spawn (tru.exe restores the checkpoint save).
    void respawn();
    const std::optional<CheckpointRef>& checkpoint() const { return checkpoint_; }
    void setCheckpoint(std::optional<CheckpointRef> c) { checkpoint_ = std::move(c); }
    // Autosaves the scripts requested (uisavemenu 5, after cinematics); the app autosaves when it changes.
    uint32_t autosaveRequests() const { return autosaveRequests_; }
    std::set<int32_t> collected; // intro ids of picked-up items (Pickup::id); saved
    std::array<int, 3> found{};  // pickups collected per PickupKind (PDA counts); saved
    int chapter = 0;             // kChapters index being played; saved
    bool treasureHunt = false;   // Treasure Hunt (after the ending: revisiting chapters for what is left); saved
    // Pad rumble and camera shake (game/Feedback.hpp): stepped each tick, read only by the app (motors, render camera).
    // Not saved: every sequence lasts seconds [I: a looping padshock running at a save stops at load].
    Rumble rumble;
    CameraShake cameraShake;
    // Lara's outfit object (objectlist.txt name, pc-w\<outfit>.drm): `costume` when one is chosen, else `levelOutfit`.
    // Rendering follows it; setOutfitRig re-rigs the pose skeleton.
    std::string outfit = "lara";
    // The level's own outfit: the level-load unit's (UnitData::playerObject, set by enterUnit), or a save's. Saved.
    std::string levelOutfit = "lara";
    // EXTRAS OUTFITS choice (ui::kCostumes object), kept over level loads and saves; empty = levelOutfit. The app keeps
    // it in save::Profile. wear() sets it and dresses Lara now (the app re-skins her on the next frame).
    std::string costume;
    void wear(std::string c) { costume = std::move(c), outfit = costume.empty() ? levelOutfit : costume; }
    void setOutfitRig(const cdc::Drm& drm); // rigOutfit on the pose library, then a pose reset
    // Lara's hidden draw groups (bit n-1 hides group n): the scripts' (script().playerHiddenGroups()) with the
    // wetsuits' face mask (group 1) only while she swims and their stowed gear (2) never (GAMEPLAY.md "Thor's gear").
    uint32_t laraHiddenGroups() const;
    // Starts chapter `n` (kChapters) at its start unit's player intro and announces it; respawns fall back there.
    // Throws as enterUnit.
    void startChapter(int n);
    // The chapter-start message: generalbank's game script (type 6 fn 30) Sends id = chapter on its +924 channel
    // "mc_CinematicData"; the start unit's script receives it in OnMessage and enters state 1, which activates the
    // chapter's intro cinematic (l4 13929 -> 5484, l5 17775 -> 7530, l7 19597 -> 9071; GAMEPLAY.md "Chapter starts").
    // Delivered next tick.
    void announceChapter() { script_.sendMessage(script::channelId("mc_CinematicData"), uint32_t(chapter)); }
    // Called when a script activates a cinematic_object that has a stream (UnitData::cinematics), between its cinematic
    // events 0 and 1, before a level end's chapter change (`levelEnd`). ou-game plays the cutscene here
    // (src/app/game/Cinematic.*); unset = skipped. Afterwards World places Lara and the bikes where the stream leaves
    // them (its header tails, 0x455D90).
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
    // Script markup.SetEnabled: disabled markup keeps its index but its type becomes kMarkupDisabled (no traversal).
    static constexpr uint8_t kMarkupDisabled = 0xFF;
    bool markupEnabled(const std::string& unit, uint16_t id) const { return !disabledMarkup_.contains({unit, id}); }
    uint64_t lightsVersion() const { return lightsVersion_; }
    // One-off splash on the active surface (shots, hazards, scripted impacts).
    void splashWater(vec3 pos, float strength = 1.0f) { water_.splash(pos, strength); }
    // Grapple: best target within range, aim cone and line of sight (World.cpp), for attach + the HUD marker.
    std::optional<GrappleTarget> grappleTarget() const;
    void attachGrapple(vec3 anchor, int32_t uid, GrappleKind kind = GrappleKind::Ceiling) { player_.attachGrapple(anchor, uid, kind); }
    int32_t lastPulled() const { return lastPulled_; } // grapple target whose pull handler ran last (World::tick), 0 = none
    void releaseGrapple(bool jump) { player_.releaseGrapple(jump); }
    // Breath 0..1 (tru.exe 0x755230: drains while the head is under water; World::tickAir).
    float air() const { return std::max(0.0f, air_); }
    bool submerged() const { return submerged_; } // head under water this tick
    // watervolumebase natives on volume `uid` (script::Runtime::WaterNative; World::bindWater): false =
    // the volume is not loaded or the call is not a binding.
    bool waterNative(int32_t uid, std::string_view type, uint32_t function, std::span<const int32_t> args, uint32_t& result);
    const Wetness& wetness() const { return wetness_; }
    const Bends& bends() const { return bends_; } // bend_* plants' bend vectors (game/Bend.hpp)

    // ---- combat (Combat.cpp; docs/GAMEPLAY.md "Combat") ----
    std::vector<Damageable*> targets; // lock-on / hitscan targets, non-owning (src/game/ai registers its enemies)
    void damageLara(float amount, vec3 from, int type = kDamageLight); // enemy blows, hazards (tru.exe 0x6F5030)
    void kill();                                                      // message 4: damage = current health
    const Health& health() const { return health_; }
    // saves (after restore(), which heals fully, and restoreCombat, which set the window / regeneration clocks)
    void setHealth(float v) { const auto c = health_.clocks(); health_.set(v); health_.setClocks(c); }
    const CombatState& combat() const { return combat_; }
    // The screen wipe (1 = black): the scripts' FadeOut / FadeIn, and her death's 3 s fade (DeathState, Combat.cpp).
    float screenFade() const;
    // The upper-body combat clip aimLayer plays now (reload, equip / unequip) and its time; empty name when none.
    // Lara's and the weapon object's animation event lists for that clip name run on it (sounds, AUDIO.md §9.7).
    std::pair<std::string, double> combatLayerClip() const;
    TargetingTuning targeting;
    // Weapon w's model `hand` (0 right / the two-handed gun, 1 left): Lara bone it rides on (hands while drawn,
    // thigh / back holster otherwise) and its matrix in that bone's frame; nullopt when it is not shown (not
    // carried, a two-handed weapon's hand 1, the uzi out of hand). World matrix = modelMatrix * pose[bone] * offset.
    std::optional<std::pair<int, glm::mat4>> gunMount(Weapon w, int hand) const;
    // The current weapon's model `hand` in the world, this tick's pose (its "fire" events play in this frame);
    // muzzle = its shot origin (WeaponDef::muzzle) in it.
    glm::mat4 gunMatrix(int hand) const;
    vec3 muzzle(int hand) const;
    // Carries `w` as the secondary (replacing the previous one; Pistols: none), full clip, `reserve` spare rounds
    // capped at its max reserve (ou-game --weapon; the original gets them from pickups). ThorHammer: Mjolnir joins her
    // carried objects (script::Runtime::playerObjects, as larabase 37 "wp_thorhammer"); `reserve` is ignored.
    void giveWeapon(Weapon w, int reserve);
    // PDA WEAPON SELECTION's EQUIP: `w` (a secondary, or Pistols for none) becomes the secondary with the clip and
    // reserve it already has (no refill); drawn, Lara switches to it as Change Weapon would.
    void equipWeapon(Weapon w);
    // A chapter start: every secondary (uzi, shotgun, speargun, rifle, tranquilizer) full clip and reserve, the
    // chapter's own one equipped (chapterSecondary) [I: the original's PDA lists them all, full, in a fresh l4 load].
    void chapterLoadout(int chapter);
    // Weapons, clips, reserves and the adrenaline meter, for saves; restoreCombat holsters (nullptr / bad: unchanged).
    nlohmann::json serializeCombat() const;
    void restoreCombat(const nlohmann::json& j);
    // The sonar map's reveal: per unit, unit-frame points Lara has stood at, kSonarRevealStep apart (the PDA shows
    // the map within ui::kSonarRevealRadius of them). Saved. GAMEPLAY.md "PDA".
    std::map<std::string, std::vector<vec3>> mapReveal;
    // Mjolnir carried: "wp_thorhammer" among her carried objects (250_val_hammer's end gives it; chapterGear from 6).
    bool hasHammer() const { return script_.playerObjects().contains("wp_thorhammer"); }
    // PLAYER TAILORING (Combat.hpp): Lara's max health (keeping its fraction), enemy health on (re)spawn, saving grab
    // window, ammunition capacity (reserves / grenades over the new capacity are dropped). Saved with the game.
    void setTailoring(const Tailoring& t);
    const Tailoring& tailoring() const { return tailoring_; }
    int maxReserve(Weapon w) const { return anims_.weapons[size_t(w)].maxReserve[size_t(std::clamp(tailoring_.ammo, 0, 2))]; }
    int maxGrenades() const { return anims_.grenade.maxCarried[size_t(std::clamp(tailoring_.ammo, 0, 2))]; }

    // ---- gadgets (Gadgets.cpp; docs/GAMEPLAY.md "Flashlight", "Grenades") ----
    bool flashlight = false;         // utility light on (Actions::light toggles it); saved
    bool flashlightSpot() const;     // the current unit lights the spot too (else the omni light alone)
    // Light origin (chest bone 15, 25 units forward [I]) and the torso's forward direction, world, blended between the
    // last two ticks; the app places the personal light's spot/omni there.
    std::pair<vec3, vec3> flashlightPose(double alpha) const;
    int grenades = 0;                // carried; World starts with maxGrenades() [I]; saved
    const std::vector<Grenade>& liveGrenades() const { return grenades_; }
    const std::vector<vec3>& explosions() const { return explosions_; } // grenades that went off this tick

    // ---- motorcycle (Vehicle.cpp; docs/GAMEPLAY.md "Motorcycle") ----
    // Every bike of the units loaded so far (by intro uid), kept across streaming; positions in their unit's frame.
    const std::map<int32_t, Bike>& bikes() const { return bikes_; }
    // World model matrix of a bike (model -Y forward) blended between the last two ticks; nullopt when its unit is
    // not loaded.
    std::optional<glm::mat4> bikeMatrix(int32_t uid, double alpha) const;
    const Bike* ridden() const; // the bike Lara is mounting, riding or leaving; null on foot
    int bikeMounts = 0;         // mounts so far (ou-game starts l4_laras_new_bike on the first)
    nlohmann::json serializeVehicles() const;
    void restoreVehicles(const nlohmann::json& j); // throws std::runtime_error on a malformed state
    // Uber section state machines (states, transitions, timers, once-events, travel) and the carried portable, for saves.
    // restoreObjects replaces them all; objects not loaded yet take theirs when they stream in (no default-state events).
    nlohmann::json serializeObjects() const;
    void restoreObjects(const nlohmann::json& j); // throws std::runtime_error on a malformed state

private:
    void updatePose(float dt, bool reset); // after the player moved; reset = spawn/teleport (no blends, hair at rest)
    void updateCamera(const Actions& a, float dt); // Camera.cpp
    void updateOrbitCamera(const Actions& a, float dt);
    bool startSplineCamera(const std::string& unit, uint32_t index, uint32_t value); // autosplinecamera 0
    bool cameraNative(int op, const std::string& unit, uint32_t index, uint32_t value); // script::Runtime::CameraNative
    bool tickSplineCamera(float dt); // false once no spline camera plays (Camera.cpp)
    LoadedUnit* findUnit(const std::string& name);
    // Load (or queue) portal neighbours of units_[center], unload the rest; `fresh`: units_ was replaced (level load).
    void streamAround(size_t center, bool fresh = false);
    void joinStreams(bool now);       // queued neighbours due by now (all: `now`) join units_ (Stream.cpp)
    bool attach(UnitData d, const std::string& from); // relocated against loaded unit `from` (else the current one)
    // After units_ changed: collision, cinematic ends, Lara's ledge / water markup (kept by (unit, local index)).
    void unitsChanged(const std::optional<std::pair<std::string, int>>& ledge, const std::optional<std::pair<std::string, int>>& water);
    void rebuildCollision();
    void crossPortals(vec3 from, vec3 to);
    void syncObjects();          // script objects -> collision dynamics + physics bodies (after units change)
    void tickObjects(float dt);  // physics step, dynamics follow their objects, Lara rides what she stands on
    void tickJoints(float dt); // script multibody joints: positions and range watches (Objects.cpp)
    // One Jolt step (Objects.cpp): hinge drives, uber joints, loose rigid bodies (tumbling onto the terrain and object
    // collision), then the uber range watches.
    void stepPhysics(float dt);
    // Dead enemies' and Lara's ragdolls (Objects.cpp): starting ones get their bodies (with the killing hit's impulse),
    // settled or streamed-out ones lose theirs; after a step (`step`) each body's pose goes back to its owner, and each
    // chunk piece's to its enemy.
    void syncRagdolls(bool step);
    // Thralls broken into chunks (ai::EnemyState::ragdollPhase 4): their pieces as loose bodies (Objects.cpp).
    void chunkPieces(std::vector<physics::Scene::Loose>& out);
    void startChunks(ai::Enemy& e); // a thrall that just broke: its pieces' poses and velocities (Objects.cpp)
    void carryPortables(bool pressed); // pick up / carry / set down `portablebase` objects (Objects.cpp)
    void touchCheckpoints();
    void recordReveal(); // mapReveal: Lara's unit-frame position once kSonarRevealStep from every recorded point
    void tickCombat(const Actions& a, float dt);
    void aimLayer(std::vector<anim::LocalTransform>& local, float dt); // weapon upper body over `local`
    std::vector<Damageable*> candidates(bool hard) const; // valid targets in selection order
    void fire(int hand);
    // One ray of a shot (from, unit dir, len; dist0 = range already flown, for the damage falloff): the nearest world
    // face or living target; a struck target / script object takes the hit. Fills shot.to / hit / target / damage.
    bool strike(Weapon w, vec3 from, vec3 dir, float len, float dist0, bool wet, Shot& shot);
    void tickProjectiles(float dt);
    void gainAdrenaline(float dealt); // the meter += the health her hit took (message 0xD9)
    void groundPound(); // Mjolnir's shock (event 0x4B): knocks every enemy within kPoundRadius off balance
    void reloadWeapon(); // refill the current weapon's clips from its reserve
    void collectPickups(bool pressed);
    void tickAir(float dt); // oxygen / drowning (World.cpp)
    // lara.drm player data (every outfit): breath +0xF8 = 4000 frames, refilled from empty in +0xFA = 60 frames; once spent,
    // +0xFC = 10 % of max health (damage type 3) every +0xFE = 20 frames (tru.exe 0x755230 / 0x756C40).
    static constexpr float kAirSeconds = 4000 / 30.0f, kAirRefillSeconds = 60 / 30.0f;
    static constexpr float kDrownDamage = 0.10f, kDrownPeriodSeconds = 20 / 30.0f;
    // Script Activate of a cinematic_object: cinematic event 0, onCinematic, placeAfterCinematic, event 1, the post-stream
    // helper; a level end's -> startChapter(next).
    void checkLevelEnd();
    void placeAfterCinematic(const std::string& stream); // Lara + bike header tails (0x455D90)
    void recentre(const std::string& unit); // Lara moved into loaded `unit`: it becomes the current one (streamAround)
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
    std::optional<vec3> focus_; // orbit camera focus point (tru.exe LaraCameraRewrite +0x3B0), lagging its goal (World.cpp)
    struct OrbitState {      // orbit camera (World.cpp): +0x150 desired point, +0x1D4 distance, +0x1D8 its return speed
        vec3 desired{0};
        float dist = 0, speed = 0;
        bool valid = false;
        float yaw = 0, pitch = 0; // the angles it left in cam_ (others changing them re-seed it)
        struct Limits {           // camera mode's focus record offset / dead zones and look limits, blended
            vec3 offset{0}, zones{-1};
            float yawRange = 0, pitchLo = 0, pitchHi = 0;
        } from, now;
        int mode = -1;
        float modeTime = 0, idle = 0; // seconds in this mode / since the last look input (+0x1E4)
        float moves[10] = {}; // her last per-frame moves (30 Hz units) for the yaw force
        vec3 lastPos{0};
        vec3 hint{0};         // level camera record point (+0x12B0), eased toward the record's
        int hintMarkup = -1;  // the markup whose record it follows, -1 = none
    } orbit_;
    std::shared_ptr<SplineCamera> splineCam_; // shared_ptr: SplineCamera is complete only in Camera.cpp
    CameraState blendFrom_; // last spline camera pose, blended out of over kCameraBlendSeconds
    float blendOut_ = 0;    // seconds of blend-out left
    bool cameraCut_ = false; // the spline camera started this tick: no interpolation from the previous tick
    uint64_t ticks_ = 0;
    UnitLoader loader_;
    std::shared_ptr<UnitStreamer> streamer_; // setAsyncStreaming; null = synchronous loads (shared_ptr: incomplete type)
    struct PendingUnit {
        std::string name, from; // queued neighbour, the unit it is relocated against
        uint64_t due = 0;       // tick it joins at
    };
    std::vector<PendingUnit> pending_;
    std::vector<LoadedUnit> units_;
    size_t current_ = 0;
    uint64_t unitsVersion_ = 0;
    std::string startUnit_;
    std::string playerUnit_; // unit the scripts were last told Lara is in (Runtime::setPlayerUnit); empty = level load
    std::optional<Spawn> startSpawn_;
    vec3 exteriorOrigin_{0};
    std::optional<CheckpointRef> checkpoint_;
    uint32_t autosaveRequests_ = 0;
    script::Runtime script_;
    ai::Enemies enemies_{&targets};
    uint64_t scriptUnits_ = ~0ull; // unitsVersion_ the script runtime was synced to
    std::vector<script::UnitRef> unitRefs() const; // loaded units + cinematic objects, for script_/enemies_ setUnits
    size_t syncedObjects_ = ~size_t(0); // script object count syncObjects ran for
    size_t syncedPlates_ = 0;           // plates + loose bodies then (constructors / pulls make them after syncObjects)
    int32_t held_ = 0;                   // portable object Lara carries (0 none)
    std::map<int32_t, physics::Joint1> plates_; // pressure-plate joints by uid (stepPhysics): Jolt's x and v between steps
    void bindMultibody();
    void bindUber();
    void bindTerrain();
    void bindLightHealth();
    void bindMarkup();
    void bindEnemies();
    // A water volume the scripts changed (tru.exe water data, docs/GAMEPLAY.md "Water"): surface +0x58 moving toward
    // target +0x5C at rate +0x60 units per 30 Hz frame (0x6C10D0), flow markers +0x64 / +0x66 with their positions
    // (0x69D690) and strength +0x68. Unit frame; World::applyWaterState writes it into the volume's markup.
    struct WaterState {
        float surface = 0, target = 0, rate = 0;
        int16_t source = 0, sink = 0;
        float strength = 0;
        vec3 sourceAt{0}, sinkAt{0};
    };
    std::map<int32_t, WaterState> waterStates_; // by watervolume intro uid
    std::vector<physics::Scene::Water> physicsWater_; // stepPhysics: the loaded volumes for buoyancy (reused)
    void bindWater();
    void bindFeedback();
    std::map<uint64_t, uint32_t> padshocks_; // padshock member key -> its last rumble handle (NtPadshockInst +0xC)
    void tickWaterStates(float dt);
    void applyWaterState(collision::Markup& m, vec3 offset) const; // a volume of rebuildCollision / a changed one
    // The volume `uid`'s markup in collision() and its unit; nulls when it is not loaded.
    std::pair<collision::Markup*, const LoadedUnit*> waterVolume(int32_t uid);
    std::map<std::pair<int32_t, uint32_t>, uint32_t> enemyValues_; // (intro uid, enemybase setter fn) -> value
    std::set<std::pair<std::string, uint16_t>> disabledMarkup_; // (unit, markup id)
    const formats::UberObjectData* prepareUber(const script::Object& o);
    std::map<int32_t, float> multibodyLo_, multibodyHi_; // native limits issued before hinges exist
    std::map<std::string, std::shared_ptr<const cdc::Drm>> objectDrms_;
    std::map<int32_t, std::optional<formats::UberObjectData>> uberData_;
    std::map<int32_t, std::vector<uint32_t>> uberStates_; // per section: current state, 0xFFFFFFFF in a transition
    // Section components (tru.exe 0x4D7550 update): current state (+0x84) or running transition (+0x80), seconds in the
    // state (+0x88) / transition; events fired once (+0x60); commanded: a command changed a section (travel latch).
    // Joints: the simulated physics joints (action 9 creates, 10 removes; types 0 / 2, links 11); watches: range watches
    // (action 0x14, 0x49A5C0 record: inside starts true, 0x4937A0).
    // health: section +0xDC (damage lowers it, 0x6D4690); removed: action 28 took the section's instance away.
    struct UberSectionState { int32_t state = -1, transition = -1; float time = 0, length = 0, health = 0; bool removed = false; };
    struct UberWatch { int32_t section = 0, joint = -1, range = -1; uint32_t mode = 0; bool inside = true; };
    struct UberRuntime {
        std::vector<UberSectionState> sections;
        std::vector<float> animLength;
        uint64_t fired = 0;
        bool commanded = false;
        std::map<int32_t, physics::Joint1> joints;
        std::set<int32_t> links; // created type-11 link joints
        std::vector<UberWatch> watches;
        std::vector<std::string> animNames; // the object's animation list, as animLength
        bool intangible = false;            // action 5: instance +0x18 bit 0x20000000, collision callbacks skip it
    };
    std::map<int32_t, UberRuntime> uberRuntime_;
    std::vector<std::pair<int32_t, uint32_t>> uberEvents_; // (uid, event index) for the script's vt 54
    void uberEnter(int32_t uid, size_t section, uint32_t state);
    void uberBegin(int32_t uid, size_t section, uint32_t transition);
    void uberFire(int32_t uid, size_t section, uint32_t kind, uint32_t value);
    bool uberHolds(int32_t uid, const std::vector<formats::UberCondition>& conditions) const;
    void tickUber(float dt);
    void uberAction(int32_t uid, size_t section, const formats::UberAction& a); // a state's entry action (0x4D86A0)
    std::optional<physics::Joint1> makeUberJoint(int32_t uid, int32_t joint); // frame, mass, gravity; at rest at 0
    void uberWatches(int32_t uid); // range watches on the last step's joint positions (triggers 14 / 15)
    std::optional<glm::mat4> uberJointPose(int32_t uid) const;
    int32_t uberModelJoint(int32_t uid) const;
    void pushUberJoints(const Actions& a); // Lara grips / pushes an uber object riding a level prismatic joint
    void useSwitchClip(const script::Object& o); // Lara plays the switch's use clip, aligned to it (Objects.cpp)
    vec3 uberGrip_{0};                     // last tick: Lara grips one (her stick moves it): its axis toward her facing, 0 = none
    struct UberPush { int32_t uid = 0, joint = -1; float before = 0; } uberPush_; // this tick's pushed joint
    std::map<std::pair<std::string, uint32_t>, std::shared_ptr<const physics::ObjectPhysics>> modelPhysics_; // (object, model)
    struct UberMotion { vec3 delta{0}, rotation{0}; float duration = 1, t = 0; bool active = false; std::string clip; };
    std::map<int32_t, UberMotion> uberMotion_;
    std::map<int32_t, nlohmann::json> uberSaved_; // restored uber state of objects not prepared since (prepareUber applies it)
    std::map<std::pair<std::string, uint32_t>, bool> lightStates_;
    uint64_t lightsVersion_ = 0;
    // One per collision().dynamics entry: owner, model segment and its bind frame, hinge (-1 = posed by script()).
    struct ObjectCollider { int32_t uid; int segment; int hinge; glm::mat4 intro, frame; };
    std::vector<ObjectCollider> colliders_;
    std::map<std::string, std::shared_ptr<const physics::ObjectPhysics>> objectPhysics_; // by object name; null = none
    const physics::ObjectPhysics* objectPhysics(const std::string& name); // objectPhysics_, loaded on first use
    std::optional<glm::mat4> markerFrame(const script::Object& o, int id); // HInfo marker `id` of `o` in the world
    physics::Scene physics_;
    Health health_;
    Tailoring tailoring_;
    WaterSim water_;
    float air_ = 1.0f;
    bool submerged_ = false;
    Wetness wetness_;
    Bends bends_;
    uint64_t bendUnits_ = ~uint64_t(0); // unitsVersion_ bends_ was synced to
    // gadgets (Gadgets.cpp)
    void tickGadgets(const Actions& a, float dt);
    void explode(vec3 at);
    void throwLayer(std::vector<anim::LocalTransform>& local) const; // throw clip over the upper body
    std::vector<Grenade> grenades_;
    std::vector<vec3> explosions_;
    bool lightHeld_ = true, grenadeHeld_ = true; // no press edge on the first tick
    float lightCooldown_ = 0, grenadeCooldown_ = 0;
    float throwT_ = -1;              // seconds into the throw clip; -1 = not throwing
    vec3 throwAt_{0};                // landing point chosen on the press
    CombatState combat_ = fullClips(anims_.weapons);
    double aimTime_ = 0; // pose clips' clock
    bool interactHeld_ = true;     // no press edge on the first tick
    bool grappleHeld_ = true;      // no press edge on the first tick
    bool recenterHeld_ = true, fieldCameraHeld_ = true; // press edges (none on the first tick)
    bool fieldCamera_ = false;
    float fieldZoom_ = 0;
    float pullT_ = 0;              // seconds of heaving on a mounted grapple target
    int32_t lastPulled_ = 0;
    std::vector<Pickup> pickedUp_;
    // motorcycle (Vehicle.cpp)
    bool tickVehicles(const Actions& a, float dt); // true while Lara is on a bike: Player::tick is skipped
    bool ridingBike() const;                       // on a bike in its ride phase (not mounting / dismounting): guns allowed
    void syncVehicles();                           // register the loaded units' bike intros (after streaming)
    void mountBike(int32_t uid);                   // larabase 52: on the bike at once, riding (script_.takeMounts)
    void driveBike(Bike& b, const Actions& a, bool jumpPressed, float dt); // rider input for the next physics step
    std::vector<physics::Scene::Vehicle> bikeVehicles(); // the ridden bike for physics_ (stepPhysics)
    void readBikes(float dt);                      // after the step: bike pose, landing, ramming; Lara on it
    void settleBike(Bike& b);                      // height and pitch from its wheels' ground contacts
    void ramEnemies(Bike& b);
    vec3 bikeOffset(const Bike& b) const;          // its unit's stream offset (0 when not loaded)
    std::map<int32_t, Bike> bikes_;
    int32_t riding_ = 0;                           // uid of the bike Lara is on, 0 = none
    bool bikeInteractHeld_ = true, bikeJumpHeld_ = true;
    std::map<const Damageable*, uint64_t> rammed_; // target -> tick of the last ram (one hit per contact)
};

// The unit's `player` intro (objectlist.txt name), else the first intro; nullopt if the unit has none.
std::optional<Spawn> findSpawn(const cdc::BigfileSet& bigfile, const cdc::Drm& unit);
// Water volumes of a unit as type-10 ("water", MUD flag 0x100000) markup boxes, as tru.exe builds them
// for each `watervolume` instance (0x5D7130): its intro data's box around the instance, surface = top.
// points = {AABB min, AABB max (max.z = surface), centre, half extents, (cos yaw, sin yaw, 0), 4 x wave (kx, ky, speed),
// 4 x (height, 0, 0) (zero without waves: data +0x4E bit 1), then the current: kWaterSink / kWaterSource positions,
// kWaterFlow (strength in units per 30 Hz frame, radius, horizontal only), kWaterIds (source, sink marker ids)
// (docs/GAMEPLAY.md "Water").
std::vector<collision::Markup> findWater(const cdc::BigfileSet& bigfile, const cdc::Drm& unit);
inline constexpr size_t kWaterSink = 13, kWaterSource = 14, kWaterFlow = 15, kWaterIds = 16;
// The current at `p` in volume `m` (tru.exe 0x6ACB60), units/s; zero for still water and two-point markups.
vec3 waterFlow(const collision::Markup& m, vec3 p);
// `p` inside water box `m` (a findWater markup) grown by `grow` per axis in the box frame.
bool insideWater(const collision::Markup& m, vec3 p, vec3 grow = vec3(0));
// Sea waves of a water volume (tru.exe 0x69DA80): sum_i height_i (cos(kx_i x + ky_i y + speed_i t) - 1), <= 0, with t =
// game seconds mod 200 pi (0x6ADB70, 0x6C10D0); 0 for volumes without waves.
float waveHeight(const collision::Markup& m, vec2 xy, double seconds);
// Water surface z of volume `m` at `xy` (top + waveHeight, tru.exe 0x6AD010).
inline float waterSurface(const collision::Markup& m, vec2 xy, double seconds) { return m.points[1].z + waveHeight(m, xy, seconds); }
// The sea program's VS InstanceParams 3-6 (c60-c63: kx, ky, phase, height per wave; 0x69DFB0); zero without waves.
std::array<vec4, 4> waveRegisters(const collision::Markup& m, double seconds);
// The water the camera looks at Lara in (tru.exe 0x5EA200, the under-water pass / fog test): the volume Lara is
// swimming in, else the one her feet are below the (wave) surface of; `surface` = that surface at `eye`'s xy, `seconds`
// the sea clock. nullopt = Lara is not in water: no under-water look, even if her box (l2 yacht deck) is inside a sea
// volume whose top is above her.
struct EyeWater { int markup; float surface; };
std::optional<EyeWater> eyeWater(const World& w, vec3 eye, double seconds);

// ---- input ------------------------------------------------------------------------------------
// Keyboard + mouse buttons + the gamepad in use (platform::DeviceState, built from SDL events) -> actions, through
// the user's bindings (config::InputConfig; defaults are tru.exe's, docs/GAMEPLAY.md "Controls": WASD/arrows move,
// Shift sprint, Alt walk, Space jump, Ctrl drop, E interact, Q grapple, mouse right / G target lock, left / H fire,
// R weapon, V health pack, F light, Caps Lock grenade, Tab PDA; pad A jump, B drop, X grapple, Y interact, LB sprint,
// RB grenade, LT lock, RT fire, Back PDA, d-pad up / left / right health / light / weapon). The move stick (default left;
// lightly = walk) and look stick (default right) are config moveStick / lookStick.
// Mouse look is added by the app: it accumulates relative motion per frame and feeds it to the next tick as a rate
// (see ou-game), scaled by mouseScale and flipped by invertX / invertY.
struct Controls {
    enum Action { Forward, Back, Left, Right, Walk, Sprint, Jump, Drop, Interact, Draw, Fire, Aim, Switch, Weapon, Heal, Grapple,
                  Save, Load, Hint, Answer, Pda, Light, Grenade, Recenter, Camera, Pause, MenuUp }; // config::bindableActions() order
    std::vector<std::vector<platform::InputRef>> actions; // per config::bindableActions() entry
    float mouseScale = 1.0f;
    bool invertY = false, invertX = false;
    int moveStick = 0, lookStick = 1; // 0 left, 1 right, -1 none
    Controls() : Controls(config::InputConfig{}) {}
    explicit Controls(const config::InputConfig& c); // throws std::runtime_error naming an unknown input
    bool bound(Action a, const platform::InputRef& r) const { return std::ranges::find(actions[a], r) != actions[a].end(); }
    // The menu navigation `r` is bound to (ui::Nav order: up, down, left, right, accept, back, delete, alt); -1 = none.
    int menuNav(const platform::InputRef& r) const;
};
Actions readDevices(const Controls& controls, const platform::DeviceState& devices);
// What button placeholders in locals.bin text name (ui::PromptNames, tru.exe 0x5A7C20): per tru.exe virtual button
// 0..24 (GAMEPLAY.md "Controls") and the keys Return / Escape / BackSpace (25..27: menu_accept / menu_back / menu_delete),
// the control bound in `controls` on the device in use. Keyboard: the first key, else the mouse button ("L Mouse",
// locals.bin 5050..5052); the pad: the first pad control's name as the Xbox 360 pad lists them (5061..5076: icon code
// points, "Start Button" / "Back Button"), else the placeholder's own icon. Empty: nothing bound (draws "<Unknown>").
std::array<std::string, 28> promptNames(const Controls& controls, bool gamepad, const locale::Strings& strings);

// CONTROL CONFIGURATION > BUTTON CONFIGURATION rows: tru.exe's list [O buttons1/2 captures; its four VEHICLE rows are
// our forward / back / jump / fire and not listed], then ours (`label`, no locals.bin name).
struct ControlRow {
    int16_t name;       // locals.bin, 0 = label
    const char* label;
    const char* action; // config::bindableActions() entry
};
inline constexpr std::array<ControlRow, 34> kControlRows{{
    {1038, "", "forward"}, {1039, "", "back"}, {1040, "", "left"}, {1041, "", "right"}, {1042, "", "drop"}, {1043, "", "jump"},
    {1044, "", "grapple"}, {1045, "", "interact"}, {1046, "", "fire"}, {1047, "", "grenade"}, {1048, "", "switch"},
    {1049, "", "pause"}, {1050, "", "pda"}, {1051, "", "heal"}, {1052, "", "weapon"}, {1053, "", "walk"}, {1055, "", "light"},
    {1054, "", "camera"}, {1056, "", "aim"}, {1057, "", "sprint"}, {0, "RECENTER CAMERA", "recenter"},
    {0, "DRAW / HOLSTER", "draw"}, {0, "QUICK SAVE", "save"}, {0, "QUICK LOAD", "load"}, {0, "HINT", "hint"}, {0, "ANSWER", "answer"},
    {0, "MENU UP", "menu_up"}, {0, "MENU DOWN", "menu_down"}, {0, "MENU LEFT", "menu_left"}, {0, "MENU RIGHT", "menu_right"},
    {0, "MENU SELECT", "menu_accept"}, {0, "MENU BACK", "menu_back"}, {0, "MENU DELETE", "menu_delete"}, {0, "MENU TASK", "menu_alt"}}};
// The inputs bound to `action` on one device class (keyboard + mouse, or the pad), as tru.exe's list shows them: names
// joined by "/" ("H/L Mouse"), pad buttons as their icons. Empty = unmapped.
std::string boundNames(const config::InputConfig& c, const std::string& action, bool gamepad, const locale::Strings& strings);
// The original's capture: `r` replaces the inputs of its device class (keyboard + mouse, or the pad) bound to `action`;
// unmap removes them (BackSpace UNMAP).
void rebind(config::InputConfig& c, const std::string& action, const platform::InputRef& r);
void unmap(config::InputConfig& c, const std::string& action, bool gamepad);

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
