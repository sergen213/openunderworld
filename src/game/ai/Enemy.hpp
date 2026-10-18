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

// Numbers per enemy type. Provenance ([O] observed / [I] inferred) in docs/GAMEPLAY.md "Enemies".
struct EnemyTuning {
    float radius = 94.8167f;       // body capsule radius; [O] enemy data +0x2C (0.3 m of 316.056 units)
    float height = 300.0f;         // body capsule height; loadEnemyType: skeleton bind-pose top
    float health = 200.0f;         // hit points, Lara's health scale
    float staggerDamage = 0;       // damage that plays a hit reaction (accumulated since the last one)
    float sightRange = 3160.56f;   // sees Lara inside this distance and cone, with a clear line
    float sightHalfAngle = 1.0472f; // rad (60 degrees)
    float senseRange = 790.14f;    // notices Lara in any direction inside this (2.5 m)
    float attackRange = 474.08f;   // starts an attack when Lara is this close (centre to centre)
    float attackReach = 632.11f;   // a blow lands if Lara is inside this during the hit window
    float attackDamage = 120.0f;   // per blow, Lara health points (before Combat's difficulty scale)
    float hitStart = 0.35f, hitEnd = 0.6f; // hit window, fraction of the attack clip
    float attackCooldown = 0.5f;   // s between blows
    float turnRate = 5.0f;         // rad/s while chasing / attacking
    float loseTime = 4.0f;         // s without seeing Lara before giving up the chase
    float stepUp = 70.0f, stepDown = 140.0f; // ground snap window
    float maxDrop = 300.0f;        // ledge avoidance: never walks off a drop deeper than this
    float minFloorNormalZ = 0.7f;
    float gravity = 5.72443f * 900.0f; // units/s^2 (tru.exe 5.72443 units per 30 Hz frame^2)
};

struct EnemyType {
    std::string name;    // objectlist.txt name ("cat_panther")
    uint16_t objectId = 0;
    int32_t enemyClass = -1; // enemy data +0x04 (3 cats, 8 thralls, 4 soldiers, 7 spiders, 1 bats); -1 synthetic
    scene::Skeleton skeleton;
    std::map<std::string, anim::Clip, std::less<>> clips; // the role clips, by animation-list name
    std::array<std::string, size_t(Role::Count)> roles;    // clip per role; "" = the type has none
    EnemyTuning tuning;
    const anim::Clip* clip(Role r) const;
    const std::string& clipName(Role r) const { return roles[size_t(r)]; }
    bool melee() const { return clip(Role::Idle) && clip(Role::Run) && clip(Role::Attack) && clip(Role::Death); }
};
// Loads pc-w\<name>.drm: model-0 skeleton, role clips (first present of kRoleCandidates), tuning (enemy data at
// ObjectBase+0xC8, magic 0xDAF00018). Throws cdc::FormatError / std::runtime_error when it is not an enemy object.
std::shared_ptr<const EnemyType> loadEnemyType(const cdc::BigfileSet& bigfile, const std::string& name);
// Clip names tried per role, first match wins (cat and thrall clip sets).
extern const std::array<std::vector<std::string_view>, size_t(Role::Count)> kRoleCandidates;
// Enemy objects: ObjectBase oflags2 bit 0x400000 (tru.exe 0x5BF0E0 picks the "enemy" family, 0x8ABA28 entry 14).
bool isEnemyObject(const cdc::BigfileSet& bigfile, const std::string& name);

enum class Mode { Idle, Alert, Chase, Attack, Hit, Dead, Spawning };
std::string_view modeName(Mode m);

// What enemies hunt: Lara's feet and whether she can still be hurt.
struct Target {
    vec3 pos{0};
    bool alive = true;
    float height = 512.0f; // Lara's capsule; enemies look at pos + 0.75 height
};
using HurtFn = std::function<void(float damage, vec3 from)>;

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
};

class Enemy final : public Damageable {
public:
    Enemy(std::shared_ptr<const EnemyType> type, int32_t uid, vec3 pos, float yaw, bool spawning = false);

    void tick(const collision::CollisionMesh& col, const Target& lara, const HurtFn& hurt, float dt);
    bool perceives(const collision::CollisionMesh& col, const Target& lara) const;

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
    EnemyState interpolated(double alpha) const;
    // Model-space bone matrices blended between the last two ticks; skin with anim::skinPalette.
    std::vector<glm::mat4> pose(double alpha) const;
    static glm::mat4 modelMatrix(vec3 pos, float yaw); // root at pos, model -Y = heading

private:
    void setMode(Mode m);
    void play(Role r, bool loop);
    bool clipDone() const;
    vec3 advanceClip(float dt);
    void turnTowards(vec3 p, float dt);
    void move(const collision::CollisionMesh& col, vec3 delta, float dt);
    void updatePose(float dt, bool reset);

    std::shared_ptr<const EnemyType> type_;
    int32_t uid_ = 0;
    EnemyState s_, prev_;
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
};

// Every enemy of the loaded units: placed enemy intros and generator spawns. World ticks it after Lara.
class Enemies {
public:
    // `targets` (World::targets): every enemy is registered there while it exists (removed before destroyed).
    explicit Enemies(std::vector<Damageable*>* targets = nullptr) : targets_(targets) {}
    Enemies(const Enemies&) = delete;
    Enemies& operator=(const Enemies&) = delete;
    void setBigfile(const cdc::BigfileSet* bigfile) { bigfile_ = bigfile; }
    // Spawns the enemies and generators of newly loaded units; drops (and remembers) those of unloaded ones.
    void setUnits(const std::vector<script::UnitRef>& units);
    Enemy& add(std::shared_ptr<const EnemyType> type, int32_t uid, vec3 pos, float yaw, bool spawning = false);
    void addGenerator(Generator g) { generators_.push_back(std::move(g)); }
    void tick(const collision::CollisionMesh& col, const Target& lara, const HurtFn& hurt, float dt);
    const std::vector<std::unique_ptr<Enemy>>& all() const { return enemies_; }
    const std::vector<Generator>& generators() const { return generators_; }
    uint64_t version() const { return version_; } // bumped whenever all() gains or loses an enemy
    std::vector<std::string> warnings;

private:
    std::shared_ptr<const EnemyType> typeOf(const std::string& name);
    void sync(); // targets_ <- enemies_

    const cdc::BigfileSet* bigfile_ = nullptr;
    std::map<std::string, std::shared_ptr<const EnemyType>, std::less<>> types_;
    std::map<uint16_t, std::string> objectNames_;
    std::vector<std::unique_ptr<Enemy>> enemies_;
    std::vector<Generator> generators_;
    std::map<int32_t, EnemyState> stash_; // state (pos in its unit's frame) of enemies whose unit streamed out, by uid
    std::set<int32_t> firedGenerators_;   // of units that streamed out
    std::vector<std::string> units_;      // units whose intros were spawned
    std::map<std::string, glm::vec3> offsets_; // their stream offsets
    std::map<std::string, std::string> generatorObjects_; // generator object -> spawned object
    std::map<std::string, int> enemyObjects_;              // object -> 1 enemy, 0 not
    std::vector<Damageable*>* targets_ = nullptr;
    std::vector<Damageable*> registered_; // our entries in *targets_
    uint64_t version_ = 0;
};

} // namespace ou::game::ai
