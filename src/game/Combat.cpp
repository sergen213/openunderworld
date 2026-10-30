// Combat: Lara's health (tru.exe 0x6F5030 / 0x6F5160 / 0x702FC0 family), death and respawn, dual pistols
// (stats from wp_handgun / pr_handgunbullet, aim grid + fire clips from wp_handgun.drm), lock-on targeting
// (Lara's targeting tiers) and hitscan. Numbers and provenance: Combat.hpp, docs/GAMEPLAY.md "Combat".
#include "game/Combat.hpp"
#include "game/Game.hpp"

#include "cdc/Cdc.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ou::game {

namespace {
constexpr float kPi = 3.14159265f;
constexpr int kPriority[] = {0, 1, 2, 4, 3, 5, 4, 4, 5, 4, 4}; // tru.exe DAT_0085de28
int priority(int type) { return type >= 0 && type < int(std::size(kPriority)) ? kPriority[type] : 0; }
// tru.exe 0x6E79D0: from + (to - from) * ((1 - e) t + e t^3), t clamped to [0, 1].
float ease(float t, float from, float to, float e) {
    if (t <= 0) return from;
    if (t >= 1) return to;
    e = std::clamp(e, 0.0f, 1.0f);
    return from + (to - from) * ((1 - e) * t + e * t * t * t);
}
float wrapAngle(float a) {
    a = std::fmod(a + kPi, 2 * kPi);
    return (a < 0 ? a + 2 * kPi : a) - kPi;
}
float approach(float v, float target, float step) { return v < target ? std::min(target, v + step) : std::max(target, v - step); }
float angleBetween(glm::vec3 a, glm::vec3 b) {
    const float l = glm::length(a) * glm::length(b);
    return l > 0 ? std::acos(std::clamp(glm::dot(a, b) / l, -1.0f, 1.0f)) : 0.0f;
}

// Per weapon: Lara's clips in its DRM and their lengths [O] (keys at 30 Hz), used as timings [I]: the exe fires on
// an animation event (0x75) of the fire clip and no refire field was found.
// - pistols: one shot per hand's fire clip (combat_pistol_RfireE/LfireE, 10 keys), hands alternate; equip 6 keys,
//   unequip 19, reload 21 (wp_handgun).
// - uzi: one shot per the uzi's own `fire` clip (5 keys), hands alternate; wp_uzi's own combat_pistols_equip /
//   unequip (15 keys each) and reload (21); the pistol aim grid and fire clips of wp_uzi.
// - shotgun: combat_shotgun_fire_* (13 keys) then combat_shotgun_pump (25) before the next shot; equip / unequip
//   24 keys, reload 117 (the whole clip per reload, however many shells are missing [I]).
// - speargun: the shotgun aim/fire set of wp_speargun; clip 1, so combat_speargun_reload (31 keys) after every
//   shot; under water combat_shotgun_equip_submerged (17) / unequip_submerged (19), combat_speargun_reload_submerged.
// - Mjolnir (wp_thorhammer): ground_equip / ground_unequip (26 keys), aim poses ground_aim_right / _front / _left, the
//   throw ground_fire_idle (29 keys, shot on event 0x75 at frame 10), the ground pound ground_melee_idle (61 keys,
//   event 0x4B at frame 12); under water combat_water_submerged_equip / _unequip and swim_submerge_thorhammer_attack_front
//   (31 keys) [I: its shot at frame 10 too, the swim attack clips carry no event list]. No clip, so no reload.
// - assault rifle / tranquilizer (wp_assault_rifle / wp_tranq): Lara's 3x5 combat_rifle_aim_* / combat_rifle_fire_*
//   (3 keys) grids of each DRM; combat_rifle_equip 19, unequip 24; reload combat_rifle_reload 41 (30 rounds) /
//   combat_tranqgun_reload 19 (one dart per clip, so after every shot).
struct WeaponInfo {
    std::string_view object, name;
    bool rifle; // two-handed: one gun, the 3x5 aim grid
    float refire, equip, unequip, reload; // seconds
    std::string_view equipClip, unequipClip, reloadClip;
    glm::vec3 muzzle; // barrel tip in the gun model (barrel -Y, top +Z) [I] from the mesh bounds
    std::string_view grid = "combat_shotgun_"; // two-handed: <grid>aim_* / <grid>fire_*
    float fire = 13 / 30.0f;                   // two-handed: length of the fire_* cells
};
const WeaponInfo kWeapons[kWeaponCount] = {
    {"wp_handgun", "pistols", false, 10 / 30.0f, 6 / 30.0f, 19 / 30.0f, 21 / 30.0f, "combat_pistols_equip", "combat_pistols_unequip",
     "combat_pistols_reload", {0, -64, 22}}, // mesh y -64.2 .. 15.4
    {"wp_uzi", "uzi", false, 5 / 30.0f, 15 / 30.0f, 15 / 30.0f, 21 / 30.0f, "combat_pistols_equip", "combat_pistols_unequip",
     "combat_pistols_reload", {0, -62, 15}}, // y -62.2 .. 38.5
    {"wp_shotgun", "shotgun", true, (13 + 25) / 30.0f, 24 / 30.0f, 24 / 30.0f, 117 / 30.0f, "combat_shotgun_equip",
     "combat_shotgun_unequip", "combat_shotgun_reload", {0, -156, 25}}, // y -155.8 .. 58.1
    {"wp_speargun", "speargun", true, 13 / 30.0f, 24 / 30.0f, 24 / 30.0f, 31 / 30.0f, "combat_shotgun_equip",
     "combat_shotgun_unequip", "combat_speargun_reload", {0, -152, 25}}, // y -151.8 .. 15.4
    {"wp_thorhammer", "thorhammer", false, 29 / 30.0f, 26 / 30.0f, 26 / 30.0f, 0, "ground_equip", "ground_unequip", "",
     {0, 0, 95}}, // handle along +Z from the grip, head z 60 .. 120
    {"wp_assault_rifle", "rifle", true, 3 / 30.0f, 19 / 30.0f, 24 / 30.0f, 41 / 30.0f, "combat_rifle_equip",
     "combat_rifle_unequip", "combat_rifle_reload", {0, -170, 20}, "combat_rifle_", 3 / 30.0f}, // y -170.6 .. 71.5
    {"wp_tranq", "tranquilizer", true, 3 / 30.0f, 19 / 30.0f, 24 / 30.0f, 19 / 30.0f, "combat_rifle_equip",
     "combat_rifle_unequip", "combat_tranqgun_reload", {0, -124, 10}, "combat_rifle_", 3 / 30.0f}, // y -124.6 .. 18.5
};
const WeaponInfo& info(Weapon w) { return kWeapons[size_t(w)]; }
constexpr float kHammerShot = 10 / 30.0f, kHammerPound = 12 / 30.0f; // events 0x75 / 0x4B
// [I] the ground pound's reach: its lightning (wp_thorhammer bolt 1, three at frame 12) ends up to +-948 from Lara.
// tru.exe applies 0x69F1E0 area hits from a runtime table (0x6E68B0) not traced to the data: damage [U], ours 0.
constexpr float kPoundRadius = 948.0f;
// Mjolnir's aim poses ground_aim_right / _front / _left by aim yaw; no pitch rows. Yaws [I]: the pointing (right) hand's
// shoulder -> hand direction in each pose relative to _front's (which points 9 deg right of the heading): -52, 0, +32.
const float kHammerCols[] = {-52 * kPi / 180, 0, 32 * kPi / 180};
constexpr float kPump = 25.0f / 30.0f;                                   // combat_shotgun_pump
constexpr float kSubEquip = 17.0f / 30.0f, kSubUnequip = 19.0f / 30.0f; // speargun under water
constexpr float kRespawnDelay = 1.0f; // after the death clip ends [I]
constexpr float kShoulderHeight = 418.0f; // bind-pose shoulder joints 53 / 78 above the feet
constexpr float kDeg = kPi / 180;
// Aim grids: columns right .. left with the aim yaw each was measured at, rows up / middle / down.
// Pistols (combat_pistol_pose[A-I], wp_handgun and wp_uzi): each pose's shoulder->hand directions (both arms,
// model space): columns C/F/I -104 deg, B/E/H 0, A/D/G +109; rows up +40, down -57.
// Two-handed (combat_shotgun_aim_* of the shotgun / speargun; combat_rifle_aim_* of the rifle / tranquilizer, the same
// columns and rows [I]): the barrel line rear (left, bone 55) -> front (right, 80) hand of each shotgun pose relative to
// _F (whose line points 15 deg right of the heading): middle row R -90, FR -47, F 0, FL +58, L +90; rows up +45, down -39
// (row means). _UFR is a copy of _FR in the data.
const float kPistolCols[] = {-104 * kDeg, 0, 109 * kDeg};
const float kRifleCols[] = {-90 * kDeg, -47 * kDeg, 0, 58 * kDeg, 90 * kDeg};
constexpr std::string_view kPistolCells[3][3] = {{"C", "B", "A"}, {"F", "E", "D"}, {"I", "H", "G"}};
constexpr std::string_view kRifleCells[3][5] = {{"UR", "UFR", "UF", "UFL", "UL"}, {"R", "FR", "F", "FL", "L"}, {"DR", "DFR", "DF", "DFL", "DL"}};
int gridCols(bool rifle) { return rifle ? 5 : 3; }
float colYaw(bool rifle, int col) { return rifle ? kRifleCols[col] : kPistolCols[col]; }
float gridUp(bool rifle) { return (rifle ? 45.0f : 40.0f) * kDeg; }
float gridDown(bool rifle) { return (rifle ? 39.0f : 57.0f) * kDeg; }
std::string cellName(const WeaponInfo& in, bool fire, int row, int col) {
    if (in.rifle) return std::string(in.grid) + (fire ? "fire_" : "aim_") + std::string(kRifleCells[row][col]);
    return "combat_pistol_pose" + std::string(kPistolCells[row][col]);
}
// Lara's bones: arms 52-55 (left, model +X) / 77-80 (right), thighs 4 (left) / 9 (right); upper body = the
// subtree of spine segment 14.
constexpr int kHandBone[2] = {80, 55}, kSpineBone = 14;

// Health packs (g_healthpack data; use = gadget fnC 0x6CBB20): heal +0xF0 = 640 through 0x6F4EF0 (clamped to max),
// cooldown +0xEA = 60 frames [I 30 Hz]; no full-health check in the exe.
constexpr float kHealthPackHeal = 640.0f, kHealthPackCooldown = 2.0f;
// Adrenaline [I]: the meter's fill rate and the slow-motion factor are not in the data we decoded (the world time
// scale DAT_00e7f1a0 is eased by script natives 0x776030 / 0x78C4E0; docs/GAMEPLAY.md "Combat").
constexpr float kAdrenalineFill = 1000.0f; // damage dealt to fill the meter
constexpr float kSlowScale = 0.2f, kSlowEase = 0.25f; // world time scale in the window, seconds to ease in / out
constexpr float kSlowWindow = 3.0f;  // real seconds to steer the pip in and fire
constexpr float kPipStart = 2.5f, kPipSpeed = 2.0f; // pip start distance / stick speed, reticle radii (per second)

bool swimHit(State s) { return s == State::SwimUnder || s == State::Surfacing || s == State::SwimDive || s == State::Hit; }
// Guns out on the ground (and in a hit reaction); under water only the speargun and Mjolnir (its swim_submerge attack
// clips) [U whether the exe allows more].
bool canShoot(const PlayerState& s, Weapon w) {
    if (s.water >= 0) return (w == Weapon::Speargun || w == Weapon::ThorHammer) && swimHit(s.state);
    switch (s.state) {
    case State::Idle: case State::Walk: case State::Run: case State::Stop: case State::Turn: case State::Land:
    case State::JumpPrep: case State::Jump: case State::Fall: case State::Hit: return true;
    default: return false;
    }
}
bool airborne(State s) { return s == State::Jump || s == State::Fall || s == State::JumpPrep; }

// ---- object data access (ObjectBase + 0xC8) -----------------------------------------------------
struct ObjectData {
    cdc::Drm drm;
    uint32_t section = 0, offset = 0; // the data block
    template <class T>
    std::optional<T> at(uint32_t off) const {
        const auto& d = drm.sections[section].data;
        if (offset + off + sizeof(T) > d.size()) return std::nullopt;
        T v;
        std::memcpy(&v, d.data() + offset + off, sizeof(T));
        return v;
    }
};
// Object section +0 = ObjectBase DTP id; ObjectBase +0xC8 -> object data (ObjectSection construct 0x54E200).
std::optional<ObjectData> objectData(const cdc::BigfileSet& bf, std::string_view object) {
    auto bytes = bf.read("pc-w\\" + std::string(object) + ".drm");
    if (!bytes) return std::nullopt;
    ObjectData o{cdc::parseDrm(std::move(*bytes))};
    const auto obj = std::find_if(o.drm.sections.begin(), o.drm.sections.end(), [](const cdc::Section& s) { return s.type == cdc::SectionType::Object; });
    if (obj == o.drm.sections.end() || obj->data.size() < 4) return std::nullopt;
    uint32_t baseId;
    std::memcpy(&baseId, obj->data.data(), 4);
    for (uint32_t i = 0; i < o.drm.sections.size(); ++i) {
        if (o.drm.sections[i].type != cdc::SectionType::DtpData || o.drm.sections[i].id != baseId) continue;
        const auto ptrs = cdc::sectionPointers(o.drm, i);
        const auto it = ptrs.find(0xC8);
        if (it == ptrs.end()) return std::nullopt;
        o.section = it->second.section, o.offset = it->second.offset;
        return o;
    }
    return std::nullopt;
}
} // namespace

// ---- health -------------------------------------------------------------------------------------
float Health::fallDamage(float h) const {
    const float a = t_.fall[0], b = t_.fall[1], c = t_.fall[2], m = t_.max;
    if (h > c) return m;
    if (h > b) return m * 0.5f + m * 0.5f * (h - b) / (c - b);
    if (h > a) return m * 0.5f * (h - a) / (b - a);
    return 0;
}

void Health::hurt(float amount, int type) {
    value_ -= amount;
    regenStart_ = now_ + t_.regenDelaySeconds; // 0x6E7880(1)
    regenBase_ = value_;
    invuln_ = t_.invulnSeconds;
    lastType_ = type;
}

bool Health::damage(float amount, int type) {
    if (dead() || amount <= 0) return false;
    if (type != kDamageAbsolute && type != kDamageBypass && invuln_ > 0 && priority(type) <= priority(lastType_)) return false;
    hurt(type == kDamageAbsolute ? amount : amount * t_.damageScale, type);
    return true;
}

void Health::fall(float height) {
    if (const float d = fallDamage(height); d > 0 && !dead()) hurt(d, kDamageAbsolute);
}

void Health::heal(float amount, bool overheal) {
    value_ += amount;
    if (!overheal) value_ = std::min(value_, t_.max);
}

void Health::tick(float dt) {
    now_ += dt;
    if (value_ > 0) {
        float delta = 0;
        const float cap = t_.max * t_.regenCap;
        if (value_ < cap && now_ > regenStart_) {
            const float frac = float(now_ - regenStart_) / t_.regenDurationSeconds;
            const float target = std::min(cap, ease(frac, 0, cap, t_.regenEase) + regenBase_);
            if (value_ <= target) delta = target - value_;
            else regenStart_ = now_, regenBase_ = value_; // 0x6E7880(0)
        }
        if (value_ > t_.max) // overheal decays to max over overhealDecaySeconds
            delta = std::max(-(t_.max * t_.overhealMax - t_.max) / t_.overhealDecaySeconds * dt, t_.max - value_);
        value_ += delta;
    }
    invuln_ = std::max(0.0f, invuln_ - dt);
}

void Health::reset() {
    value_ = t_.max;
    invuln_ = 0;
    lastType_ = 0;
    regenStart_ = now_;
    regenBase_ = value_;
}

// ---- weapons ------------------------------------------------------------------------------------
std::string_view weaponObject(Weapon w) { return info(w).object; }
std::string_view weaponName(Weapon w) { return info(w).name; }
std::optional<Weapon> weaponByName(std::string_view name) {
    for (int i = 0; i < kWeaponCount; ++i)
        if (kWeapons[i].name == name) return Weapon(i);
    return std::nullopt;
}

WeaponDef weaponDefaults(Weapon w) {
    WeaponDef d; // pistols
    switch (w) {
    case Weapon::Pistols: break;
    case Weapon::Uzi:
        d.object = "wp_uzi", d.ammo = "pr_uzibullet", d.ammoSlot = 0, d.maxReserve = {400, 240, 160}, d.clip = 40, d.infinite = false, d.stow = {36, 37};
        d.shortRange = 2000, d.maxRange = 4500, d.maxSpreadDeg = 5, d.spreadPerShot = 1, d.spreadRecovery = 0.1f;
        d.damage[0] = 25, d.damage[1] = 25, d.damage[2] = 10;
        d.wetDamage[0] = 25, d.wetDamage[1] = 25, d.wetDamage[2] = 10;
        break;
    case Weapon::Shotgun:
        d.object = "wp_shotgun", d.ammo = "pr_shotgunbullet", d.ammoSlot = 1, d.maxReserve = {90, 72, 42}, d.pellets = 7, d.clip = 6;
        d.hand = {5, 7}, d.stow = {15, 6};
        d.infinite = d.dual = false, d.shortRange = 2000, d.maxRange = 3000, d.spreadDeg = d.maxSpreadDeg = 15;
        d.damage[0] = 50, d.damage[1] = 50, d.damage[2] = 40;
        d.wetDamage[0] = 50, d.wetDamage[1] = 50, d.wetDamage[2] = 40;
        d.hitClass = 2;
        break;
    case Weapon::Speargun:
        d.object = "wp_speargun", d.ammo = "pr_spear", d.ammoSlot = 4, d.maxReserve = {40, 25, 15}, d.clip = 1, d.infinite = d.dual = false;
        d.hand = {5, 7}, d.stow = {15, 6};
        d.shortRange = 2500, d.maxRange = 5000, d.mode = 2, d.damage[0] = d.damage[1] = d.damage[2] = 250;
        d.wetDamage[0] = d.wetDamage[1] = d.wetDamage[2] = 250;
        break;
    case Weapon::ThorHammer: // pr_thorbolt: 1000 at every range on land, 1000 / 400 / 100 under water
        d.object = "wp_thorhammer", d.ammo = "pr_thorbolt", d.clip = -1, d.dual = false, d.shortRange = 1000, d.maxRange = 8000;
        d.hand = {5, 7}, d.stow = {31, 6};
        d.damage[0] = d.damage[1] = d.damage[2] = 1000;
        d.wetDamage[0] = 1000, d.wetDamage[1] = 400, d.wetDamage[2] = 100;
        d.hitClass = 13, d.hitType = 3;
        break;
    case Weapon::AssaultRifle: // pr_akriflebullet (as wp_akrifle, the poachers' gun)
        d.object = "wp_assault_rifle", d.ammo = "pr_akriflebullet", d.ammoSlot = 2, d.maxReserve = {300, 210, 120}, d.clip = 30;
        d.hand = {5, 7}, d.stow = {15, 6};
        d.infinite = d.dual = false, d.shortRange = 5000, d.maxRange = 8000;
        d.maxSpreadDeg = 5, d.spreadPerShot = 1.2f, d.spreadRecovery = 0.125f;
        d.damage[0] = 27, d.damage[1] = 27, d.damage[2] = 21;
        d.wetDamage[0] = 23, d.wetDamage[1] = 23, d.wetDamage[2] = 18;
        break;
    case Weapon::Tranquilizer: // pr_tranquilizerdart: a projectile (mode 2) whose +0x1C is the 500 dose
        d.object = "wp_tranq", d.ammo = "pr_tranquilizerdart", d.ammoSlot = 3, d.maxReserve = {25, 15, 10}, d.clip = 1;
        d.hand = {5, 7}, d.stow = {15, 6};
        d.infinite = d.dual = false, d.shortRange = 4000, d.maxRange = 6000, d.spreadDeg = d.maxSpreadDeg = 5, d.mode = 2;
        d.damage[0] = d.damage[1] = d.damage[2] = 500;
        d.wetDamage[0] = d.wetDamage[1] = d.wetDamage[2] = 500;
        break;
    }
    return d;
}

Weapon chapterSecondary(int chapter) {
    static constexpr Weapon k[] = {Weapon::Pistols, Weapon::Speargun, Weapon::Uzi, Weapon::Shotgun, Weapon::Tranquilizer,
                                   Weapon::Uzi, Weapon::AssaultRifle, Weapon::Speargun, Weapon::Uzi};
    return chapter >= 0 && size_t(chapter) < std::size(k) ? k[chapter] : Weapon::Pistols;
}

std::optional<Weapon> ammoWeapon(std::string_view object) {
    if (object == "wp_akrifle") return Weapon::AssaultRifle;
    for (const Weapon w : {Weapon::Uzi, Weapon::Shotgun, Weapon::Speargun, Weapon::AssaultRifle, Weapon::Tranquilizer})
        if (weaponObject(w) == object) return w;
    return std::nullopt;
}

CombatState fullClips(const std::array<WeaponDef, kWeaponCount>& weapons) {
    CombatState c;
    for (int i = 0; i < kWeaponCount; ++i) c.clip[i][0] = weapons[size_t(i)].clip, c.clip[i][1] = weapons[size_t(i)].dual ? weapons[size_t(i)].clip : 0;
    c.spread = weapons[0].spreadDeg;
    return c;
}

const std::vector<std::string_view>& weaponClipNames(Weapon w) {
    static const auto lists = [] {
        static std::array<std::vector<std::string>, kWeaponCount> own;
        std::array<std::vector<std::string_view>, kWeaponCount> out;
        for (int i = 0; i < kWeaponCount; ++i) {
            const WeaponInfo& wi = kWeapons[i];
            auto& o = own[size_t(i)];
            if (Weapon(i) == Weapon::ThorHammer) {
                o = {"ground_equip", "ground_unequip", "ground_aim_right", "ground_aim_front", "ground_aim_left",
                     "ground_fire_idle", "ground_melee_idle", "swim_submerge_thorhammer_attack_front"};
            } else {
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < gridCols(wi.rifle); ++c) {
                        o.push_back(cellName(wi, false, r, c));
                        if (wi.rifle) o.push_back(cellName(wi, true, r, c));
                    }
                if (!wi.rifle) o.insert(o.end(), {"combat_pistol_RfireE", "combat_pistol_LfireE"});
                o.insert(o.end(), {std::string(wi.equipClip), std::string(wi.unequipClip), std::string(wi.reloadClip)});
            }
            if (Weapon(i) == Weapon::Shotgun) o.push_back("combat_shotgun_pump");
            if (Weapon(i) == Weapon::Speargun)
                o.insert(o.end(), {"combat_shotgun_equip_submerged", "combat_shotgun_unequip_submerged", "combat_speargun_reload_submerged"});
            for (const auto& s : o) out[size_t(i)].push_back(s);
        }
        return out;
    }();
    return lists[size_t(w)];
}

const anim::Clip* AnimLibrary::weaponClip(Weapon w, std::string_view name) const {
    return find(std::string(weaponObject(w)) + ":" + std::string(name));
}

float WeaponDef::damageAt(float d, bool wet) const {
    const float* v = wet ? wetDamage : damage;
    if (d <= shortRange) return glm::mix(v[0], v[1], std::max(0.0f, d) / shortRange);
    return glm::mix(v[1], v[2], std::min(1.0f, (d - shortRange) / (maxRange - shortRange)));
}

std::optional<WeaponDef> loadWeapon(const cdc::BigfileSet& bf, std::string_view object) {
    const auto w = objectData(bf, object);
    if (!w) return std::nullopt;
    constexpr uint32_t S = 0xE0; // stats block (query 0x36, 0x6C1CF0)
    const auto slot = w->at<uint8_t>(S + 0x04);
    const auto pellets = w->at<int8_t>(S + 0x05), clip = w->at<int8_t>(S + 0x0B);
    const auto ammoId = w->at<int16_t>(S + 0x06);
    const auto maxRange = w->at<float>(S + 0x18), spread = w->at<float>(S + 0x1C), shortRange = w->at<float>(S + 0x20);
    const auto maxSpread = w->at<float>(S + 0x2C), perShot = w->at<float>(S + 0x30), recovery = w->at<float>(S + 0x34);
    const auto flags = w->at<uint8_t>(S + 0x88);
    const auto markers = w->at<std::array<uint8_t, 4>>(0xB8);
    if (!slot || !pellets || !clip || !ammoId || !maxRange || !spread || !shortRange || !maxSpread || !perShot || !recovery || !flags || !markers)
        return std::nullopt;
    const auto list = bf.read("\\tr8\\pc-w\\objectlist.txt");
    if (!list) return std::nullopt;
    const auto names = formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()});
    const auto ammoName = names.find(uint16_t(*ammoId));
    if (ammoName == names.end()) return std::nullopt;
    const auto ammo = objectData(bf, ammoName->second);
    if (!ammo) return std::nullopt;
    constexpr uint32_t R = 0x18, R2 = 0x84; // fire-mode records (0x69E770: +0x84 while instance +0x131 is set [I under water])
    const auto mode = ammo->at<int32_t>(R);
    if (!mode) return std::nullopt;
    const auto d0 = ammo->at<float>(*mode == 2 ? R + 0x1C : R + 4), d1 = ammo->at<float>(*mode == 2 ? R + 0x1C : R + 0x10),
               d2 = ammo->at<float>(*mode == 2 ? R + 0x1C : R + 0x14);
    if (!d0 || !d1 || !d2) return std::nullopt;
    // the second record's hitscan damage; projectiles (mode 2: its layout not decoded) keep the first's
    const bool wetScan = ammo->at<int32_t>(R2) == 1;
    const auto w0 = ammo->at<float>(R2 + 4), w1 = ammo->at<float>(R2 + 0x10), w2 = ammo->at<float>(R2 + 0x14);
    if (wetScan && (!w0 || !w1 || !w2)) return std::nullopt;
    WeaponDef def;
    def.object = std::string(object), def.ammo = ammoName->second;
    def.ammoSlot = *slot == 0xFF ? -1 : *slot;
    if (def.ammoSlot >= 0) { // Lara's max-reserve table (0x6EEAE0): u16 per slot, a column per ammunition capacity level
        const auto lara = objectData(bf, "lara");
        for (size_t l = 0; l < 3; ++l) {
            constexpr uint32_t kColumn[3] = {0x868, 0x87C, 0x872};
            const auto max = lara ? lara->at<uint16_t>(kColumn[l] + 2 * uint32_t(def.ammoSlot)) : std::nullopt;
            if (!max) return std::nullopt;
            def.maxReserve[l] = *max;
        }
    }
    def.pellets = *pellets, def.clip = *clip;
    def.infinite = *flags & 0x80, def.dual = *flags & 0x20;
    def.shortRange = *shortRange, def.maxRange = *maxRange;
    def.spreadDeg = *spread, def.maxSpreadDeg = *maxSpread, def.spreadPerShot = *perShot, def.spreadRecovery = *recovery;
    def.mode = *mode;
    if (*mode == 1) def.hitClass = ammo->at<int32_t>(R + 8).value_or(0), def.hitType = ammo->at<int32_t>(R + 0xC).value_or(1);
    def.hand = {(*markers)[0], (*markers)[2]}, def.stow = {(*markers)[1], (*markers)[3]};
    def.damage[0] = *d0, def.damage[1] = *d1, def.damage[2] = *d2;
    if (wetScan) def.wetDamage[0] = *w0, def.wetDamage[1] = *w1, def.wetDamage[2] = *w2;
    else std::copy(def.damage, def.damage + 3, def.wetDamage);
    return def;
}

std::optional<GrenadeTuning> loadGrenadeTuning(const cdc::BigfileSet& bf) {
    const auto g = objectData(bf, "g_grenade_thrower"), p = objectData(bf, "pr_grenade");
    if (!g || !p) return std::nullopt;
    const auto max0 = g->at<int16_t>(0xE6), max1 = g->at<int16_t>(0xE4), max2 = g->at<int16_t>(0xE2), cooldown = g->at<int16_t>(0xEA);
    const auto vz = g->at<float>(0x10C), gravity = g->at<float>(0x110), lo = g->at<float>(0x134), hi = g->at<float>(0x138);
    // pr_grenade explosion record (+0x44 = 2): +0x48 damage, +0x4C radius, +0x50 edge damage, +0x58 / +0x5C knock speeds,
    // +0x80 frames (0x69F1E0 parameters [I mapping])
    const auto kind = p->at<int32_t>(0x44), fuse = p->at<int32_t>(0x80);
    const auto dmg = p->at<float>(0x48), radius = p->at<float>(0x4C), edge = p->at<float>(0x50);
    const auto imp = p->at<float>(0x58), edgeImp = p->at<float>(0x5C);
    if (!max0 || !max1 || !max2 || !cooldown || !vz || !gravity || !lo || !hi || kind != 2 || !fuse || !dmg || !radius || !edge || !imp || !edgeImp)
        return std::nullopt;
    GrenadeTuning t;
    t.maxCarried = {*max0, *max1, *max2}, t.cooldown = *cooldown / 30.0f;
    t.launchVz = *vz, t.gravity = *gravity, t.minRange = *lo, t.maxRange = *hi;
    t.damage = *dmg, t.radius = *radius, t.edgeDamage = *edge, t.impulse = *imp, t.edgeImpulse = *edgeImp;
    t.fuse = *fuse / 30.0f;
    return t;
}

std::optional<DifficultyTable> loadDifficulty(const cdc::BigfileSet& bf) {
    const auto g = objectData(bf, "generalbank"); // its object data is GlobalInfo (tru.exe DAT_00e7ee50)
    if (!g) return std::nullopt;
    const auto ptrs = cdc::sectionPointers(g->drm, g->section);
    const auto table = ptrs.find(g->offset + 0x24);
    if (table == ptrs.end()) return std::nullopt;
    const auto& d = g->drm.sections[table->second.section].data;
    auto at = [&](uint32_t level, uint32_t off) -> std::optional<float> {
        const size_t o = table->second.offset + level * 0x18 + off;
        if (o + 4 > d.size()) return std::nullopt;
        float v;
        std::memcpy(&v, d.data() + o, 4);
        return v;
    };
    DifficultyTable t;
    for (uint32_t l = 0; l < 3; ++l) {
        const auto health = at(l, 0x08), grab = at(l, 0x14);
        if (!health || !grab || !(*health > 0) || !(*grab > 0)) return std::nullopt;
        t.healthScale[l] = *health, t.savingGrabSeconds[l] = *grab / 30.0f;
    }
    return t;
}

std::optional<float> rayCapsule(glm::vec3 ro, glm::vec3 rd, glm::vec3 base, float r, float h, float maxDist) {
    const glm::vec3 pa = base + glm::vec3(0, 0, r), pb = base + glm::vec3(0, 0, std::max(r, h - r));
    std::optional<float> best;
    const auto take = [&](float t) {
        if (t >= 0 && t <= maxDist && (!best || t < *best)) best = t;
    };
    // body: infinite cylinder around the axis, limited to the segment
    const glm::vec3 ba = pb - pa, oa = ro - pa;
    const float baba = glm::dot(ba, ba), bard = glm::dot(ba, rd), baoa = glm::dot(ba, oa);
    const float a = baba - bard * bard, b = baba * glm::dot(rd, oa) - baoa * bard,
                c = baba * glm::dot(oa, oa) - baoa * baoa - r * r * baba;
    if (const float disc = b * b - a * c; a > 1e-6f && disc >= 0) {
        const float t = (-b - std::sqrt(disc)) / a, y = baoa + t * bard;
        if (y > 0 && y < baba) take(t);
    }
    for (const glm::vec3 cap : {pa, pb}) { // end spheres
        const glm::vec3 oc = ro - cap;
        const float hb = glm::dot(oc, rd), hc = glm::dot(oc, oc) - r * r, disc = hb * hb - hc;
        if (disc >= 0) take(-hb - std::sqrt(disc));
    }
    return best;
}

// ---- World: damage, hit reactions, death --------------------------------------------------------
// Hit reactions: tru.exe picks the clip in Lara's anim graph from the attacker's angle to her heading (0x6EB250;
// thresholds not decoded) and mirrors for the other side (flag 0x20). Ours [I]: light (type 1) hit_light_*,
// heavy (2) hit_medium*; front within 45 deg of her heading, back beyond 135, else side (no mirroring: the side /
// right clip plays for both sides); swimming the *_swim variants. Only on the ground or swimming, and only when
// Health::damage accepted the hit.
void World::damageLara(float amount, vec3 from, int type) {
    if (!health_.damage(amount, type) || health_.dead() || (type != kDamageLight && type != kDamageHeavy)) return;
    const PlayerState& s = player_.state();
    const bool swim = s.water >= 0;
    if (swim ? !(swimHit(s.state) || s.state == State::SwimTread || s.state == State::SwimSurface)
             : !(s.state == State::Idle || s.state == State::Walk || s.state == State::Run || s.state == State::Stop ||
                 s.state == State::Turn || s.state == State::Land || s.state == State::Hit))
        return;
    const float a = std::abs(wrapAngle(std::atan2(from.y - s.pos.y, from.x - s.pos.x) - s.yaw));
    const int dir = a <= 45 * kDeg ? 0 : a >= 135 * kDeg ? 1 : 2;
    static constexpr std::string_view kLight[] = {"hit_light_front", "hit_light_back", "hit_light_side"};
    static constexpr std::string_view kHeavy[] = {"hit_mediumfront", "hit_mediumback", "hit_mediumright"};
    player_.hitReact(std::string(type == kDamageHeavy ? kHeavy[dir] : kLight[dir]) + (swim ? "_swim" : ""));
}

void World::kill() { health_.damage(health_.value(), kDamageKill); }

// ---- World: guns --------------------------------------------------------------------------------
std::optional<std::pair<int, glm::mat4>> World::gunMount(Weapon w, int hand) const {
    const CombatState& c = combat_;
    const bool inHand = w == c.weapon && c.drawT >= 0.5f; // [I] the gun changes bones half way through equip / unequip
    const WeaponDef& d = anims_.weapons[size_t(w)];
    // [O] the weapon model sits at one of Lara's HInfo markers (WeaponDef::hand / stow, data +0xB8..+0xBB) with the
    // marker's frame as its own. A library without markers (synthetic tests) draws no stowed weapon.
    const auto marker = [&](int id) -> std::optional<std::pair<int, glm::mat4>> {
        const auto it = anims_.markers.find(id);
        if (it == anims_.markers.end()) return std::nullopt;
        return it->second;
    };
    glm::mat4 m(1);
    if (w == Weapon::ThorHammer) {
        // In hand marker 5 (segment 71, the left hand's grip child of 55), stowed 31 (chest segment 15, on her back):
        // the left hand draws it from the back (ground_equip) and holds it up while the right one points.
        if (hand != 0 || (w != c.weapon && !hasHammer())) return std::nullopt;
        return marker(inHand ? d.hand[0] : d.stow[0]);
    }
    if (!info(w).rifle) {
        if (!inHand) return w == Weapon::Pistols || w == c.secondary ? marker(d.stow[size_t(hand)]) : std::nullopt;
        // barrel (gun -Y) along the arm, gun top (+Z) along the thumb (-Y)
        const float side = hand == 0 ? -1.0f : 1.0f; // right arm extends along -X, left along +X
        m[0] = glm::vec4(0, 0, side, 0);
        m[1] = glm::vec4(-side, 0, 0, 0);
        m[2] = glm::vec4(0, -1, 0, 0);
        m[3] = glm::vec4(side * 18.0f, 0, 0, 1);
        return std::pair{kHandBone[hand], m};
    }
    if (hand != 0 || (w != c.weapon && w != c.secondary)) return std::nullopt;
    if (!inHand) return marker(d.stow[0]); // marker 15: across her back
    // two-handed: the grip in the rear (left, 55) hand, the barrel along the line to the front (right, 80) hand
    // (the aim poses' barrel line), gun top toward model up
    if (bones_.size() <= size_t(kHandBone[0])) return std::pair{kHandBone[1], m}; // synthetic skeleton
    const vec3 rear(bones_[size_t(kHandBone[1])][3]), front(bones_[size_t(kHandBone[0])][3]);
    const vec3 fwd = glm::normalize(front - rear + vec3(0, -1e-3f, 0));
    const vec3 up = glm::normalize(vec3(0, 0, 1) - fwd * fwd.z + vec3(0, 1e-3f, 0));
    m[1] = glm::vec4(-fwd, 0);
    m[2] = glm::vec4(up, 0);
    m[0] = glm::vec4(glm::cross(-fwd, up), 0);
    m[3] = glm::vec4(rear, 1);
    return std::pair{kHandBone[1], glm::inverse(bones_[size_t(kHandBone[1])]) * m};
}

vec3 World::muzzle(int hand) const {
    const PlayerState& s = player_.state();
    const auto mount = gunMount(combat_.weapon, hand);
    glm::mat4 b(1), offset(1); // no such bone (synthetic skeletons): the gun at shoulder height
    if (mount) offset = mount->second;
    if (mount && size_t(mount->first) < bones_.size()) b = bones_[size_t(mount->first)];
    else b[3].z = kShoulderHeight;
    return vec3(player_.modelMatrix(s.pos, s.yaw, s.pitch) * b * offset * glm::vec4(info(combat_.weapon).muzzle, 1));
}

void World::giveWeapon(Weapon w, int reserve) {
    CombatState& c = combat_;
    if (w == Weapon::ThorHammer) { // her own slot, as larabase 37("wp_thorhammer") in 250_val_hammer's end handler
        auto objects = script_.playerObjects();
        objects.insert(std::string(weaponObject(w)));
        script_.setPlayerObjects(std::move(objects));
        return;
    }
    if (const Weapon old = c.secondary; old != Weapon::Pistols) { // the new secondary takes the old one's place
        if (c.weapon == old) c.weapon = w;
        if (c.want == old) c.want = w;
    }
    c.secondary = w;
    if (w == Weapon::Pistols) return;
    const WeaponDef& d = anims_.weapons[size_t(w)];
    const auto i = size_t(w);
    c.clip[i][0] = d.clip, c.clip[i][1] = d.dual ? d.clip : 0;
    const int max = d.maxReserve[size_t(std::clamp(tailoring_.ammo, 0, 2))];
    c.reserve[i] = std::clamp(reserve, 0, max > 0 ? max : reserve);
}

void World::equipWeapon(Weapon w) {
    CombatState& c = combat_;
    if (w == Weapon::ThorHammer || w == c.secondary) return;
    if (c.secondary != Weapon::Pistols && c.want == c.secondary) { // the old secondary in hand: switch as Change Weapon
        c.redraw = c.redraw || c.drawn;
        c.want = w;
    }
    c.secondary = w;
}

void World::chapterLoadout(int chapter) {
    CombatState& c = combat_;
    for (const Weapon w : {Weapon::Uzi, Weapon::Shotgun, Weapon::Speargun, Weapon::AssaultRifle, Weapon::Tranquilizer}) {
        const WeaponDef& d = anims_.weapons[size_t(w)];
        c.clip[size_t(w)][0] = d.clip, c.clip[size_t(w)][1] = d.dual ? d.clip : 0;
        c.reserve[size_t(w)] = maxReserve(w);
    }
    c.weapon = c.want = c.previous = Weapon::Pistols;
    c.drawn = c.redraw = false, c.drawT = 0, c.reload = 0, c.hammer = {};
    c.secondary = chapterSecondary(chapter);
}

nlohmann::json World::serializeCombat() const {
    const CombatState& c = combat_;
    nlohmann::json guns = nlohmann::json::object();
    for (int i = 0; i < kWeaponCount; ++i)
        guns[std::string(weaponName(Weapon(i)))] = {{"clip", {c.clip[i][0], c.clip[i][1]}}, {"reserve", c.reserve[i]}};
    return {{"weapon", weaponName(c.weapon)}, {"secondary", weaponName(c.secondary)}, {"guns", guns}, {"adrenaline", c.adrenaline}};
}

void World::restoreCombat(const nlohmann::json& j) {
    if (!j.is_object()) return;
    const auto weapon = weaponByName(j.value("weapon", std::string{})), secondary = weaponByName(j.value("secondary", std::string{}));
    if (!weapon || !secondary) return;
    CombatState c = fullClips(anims_.weapons); // holstered, as a respawn
    c.weapon = c.want = c.previous = *weapon, c.secondary = *secondary;
    const nlohmann::json guns = j.value("guns", nlohmann::json::object());
    for (int i = 0; i < kWeaponCount && guns.is_object(); ++i) {
        const auto g = guns.find(std::string(weaponName(Weapon(i))));
        if (g == guns.end() || !g->is_object()) continue;
        const auto clip = g->value("clip", std::array<int, 2>{});
        const int max = anims_.weapons[size_t(i)].clip;
        if (max < 0) continue; // Mjolnir: no clip
        c.clip[i][0] = std::clamp(clip[0], 0, max), c.clip[i][1] = std::clamp(clip[1], 0, max);
        c.reserve[i] = std::max(0, g->value("reserve", 0));
    }
    c.adrenaline = std::clamp(j.value("adrenaline", 0.0f), 0.0f, 1.0f);
    c.spread = anims_.weapons[size_t(c.weapon)].spreadDeg;
    c.rng = combat_.rng;
    combat_ = c;
}

void World::setTailoring(const Tailoring& t) {
    tailoring_ = t;
    const auto level = [](int v) { return size_t(std::clamp(v, 0, 2)); };
    health_.setMax(HealthTuning{}.max * anims_.difficulty.healthScale[level(t.laraHealth)]);
    enemies_.setHealthLevel(int(level(t.enemyHealth)));
    player_.setSavingGrabSeconds(anims_.difficulty.savingGrabSeconds[level(t.savingGrab)]);
    for (int i = 0; i < kWeaponCount; ++i) // a smaller capacity drops what no longer fits
        if (const int max = anims_.weapons[size_t(i)].maxReserve[level(t.ammo)]; max > 0) combat_.reserve[i] = std::min(combat_.reserve[i], max);
    grenades = std::min(grenades, anims_.grenade.maxCarried[level(t.ammo)]);
}

void World::reloadWeapon() {
    CombatState& c = combat_;
    const auto i = size_t(c.weapon);
    const WeaponDef& w = anims_.weapons[i];
    for (int h = 0; h < (w.dual ? 2 : 1); ++h) {
        const int take = w.infinite ? w.clip - c.clip[i][h] : std::min(w.clip - c.clip[i][h], c.reserve[i]);
        c.clip[i][h] += take;
        if (!w.infinite) c.reserve[i] -= take;
    }
}

std::vector<Damageable*> World::candidates(bool hard) const {
    const PlayerState& s = player_.state();
    const vec3 eye = s.pos + vec3(0, 0, targeting.eyeHeight);
    const vec3 camFwd = cam_.target - cam_.position;
    const vec2 laraFwd(std::cos(s.yaw), std::sin(s.yaw));
    struct C { Damageable* t; float d3, d2, cam, horiz; };
    std::vector<C> cs;
    for (Damageable* t : targets) {
        if (!t || !t->alive() || t->team() != Team::Enemy) continue;
        const vec3 p = t->aimPoint(), v = p - eye;
        const float d3 = glm::length(v);
        if (d3 < 1) continue;
        // line of sight from Lara's eye (flag 0x04000000): geometry nearer than the target's surface blocks
        if (const auto hit = col_.raycast(eye, v / d3, d3); hit && hit->t < d3 - t->radius()) continue;
        cs.push_back({t, d3, glm::length(vec2(v)), angleBetween(camFwd, p - cam_.position),
                      angleBetween(vec3(laraFwd, 0), vec3(vec2(v), 0))});
    }
    const float rad = kPi / 180;
    std::vector<Damageable*> out;
    const auto tier = [&](auto pass, auto key) {
        std::vector<C> t;
        for (const C& c : cs)
            if (pass(c) && std::find(out.begin(), out.end(), c.t) == out.end()) t.push_back(c);
        std::stable_sort(t.begin(), t.end(), [&](const C& a, const C& b) { return key(a) < key(b); });
        for (const C& c : t) out.push_back(c.t);
    };
    const auto byDist = [](const C& c) { return c.d3; };
    const auto byCam = [](const C& c) { return c.cam; };
    if (hard) { // +0x480: three tiers, the first non-empty one wins
        tier([&](const C& c) { return c.d3 <= targeting.closeRange; }, byDist);
        tier([&](const C& c) { return c.d3 <= targeting.lockRange && c.cam <= targeting.cameraConeDeg * rad; }, byCam);
        tier([&](const C& c) { return c.d3 <= targeting.lockRange && c.horiz <= targeting.cameraConeDeg * rad; }, byCam);
    } else { // +0x470: nearest within the soft cone in front of Lara
        tier([&](const C& c) {
            return c.d2 <= targeting.softRange && c.horiz <= targeting.softConeDeg * rad && c.cam <= targeting.cameraConeDeg * rad;
        }, byDist);
    }
    return out;
}

namespace {
// 32-bit LCG in [-0.5, 0.5]: spread and the adrenaline pip stay replay-exact
float rnd(uint64_t& rng) {
    rng = rng * 1664525u + 1013904223u;
    return float((rng >> 8) & 0xFFFF) / 65535.0f - 0.5f;
}
} // namespace

void World::fire(int hand) {
    CombatState& c = combat_;
    const WeaponDef& w = anims_.weapons[size_t(c.weapon)];
    const vec3 from = muzzle(hand);
    vec3 aim = c.aimPoint - from;
    if (glm::length(aim) < 1) aim = vec3(std::cos(player_.state().yaw), std::sin(player_.state().yaw), 0);
    aim = glm::normalize(aim);
    const vec3 right = glm::normalize(glm::cross(aim, vec3(0, 0, 1)) + vec3(1e-6f, 0, 0));
    for (int pellet = 0; pellet < std::max(1, w.pellets); ++pellet) {
        // spread: yaw and pitch offsets within +-spread/2 degrees (0x69E300), per pellet
        const float dy = rnd(c.rng) * c.spread * kDeg, dp = rnd(c.rng) * c.spread * kDeg;
        const vec3 dir = glm::normalize(glm::angleAxis(dy, vec3(0, 0, 1)) * (glm::angleAxis(dp, right) * aim));
        float best = w.maxRange;
        bool hitAny = false;
        int32_t object = 0; // the dynamic collider (script object) the bullet hit
        if (const auto g = col_.raycast(from, dir, best))
            best = g->t, hitAny = true, object = g->dynamic >= 0 ? col_.dynamics[size_t(g->dynamic)].id : 0;
        Damageable* struck = nullptr;
        for (Damageable* t : targets) {
            if (!t || !t->alive() || t->team() == Team::Player) continue;
            if (const auto d = rayCapsule(from, dir, t->position(), t->radius(), t->height(), best)) best = *d, struck = t, hitAny = true;
        }
        Shot shot{from, from + dir * best, hand, hitAny, struck, 0};
        if (struck) {
            shot.damage = w.damageAt(best, submerged());
            Hit hit{shot.damage, shot.to, dir, Team::Player};
            hit.tranq = c.weapon == Weapon::Tranquilizer; // pr_tranquilizerdart: the damage is its dose
            hit.damageClass = w.hitClass, hit.damageType = w.hitType;
            struck->onHit(hit);
            c.adrenaline = std::min(1.0f, c.adrenaline + shot.damage / kAdrenalineFill); // "damaging enemies" (locals 1310)
        }
        if (!struck && object) uberShot(object, dir);
        c.shots.push_back(shot);
    }
    c.spread = std::min(w.maxSpreadDeg, c.spread + w.spreadPerShot);
    c.sinceShot[hand] = 0;
    ++c.shotsFired;
}

void World::groundPound() {
    const vec3 p = player_.state().pos;
    for (Damageable* t : targets) {
        if (!t || !t->alive() || t->team() != Team::Enemy) continue;
        const vec3 d = t->position() - p;
        if (glm::length(vec2(d)) > kPoundRadius + t->radius() || std::abs(d.z) > kPoundRadius) continue;
        Hit hit{0, t->aimPoint(), glm::normalize(vec3(vec2(d), 0) + vec3(1e-4f, 0, 0)), Team::Player, false, true};
        hit.damageClass = 13, hit.damageType = 3; // [I] as pr_thorbolt: Mjolnir's blows finish thralls
        t->onHit(hit);
    }
}

void World::tickCombat(const Actions& a, float dt) {
    CombatState& c = combat_;
    c.shots.clear();
    const PlayerState& s = player_.state();
    if (s.fallHeight > 0) health_.fall(s.fallHeight); // landing (tru.exe 0x75D080 -> 0x702FC0)
    health_.tick(dt);
    const bool drawPressed = a.draw && !c.drawHeld, switchPressed = a.switchTarget && !c.switchHeld,
               weaponPressed = a.weapon && !c.weaponHeld, healPressed = a.heal && !c.healHeld,
               interactPressed = a.interact && !c.interactHeld, firePressed = a.fire && !c.fireHeld;
    c.drawHeld = a.draw, c.switchHeld = a.switchTarget, c.weaponHeld = a.weapon, c.healHeld = a.heal;
    c.interactHeld = a.interact, c.fireHeld = a.fire;
    // health pack: the carried count is World::found[2] (picked-up packs)
    c.healCooldown = std::max(0.0f, c.healCooldown - dt);
    if (healPressed && c.healCooldown <= 0 && found[2] > 0 && !health_.dead()) {
        health_.heal(kHealthPackHeal);
        --found[2];
        c.healCooldown = kHealthPackCooldown;
        ++c.healthPacksUsed;
    }
    // world (enemy) time eases into / out of the adrenaline window; Lara keeps real time
    c.timeScale = approach(c.timeScale, c.slowMo > 0 ? kSlowScale : 1.0f, (1 - kSlowScale) * dt / kSlowEase);
    if (s.state == State::Dead) {
        c.deathTime += dt;
        const anim::Clip* clip = anims_.find(s.clip);
        if (c.deathTime >= (clip ? clip->durationSeconds() : 0) + kRespawnDelay) respawn();
        return;
    }
    if (health_.dead() && !airborne(s.state)) { // [I] dies on the ground; fatal falls die on landing
        player_.die(health_.lastType() == kDamageAbsolute ? "death_fall_normal" : "death_toknee");
        CombatState d; // holstered; the weapons and their rounds are kept [I]
        d.weapon = d.want = c.weapon, d.previous = c.previous, d.secondary = c.secondary;
        std::copy(&c.clip[0][0], &c.clip[0][0] + 2 * kWeaponCount, &d.clip[0][0]);
        std::copy(c.reserve, c.reserve + kWeaponCount, d.reserve);
        d.spread = anims_.weapons[size_t(c.weapon)].spreadDeg;
        d.rng = c.rng, d.deathTime = 0;
        c = d;
        return;
    }
    // weapon switch: the weapon in hand holsters (unequip clip), then the next is drawn; tru.exe 0x704930 cycles
    // pistols -> secondary -> Mjolnir, skipping what she lacks
    if (c.want == Weapon::ThorHammer && !hasHammer()) c.want = Weapon::Pistols, c.hammer = {};
    if (weaponPressed && c.slowMo <= 0) {
        Weapon cycle[3] = {Weapon::Pistols};
        int n = 1;
        if (c.secondary != Weapon::Pistols) cycle[n++] = c.secondary;
        if (hasHammer()) cycle[n++] = Weapon::ThorHammer;
        const auto at = std::find(cycle, cycle + n, c.want);
        if (n > 1) c.want = at == cycle + n || at + 1 == cycle + n ? cycle[0] : *(at + 1);
        c.redraw = c.redraw || (n > 1 && c.drawn);
    }
    if (c.want != c.weapon) {
        c.drawn = false, c.reload = 0, c.hammer = {};
        if (c.drawT <= 0) {
            c.previous = c.weapon, c.weapon = c.want;
            c.drawn = c.redraw && canShoot(s, c.weapon);
            c.redraw = false, c.cooldown = 0, c.hand = 0;
            c.spread = anims_.weapons[size_t(c.weapon)].spreadDeg;
        }
    } else if (!canShoot(s, c.weapon)) c.drawn = false; // hanging, swimming (not the speargun under water): holstered
    else if (c.redraw) c.drawn = true, c.redraw = false;
    else if (drawPressed) c.drawn = !c.drawn;
    else if (a.fire && !c.drawn) c.drawn = true; // [I] fire draws
    const auto wi = size_t(c.weapon);
    const WeaponDef& w = anims_.weapons[wi];
    const WeaponInfo& in = info(c.weapon);
    const bool sub = s.water >= 0 && c.weapon == Weapon::Speargun;
    c.drawT = approach(c.drawT, c.drawn ? 1.0f : 0.0f, dt / (c.drawn ? (sub ? kSubEquip : in.equip) : (sub ? kSubUnequip : in.unequip)));
    for (float& t : c.sinceShot) t += dt;
    c.cooldown = std::max(0.0f, c.cooldown - dt);
    // spread: reset on draw (0x6E6C60), grows per shot, recovers S+0x34 per 30 Hz frame [I unit]
    c.spread = c.drawn ? std::max(w.spreadDeg, c.spread - w.spreadRecovery * 30 * dt) : w.spreadDeg;
    if (c.reload > 0 && (c.reload -= dt) <= 0) c.reload = 0, reloadWeapon();
    const bool known = std::find(targets.begin(), targets.end(), c.target) != targets.end();
    if (!c.drawn || !known || !c.target->alive() ||
        glm::length(c.target->aimPoint() - s.pos) > targeting.lockRange) // [I] the lock drops past the tier range
        c.target = nullptr, c.locked = false;
    if (!c.target) c.slowMo = 0; // the window needs its target
    if (!c.drawn) {
        c.hammer = {};
        return;
    }

    // targeting: aim held = hard lock (kept, switchable), else the soft target in front of Lara; the adrenaline
    // window keeps its target
    if (c.slowMo > 0) {
    } else if (a.aim) {
        if (!c.locked || !c.target) {
            const auto cand = candidates(true);
            c.target = cand.empty() ? nullptr : cand.front();
            c.locked = c.target != nullptr;
        } else if (switchPressed) { // [I] next target in tier order
            const auto cand = candidates(true);
            const auto it = std::find(cand.begin(), cand.end(), c.target);
            if (!cand.empty()) c.target = it == cand.end() || it + 1 == cand.end() ? cand.front() : *(it + 1);
        }
    } else {
        c.locked = false;
        const auto cand = candidates(false);
        c.target = cand.empty() ? nullptr : cand.front();
    }
    if (c.target) {
        c.aimPoint = c.target->aimPoint();
    } else { // free aim: the camera's view ray (DAT_00fadb58+0x800 free-aim point)
        const vec3 d = glm::normalize(cam_.target - cam_.position);
        const auto hit = col_.raycast(cam_.position, d, w.maxRange);
        c.aimPoint = cam_.position + d * (hit ? hit->t : w.maxRange);
    }
    const vec3 v = c.aimPoint - (s.pos + vec3(0, 0, kShoulderHeight));
    if (c.locked && (s.state == State::Idle || s.state == State::Turn)) // [I] standing: turn to face the lock
        player_.face(s.yaw + std::clamp(wrapAngle(std::atan2(v.y, v.x) - s.yaw), -player_.tuning().turnRate * dt,
                                        player_.tuning().turnRate * dt));
    const bool hammer = c.weapon == Weapon::ThorHammer;
    const float yawLo = hammer ? kHammerCols[0] : colYaw(in.rifle, 0), yawHi = hammer ? kHammerCols[2] : colYaw(in.rifle, gridCols(in.rifle) - 1);
    c.aim = {std::clamp(wrapAngle(std::atan2(v.y, v.x) - player_.state().yaw), yawLo, yawHi),
             hammer ? 0.0f : std::clamp(std::atan2(v.z, glm::length(vec2(v))), -gridDown(in.rifle), gridUp(in.rifle))};

    const int hand = w.dual ? c.hand : 0;
    if (hammer) { // Mjolnir: swings and ground pounds play out; no adrenaline head shot [I]
        HammerAction& h = c.hammer;
        if (!h.clip.empty()) {
            h.prevT = h.t, h.t += dt;
            const auto reached = [&](float at) { return h.prevT < at && h.t >= at; };
            if (h.clip == "ground_melee_idle") {
                if (reached(kHammerPound)) groundPound();
            } else if (reached(kHammerShot)) {
                fire(0);
            }
            const anim::Clip* k = anims_.weaponClip(Weapon::ThorHammer, h.clip);
            if (h.t >= (k ? float(k->durationSeconds()) : in.refire)) h = {};
        } else if (c.drawT >= 1 && interactPressed && c.locked && s.water < 0) {
            h = {"ground_melee_idle", 0, -1 / 30.0f}; // target lock + interact
        } else if (c.drawT >= 1 && a.fire) {
            h = {s.water >= 0 ? "swim_submerge_thorhammer_attack_front" : "ground_fire_idle", 0, -1 / 30.0f};
        }
        return;
    }
    if (c.slowMo > 0) { // adrenaline window: the stick steers the pip; fire with it in the reticle = head shot
        c.pip += a.move * (kPipSpeed * dt);
        c.slowMo = std::max(0.0f, c.slowMo - dt);
        if (firePressed) {
            if (glm::length(c.pip) <= 1) { // Hit::headshot, damage = the target's remaining health
                const vec3 from = muzzle(hand), to = c.target->aimPoint();
                const Shot shot{from, to, hand, true, c.target, c.target->health()};
                Hit hit{shot.damage, to, glm::normalize(to - from + vec3(1e-6f, 0, 0)), Team::Player, true};
                hit.damageClass = anims_.weapons[size_t(c.weapon)].hitClass, hit.damageType = anims_.weapons[size_t(c.weapon)].hitType;
                c.target->onHit(hit);
                c.shots.push_back(shot);
                c.sinceShot[hand] = 0;
                ++c.shotsFired;
            }
            c.slowMo = 0; // one shot per window [I]
        }
        if (c.slowMo <= 0) c.adrenaline = 0; // spent
    } else if (interactPressed && c.adrenaline >= 1 && c.locked && c.target && c.drawT >= 1 &&
               glm::length(c.target->aimPoint() - (s.pos + vec3(0, 0, targeting.eyeHeight))) <= targeting.closeRange) {
        // "full meter, lock on, get close and press X" (locals 1333/1334); close = the close lock tier [I]
        const float ang = (rnd(c.rng) + 0.5f) * 2 * kPi;
        c.slowMo = kSlowWindow;
        c.pip = kPipStart * vec2(std::cos(ang), std::sin(ang));
    } else if (a.fire && c.drawT >= 1 && c.reload <= 0 && c.cooldown <= 0) {
        const bool canReload = w.infinite || c.reserve[wi] > 0;
        if (c.clip[wi][hand] <= 0) {
            if (canReload) c.reload = in.reload;
            else if (c.weapon != Weapon::Pistols) c.want = Weapon::Pistols, c.redraw = true; // [I] dry: back to the pistols
        } else {
            fire(hand);
            --c.clip[wi][hand]; // the clip empties even with infinite reserve (0x6C9E50)
            if (w.dual) c.hand ^= 1;
            c.cooldown = in.refire;
            if ((w.dual ? c.clip[wi][0] <= 0 && c.clip[wi][1] <= 0 : c.clip[wi][0] <= 0) && canReload) c.reload = in.reload;
        }
    }
}

// Upper body while a weapon is out (subtree of spine segment 14): its aim grid blended bilinearly by the aim
// angles; pistols / uzi add the fire clip of the hand that just fired (its motion relative to its first frame),
std::pair<std::string, double> World::combatLayerClip() const {
    const CombatState& c = combat_;
    const WeaponInfo& in = info(c.weapon);
    const bool sub = player_.state().water >= 0 && c.weapon == Weapon::Speargun;
    if (c.drawT <= 0) return {};
    if (c.reload > 0) return {sub ? "combat_speargun_reload_submerged" : std::string(in.reloadClip), in.reload - c.reload};
    if (c.drawT >= 1) return {};
    const float dur = c.drawn ? (sub ? kSubEquip : in.equip) : (sub ? kSubUnequip : in.unequip);
    return {std::string(c.drawn ? in.equipClip : in.unequipClip) + (sub ? "_submerged" : ""), (c.drawn ? c.drawT : 1 - c.drawT) * dur};
}

// rifles show the fire_* grid for the shot and the shotgun adds its pump; the reload clip replaces it while
// reloading. Drawing / holstering play the equip / unequip clip while drawT ramps, blended into the aim pose over
// the last / first 30 % of the ramp, the layer fading in / out over 0.1 s at the holstered end [I].
void World::aimLayer(std::vector<anim::LocalTransform>& local, float dt) {
    aimTime_ += dt;
    const CombatState& c = combat_;
    const auto& sk = anims_.skeleton;
    const Weapon wpn = c.weapon;
    const WeaponInfo& in = info(wpn);
    const bool rifle = in.rifle, sub = player_.state().water >= 0 && wpn == Weapon::Speargun, hammer = wpn == Weapon::ThorHammer;
    const int cols = gridCols(rifle);
    const auto clip = [&](std::string_view n) { return anims_.weaponClip(wpn, n); };
    const anim::Clip* centre = clip(hammer ? std::string("ground_aim_front") : cellName(in, false, 1, cols / 2));
    if (c.drawT <= 0 || local.size() != sk.bones.size() || sk.bones.size() <= size_t(kHandBone[0]) || !centre) return;
    const auto sample = [&](const anim::Clip* k, double t, bool loop) { return anim::sample(*k, sk, t, loop); };
    const auto additive = [&](std::vector<anim::LocalTransform>& p, const anim::Clip* k, double t) {
        const auto f0 = sample(k, 0, false), ft = sample(k, t, false);
        for (size_t i = 0; i < p.size(); ++i) {
            const glm::quat q0 = glm::quat_cast(anim::rotationFromVector(f0[i].rot)), qt = glm::quat_cast(anim::rotationFromVector(ft[i].rot));
            const glm::quat r = glm::normalize(glm::quat_cast(anim::rotationFromVector(p[i].rot)) * glm::inverse(q0) * qt);
            p[i].rot = glm::angle(r) * glm::axis(r);
            p[i].trans += ft[i].trans - f0[i].trans;
        }
    };
    std::vector<anim::LocalTransform> p(local.size()), q(local.size());
    if (hammer) { // Mjolnir: its three aim poses by yaw; a swing / ground pound replaces them while it plays
        static constexpr std::string_view kAim[3] = {"ground_aim_right", "ground_aim_front", "ground_aim_left"};
        const int j = c.aim.x > kHammerCols[1] ? 1 : 0;
        const float u = std::clamp((c.aim.x - kHammerCols[j]) / (kHammerCols[j + 1] - kHammerCols[j]), 0.0f, 1.0f);
        const auto aim = [&](int k) {
            const anim::Clip* a = clip(kAim[k]);
            return sample(a ? a : centre, aimTime_, true);
        };
        anim::blendPoses(aim(j), aim(j + 1), u, p);
        if (const anim::Clip* k = c.hammer.clip.empty() ? nullptr : clip(c.hammer.clip))
            anim::blendPoses(p, sample(k, std::max(0.0f, c.hammer.t), false), 1.0f, p);
    } else {
        int i0 = 0;
        while (i0 + 2 < cols && c.aim.x > colYaw(rifle, i0 + 1)) ++i0;
        const float u = std::clamp((c.aim.x - colYaw(rifle, i0)) / (colYaw(rifle, i0 + 1) - colYaw(rifle, i0)), 0.0f, 1.0f);
        const int row = c.aim.y >= 0 ? 0 : 2;
        const float v = std::clamp(c.aim.y >= 0 ? c.aim.y / gridUp(rifle) : -c.aim.y / gridDown(rifle), 0.0f, 1.0f);
        const bool firing = rifle && c.sinceShot[0] < in.fire;
        const auto cell = [&](int r, int col) {
            if (const anim::Clip* k = firing ? clip(cellName(in, true, r, col)) : nullptr) return sample(k, c.sinceShot[0], false);
            const anim::Clip* k = clip(cellName(in, false, r, col));
            return sample(k ? k : centre, aimTime_, true);
        };
        anim::blendPoses(cell(1, i0), cell(1, i0 + 1), u, p);
        anim::blendPoses(cell(row, i0), cell(row, i0 + 1), u, q);
        anim::blendPoses(p, q, v, p);
        if (!rifle) {
            for (int hand = 0; hand < 2; ++hand) // recoil
                if (const anim::Clip* k = clip(hand == 0 ? "combat_pistol_RfireE" : "combat_pistol_LfireE"); k && c.sinceShot[hand] < k->durationSeconds())
                    additive(p, k, c.sinceShot[hand]);
        } else if (const anim::Clip* k = wpn == Weapon::Shotgun ? clip("combat_shotgun_pump") : nullptr;
                   k && c.sinceShot[0] >= in.fire && c.sinceShot[0] < in.fire + kPump) {
            additive(p, k, c.sinceShot[0] - in.fire);
        }
        if (const anim::Clip* k = clip(sub ? "combat_speargun_reload_submerged" : in.reloadClip); k && c.reload > 0)
            anim::blendPoses(p, sample(k, in.reload - c.reload, false), 1.0f, p);
    }
    float layer = 1;
    if (c.drawT < 1) {
        const float dur = c.drawn ? (sub ? kSubEquip : in.equip) : (sub ? kSubUnequip : in.unequip);
        if (const anim::Clip* k = clip(std::string(c.drawn ? in.equipClip : in.unequipClip) + (sub ? "_submerged" : ""))) {
            const auto e = sample(k, (c.drawn ? c.drawT : 1 - c.drawT) * dur, false);
            anim::blendPoses(e, p, std::clamp((c.drawT - 0.7f) / 0.3f, 0.0f, 1.0f), p);
            layer = std::min(1.0f, c.drawT * dur / 0.1f);
        } else {
            layer = c.drawT * c.drawT * (3 - 2 * c.drawT); // smoothstep fade without the clip
        }
    }
    // upper-body mask: bones descending from the spine (parents precede children)
    std::vector<char> upper(sk.bones.size(), 0);
    upper[size_t(kSpineBone)] = 1;
    for (size_t i = size_t(kSpineBone) + 1; i < sk.bones.size(); ++i)
        upper[i] = sk.bones[i].parent >= 0 && upper[size_t(sk.bones[i].parent)];
    for (size_t i = 0; i < local.size(); ++i)
        if (upper[i]) local[i] = anim::blend(local[i], p[i], layer);
}

} // namespace ou::game
