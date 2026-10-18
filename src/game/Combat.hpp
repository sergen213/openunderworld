// Combat: Lara's health/damage/death, weapons, targeting. Evidence and numbers: docs/GAMEPLAY.md "Combat".
// Frame: Z-up world units (as World).
#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ou::cdc { class BigfileSet; }

namespace ou::game {

enum class Team { Player, Enemy, Neutral };

struct Hit {
    float damage = 0;   // health points (Lara: HealthTuning::max scale)
    glm::vec3 point{0}; // world impact point
    glm::vec3 dir{0};   // unit direction the shot / blow travelled
    Team source = Team::Neutral;
};

// Anything Lara can target and shoot. Enemies (src/game/ai) implement it and register the pointer in
// World::targets; they must remove it before destroying the object. Body = vertical capsule from
// position() (feet) up to position().z + height(), radius radius().
class Damageable {
public:
    virtual ~Damageable() = default;
    virtual glm::vec3 position() const = 0; // feet, world
    virtual float radius() const = 0;
    virtual float height() const = 0;
    virtual float health() const = 0;      // <= 0: dead (not targeted, not hit)
    virtual Team team() const = 0;
    virtual void onHit(const Hit& hit) = 0; // apply damage, react
    // Lock-on aim point; default chest height.
    virtual glm::vec3 aimPoint() const { return position() + glm::vec3(0, 0, height() * 0.7f); }
    bool alive() const { return health() > 0; }
};

// A standing shootable capsule (ou-game --target, tests).
struct DummyTarget : Damageable {
    glm::vec3 pos{0};
    float hp = 700, r = 100, h = 500; // [I] panther-sized: 700 hp (Normal cat_panther, enemy data)
    int hits = 0;
    glm::vec3 position() const override { return pos; }
    float radius() const override { return r; }
    float height() const override { return h; }
    float health() const override { return hp; }
    Team team() const override { return Team::Enemy; }
    void onHit(const Hit& hit) override { hp -= hit.damage, ++hits; }
};

// Distance along the unit ray `dir` to a vertical capsule (feet `base`, radius r, height h); nullopt if
// missed or farther than maxDist.
std::optional<float> rayCapsule(glm::vec3 origin, glm::vec3 dir, glm::vec3 base, float r, float h, float maxDist);

// ---- Lara's health (tru.exe player globals DAT_00fadb58 +0x628..+0x63C) -------------------------
// Damage types index tru.exe's priority table DAT_0085de28 {0,1,2,4,3,5,4,4,5,4,4}: while Lara is
// invulnerable after a hit, only a type of higher priority than the last one hurts (0x6F5030).
inline constexpr int kDamageLight = 1;    // enemy hit messages: 1 light, 2 heavy (0x6EB590)
inline constexpr int kDamageHeavy = 2;
inline constexpr int kDamagePeriodic = 3; // 0x755230: every data+0xFE frames, data+0xFC % of max
inline constexpr int kDamageBypass = 5;   // ignores the invulnerability window (0x6F5030)
inline constexpr int kDamageKill = 6;     // message 4: damage = current health (0x741300)
inline constexpr int kDamageAbsolute = 8; // not scaled by difficulty; fall damage records this type (0x702FC0)

// Lara's player data (lara.drm ObjectBase+0xC8 = DTP 850 + 0x28F80) and the generalbank difficulty
// table (object data +0x24, 0x18-byte entries; Normal = entry 1). Seconds = 30 Hz frames / 30.
struct HealthTuning {
    float max = 1000.0f;                 // data +0x44 x difficulty +0x08 (Normal 1.0)
    float damageScale = 1.0f;            // difficulty +0x0C (Easy 0.5, Normal 1.0, Hard 1.3)
    float fall[3] = {1280.03f, 1817.32f, 2215.55f}; // data +0x4C/+0x50/+0x54, fall heights (units)
    float invulnSeconds = 1.0f;          // data +0x60: 30 frames
    float regenCap = 0.25f;              // data +0x70: regenerates up to this fraction of max
    float regenDurationSeconds = 400.0f / 30.0f; // data +0x74: 400 frames
    float regenDelaySeconds = 1.5f;      // data +0x78: 45 frames after the last damage
    float regenEase = 0.7f;              // data +0x7C: curve (1-e) t + e t^3 (0x6E79D0)
    float overhealMax = 1.4f;            // data +0x80: x max (health packs past full)
    float overhealDecaySeconds = 30.0f;  // data +0x84: 900 frames from overhealMax to max
};

class Health {
public:
    explicit Health(HealthTuning t = {}) : t_(t), value_(t.max) {}
    float value() const { return value_; }
    float max() const { return t_.max; }
    bool dead() const { return value_ <= 0; }
    int lastType() const { return lastType_; }
    const HealthTuning& tuning() const { return t_; }

    // tru.exe 0x6F1790: 0 up to fall[0], then linear to max/2 at fall[1], linear to max at fall[2], max above.
    float fallDamage(float height) const;
    // 0x6F5030 (scaled by damageScale unless type is kDamageAbsolute) and the invulnerability window.
    // Returns false if the hit was ignored.
    bool damage(float amount, int type = kDamageLight);
    // 0x702FC0: fall damage bypasses the window and is never scaled; records type kDamageAbsolute.
    void fall(float height);
    void heal(float amount, bool overheal = false); // 0x6F4EF0
    void tick(float dt);                            // 0x6F5160: regeneration, overheal decay, window
    void reset();                                   // 0x700060: full health (level start / respawn)

private:
    void hurt(float amount, int type); // common tail of damage()/fall()
    HealthTuning t_;
    float value_;
    double now_ = 0, regenStart_ = 0; // seconds; regen starts at regenStart_
    float regenBase_ = 0;             // health when the regeneration clock was (re)started (+0x634)
    float invuln_ = 0;                // seconds left (+0x638)
    int lastType_ = 0;                // +0x63C
};

// ---- weapons ----------------------------------------------------------------------------------
// A weapon object's stats block S (object data +0xE0) and its ammo object's fire-mode record (ammo
// object data +0x18), as tru.exe's fire function 0x6C9E50 / hitscan 0x6C64D0 read them. Defaults: the
// dual pistols (wp_handgun + pr_handgunbullet) [O].
struct WeaponDef {
    std::string object = "wp_handgun", ammo = "pr_handgunbullet";
    int pellets = 1;              // S+0x05
    int clip = 12;                // S+0x0B, rounds per gun
    bool infinite = true, dual = true; // S+0x88 bits 0x80 / 0x20
    float shortRange = 4000.0f;   // S+0x20: damage knee, targeting range
    float maxRange = 6500.0f;     // S+0x18: damage falloff end; [I] no hit past it
    float spreadDeg = 1.0f, maxSpreadDeg = 1.0f, spreadPerShot = 0.0f; // S+0x1C / +0x2C / +0x30
    int mode = 1;                 // record +0: 1 hitscan, 2 projectile
    float damage[3] = {40.0f, 30.0f, 20.0f}; // record +4 / +0x10 / +0x14: at 0, shortRange, maxRange
    float damageAt(float dist) const;        // linear between the three
};
// Decodes `object` (pc-w\<object>.drm) and its ammo object; nullopt if a DRM or block is missing.
std::optional<WeaponDef> loadWeapon(const cdc::BigfileSet& bigfile, std::string_view object);

// One bullet, for effects/tests.
struct Shot {
    glm::vec3 from{0}, to{0}; // muzzle, impact (or range end)
    int hand = 0;             // 0 right, 1 left
    bool hit = false;         // struck geometry or a target
    Damageable* target = nullptr;
    float damage = 0;
};

// Lara's weapon/target state, advanced by World every tick.
struct CombatState {
    bool drawn = false;       // guns in hand (or being drawn)
    float drawT = 0;          // upper-body layer: 0 holstered .. 1 aiming (equip / unequip clips)
    Damageable* target = nullptr;
    bool locked = false;      // hard lock (aim held), else soft target
    glm::vec2 aim{0};         // yaw (+ = Lara's left), pitch (+ = up) of the aim, rad, relative to her heading
    glm::vec3 aimPoint{0};
    int clip[2] = {12, 12};   // rounds in the right / left gun
    float reload = 0;         // seconds left of a reload
    float cooldown = 0;       // seconds until the next shot
    int hand = 0;             // next hand to fire: 0 right, 1 left
    float sinceShot[2] = {1e9f, 1e9f}; // per hand, seconds since its last shot (recoil, flash)
    float spread = 1.0f;      // degrees
    std::vector<Shot> shots;  // fired this tick
    uint64_t shotsFired = 0, rng = 0x2545F491u;
    float deathTime = -1;     // seconds since death; -1 alive
    bool drawHeld = false, switchHeld = false;
};

// Lara's targeting sets in her player data (+0x480: acquisition tiers, +0x470: soft target), criteria
// 0x712090. Flags 0xBC000000 on both: line of sight from Lara + data+0x48C (375) required.
struct TargetingTuning {
    float eyeHeight = 375.0f;    // data +0x48C
    float closeRange = 1264.0f;  // tier 1: nearest within this 3D distance
    float lockRange = 8000.0f;   // tiers 2/3: 3D distance
    float cameraConeDeg = 90.0f; // tier 2: angle from the camera axis; tier 3: horizontal angle from Lara's facing
    float softRange = 5000.0f;   // +0x470: 2D distance
    float softConeDeg = 27.5f;   // +0x470: angle from Lara's facing
};

} // namespace ou::game
