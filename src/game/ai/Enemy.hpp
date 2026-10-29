// Enemy AI: enemy instances from unit intros and spawn generators, perception, chase, melee attack, hit
// reaction, death. A small native state machine standing in for tru.exe's enemy behaviour trees
// (EnemyBehaviorTree_Creature / _Thrall, RTTI). Evidence, numbers and deviations: docs/GAMEPLAY.md "Enemies".
// Frame: Z-up world units (as World); model -Y is forward (as Lara).
#pragma once

#include "anim/Anim.hpp"
#include "collision/Collision.hpp"
#include "game/Combat.hpp"
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
// Death record (list node +0x64, tru.exe 0x66C500): count +0xC8, 0x14-byte entries at +0xCC {clip ref, ?, ?,
// category, flag}; FUN_00642210 picks by the attacker's quadrant (1 front, 3 left, 5 back, 7 right).
struct DeathClip {
    std::string clip; // "" = a type-2 ref (anim request) we do not resolve
    int category = 1;
    int flag = 0;     // 0 plain, 3 adrenaline / head shot, 5 running, 7 special (vault, thrown)
};

// Numbers per enemy type. Provenance ([O] observed / [I] inferred) in docs/GAMEPLAY.md "Enemies".
struct EnemyTuning {
    float radius = 94.8167f;       // body capsule radius; [O] enemy data +0x2C (0.3 m of 316.056 units)
    float height = 300.0f;         // body capsule height; loadEnemyType: skeleton bind-pose top
    float health = 200.0f;         // hit points, Lara's health scale (TOMB RAIDER level)
    // Health block {CASUAL, TOMB RAIDER, MASTER} picked by Tailoring::enemyHealth; non-positive -> `health` (0x6E77B0).
    std::array<float, 3> healthLevels{};
    float healthAt(int level) const { return level >= 0 && level < 3 && healthLevels[size_t(level)] > 0 ? healthLevels[size_t(level)] : health; }
    float staggerDamage = 0;       // damage that plays a hit reaction (accumulated since the last one)
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

enum class Mode { Idle, Alert, Chase, Attack, Hit, Dead, Spawning, Fire, Reload };
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
};
using HurtFn = std::function<void(float damage, vec3 from, int type)>; // type: game::kDamageLight / kDamageHeavy

struct EnemyState {
    Mode mode = Mode::Idle;
    vec3 pos{0}; // feet
    float yaw = 0; // heading: forward = (cos yaw, sin yaw, 0)
    float vz = 0;  // falling speed
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
};

class Enemy final : public Damageable {
public:
    // `healthLevel`: Tailoring::enemyHealth (EnemyTuning::healthAt).
    Enemy(std::shared_ptr<const EnemyType> type, int32_t uid, vec3 pos, float yaw, bool spawning = false, int healthLevel = 1);

    // Appends this tick's shots to `shots` (may be null).
    void tick(const collision::CollisionMesh& col, const Target& lara, const HurtFn& hurt, float dt, std::vector<Shot>* shots = nullptr);
    bool perceives(const collision::CollisionMesh& col, const Target& lara) const;
    const Senses& senses() const { return s_.mode == Mode::Idle ? type_->tuning.senses : type_->tuning.combatSenses; }

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

    void restore(const EnemyState& s); // streamed back in / loaded: no interpolation, no blend
    const EnemyState& start() const { return start_; } // as constructed (placement, full health)
    EnemyState interpolated(double alpha) const;
    // Model-space bone matrices blended between the last two ticks; skin with anim::skinPalette.
    std::vector<glm::mat4> pose(double alpha) const;
    static glm::mat4 modelMatrix(vec3 pos, float yaw, float tip = 0); // root at pos, model -Y = heading; tip: rolled onto its side

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
    int readyFire() const; // first fire record off cooldown, -1 none
    void shoot(const collision::CollisionMesh& col, const Target& lara, const HurtFn& hurt, std::vector<Shot>* shots);
    void updatePose(float dt, bool reset);

    std::shared_ptr<const EnemyType> type_;
    int32_t uid_ = 0;
    EnemyState s_, prev_, start_;
    anim::AnimController anim_;
    std::vector<glm::mat4> bones_, prevBones_;
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
    Enemy& add(std::shared_ptr<const EnemyType> type, int32_t uid, vec3 pos, float yaw, bool spawning = false);
    void addGenerator(Generator g) { generators_.push_back(std::move(g)); }
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
    std::vector<Shot> shots;
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
    std::map<std::string, std::string> generatorObjects_; // generator object -> spawned object
    std::map<std::string, int> enemyObjects_;              // object -> 1 enemy, 0 not
    std::vector<Damageable*>* targets_ = nullptr;
    std::vector<Damageable*> registered_; // our entries in *targets_
    uint64_t version_ = 0;
    int healthLevel_ = 1;
};

} // namespace ou::game::ai
