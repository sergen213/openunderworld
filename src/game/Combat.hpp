// Combat: Lara's health/damage/death, weapons, targeting. Evidence and numbers: docs/GAMEPLAY.md "Combat".
// Frame: Z-up world units (as World).
#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ou::cdc { class BigfileSet; }

namespace ou::game {

// ---- PLAYER TAILORING (docs/GAMEPLAY.md "Player tailoring") ------------------------------------------
// tru.exe keeps five levels 0..2 in its game state (DAT_00c59264): +0x3EF98 enemy health ("EnemyHP"), +0x3EF9C
// ammunition capacity ("AmmoAdj"), +0x3EFA4 Lara's health ("PlayerHP", DAMAGE TO LARA), +0x3EFA8 saving grab time
// ("SavingGrabAdj"), +0x3EFAC (`unknown`: no reader in tru.exe, unit scripts read it and compare with 2 [U what for]);
// defaults 1 (0x6E7500). uiexperienceadjustmentmenu natives 6..10 set them, 13..17 read them (script::Runtime::
// Experience); a preset sets all five to its level: 0 CASUAL EXPLORER, 1 TOMB RAIDER, 2 MASTER SURVIVALIST [O generalbank
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
    // Thrown (message 0x91 from the area hit 0x69F1E0): world velocity, units/s [I unit, as the grenade's]; 0 none.
    glm::vec3 knockVel{0};
    bool tranq = false;    // tranquilizer dart: animals with sleep clips are dosed instead of hurt (ai::Mode::Sleep)
    // tru.exe hit message 0x11C (built by 0x63C9B0): +0x50 damage class, +0x64 damage type. Lara's bullets: the ammo
    // object data's +0x20 / +0x24 (record +8 / +0xC: pr_*bullet 0 / 1, pr_shotgunbullet 2 / 1, pr_thorbolt 13 / 3);
    // projectiles (pr_spear, pr_tranquilizerdart: 0x6C9670) class 4, type 0; 0 / 0 raw (the bike's ram, 0x6AE240).
    // Thralls pick get-up or death by them (ai::Regenerate::kills).
    int damageClass = 0, damageType = 0;
    // Lara's close combat (a kick, a swung or thrown object; Melee.cpp): knocks an enemy off its feet when it has the
    // clips (ai::Mode::Knocked) whatever the damage.
    bool melee = false;
};
// The hit class of Lara's finishing move (her fatality attack record, lara.drm; MeleeTuning::fatality): it ends a
// regenerating thrall (ai::Enemy::onHit).
inline constexpr int kFinisherClass = 16;
// Lara's reaction to an accepted hit (0x6EB590 by the message's damage class): 2 heavy (state value +0x208), 1 light
// (+0x204); class 6 by damage: >= Lara data +0x64 (250) heavy, >= +0x66 (100) light; 0 none.
inline constexpr float kHeavyHit = 250.0f, kLightHit = 100.0f;
int hitReaction(int damageClass, float damage);

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
inline constexpr int kDamageContact = 6; // class of blows and damaging collision (0x624FB0, 0x5D4C60); as a message-4 kill: current health
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
    // The window / regeneration clocks (+0x630..+0x63C) for saves, so damage over time continues bit for bit.
    struct Clocks { double now = 0, regenStart = 0; float regenBase = 0, invuln = 0; int lastType = 0; };
    Clocks clocks() const { return {now_, regenStart_, regenBase_, invuln_, lastType_}; }
    void setClocks(const Clocks& c) { now_ = c.now, regenStart_ = c.regenStart, regenBase_ = c.regenBase, invuln_ = c.invuln, lastType_ = c.lastType; }
    // PLAYER TAILORING changed Lara's max (tru.exe 0x6F5160): health keeps its fraction of the max.
    void setMax(float m) {
        if (m == t_.max || !(m > 0)) return;
        value_ *= m / t_.max, regenBase_ *= m / t_.max, t_.max = m;
    }
    // A relic changed the maximum (0x6E7990 reads the relic count live): health stays what it is.
    void raiseMax(float m) { if (m > 0) t_.max = m; }

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
    std::array<int, 3> startReserve{}; // a level start's count: +0x848 / +0x85C / +0x852 + 2 slot by Tailoring::ammo (0x6EE930)
    int pellets = 1;              // S+0x05
    int clip = 12;                // S+0x0B, rounds per gun; -1 (Mjolnir) no clip: never empties, never reloads
    bool infinite = true, dual = true; // S+0x88 bits 0x80 / 0x20
    float shortRange = 4000.0f;   // S+0x20: damage knee, targeting range
    float maxRange = 6500.0f;     // S+0x18: damage falloff end; [I] no hit past it
    float spreadDeg = 1.0f, maxSpreadDeg = 1.0f, spreadPerShot = 0.0f; // S+0x1C / +0x2C / +0x30
    float spreadRecovery = 0.0f;  // S+0x34, degrees per 30 Hz frame [I unit]
    int mode = 1;                 // record +0: 1 hitscan, 2 projectile
    // Hitscan: record +4 / +0x10 / +0x14 = damage at 0, shortRange, maxRange. Projectile (mode 2): record +0x1C (pr_spear
    // 250, pr_tranquilizerdart 500) at every range, carried by the projectile (instance +0x54, 0x6C9670).
    float damage[3] = {40.0f, 30.0f, 20.0f};
    // The ammo's second fire record (+0x84), which 0x69E770 picks while instance +0x131 is set: [I] under water (its hit
    // object +0xA8 is the *_underwater one). Mjolnir 1000 / 400 / 100; pr_spear 550.
    float wetDamage[3] = {40.0f, 40.0f, 20.0f};
    // Hit class / type (hitscan: record +8 / +0xC, the hit message's +0x50 / +0x64: 0x6C64D0); projectile hits always
    // post class 4, type 0 (0x6C9670), whatever the record holds.
    int hitClass = 0, hitType = 1;
    // Projectiles (0x6C9E50 mode 2 -> 0x6C95F0 -> 0x6C9300): launch speed S+0x10 (fire message +0x30, 0x719990) and
    // gravity record +0x4C (0x6B6D10 query 0x3A = record +4, its +0x48: 0x6C91D0 adds (g + 3) x mass against the world's
    // -3 per frame^2), both per 30 Hz frame [I unit], here x30 / x900: units/s, units/s^2 (down). [0] dry, [1] the +0x84
    // record. pr_spear 1000 / 2, 6; pr_tranquilizerdart 1500 / 1, 3. 0 for hitscan weapons.
    float projectileSpeed = 0;
    std::array<float, 2> projectileGravity{0, 0};
    // Seconds between shots: the length of the clip whose event 0x75 fires (0x716F30 -> message 0x19B -> 0x6C9E50; every
    // such clip has it at frame 0), plus the shotgun's pump: pistols / uzi combat_pistol_RfireA / the uzi's
    // combat_fire_overlay, two-handed combat_shotgun_fire_UL / combat_rifle_fire_UL, Mjolnir ground_fire_idle.
    float refire = 10 / 30.0f;
    // Event 0x7x commands of the weapon DRM's animation lists (info & 3 == 2: record +8 = command, frame +2): per clip name,
    // {command, 30 Hz frame}. 0x75 shot (byte +2: hand), 0x76 / 0x77 / 0x78 gun into the hand (0x6F64C0), 0x74 gun
    // stowed (0x6E8FC0), 0x4B Mjolnir's ground pound (0x6E68B0). Empty without the data.
    std::map<std::string, std::vector<std::pair<int, int>>, std::less<>> commands;
    std::optional<int> commandFrame(std::string_view clip, std::initializer_list<int> commands) const; // first match
    // Data +0xB8 / +0xB9 (gun 0) and +0xBA / +0xBB (gun 1): Lara's HInfo markers in hand and stowed (the weapon model is
    // mounted at the marker; its frame is the model's). Handgun 7/6, 5/4 (grips, thighs); uzi 7/36, 5/37 (back);
    // two-handed 5/15 (left grip, across her back); Mjolnir 5/31.
    std::array<int, 2> hand{7, 5}, stow{6, 4};
    // S+0x14: the shot origin in the gun model (0x6ADBB0 -> 0x615930: < 0 HInfo marker -id, >= 0 segment): handgun /
    // speargun -10, uzi / rifle -1, tranquilizer -15, akrifle -30, shotgun / Mjolnir 0 (the root segment). `muzzle` is
    // its bind frame in the model (barrel -Y); identity without the model. The flashes are the gun's own "fire" events.
    int muzzleMarker = -10;
    glm::mat4 muzzle{1};
    float damageAt(float dist, bool wet = false) const; // linear between the three
};
// Decodes `object` (pc-w\<object>.drm) and its ammo object, and the max reserve from Lara's data (pc-w\lara.drm);
// nullopt if a DRM or block is missing.
std::optional<WeaponDef> loadWeapon(const cdc::BigfileSet& bigfile, std::string_view object);
// The decoded values for each weapon (what loadWeapon returns for the game's data; tests check it).
WeaponDef weaponDefaults(Weapon w);

// A weapon object fired by script (`weaponbase` 0, tru.exe 0x7784F0 -> message 0x19B -> 0x6C9E50 mode 2): Natla's
// wp_natla_fireball / wp_natla_fireball_adren with their projectiles pr_natla_fireball(_adren). Weapon stats block
// (data +0xE0, query 0x36): +0x06 ammo object, +0x10 launch speed per 30 Hz frame (300 / 50). Projectile data: fire record
// +0x18 (mode 2), its +0x1C the damage a direct hit carries (0 / 3000, 0x6C9670), +0x4C gravity (0: a straight line);
// explosion record +0x44 (kind 2) as pr_grenade's: +0x48 damage, +0x4C radius, +0x50 damage at the edge, +0x54 degrees
// up [I by the grenade's 70 and the squib record's angle], +0x58 / +0x5C knock speed centre / edge, +0x80 frames (150)
// [I: ours the flight time after which it is dropped; the lingering flames it may describe are [U]].
struct ObjectWeaponDef {
    std::string object, ammo;
    float speed = 0;         // units/s
    float directDamage = 0;  // a direct hit on Lara (message 0x11C before the blast)
    float damage = 0, edgeDamage = 0, radius = 0, knock = 0, knockEdge = 0, angle = 0; // the blast
    float life = 5;          // seconds
};
std::optional<ObjectWeaponDef> loadObjectWeapon(const cdc::BigfileSet& bigfile, std::string_view object);
// A fired one in flight (World::tickHostile): straight at the point its target stood on when it was fired (no homing:
// 0x7784F0 copies the target's position into the fire message).
struct HostileShot {
    std::string weapon;         // ObjectWeaponDef::object
    int32_t owner = 0;          // the weapon intro that fired it
    glm::vec3 pos{0}, vel{0};
    float age = 0;
};

// One bullet, for effects/tests.
struct Shot {
    glm::vec3 from{0}, to{0}; // muzzle, impact (or range end)
    int hand = 0;             // 0 right, 1 left
    bool hit = false;         // struck geometry or a target
    Damageable* target = nullptr;
    float damage = 0;
    glm::mat4 gun{1};         // the gun model's world frame (World::gunMatrix): its "fire" events play there
    bool impact = false;      // a projectile arriving (from = its launch point): no muzzle flash
};

// A projectile in flight (pr_spear / pr_tranquilizerdart), advanced every tick: gravity, then the segment it swept
// tested against the world and the targets as a hitscan ray.
struct Projectile {
    glm::vec3 from{0};           // muzzle at launch
    glm::vec3 pos{0}, vel{0};    // units, units/s
    float gravity = 0;           // units/s^2, down
    float travelled = 0;         // units; it is dropped past WeaponDef::maxRange [I]
    Weapon weapon = Weapon::Speargun;
    int hand = 0;
    bool wet = false;            // fired under water (the +0x84 record)
};

// Mjolnir's actions (wp_thorhammer's clips of Lara, docs/GAMEPLAY.md "Mjolnir"): fire = the swing that throws the bolt
// (ground_fire_idle, the shot on its animation event 0x75 at frame 10), target lock + interact = the ground pound
// (ground_melee_idle, its shock on event 0x4B at frame 12, tru.exe 0x6E68B0). Times in seconds since the start.
struct HammerAction {
    std::string clip; // "" none, else a wp_thorhammer clip name
    float t = 0, prevT = 0;
};

// The ground pound's area hits (0x6E68B0 -> 0x69F1E0 per 0x1C-byte ring) [O]: lara.drm object data, the attack record
// {+4 120.0, +0xC damage class 13, +0x20 2 rings, +0x24 rings} (section 850 +0x35EB8 -> rings +0x399F0). Which record
// tru.exe's weapon controller +0xC4 holds during the pound is [I]: the only one with rings and class 13.
struct PoundRing {
    float inner, outer;   // +0 / +4 radii: inner < distance < outer
    float edge, damage;   // +8 at the outer radius, +0xC at the inner
    float knockEdge, knock; // +0x10 / +0x14 knock-back speed at the outer / inner radius (knock 0: none; knockEdge < 0: no lerp)
    float angle;          // +0x18 degrees up of the knock-back
};
inline constexpr PoundRing kPoundRings[] = {{0, 500, 1200, 1200, 400, 500, 65}, {500, 3000, 100, 400, -1, 0, 0}};

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
    // Lara's death ragdoll (AnimLibrary::ragdoll), as ai::EnemyState's: 0 none, 1 starting (World builds the bodies at
    // `ragdoll`), 2 simulating, 3 settled; `ragdoll` = each bone's start-segment frame in her model space.
    int ragdollPhase = 0;
    std::vector<glm::mat4> ragdoll;
    bool drawHeld = false, switchHeld = false, weaponHeld = false, healHeld = false, interactHeld = false, fireHeld = false;
    bool manualHeld = false, aimHeld = false; // Manual Aim / Target Lock held last tick (press edges)
    float healCooldown = 0;   // seconds until the next health pack (g_healthpack data +0xEA: 60 frames)
    int healthPacksUsed = 0;  // count, for logs
    // Adrenaline (Lara data; docs/GAMEPLAY.md "Adrenaline (decoded)"). Meter += the damage she deals (message 0xD9,
    // 0x63AD50 -> 0x6F28C0), up to +0x2D0 = 500; it drains +0x2D4 = 20 per second, +0x2D8 = 225 in adrenaline mode
    // (0x6F2930). Reaching the max latches `headShotReady` (weapon controller flag 0x400000, 0x6E6630): the meter then
    // neither gains nor drains until it is spent.
    // - Focused fire (adrenaline mode, flag 0x8000000): Manual Aim pressed with a hard lock and the meter not empty
    //   (0x718BD0: input action 37 = virtual button 9; not with Mjolnir) toggles it; starting it gives the head shot up
    //   (0x7189C0 -> 0x6E6670 clears the latch, the meter stays and drains); the game's time scale ramps to +0x31C = 0.5
    //   over +0x2CC = 10 frames and back to 1 when it ends (0x718AD0): the meter empty, the lock gone, or the press.
    // - Head shot: the Grapple button with the latch set and a hard-locked enemy close by (state chart T1404: action 46 =
    //   virtual button 1; condition 0x21A = the latch). 0x71D4F0: she is carried to `headShotDistance` from the enemy
    //   over `headShotBlend` (the enemy's record), plays the enemy's hosted clip for her weapon and the enemy its
    //   reaction; the meter empties. The clip's event 5 opens the aim (time -> +0x2C8 = 0.2, 0x715020), its event 4 ends
    //   it (0x718EA0). In between the look input steers the pip into the reticle and fire kills.
    float adrenaline = 0;     // meter 0..1 (x 500)
    bool adrenalineMode = false;
    bool headShotReady = false; // the meter reached its max (flag 0x400000): the Grapple button starts the head shot
    // The head shot move in progress: real seconds since it began, -1 none; the enemy's clip she plays ("<enemy>:<clip>"
    // in the animation library), its length and the aim window [open, close) (events 5 / 4), all in clip seconds.
    float headShot = -1;
    std::string headShotClip;
    float headShotLength = 0, headShotOpen = 0, headShotClose = 0;
    bool headShotRequest = false; // World::tick: this tick's Grapple press starts it (the grapple is not thrown)
    float slowMo = 0;         // real seconds left of the head-shot window; 0 = closed
    glm::vec2 pip{0};         // pip offset from the reticle, reticle radii (inside 1 = on target)
    // tru.exe's two time scales (game loop 0x5DA1C0; ramps 0x78C160 read by 0x78C4E0): `timeScale` = 0xE7F1D4, every
    // update's time step (adrenaline 0x715020 / 0x7189C0, scripts' gamebase 2 / 24); `worldScale` = 0xE7F1A0 (gamebase
    // 23), which Lara's own update divides back out (0x622418): the world slows, she does not.
    struct TimeRamp { // {target, seconds, from, start}: value = from + (target - from) x ease(elapsed / seconds)
        float target = 1, seconds = -1, from = 1, elapsed = 0; // seconds < 0: idle (0x78C4E0 sets -1 when done)
        float step(float value, float dt);                     // advances by real time; returns the new value
        void start(float to, float over, float now) { target = to, seconds = over, from = now, elapsed = 0; }
    };
    float timeScale = 1, worldScale = 1;
    TimeRamp timeRamp, worldRamp;
    // Manual (precision) aim: the over-the-shoulder view (camera mode 23), toggled by Manual Aim on her feet without a
    // target lock (state chart T55 / T1887 / T1886); fire goes where the reticle (the view's centre) points.
    bool manualAim = false;
    // Dual targeting (0x71E490): with a two-gun weapon and a primary target, the left gun takes a second target (the
    // best other candidate) when there is one; `secondHold` = seconds it has been held (kept `Lara data +0x286` frames).
    Damageable* second = nullptr;
    float secondHold = 0;
    std::vector<Projectile> projectiles;
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

// ---- grapple (gadget "Grapple", g_grapple data; docs/GAMEPLAY.md "Grapple") ----
// g_grapple data [O values]: +0xF8 hook object 77 (pr_grapplinghook), +0xFC hook launch speed (0x6C85C0 -> 0x6A1980,
// projectile +0x70), +0x100 throw range (0x698AC0: criterion 15 of the target sets, the hook aimed short past it),
// +0x104 longest rope and +0x108 shortest (0x698B40 / 0x6A4A40 clamp of anchor -> bends -> hand), +0x10C bends in the
// rope's pool (0x6A3430). A unit (stream data +0x2B4: +0x18 flag, +0x1C / +0x20) or the target (its 0x404 block: flag,
// +4 / +8) can override range / rope; not read here [U whether any unit / target sets one].
constexpr int kRopeBends = 8; // PlayerState::ropeBend capacity: g_grapple's pool (+0x10C)
struct GrappleTuning {
    float range = 4424.78f;     // +0x100
    float maxRope = 4424.78f;   // +0x104
    float minRope = 553.09735f; // +0x108
    int bends = kRopeBends;     // +0x10C
    // +0xB4..+0xB7 Lara's HInfo markers {in hand, stowed, ...} as a weapon's +0xB8 (g_grapple: 30, 9). Stowed, the
    // g_grapple instance (player message 0x175: grapple controller +0x34) hangs at marker 9 on her pelvis (segment
    // 3), where the target sets' "in range" is measured from [O Wine: idle, the instance 30 ahead, 37 right, 295 up of
    // her root; marker 9 in ours 22 / 37 / 303].
    int stowMarker = 9;
};
std::optional<GrappleTuning> loadGrappleTuning(const cdc::BigfileSet& bigfile);
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

// Lara's adrenaline numbers (player data = lara.drm ObjectBase+0xC8; CombatState::adrenaline describes the use).
struct AdrenalineTuning {
    float max = 500.0f;           // +0x2D0: the full meter, in damage dealt
    float drain = 20.0f;          // +0x2D4 per second (0x6F2930: x timeMult / 30 per frame)
    float modeDrain = 225.0f;     // +0x2D8 per second in adrenaline mode (flag 0x8000000)
    float modeScale = 0.5f;       // +0x31C world time scale in adrenaline mode (0x7189C0)
    float headShotScale = 0.2f;   // +0x2C8 world time scale while aiming the head shot (0x715020)
    float ease = 10 / 30.0f;      // +0x2CC frames for each time-scale ramp (x 33.33 ms, 0x78C160)
    float secondTargetHold = 1000 / 30.0f; // +0x286 u16 frames a second (dual) target is kept before another may take over
};
std::optional<AdrenalineTuning> loadAdrenalineTuning(const cdc::BigfileSet& bigfile); // nullopt: lara.drm missing

// ---- close combat and carried objects (Melee.cpp; docs/GAMEPLAY.md "Close combat", "Carried objects") ----
// An attack record of Lara's player data (0x28 bytes: +1 hit effect id, +4 damage, +0xC hit class, +0x20 / +0x24 rings):
// state property 1005 of her chart states names the one in force (weapon controller +0xC4), and the hit message 0x11C
// carries its damage and class (0x6E6840 / the limb contacts opened by animation events 0xE .. 0xD).
struct AttackRecord {
    float damage = 0;
    int hitClass = 0;
    bool operator==(const AttackRecord&) const = default;
};
// Lara's melee numbers [O lara.drm]: the chart states' attack records (S1426 the kicks, S1432 the sprint kick, S737 the
// thrall fatality, S1311 a swung portable, S736 the enemy vault), the melee target height classes (player data +0x2B4 /
// +0x2B8 / +0x2BC: the target's top over her feet, 0x765810), the melee / fatality targeting slots (+0x474 / +0x478:
// 3D distance from her eye <= 640, the melee one also within 90 degrees [I: criterion 5 read as the horizontal angle
// from her facing]) and each clip's hit window from its animation events (0xE opens limb `param`, 0xD closes it; 0x6D
// = the fatality's blow), in 30 Hz frames.
struct MeleeTuning {
    AttackRecord kick{60, 2}, sprintKick{3, 2}, fatality{100, kFinisherClass}, swing{100, 2}, vault{0, 1};
    float low = 63.211128f, mid = 316.05563f, high = 537.29456f; // 0.2 / 1 / 1.7 m
    float range = 640.0f, coneDeg = 90.0f;
    struct Window { int open = 0, close = 0; bool operator==(const Window&) const = default; };
    std::map<std::string, Window, std::less<>> windows{{"melee_kick_low", {4, 10}},  {"melee_kick_mid", {4, 10}},
                                                       {"melee_kick_high", {3, 13}}, {"melee_kick_highest", {5, 10}},
                                                       {"melee_kick_move", {6, 11}}, {"melee_thrall_fatality", {23, 23}}};
    Window window(std::string_view clip) const {
        const auto it = windows.find(clip);
        return it == windows.end() ? Window{} : it->second;
    }
};
// Decodes them from pc-w\lara.drm (the state chart at player data +0x12508, the event lists); nullopt without it.
std::optional<MeleeTuning> loadMeleeTuning(const cdc::BigfileSet& bigfile);
// Lara's clips Melee.cpp plays (lara.drm; loadLaraAnims loads them beside kLaraClipNames).
extern const std::vector<std::string_view> kMeleeClipNames;

// A carryable object's (`portablebase`) data [O object data, ObjectBase +0xC8; tru.exe 0x6B5890 / 0x6B53A0]:
// +0x18 it encumbers her (the carved weights: the portable_encumbered_* walk; poles and keys do not), the throw: +0x34
// near distance, +0x38 the aim point's distance ahead, +0x3C / +0x40 the horizontal launch speed for a near / far aim
// point in units per 30 Hz frame (the vertical one is capped at the far value); +0x44.. the same with a combat target.
// Lara's clips with it live in its DRM ("hosted_*": pick-up, put-down, throw, melee; they animate her skeleton), kept
// in the AnimLibrary as "hosted:<animation id>"; `events`: their command events {command, 30 Hz frame} (0x4E attach,
// 0x4F let go, 0x53 throw, 0xE / 0xD the swing's hit window).
struct PortableDef {
    bool encumbers = false;
    float thick = 20.0f; // the smallest half extent of its collision box: its sphere against the level while carried
    float nearDist = 632.111f, aimDist = 2528.45f, nearSpeed = 53.7295f, speed = 53.7295f;
    float lockNearDist = 632.111f, lockAimDist = 2528.45f, lockNearSpeed = 53.7295f, lockSpeed = 53.7295f;
    std::map<std::string, std::string, std::less<>> hosted; // "hosted_throw" -> "hosted:2468"
    std::map<std::string, std::vector<std::pair<int, int>>, std::less<>> events;
    const std::string* clip(std::string_view name) const {
        const auto it = hosted.find(name);
        return it == hosted.end() ? nullptr : &it->second;
    }
    // A key item: its pick-up clip stows it (hosted_pickup's event 0x52 after 0x4E; World::stow).
    bool stows() const { return frame("hosted_pickup", 0x52).has_value(); }
    std::optional<int> frame(std::string_view name, int command) const {
        if (const auto it = events.find(name); it != events.end())
            for (const auto& [c, f] : it->second)
                if (c == command) return f;
        return std::nullopt;
    }
};
// The launch velocity (units/s) of a thrown portable as tru.exe 0x6B53A0 -> 0x6A7510 -> 0x6A6F40 solves it: from
// `origin` at the aim point `aim`, the horizontal speed `speed` (units per 30 Hz frame), gravity 3 per frame^2 in the
// solve, the vertical speed capped at `cap` (units per frame).
glm::vec3 throwVelocity(glm::vec3 origin, glm::vec3 aim, float speed, float cap);

// Lara's close-combat / carry action in progress (World::tickMelee), saved with the objects.
struct MeleeState {
    enum class Move : uint8_t { None, Kick, Fatality, Swing, Vault, Evade, PickUp, PutDown, Throw, Knocked };
    Move move = Move::None;
    int32_t target = 0;    // World::meleeId of the target (kick, swing, fatality, vault) / the portable's uid (pick-up)
    int32_t object = 0;    // Kick: the breakable she kicks at (uber object uid)
    bool landed = false;   // the blow / the release happened
    bool sprint = false;   // Kick: the sprint kick (its own attack record)
    int phase = 0;         // Knocked: 0 thrown, 1 landing, 2 getting up; Evade: 0 take-off / roll, 1 in the air
    glm::vec3 vel{0};      // Knocked / Evade: the launch velocity
    float timer = 0;       // Knocked: seconds she has lain
    bool swallowInteract = false; // the interact press started a move: the key reads released until it is let go
    bool dropHeld = false, grenadeHeld = false, jumpHeld = false, fireHeld = false; // press edges of the carry keys
    // Thrown portables in flight (tru.exe portable +0x84: now + 1500 ms): uid -> seconds left as a weapon; `launch`:
    // the velocity a portable leaves her hands with on the next physics step (units/s).
    std::map<int32_t, float> thrown;
    std::map<int32_t, glm::vec3> launch;
    std::map<int32_t, glm::vec3> lastPos; // thrown / dropped bodies' positions last tick (their speed at a contact)
};

} // namespace ou::game
