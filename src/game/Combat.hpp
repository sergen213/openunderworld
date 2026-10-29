// Combat: Lara's health/damage/death, weapons, targeting. Evidence and numbers: docs/GAMEPLAY.md "Combat".
// Frame: Z-up world units (as World).
#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ou::cdc { class BigfileSet; }

namespace ou::game {

// ---- PLAYER TAILORING (docs/GAMEPLAY.md "Player tailoring") ------------------------------------------
// tru.exe keeps five levels 0..2 in its game state (DAT_00c59264): +0x3EF98 enemy health ("EnemyHP"), +0x3EF9C
// ammunition capacity ("AmmoAdj"), +0x3EFA4 Lara's health ("PlayerHP", DAMAGE TO LARA), +0x3EFA8 saving grab time
// ("SavingGrabAdj"), +0x3EFAC (no reader in tru.exe [U]); defaults 1 (0x6E7500). uiexperienceadjustmentmenu natives 4..8
// set them; a preset sets all five to its level: 0 CASUAL EXPLORER, 1 TOMB RAIDER, 2 MASTER SURVIVALIST [O generalbank
// script 132 fn 4]. Level 0 is the lenient end of each.
struct Tailoring {
    int enemyHealth = 1, ammo = 1, laraHealth = 1, savingGrab = 1, unknown = 1;
    static Tailoring preset(int level) { return {level, level, level, level, level}; }
    int preset() const { return enemyHealth == ammo && ammo == laraHealth && laraHealth == savingGrab ? ammo : -1; } // -1 = custom
    bool operator==(const Tailoring&) const = default;
};
// generalbank's difficulty table (GlobalInfo +0x24: three 0x18-byte entries by level) [O values]. +0x08 scales Lara's max
// health by Tailoring::laraHealth (0x6E7960); +0x14 = saving grab frames (TR7 GameDifficulty::savingGrabTime), by
// Tailoring::savingGrab [I: no exe reader of either]. The other fields are read only at entry 1 (0x5D9E50 returns 1):
// +0 enemy accuracy, +4 enemy damage, +0x0C damage to Lara (HealthTuning::damageScale), +0x10 health pack drops.
struct DifficultyTable {
    std::array<float, 3> healthScale{1.3f, 1.0f, 0.8f};
    std::array<float, 3> savingGrabSeconds{3.0f, 50 / 30.0f, 0.5f}; // 90 / 50 / 15 frames at 30 Hz
};
std::optional<DifficultyTable> loadDifficulty(const cdc::BigfileSet& bigfile); // nullopt: generalbank or table missing

enum class Team { Player, Enemy, Neutral };

struct Hit {
    float damage = 0;   // health points (Lara: HealthTuning::max scale)
    glm::vec3 point{0}; // world impact point
    glm::vec3 dir{0};   // unit direction the shot / blow travelled
    Team source = Team::Neutral;
    bool headshot = false; // adrenaline head shot (kills; the enemy may pick a special death clip)
    bool knock = false;    // knocked off balance whatever the damage (Mjolnir's ground pound)
    bool tranq = false;    // tranquilizer dart: animals with sleep clips are dosed instead of hurt (ai::Mode::Sleep)
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
    float max = 1000.0f;                 // data +0x44; World::setTailoring scales it by DifficultyTable::healthScale
    float damageScale = 1.0f;            // difficulty +0x0C, always entry 1 (0x5D9E50) = 1.0
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
    void set(float v) { reset(), value_ = v, regenBase_ = v; } // saves: restored health, window/regen clocks idle
    // PLAYER TAILORING changed Lara's max (tru.exe 0x6F5160): health keeps its fraction of the max.
    void setMax(float m) {
        if (m == t_.max || !(m > 0)) return;
        value_ *= m / t_.max, regenBase_ *= m / t_.max, t_.max = m;
    }

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
// Lara's weapon set: the dual pistols plus one secondary (weapon object data +0xDC = 1; picking up another
// replaces it), and Thor's hammer Mjolnir once carried (data +0xDC = 2, its own inventory slot +0x24: tru.exe
// 0x704930 cycles pistols -> secondary -> hammer, skipping what she lacks). Sticky grenades are a gadget (GrenadeTuning).
enum class Weapon { Pistols, Uzi, Shotgun, Speargun, ThorHammer, AssaultRifle, Tranquilizer };
inline constexpr int kWeaponCount = 7;
// "wp_handgun", "wp_uzi", "wp_shotgun", "wp_speargun", "wp_thorhammer", "wp_assault_rifle", "wp_tranq"
std::string_view weaponObject(Weapon w);
std::string_view weaponName(Weapon w); // "pistols", "uzi", "shotgun", "speargun", "thorhammer", "rifle", "tranquilizer"
std::optional<Weapon> weaponByName(std::string_view name); // weaponName inverse
// The secondary every unit of chapter n (kChapters index) loads besides wp_handgun / wp_uzi [O object deps of the
// pc-w\l<N>_*.drm units]: l1 / l7 wp_speargun, l3 wp_shotgun, l4 wp_tranq, l6 wp_assault_rifle; l2 / l5 / the epilogue
// only wp_uzi. Lara starts the chapter with it, its reserve full [I]; the prologue (Croft Manor units, wp_shotgun loaded
// for chapter 3) with the pistols only [I]. GAMEPLAY.md "Weapons".
Weapon chapterSecondary(int chapter);
// Picking up a weapon object adds rounds to the gun with its ammunition slot (tru.exe 0x70E530 -> 0x6F5E70: the
// intro's property 0x55 into slot S+4, only while below the max reserve) [O]: the secondaries' own objects, and
// wp_akrifle (the poachers' rifle: pr_akriflebullet, slot 2) for the assault rifle. nullopt: no ammunition (pistols,
// Mjolnir, anything else). The count, property 0x55, is not decoded: one clip [I].
std::optional<Weapon> ammoWeapon(std::string_view object);

// A weapon object's stats block S (object data +0xE0) and its ammo object's fire-mode record (ammo
// object data +0x18), as tru.exe's fire function 0x6C9E50 / hitscan 0x6C64D0 read them. Defaults: the
// dual pistols (wp_handgun + pr_handgunbullet) [O].
struct WeaponDef {
    std::string object = "wp_handgun", ammo = "pr_handgunbullet";
    int ammoSlot = -1;            // S+0x04 (0xFF none): live count fadb58+0x51C + 2 slot, max-reserve table index
    std::array<int, 3> maxReserve{}; // Lara data +0x868 / +0x87C / +0x872 + 2 slot by Tailoring::ammo (0x6EEAE0); 0 without a slot
    int pellets = 1;              // S+0x05
    int clip = 12;                // S+0x0B, rounds per gun; -1 (Mjolnir) no clip: never empties, never reloads
    bool infinite = true, dual = true; // S+0x88 bits 0x80 / 0x20
    float shortRange = 4000.0f;   // S+0x20: damage knee, targeting range
    float maxRange = 6500.0f;     // S+0x18: damage falloff end; [I] no hit past it
    float spreadDeg = 1.0f, maxSpreadDeg = 1.0f, spreadPerShot = 0.0f; // S+0x1C / +0x2C / +0x30
    float spreadRecovery = 0.0f;  // S+0x34, degrees per 30 Hz frame [I unit]
    int mode = 1;                 // record +0: 1 hitscan, 2 projectile
    // Hitscan: record +4 / +0x10 / +0x14 = damage at 0, shortRange, maxRange. Projectile (mode 2, pr_spear): the
    // record's +0x1C (250) for all three [I]; we fire projectiles as hitscan (the projectile record is not decoded).
    float damage[3] = {40.0f, 30.0f, 20.0f};
    // The ammo's second fire record (+0x84), which 0x69E770 picks while instance +0x131 is set: [I] under water (its hit
    // object +0xA8 is the *_underwater one). Mjolnir 1000 / 400 / 100.
    float wetDamage[3] = {40.0f, 40.0f, 20.0f};
    float damageAt(float dist, bool wet = false) const; // linear between the three
};
// Decodes `object` (pc-w\<object>.drm) and its ammo object, and the max reserve from Lara's data (pc-w\lara.drm);
// nullopt if a DRM or block is missing.
std::optional<WeaponDef> loadWeapon(const cdc::BigfileSet& bigfile, std::string_view object);
// The decoded values for each weapon (what loadWeapon returns for the game's data; tests check it).
WeaponDef weaponDefaults(Weapon w);

// One bullet, for effects/tests.
struct Shot {
    glm::vec3 from{0}, to{0}; // muzzle, impact (or range end)
    int hand = 0;             // 0 right, 1 left
    bool hit = false;         // struck geometry or a target
    Damageable* target = nullptr;
    float damage = 0;
};

// Mjolnir's actions (wp_thorhammer's clips of Lara, docs/GAMEPLAY.md "Mjolnir"): fire = the swing that throws the bolt
// (ground_fire_idle, the shot on its animation event 0x75 at frame 10), target lock + interact = the ground pound
// (ground_melee_idle, its shock on event 0x4B at frame 12, tru.exe 0x6E68B0). Times in seconds since the start.
struct HammerAction {
    std::string clip; // "" none, else a wp_thorhammer clip name
    float t = 0, prevT = 0;
};

// Lara's weapon/target state, advanced by World every tick.
struct CombatState {
    Weapon weapon = Weapon::Pistols;    // in hand or holstered ready (inventory +0x134)
    Weapon previous = Weapon::Pistols;  // inventory +0x138
    Weapon secondary = Weapon::Pistols; // carried secondary (World::giveWeapon); Pistols = none
    Weapon want = Weapon::Pistols;      // switch target: the drawn weapon holsters, then this one is drawn
    HammerAction hammer;      // Mjolnir's swing / ground pound in progress
    bool drawn = false;       // weapon in hand (or being drawn)
    bool redraw = false;      // switching: draw `want` once the current one is holstered
    float drawT = 0;          // upper-body layer: 0 holstered .. 1 aiming (equip / unequip clips)
    Damageable* target = nullptr;
    bool locked = false;      // hard lock (aim held), else soft target
    glm::vec2 aim{0};         // yaw (+ = Lara's left), pitch (+ = up) of the aim, rad, relative to her heading
    glm::vec3 aimPoint{0};
    int clip[kWeaponCount][2] = {}; // rounds per weapon in the right / left gun (two-handed: [0]); World fills them
    int reserve[kWeaponCount] = {}; // spare rounds per weapon (pistols: infinite)
    float reload = 0;         // seconds left of a reload
    float cooldown = 0;       // seconds until the next shot
    int hand = 0;             // next hand to fire: 0 right, 1 left
    float sinceShot[2] = {1e9f, 1e9f}; // per hand, seconds since its last shot (recoil, flash)
    float spread = 1.0f;      // degrees
    std::vector<Shot> shots;  // fired this tick
    uint64_t shotsFired = 0, rng = 0x2545F491u;
    float deathTime = -1;     // seconds since death; -1 alive
    bool drawHeld = false, switchHeld = false, weaponHeld = false, healHeld = false, interactHeld = false, fireHeld = false;
    float healCooldown = 0;   // seconds until the next health pack (g_healthpack data +0xEA: 60 frames)
    int healthPacksUsed = 0;  // count, for logs
    // Adrenaline (head shot): meter filled by damage dealt; full + hard lock + close + interact opens a window of
    // slowed world time in which the move stick steers the pip into the reticle and fire kills.
    float adrenaline = 0;     // meter 0..1
    float slowMo = 0;         // real seconds left of the head-shot window; 0 = closed
    glm::vec2 pip{0};         // pip offset from the reticle, reticle radii (inside 1 = on target)
    float timeScale = 1;      // world (enemy) time scale; adrenaline slows it, Lara keeps real time (tru.exe 0x622418)
};

// ---- sticky grenades (gadget "Sticky Bomb", tru.exe gadget table 0x8AFC70 entry 4; docs/GAMEPLAY.md "Grenades") ----
// g_grenade_thrower data (+0xE2.. per difficulty, +0xEA cooldown, +0xF0 projectile pr_grenade, +0x10C/+0x110 launch,
// +0x134/+0x138 range) and pr_grenade's explosion record (+0x44: 2, 1000, 1100, 200, 70, 300, 100, ..., +0x80 = 120)
// as read by the area-damage function 0x69F1E0 [O values; field meanings I where marked in GAMEPLAY.md].
struct GrenadeTuning {
    std::array<int, 3> maxCarried{10, 6, 4}; // +0xE6 / +0xE4 / +0xE2 by Tailoring::ammo (0x6E7760)
    float cooldown = 20 / 30.0f; // +0xEA frames
    float launchVz = 632.1f;    // +0x10C units/s [I meaning]
    float gravity = 4741.0f;    // +0x110 units/s^2 [I meaning]
    float minRange = 200.0f, maxRange = 1000.0f; // +0x134 / +0x138 [I meaning]
    float damage = 1000.0f, radius = 1100.0f, edgeDamage = 200.0f; // explosion +0x48 / +0x4C / +0x50 [I meaning]
    float impulse = 300.0f, edgeImpulse = 100.0f; // +0x58 / +0x5C: knock speed at the centre / the edge [I meaning]
    float fuse = 120 / 30.0f;   // explosion +0x80 frames, from the moment it sticks [I meaning]
};
// Decodes g_grenade_thrower + pr_grenade; nullopt if a DRM or block is missing.
std::optional<GrenadeTuning> loadGrenadeTuning(const cdc::BigfileSet& bigfile);
// A thrown grenade: flying, then stuck to the world or to a target (moving with it) until the fuse runs out.
struct Grenade {
    glm::vec3 pos{0}, vel{0};
    bool stuck = false;
    Damageable* target = nullptr; // stuck to (World drops it when the target leaves World::targets)
    glm::vec3 offset{0};          // from target->position() while stuck to it
    float fuse = 0;               // seconds left once stuck
    float age = 0;                // seconds since the throw
};
// A fresh state with every weapon's clips full (reserves empty).
CombatState fullClips(const std::array<WeaponDef, kWeaponCount>& weapons);

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
