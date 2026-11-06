// Enemy AI: enemy instances from unit intros and spawn generators, perception, chase, melee attack, hit
// reaction, death. A small native state machine standing in for tru.exe's enemy behaviour trees
// (EnemyBehaviorTree_Creature / _Thrall, RTTI). Evidence, numbers and deviations: docs/GAMEPLAY.md "Enemies".
// Frame: Z-up world units (as World); model -Y is forward (as Lara).
#pragma once

#include "anim/Anim.hpp"
#include "collision/Collision.hpp"
#include "game/Combat.hpp"
#include "formats/Formats.hpp"
#include "physics/Physics.hpp"
#include "script/Script.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <set>
#include <string_view>
#include <vector>

namespace ou::cdc { class BigfileSet; }

namespace ou::game::ai {

using glm::vec3;

// Animation roles; EnemyType::roles names the clip for each.
enum class Role { Idle, Alert, Walk, Run, Attack, Hit, Death, Spawn, Count };

// One perception cone: Lara is inside when closer than `range` and within ±yaw / ±pitch of the facing.
struct Cone {
    float range = 0, yaw = 0, pitch = 0; // units, rad (data: s16 range, s16 degrees, s16 degrees)
};
// Senses record (list node +0x30 default, +0x38 combat; tru.exe 0x664B20): seen = inside the eye cone (s16
// +0x10/+0x12/+0x14, each falling back to +4/+6/+8 when negative) or a body cone (+4/+6/+8, +0xA/+0xC/+0xE), then
// a line-of-sight ray unless byte +0x44 is set.
struct Senses {
    Cone eye{3160.56f, 1.0472f, 1.5708f}; // [I] defaults (10 m, ±60°): thralls have no senses data
    std::array<Cone, 2> body{{{790.14f, 3.14159265f, 1.5708f}, {}}}; // [I] 2.5 m all round
    bool ray = true;
    float loseTrack = 0; // +0x20; [I] unseen Lara closer than this to the enemy is still tracked
};
// Attack record (list node +0x68..+0x9C, tru.exe 0x642D80 / EnemyState_Fire 0x666D10/0x666EB0).
struct Attack {
    uint32_t node = 0;              // list node offset it came from
    std::string clip;               // rec+0x78 clip ref (melee blow)
    std::string shotClip, followClip; // rec+0x7C per-shot clip, rec+0x84 follow-up (reload)
    float damage = 0;               // rec+0x154, Lara hp per blow / shot
    int type = 0;                   // rec+0x158: 0 / 1 / 2 ([I] 2 = heavy, else light)
    int shots = 0, gap = 0, startDelay = 0; // s16 +0x13E shots per burst, +0x144 gap, +0x13A start delay ([I] 30 Hz counts)
    float cooldown = 0;             // rec+0x34 ([I] ms), seconds
    float maxYaw = 0;               // byte +0x1F0 degrees, rad: fires only facing Lara this closely
    bool ranged() const { return !shotClip.empty(); }
};
// Death record (list node +0x64, tru.exe 0x66C500): count +0xC8, 0x14-byte entries at +0xCC {clip ref, s32 min,
// s32 max, category, flag}; FUN_00642210 picks by the attacker's quadrant (1 front, 3 left, 5 back, 7 right) and
// FUN_00642170 returns the entry's min / max: the physics takeover (ragdoll / chunks) comes min + rand(max - min) counts
// after the death (0x66C500 arms the death timer +0x10 with it, 0x66C950 runs when it expires).
struct DeathClip {
    std::string clip; // "" = a type-2 ref (anim request) we do not resolve
    int category = 1;
    int flag = 0;     // 0 plain, 3 adrenaline / head shot, 5 running, 7 special (vault, thrown)
    int min = 0, max = 0; // ragdoll delay, counts ([I] 30 Hz as the other record counts)
    bool valid = true;    // clip ref index != -1 (FUN_00642210 skips -1 entries)
};
// Chunk object (death record s16 +0x46 -> objectlist id, e.g. viking_thrall_chunkdoll): ObjectBase +0xC8 data {+0x10
// s32 minimum, +0x14 s32 kept, +0x18 count, +0x1C -> 0x14-byte pieces} (tru.exe 0x696F90 / 0x696B90). Each piece is an
// instance of the chunk object showing `model`, placed at mix(segment B, segment A, t) of the dying body and moving as
// those segments did; 0x696A60 then pushes every piece with death record +0x4C x 2.5 along the hit.
struct ChunkPiece {
    int model = 0;          // +0 u8: chunk object model index
    int segA = 0, segB = 0; // +2 / +4 s16: body segments
    float t = 1;            // +8
    uint32_t flags = 0;     // +0x10: bit 0 [U] (instance +0x14 bit 0x40000000 cleared, else +0x18 |= 0x20000)
};
struct ChunkDoll {
    std::string object;
    int32_t minimum = -1, kept = 0; // +0x10 / +0x14: budget checks against the free instance slots (0x696F90)
    std::vector<physics::ObjectPhysics> models; // per model: shape (its primitives) and bounds
    std::vector<ChunkPiece> pieces;
};
// Regenerate (death record +0x20..+0x70, tru.exe EnemyRegenerateData 0x67ED20 / hit filter 0x682490): a body with
// time > 0 lies down for that long and gets back up at full health (thralls), unless a hit matching one of `kills`
// lands while it is down (0x67F040: source -1 = any, type 0 = any, flag bit 0 = the hit's +0x6E byte).
struct Regenerate {
    std::string clip, up; // +0x20 clip ref: falls (played on entering, 0x67ED20); +0x24: gets back up
    float time = 0;    // +0x28 s down ([I] 30 Hz counts); used only with a clip (0x67F040: ref +0x22 or +0x26 set)
    struct Kill { int32_t source = -1, type = 0; bool flag = false; };
    std::vector<Kill> kills; // count +0x64, 0xC-byte entries at +0x68
};

// Numbers per enemy type. Provenance ([O] observed / [I] inferred) in docs/GAMEPLAY.md "Enemies".
struct EnemyTuning {
    float radius = 94.8167f;       // body capsule radius; [O] enemy data +0x2C (0.3 m of 316.056 units)
    float height = 300.0f;         // body capsule height; loadEnemyType: skeleton bind-pose top
    float health = 200.0f;         // hit points, Lara's health scale (TOMB RAIDER level)
    // Health block {CASUAL, TOMB RAIDER, MASTER} picked by Tailoring::enemyHealth; non-positive -> `health` (0x6E77B0).
    std::array<float, 3> healthLevels{};
    float healthAt(int level) const { return level >= 0 && level < 3 && healthLevels[size_t(level)] > 0 ? healthLevels[size_t(level)] : health; }
    float staggerDamage = 0;       // damage that plays a hit reaction (accumulated since the last one); 0 never [I]
    Senses senses, combatSenses;   // idle / once alerted
    float attackRange = 474.08f;   // starts an attack when Lara is this close (centre to centre)
    float attackReach = 632.11f;   // a blow lands if Lara is inside this during the hit window
    float attackDamage = 120.0f;   // per blow, Lara health points (before Combat's difficulty scale); records override
    int attackType = kDamageLight;
    float hitStart = 0.35f, hitEnd = 0.6f; // hit window, fraction of the attack clip
    float attackCooldown = 0.5f;   // s between blows
    float turnRate = 5.0f;         // rad/s while chasing / attacking
    float loseTime = 4.0f;         // s without seeing Lara before giving up the chase
    float stepUp = 70.0f, stepDown = 140.0f; // ground snap window
    float maxDrop = 300.0f;        // ledge avoidance: never walks off a drop deeper than this
    float minFloorNormalZ = 0.7f;
    float gravity = 5.72443f * 900.0f; // units/s^2 (tru.exe 5.72443 units per 30 Hz frame^2)
    float flySpeed = 700.0f;       // [I] flyers, units/s
    std::vector<Attack> attacks;   // every record, node order
    std::vector<DeathClip> deaths;
    Regenerate regenerate;
    float deathImpulse = 0;        // death record +0x4C (thralls 2000 for their chunks; soldiers, cats 0)
    float deathTimer = 4.0f;       // death record s16 +0x1C (120 counts): a ragdoll refused (6 live) is retried this late
};

// Weighted animation list entry (tru.exe 0x60F450: list +0x0C count, 0xC-byte entries at +0x10 {clip ref, u32
// weight, s16 +8, s16 +0xA}): picked by weight, played for [I] +8..+0xA counts (30 Hz).
struct IdleAnim {
    std::string clip;
    uint32_t weight = 0;
    float minTime = 0, maxTime = 0; // s
};
// Patrol parameters (behaviour-list node +0x48; EnemyBehavior_Patrol* 0x61AED0 / 0x628990 / 0x6285F0): the walk
// clip of its MoveNav set (+0x00 -> set +0x18), the idles played at each point (+0x04 list, when +0x0C > 0), the
// arrival distance (set s16 max(+0x1C0, +0x1C2)). PatrolLookTurn (0x628730 / 0x628800) turns with the set's +0x58 /
// +0x5C clips (90 left / right); PatrolAttack (0x635920 / 0x61B260) uses record +0x10 (point mode 2) / +0x14 (3).
struct PatrolStyle {
    std::string walk, turnLeft, turnRight;
    std::vector<IdleAnim> idles;
    float arrive = 0;
    std::array<int, 2> attack{-1, -1}; // tuning.attacks index, -1 none
};
// Search parameters (behaviour-list node +0xD8; EnemyBehavior_Search* 0x61E480..0x637130): its MoveNav walk clip
// (+0x00 -> set +0x18), the start clip (+0x04), the idles (+0x24 list), [I] the whole search time (s32 +0x18
// counts); [I] possible positions: `step` within ±`angle` of its heading (the exe asks its nav mesh, 0x630B90).
struct SearchStyle {
    std::string walk, start;
    std::vector<IdleAnim> idles;
    float time = 0, step = 0, angle = 0; // s, units, rad
    float arrive = 0;
};
// Flank (behaviour-list node +0xCC record R = +0x10; EnemyBehavior_Flank 0x6365B0 / 0x639B20 / 0x62A180, position
// search 0x637D10 -> 0x632F80, cost 0x622670, side 0x62D930). Gathers the planner nodes within `gather` (enemy data
// s16 +0x0E) of it and takes the cheapest under `maxCost`; costs and fields in docs/GAMEPLAY.md "Flank".
struct FlankStyle {
    bool valid = false;
    std::string run;    // R+0x00 MoveNav set +0x18 (type-2 for soldiers: "" = the run role clip)
    float arrive = 0;   // the set's s16 max(+0x1C0, +0x1C2)
    bool inside = false; // R+0x04 bit 0: Lara must be inside [minRange, maxRange) (else outside)
    float travelW = 0, laraW = 0, dirW = 0, coneW = 0, maxCost = 0; // R+0x08, +0x0C, +0x10, +0x14, +0x18
    float frontRange = 0, frontAngle = 0; // R+0x1C, R+0x28 degrees (0x62D930: Lara behind it)
    float minRange = 0, maxRange = 0;     // R+0x20 / +0x24
    float closeDist = 0, closeCost = 0, nearCost = 0, travel = 0, lara = 0, nearDist = 0; // s16 +0x2C, +0x30, +0x34, s16 +0x38, +0x3A, +0x44
    float gather = 0;
};
// Sidestep (node +0xE4; EnemyBehavior_Sidestep 0x677D70 / 0x678530 / 0x678B50 / 0x674320): Lara closer than `trigger`
// -> a random side, `step` away; its MoveNav set's +0x34 / +0x38 clips (left / right). Cooldowns +0x0C after a step,
// +0x10 after a failed try ([I] 30 Hz counts).
struct DodgeStyle {
    std::string left, right;
    float step = 0, trigger = 0, rest = 0, retry = 0;
};
// Investigate (node +0x4C; EnemyBehavior_Investigate 0x61DA60 / 0x61DD10, InvestigateStart / Move / End
// 0x61DF40 / 0x62C070 / 0x61E320): a disturbance message (0xBF: Lara's shots, 0x6FFFC0 / 0x69E510) far (> 1580) from the
// last one makes it walk there (+0x08 MoveNav set) between its +0x0C / +0x10 clips; [I] `hear`: +0x18.
struct InvestigateStyle {
    std::string walk, start, end;
    float arrive = 0, hear = 0;
};

struct EnemyType {
    std::string name;    // objectlist.txt name ("cat_panther")
    uint16_t objectId = 0;
    int32_t enemyClass = -1; // enemy data +0x04 (3 cats, 8 thralls, 4 soldiers, 7 spiders, 1 bats); -1 synthetic
    int behaviour = -1;      // enemy data +0x0C byte (0 soldiers, 1 cats, 2 bats / sharks, 3 thralls)
    scene::Skeleton skeleton;
    std::map<std::string, anim::Clip, std::less<>> clips; // role, record and death clips, by animation-list name
    std::set<std::string, std::less<>> lyingDeaths;       // death clips whose last frame lies on the floor
    std::array<std::string, size_t(Role::Count)> roles;    // clip per role; "" = the type has none
    EnemyTuning tuning;
    const anim::Clip* clip(Role r) const;
    const std::string& clipName(Role r) const { return roles[size_t(r)]; }
    uint16_t weapon = 0;     // list node +0x20 weapon record u16 +2: weapon object id (poacher 114 wp_akrifle); 0 none
    std::string weaponObject; // its objectlist name ("wp_akrifle"): Lara picks it up from the body (World::enemyDrops)
    // The gun model rides on this model's HInfo marker (weapon record +8, a segment field: poachers -75; 0x614950 -> +0x2C)
    // with the marker's frame:
    // `gunSegment` (-1: no such marker, the gun at the shot origin [I]) and the marker in that segment's frame. `muzzle`
    // = the gun's shot origin in its model (WeaponDef::muzzle).
    int gunMarker = 0, gunSegment = -1;
    glm::mat4 gunMount{1}, muzzle{1};
    bool sleeps = false;     // has sleepClips: tranquilizer darts put it to sleep (cats, nagas)
    std::array<std::string, 3> sleepClips; // start, loop, wake: sleep_start/_loop/_end or tranq_sleep_start/_loop, rest_to_idle [I]
    std::string fallClip, landClip; // dropping off a wall (Mode::Drop): fall_loop / falling_loop, jump_end / fall_land_front
    int spit = -1;           // attacks[] index of a ranged spit (green naga +0x88 spit_attack), -1 none
    PatrolStyle patrol;      // behaviour-list node +0x48
    SearchStyle search;      // behaviour-list node +0xD8; time 0: no search (spiders), loses Lara straight to idle
    FlankStyle flank;        // node +0xCC
    DodgeStyle dodge;        // node +0xE4; no clips: no sidestep
    InvestigateStyle investigate; // node +0x4C; no walk clip: does not investigate
    // Ragdoll (ObjectBase +0x7C, physics::loadRagdoll) and each skeleton segment's bind position (model space).
    std::optional<physics::RagdollDef> ragdoll;
    std::vector<vec3> bind;
    std::optional<ChunkDoll> chunks; // thralls: death record +0x46
    // Wall crawlers (enemy data class 5 / 6 nagas, 7 spiders): their tilted intros cling to walls and ceilings.
    bool crawler() const { return enemyClass == 5 || enemyClass == 6 || enemyClass == 7; }
    bool regenerates() const { return !tuning.regenerate.clip.empty() || !tuning.regenerate.up.empty(); }
    bool flyer() const { return behaviour == 2; }
    // The shared flyer behaviour tree also drives sharks; their animation set selects the aquatic controller.
    bool aquatic() const { return flyer() && clipName(Role::Run).starts_with("swim_"); }
    // Armed, with a fire record whose shot clip we have (soldiers). Cats' records may name a +0x7C clip too.
    bool ranged() const {
        return weapon && std::any_of(tuning.attacks.begin(), tuning.attacks.end(), [&](const Attack& a) { return a.ranged() && clips.count(a.shotClip); });
    }
    bool spawnable() const { return clip(Role::Idle) && clip(Role::Run) && clip(Role::Death) && (clip(Role::Attack) || ranged()); }
};
// Loads pc-w\<name>.drm: model-0 skeleton, role clips (first present of kRoleCandidates), tuning (enemy data at
// ObjectBase+0xC8, magic 0xDAF00018). Throws cdc::FormatError / std::runtime_error when it is not an enemy object.
std::shared_ptr<const EnemyType> loadEnemyType(const cdc::BigfileSet& bigfile, const std::string& name);
// Clip names tried per role, first match wins (cat and thrall clip sets).
extern const std::array<std::vector<std::string_view>, size_t(Role::Count)> kRoleCandidates;
// Enemy objects: ObjectBase oflags2 bit 0x400000 (tru.exe 0x5BF0E0 picks the "enemy" family, 0x8ABA28 entry 14).
bool isEnemyObject(const cdc::BigfileSet& bigfile, const std::string& name);

// Sleep: knocked out by tranquilizer darts (EnemyBehavior_Nap / EnemyState_Nap in tru.exe; docs/GAMEPLAY.md "Combat").
// Cling / Drop: a crawler on a wall or ceiling, dropping to the floor. Down / Rise: a thrall's regenerate (lying, then
// getting back up at full health). Return: walking back into its guard zone (EnemyBehavior_MoveToGuardZone).
// Cover: running to a cover node, crouched behind it, standing up (EnemyBehavior_Cover / AtCover*).
// Crawl: a clinging crawler walking along its wall or ceiling towards Lara.
// Patrol: walking its intro's patrol path, an idle at each point (EnemyBehavior_Patrol / PatrolMove / PatrolAnim /
// PatrolIncr). Search: lost Lara: start clip, to where she was, idles and steps to possible positions, then back to
// its patrol or post (EnemyBehavior_Search / SearchStart / SearchToLastKnownPos / SearchIdle / SearchToPossiblePos).
// Flank: running to a planner node chosen around Lara (EnemyBehavior_Flank). Dodge: a sidestep (EnemyBehavior_Sidestep).
// Investigate: walking to a disturbance (EnemyBehavior_Investigate*).
enum class Mode { Idle, Alert, Chase, Attack, Hit, Dead, Spawning, Fire, Reload, Sleep, Cling, Drop, Down, Rise, Return, Cover, Crawl,
                  Patrol, Search, Flank, Dodge, Investigate };
std::string_view modeName(Mode m);

// What enemies hunt: Lara's feet and whether she can still be hurt.
struct Target {
    vec3 pos{0};
    bool alive = true;
    float height = 512.0f; // Lara's capsule; enemies look at pos + 0.75 height
    float radius = 100.0f; // game::Tuning::radius; enemy shots test this capsule
    bool inWater = false;  // swimming (PlayerState::water >= 0): sharks only sense her then
};
// One enemy shot (this tick): muzzle -> where it stopped (Lara, a wall, or its range); hit = it hurt Lara.
struct Shot {
    int32_t uid = 0;
    vec3 from{0}, to{0};
    bool hit = false;
    glm::mat4 gun{1}; // the gun model's world frame: its "fire" events play there
};
using HurtFn = std::function<void(float damage, vec3 from, int type)>; // type: game::kDamageLight / kDamageHeavy

struct EnemyState {
    Mode mode = Mode::Idle;
    vec3 pos{0}; // feet
    float yaw = 0; // heading: forward = (cos yaw, sin yaw, 0)
    float vz = 0;  // falling speed
    vec3 knock{0}; // thrown (Hit::knockVel, message 0x91): horizontal speed until it lands; a ragdoll starts with it
    std::string clip;
    double clipTime = 0;
    bool loop = true;
    float health = 0;
    float modeTime = 0;   // s in the current mode
    float unseenTime = 0; // s since Lara was last perceived
    float cooldown = 0;
    bool blowLanded = false; // this attack already hurt Lara
    float damageSinceReaction = 0;
    vec3 lastSeen{0};
    int fire = -1;           // Fire / Reload: index into tuning.attacks
    int shotsLeft = 0;
    float shotTimer = 0;     // s to the next shot
    std::array<float, 4> fireCooldown{}; // s per fire record (first four)
    uint32_t rng = 1;        // LCG for shot accuracy (seeded by uid)
    float tip = 0;           // dead: rad the body has tipped over onto its side (clips ending upright)
    float sleep = 0;         // Sleep: seconds left in sleep_loop
    float drowsy = 0;        // tranquilizer dose since it last woke (sleeps at its health)
    glm::vec2 tilt{0};       // Cling: the intro's rotation x / y (R = Rx Ry Rz(yaw + 90°)); 0 upright
    float down = 0;          // Down: seconds left lying
    int melee = -1;          // Attack: tuning.attacks index of the blow (-1: the role clip's record)
    vec3 cover{0};           // Cover: the node it hides at
    float coverCooldown = 0; // s before it seeks cover again
    int path = 0;            // patrol path (enemybase 22 n; 0 the intro's own)
    int patrolIndex = -1;    // point it walks to / idles at; -1 none or done (0x637870 +0x34)
    int patrolStep = 1;      // ping-pong direction (+0x44)
    uint32_t visited = 0;    // mode 4: points visited this round (+0x654)
    int phase = 0;           // Patrol: 0 walking, 1 idle at the point, 2 look-turn; Search: 0 start, 1 to last known, 2 idle, 3 to a
                             // possible position; Investigate: 0 start clip, 1 walking there, 2 end clip
    float phaseTime = 0, phaseLength = 0; // s in the phase, s it lasts (idles)
    vec3 goal{0};            // Search / Flank / Investigate: where it walks
    float searchTime = 0;    // s searching
    float flankWait = 0, dodgeWait = 0; // s before it flanks / sidesteps again
    std::array<vec3, 8> recent{};       // its last flank destinations (0x61F6B0's 8 positions: +2000 cost within 640)
    int recentNext = 0, recentCount = 0;
    // Physics takeover of a dead body, `ragdollAt` s into Mode::Dead (the death entry's delay): ragdoll
    // (EnemyType::ragdoll) 0 none, 1 starting (World builds the bodies at `ragdoll` with the killing hit's impulse), 2
    // simulating, 3 settled (the pose stays, the bodies are gone); 4 broken into chunks (EnemyType::chunks; the body is
    // gone, World builds the pieces at the body's last pose when `pieces` is empty).
    int ragdollPhase = 0;
    float ragdollAt = 0;
    std::vector<glm::mat4> pieces; // phase 4: each chunk piece's model -> enemy model space (modelMatrix(pos, yaw, 0, tilt))
    std::vector<vec3> pieceVel;    // pieces made this tick: each one's velocity (units/s) and spin (rad/s), world
    std::vector<glm::mat4> ragdoll; // each ragdoll bone's start-segment frame, model space (modelMatrix(pos, yaw, 0, tilt))
    vec3 killAt{0}, killDir{0};     // the killing hit: point relative to pos, direction
};

// A guard zone (enemybase 27 GuardNode, tru.exe 0x61F7A0: up to six {point, inner, outer = max(r1, r2)}).
struct GuardZone {
    vec3 point{0};
    float inner = 0, outer = 0;
    // 0x61F810: within 320 along the up axis and within the radius across it
    bool contains(vec3 p, bool outerRadius) const;
};

// The loaded units' nav graphs of one layer, world frame (Nav.cpp): the areas, A* through their shared edges, funnel.
class NavMesh {
public:
    void add(const formats::NavGraph& g, vec3 offset); // appends the graph's areas (adjacency stays within it)
    bool empty() const { return areas_.empty(); }
    size_t size() const { return areas_.size(); }
    float radius = 0;                    // the agent radius it was built for
    int locate(vec3 feet) const;         // area under / around these feet, -1 off the mesh
    // Corners from `from` to `to` (the last one `to`); empty when either end is off the mesh or no route joins them.
    std::vector<vec3> path(vec3 from, vec3 to) const;
    vec3 steer(vec3 from, vec3 to) const; // the first corner more than 10 away; `to` without a route

private:
    struct Area {
        vec3 centre{0}, normal{0, 0, 1};
        std::vector<vec3> verts;
        std::vector<int> adj;
        glm::vec2 lo{0}, hi{0};
        bool ccw = true;
    };
    float floorZ(const Area& a, vec3 p) const;
    std::vector<Area> areas_;
};

// A patrol point: its planner node's position, facing (cdc -Y of the node frame), PatrolLookTurn flag (node +0x50
// bit 1) and point mode (node +0x40, < 1: the path's; Intro::PatrolPath::pointMode).
struct PatrolPoint {
    vec3 pos{0}, facing{0, -1, 0};
    bool look = false;
    int mode = 0;
};

class Enemy final : public Damageable {
public:
    // `healthLevel`: Tailoring::enemyHealth (EnemyTuning::healthAt).
    // `tilt`: a crawler's intro rotation x / y (clings when the intro is not upright).
    Enemy(std::shared_ptr<const EnemyType> type, int32_t uid, vec3 pos, float yaw, bool spawning = false, int healthLevel = 1,
          glm::vec2 tilt = {});

    // Appends this tick's shots to `shots` (may be null).
    void tick(const collision::CollisionMesh& col, const Target& lara, const HurtFn& hurt, float dt, std::vector<Shot>* shots = nullptr);
    bool perceives(const collision::CollisionMesh& col, const Target& lara) const;
    const Senses& senses() const {
        return s_.mode == Mode::Idle || s_.mode == Mode::Patrol || s_.mode == Mode::Investigate ? type_->tuning.senses : type_->tuning.combatSenses;
    }

    // Damageable
    vec3 position() const override { return s_.pos; }
    float radius() const override { return type_->tuning.radius; }
    float height() const override { return type_->tuning.height; }
    float health() const override { return s_.health; }
    Team team() const override { return Team::Enemy; }
    void onHit(const Hit& hit) override;

    int32_t uid() const { return uid_; }
    const EnemyType& type() const { return *type_; }
    const EnemyState& state() const { return s_; }
    const EnemyState& previous() const { return prev_; }
    std::string unit;     // owning unit (streaming); "" = added by hand
    int32_t generator = 0; // spawned by this generator intro (0 = placed)
    std::vector<GuardZone> zones;     // world frame; empty: guards nothing
    // Planner nodes of the loaded units (world frame) and the nav mesh of its layer (null: none, straight lines), set by
    // Enemies each tick.
    std::span<const formats::PlannerNode> nodes;
    const NavMesh* nav = nullptr;
    void alert(vec3 at);              // a squad mate saw Lara at `at` (idle / patrolling / searching / returning: turns, then hunts)
    void hear(vec3 at);               // a disturbance (Lara's shot) at `at`: investigates it when calm (EnemyBehavior_Investigate)
    // Patrol paths from the intro data (world frame; [0] its own, [n] enemybase 22 alternatives) and their order
    // mode (Intro::patrolMode). setPatrol starts path `path` at its first point (modes 3 / 4: a random one).
    std::vector<std::vector<PatrolPoint>> patrolPaths;
    uint32_t patrolMode = 0;
    void setPatrol(int path);
    // Points other enemies walk to or stand at this tick (patrol points, flank nodes; mode 3 and flanks pick a free
    // one: 0x633F10 / 0x61F5D0), set by Enemies.
    std::span<const vec3> claimed;
    // Live ragdolls (phases 1 / 2) and chunk sets (4) of the other enemies, set by Enemies: tru.exe allows 6 ragdolls
    // (0x60EF50) and 4 chunk sets (0x696F90).
    int ragdolls = 0, chunkSets = 0;

    void restore(const EnemyState& s); // streamed back in / loaded: no interpolation, no blend
    const EnemyState& start() const { return start_; } // as constructed (placement, full health)
    EnemyState interpolated(double alpha) const;
    // Model-space bone matrices blended between the last two ticks; skin with anim::skinPalette.
    std::vector<glm::mat4> pose(double alpha) const;
    // Root at pos, model -Y = heading; tip: rolled onto its side; tilt: clinging (Rx(x) Ry(y) before the heading).
    static glm::mat4 modelMatrix(vec3 pos, float yaw, float tip = 0, glm::vec2 tilt = {});
    // The ragdoll's bone frames (world) after a physics step (World::stepRagdolls); asleep or kRagdollSeconds after the
    // death: settled.
    void setRagdoll(std::span<const glm::mat4> world, bool asleep);
    // Phase 4: each chunk piece's world model matrix (World::startChunks with their starting velocities, then after
    // every physics step).
    void setPieces(std::span<const glm::mat4> world, std::span<const vec3> vel = {});
    static constexpr float kRagdollSeconds = 20.0f; // [I] then the pose freezes even if a body still moves

private:
    void setMode(Mode m);
    void play(Role r, bool loop);
    void playClip(const std::string& name, bool loop);
    bool clipDone() const;
    vec3 advanceClip(float dt);
    void turnTowards(vec3 p, float dt);
    float aimError(vec3 p) const; // |heading - bearing to p|, rad
    void move(const collision::CollisionMesh& col, vec3 delta, float dt);
    void fly(const collision::CollisionMesh& col, vec3 delta);
    void die(const Hit& hit);
    bool getsUp(const Hit& hit) const; // Regenerate::kills match (0x67F040)
    vec3 up() const;                    // body up axis (tilted while clinging)
    int readyFire() const; // first fire record off cooldown, -1 none
    void shoot(const collision::CollisionMesh& col, const Target& lara, const HurtFn& hurt, std::vector<Shot>* shots);
    void updatePose(float dt, bool reset);
    void notice(); // saw Lara itself: alert (and bark)
    vec3 zoneGoal(vec3 p) const;   // 0x61F990: p pulled inside zone 0 (0.8 x outer)
    bool inZone(bool outerRadius) const;
    bool seekCover(const collision::CollisionMesh& col, const Target& lara);
    bool flank(const Target& lara);  // picks a flank node and runs (Mode::Flank); false: none
    bool dodge(const collision::CollisionMesh& col, const Target& lara); // a sidestep (Mode::Dodge); false: none now
    bool crawl(const collision::CollisionMesh& col, const Target& lara, float dt);
    const std::vector<PatrolPoint>* patrolPoints() const; // the current path, null without one
    bool nextPatrolPoint(uint32_t mode);            // 0x637870 in that order mode; false: the path is done
    void playIdle(const std::vector<IdleAnim>& idles); // a weighted pick; sets phaseLength
    vec3 steer(vec3 goal) const;     // the next nav corner towards goal (goal itself without a nav mesh)
    // Turns along the nav path (flyers: straight, and `flight` set to the step there); true: arrived.
    bool walkTo(vec3 goal, float arrive, const std::string& clip, float dt, vec3* flight = nullptr);
    void startSearch();
    void ragdollOffsets(const std::vector<glm::mat4>& pose); // ragdollOwner_ / ragdollOffset_ from a death pose
    std::vector<glm::mat4> ragdollBones() const;             // model space, from s_.ragdoll

    std::shared_ptr<const EnemyType> type_;
    int32_t uid_ = 0;
    EnemyState s_, prev_, start_;
    anim::AnimController anim_;
    std::vector<glm::mat4> bones_, prevBones_;
    uint32_t clipSerial_ = 0, poseSerial_ = 0; // playClip count; the one anim_ plays
    // Each skeleton segment follows a ragdoll bone (its nearest ancestor-or-self that starts one) with its death-pose
    // offset from that bone's segment [I].
    std::vector<int> ragdollOwner_;
    std::vector<glm::mat4> ragdollOffset_;
};

// A spawn generator intro (objectlist "*_generator"): spawns its object (generator ObjectBase +0xDE) once.
struct Generator {
    int32_t uid = 0;
    std::string unit;
    std::string object; // spawned enemy object name
    vec3 pos{0};
    float yaw = 0;
    float range = 2528.45f; // [I] spawns when Lara comes this close (8 m), at most 2000 units above/below
    bool fired = false;
    bool enabled = true; // generatorbase Enable (3) / Disable (4): tru.exe generator flag 0x84 bit 0 clear / set
    // The generator intro's own enemy data (type 13): its spawn walks these paths (as a placed enemy's).
    std::vector<std::vector<PatrolPoint>> patrolPaths;
    uint32_t patrolMode = 0;
};

// Every enemy of the loaded units: placed enemy intros and generator spawns. World ticks it after Lara.
class Enemies {
public:
    // `targets` (World::targets): every enemy is registered there while it exists (removed before destroyed).
    explicit Enemies(std::vector<Damageable*>* targets = nullptr) : targets_(targets) {}
    Enemies(const Enemies&) = delete;
    Enemies& operator=(const Enemies&) = delete;
    void setBigfile(const cdc::BigfileSet* bigfile) { bigfile_ = bigfile; }
    void setHealthLevel(int level) { healthLevel_ = level; } // Tailoring::enemyHealth for enemies added from now on
    // Spawns the enemies and generators of newly loaded units; drops (and remembers) those of unloaded ones.
    void setUnits(const std::vector<script::UnitRef>& units);
    // Dormant intros (formats::dormant) are not spawned until birth(uid) unless `born` says so (World:
    // script::Runtime::born, instanceref Birth). Unset: every intro spawns.
    std::function<bool(int32_t uid)> born;
    void birth(int32_t uid);
    Enemy& add(std::shared_ptr<const EnemyType> type, int32_t uid, vec3 pos, float yaw, bool spawning = false, glm::vec2 tilt = {});
    void addGenerator(Generator g) { generators_.push_back(std::move(g)); }
    // enemybase 27 GuardNode(unitref, noderef {record, node}, r1, r2) on the intro `uid` (resolved against its unit's
    // planner nodes once the enemy exists; at most six, repeats ignored).
    void guard(int32_t uid, uint32_t record, uint32_t node, float r1, float r2);
    // enemybase 22 n (0x63AC40): patrol path n of the intro `uid` (0 its own); false: no such enemy.
    bool selectPatrol(int32_t uid, int path);
    void addType(std::shared_ptr<const EnemyType> t) { types_[t->name] = std::move(t); } // spawnable by name without a bigfile
    // A checkpoint was reached: remembers which enemies (by uid; generator spawns by their generator's) are dead.
    void checkpoint();
    // Lara respawned at that checkpoint [I]: enemies dead then stay dead (their generators stay fired); every
    // other enemy is back at its placement with full health, generator spawns are removed and generators re-armed.
    void reset();
    // Save games: every unit-spawned enemy's full state in its unit's frame (loaded and streamed out; generator
    // spawns by their generator's uid), fired generators and the checkpoint's dead set.
    nlohmann::json serialize() const;
    // Replaces all unit-spawned state with a serialize() result. The enemies come back from it on the next
    // setUnits() (World::restoreEnemies calls it at once). Throws std::runtime_error on a malformed value.
    void restore(const nlohmann::json& j);
    // Ticks every enemy; `shots` then holds this tick's enemy shots.
    void tick(const collision::CollisionMesh& col, const Target& lara, const HurtFn& hurt, float dt);
    // A disturbance message (tru.exe 0xBF, broadcast to every enemy by 0x655280; Lara's shots): calm enemies with an
    // investigate record that hear it walk there (Enemy::hear).
    void disturb(vec3 at);
    const NavMesh* navFor(const EnemyType& t) const; // the loaded layer built for its radius (null: none / flyers)
    std::vector<Shot> shots;
    struct Bark { int32_t uid = 0; std::string category; vec3 pos{0}; }; // a soldier's chatter line this tick
    std::vector<Bark> barks;
    const std::vector<std::unique_ptr<Enemy>>& all() const { return enemies_; }
    const std::vector<Generator>& generators() const { return generators_; }
    // Script natives on an enemy or generator intro (tru.exe NsEnemyBase / NsGeneratorBase bind tables 0x7B4060,
    // 0x7B4B60; docs/formats/SCRIPTS.md). False: no such object, or a function not modelled.
    bool generatorNative(int32_t uid, uint32_t function, uint32_t& result);
    bool damage(int32_t uid, float amount); // health 1/2 on an enemy's own script
    uint64_t version() const { return version_; } // bumped whenever all() gains or loses an enemy
    std::vector<std::string> warnings;

private:
    std::shared_ptr<const EnemyType> typeOf(const std::string& name);
    void sync(); // targets_ <- enemies_
    void spawn(Generator& g);
    void spawnIntro(const std::string& unit, const formats::Intro& in); // one intro of a loaded unit (setUnits / birth)
    std::map<int32_t, std::pair<std::string, formats::Intro>> dormant_; // unborn intros of the loaded units: unit, intro

    const cdc::BigfileSet* bigfile_ = nullptr;
    std::map<std::string, std::shared_ptr<const EnemyType>, std::less<>> types_;
    std::map<uint16_t, std::string> objectNames_;
    std::vector<std::unique_ptr<Enemy>> enemies_;
    std::vector<Generator> generators_;
    // state (pos in its unit's frame) of enemies whose unit streamed out, by uid (generator spawns: generator uid)
    std::map<int32_t, EnemyState> stash_;
    std::set<int32_t> firedGenerators_;   // of units that streamed out
    std::set<int32_t> deadAtCheckpoint_;  // checkpoint()
    std::vector<std::string> units_;      // units whose intros were spawned
    std::map<std::string, glm::vec3> offsets_; // their stream offsets
    std::map<std::string, std::vector<std::vector<formats::PlannerNode>>> nodes_; // their planner nodes (unit frame)
    std::vector<formats::PlannerNode> worldNodes_;                                // all of them, world frame
    std::map<std::string, std::vector<formats::NavGraph>> graphs_;                // their nav graphs (unit frame)
    std::map<uint32_t, NavMesh> navs_;                                            // all of them by layer, world frame
    std::vector<vec3> claims_; // patrol points / flank nodes enemies walk to / idle at this tick
    struct Guard { uint32_t record = 0, node = 0; float inner = 0, outer = 0; auto operator<=>(const Guard&) const = default; };
    std::map<int32_t, std::vector<Guard>> guards_; // by intro uid; saved
    void resolveZones(Enemy& e) const;
    std::map<std::string, std::string> generatorObjects_; // generator object -> spawned object
    std::map<std::string, int> enemyObjects_;              // object -> 1 enemy, 0 not
    std::vector<Damageable*>* targets_ = nullptr;
    std::vector<Damageable*> registered_; // our entries in *targets_
    uint64_t version_ = 0;
    int healthLevel_ = 1;
    float barkWait_ = 0; // s until the next bark
};

} // namespace ou::game::ai
