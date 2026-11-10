// Combat: Lara's health (tru.exe 0x6F5030 / 0x6F5160 / 0x702FC0 family), death and respawn, dual pistols
// (stats from wp_handgun / pr_handgunbullet, aim grid + fire clips from wp_handgun.drm), lock-on targeting
// (Lara's targeting tiers) and hitscan. Numbers and provenance: Combat.hpp, docs/GAMEPLAY.md "Combat".
#include "game/Combat.hpp"
#include "anim/Anim.hpp"
#include "game/Game.hpp"
#include "physics/Physics.hpp"

#include "cdc/Cdc.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

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

// Per weapon: Lara's clips in its DRM and their lengths [O] (keys at 30 Hz). The shot is the fire clip's animation event
// 0x75 (0x716F30 -> message 0x19B -> fire 0x6C9E50), at frame 0 of every such clip [O]; the next shot waits for the clip
// to end (WeaponDef::refire, decoded from the clip's length).
// - pistols: one shot per hand's fire clip (combat_pistol_RfireA/LfireA, 10 keys, the 0x75 hand byte 0 / 1), hands
//   alternate; equip 6 keys, unequip 19, reload 21 (wp_handgun).
// - uzi: one shot per the uzi's own combat_fire_overlay (3 keys), hands alternate; wp_uzi's own combat_pistols_equip /
//   unequip (15 keys each) and reload (21); the pistol aim grid and fire clips of wp_uzi.
// - shotgun: combat_shotgun_fire_* (13 keys; the 0x75 is on _UL) then combat_shotgun_pump (25) before the next shot;
//   equip / unequip 24 keys, reload 117 (the whole clip per reload, however many shells are missing [I]).
// - speargun: the shotgun aim/fire set of wp_speargun; clip 1, so combat_speargun_reload (31 keys) after every
//   shot; under water combat_shotgun_equip_submerged (17) / unequip_submerged (19), combat_speargun_reload_submerged.
// - Mjolnir (wp_thorhammer): ground_equip / ground_unequip (26 keys), aim poses ground_aim_right / _front / _left, the
//   throw ground_fire_idle (29 keys, 0x75 at frame 10), the ground pound ground_melee_idle (61 keys, 0x4B at frame 12);
//   under water combat_water_submerged_equip / _unequip and swim_submerge_thorhammer_attack_front (31 keys, 0x75 at
//   frame 12). No clip, so no reload.
// - assault rifle / tranquilizer (wp_assault_rifle / wp_tranq): Lara's 3x5 combat_rifle_aim_* / combat_rifle_fire_*
//   (3 keys; 0x75 on _UL) grids of each DRM; combat_rifle_equip 19, unequip 24; reload combat_rifle_reload 41 (30 rounds)
//   / combat_tranqgun_reload 19 (one dart per clip, so after every shot).
// The gun moves between holster and hand on the equip clip's 0x76 / 0x77 / 0x78 and the unequip clip's 0x74 events.
struct WeaponInfo {
    std::string_view object, name;
    bool rifle; // two-handed: one gun, the 3x5 aim grid
    std::string_view fireClip; // the clip carrying the shot event 0x75 (WeaponDef::refire = its length)
    float equip, unequip, reload; // seconds
    std::string_view equipClip, unequipClip, reloadClip;
    std::string_view grid = "combat_shotgun_"; // two-handed: <grid>aim_* / <grid>fire_*
    float fire = 13 / 30.0f;                   // two-handed: length of the fire_* cells
};
const WeaponInfo kWeapons[kWeaponCount] = {
    {"wp_handgun", "pistols", false, "combat_pistol_RfireA", 6 / 30.0f, 19 / 30.0f, 21 / 30.0f, "combat_pistols_equip",
     "combat_pistols_unequip", "combat_pistols_reload"},
    {"wp_uzi", "uzi", false, "combat_fire_overlay", 15 / 30.0f, 15 / 30.0f, 21 / 30.0f, "combat_pistols_equip",
     "combat_pistols_unequip", "combat_pistols_reload"},
    {"wp_shotgun", "shotgun", true, "combat_shotgun_fire_UL", 24 / 30.0f, 24 / 30.0f, 117 / 30.0f, "combat_shotgun_equip",
     "combat_shotgun_unequip", "combat_shotgun_reload"},
    {"wp_speargun", "speargun", true, "combat_shotgun_fire_UL", 24 / 30.0f, 24 / 30.0f, 31 / 30.0f, "combat_shotgun_equip",
     "combat_shotgun_unequip", "combat_speargun_reload"},
    {"wp_thorhammer", "thorhammer", false, "ground_fire_idle", 26 / 30.0f, 26 / 30.0f, 0, "ground_equip", "ground_unequip", ""},
    {"wp_assault_rifle", "rifle", true, "combat_rifle_fire_UL", 19 / 30.0f, 24 / 30.0f, 41 / 30.0f, "combat_rifle_equip",
     "combat_rifle_unequip", "combat_rifle_reload", "combat_rifle_", 3 / 30.0f},
    {"wp_tranq", "tranquilizer", true, "combat_rifle_fire_UL", 19 / 30.0f, 24 / 30.0f, 19 / 30.0f, "combat_rifle_equip",
     "combat_rifle_unequip", "combat_tranqgun_reload", "combat_rifle_", 3 / 30.0f},
};
const WeaponInfo& info(Weapon w) { return kWeapons[size_t(w)]; }
// Defaults without the data (synthetic tests): the decoded event frames (CombatReal.WeaponData checks them).
constexpr float kHammerShot = 10 / 30.0f, kHammerSwimShot = 12 / 30.0f, kHammerPound = 12 / 30.0f; // events 0x75 / 0x4B
// Mjolnir's aim poses ground_aim_right / _front / _left by aim yaw; no pitch rows. Yaws [I]: the pointing (right) hand's
// shoulder -> hand direction in each pose relative to _front's (which points 9 deg right of the heading): -52, 0, +32.
const float kHammerCols[] = {-52 * kPi / 180, 0, 32 * kPi / 180};
constexpr float kPump = 25.0f / 30.0f;                                   // combat_shotgun_pump
constexpr float kSubEquip = 17.0f / 30.0f, kSubUnequip = 19.0f / 30.0f; // speargun under water
// DeathState (0x75AA50 entry, 0x75AF90 process): at once the screen wipe 10 fades to black over 90 frames
// (GAMELOOP_SetScreenWipe(10, 100, 90); 210 for death type 1, or the request's own count [U: which deaths those are]);
// when it is done, 0x6F55A0 reloads the checkpoint (flag 0x80000), whose level load clears the wipes (0x52E850).
constexpr float kDeathFade = 90 / 30.0f;
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
// On the motorcycle (wp_handgun / wp_uzi combat_pistol_motorcycle_*): one gun, in the right hand (the left stays on the
// handlebar), a 5 x 3 grid. The clips are named for the left side but every pose points the right arm (bones 78 -> 80,
// model space) to her right / behind [O]: columns back -189 (wrapped), back_diag -134, side -94, front_diag -47,
// straight -2; rows _u +33, _d -30 (row means; _front_diag_d is a copy of _front_diag_u in the data). How the original
// covers her left (switch_hands, mirroring?) [U]: aims there clamp to straight.
constexpr std::string_view kBikeCells[3][5] = {
    {"left_back_u", "left_back_diag_u", "left_side_u", "left_front_diag_u", "straight_u"},
    {"left_back", "left_back_diag", "left_side", "left_front_diag", "straight"},
    {"left_back_d", "left_back_diag_d", "left_side_d", "left_front_diag_d", "straight_d"}};
const float kBikeCols[] = {-189 * kDeg, -134 * kDeg, -94 * kDeg, -47 * kDeg, -2 * kDeg};
constexpr float kBikeUp = 33 * kDeg, kBikeDown = 30 * kDeg;
std::string bikeCell(int row, int col) { return "combat_pistol_motorcycle_aim_" + std::string(kBikeCells[row][col]); }
// Only the pistols and the uzi have bike clips [O data]; the other weapons stay holstered on it [I].
bool bikeWeapon(Weapon w) { return w == Weapon::Pistols || w == Weapon::Uzi; }
// On the rope (State::GrappleSwing / GrappleRappel; wp_handgun / wp_uzi combat_pistol_grapple_rappel(_free)_*, _free
// hanging free, without it feet on a wall): one gun in the right hand, the other holds the rope; a 5 x 3 grid of
// one-handed aims named by direction, F / FR / R / RB / B, rows U / (none) / D [O names]. Column yaws by those names
// (0, -45, -90, -135, -180) and rows +-45 [I]; aims to her left clamp as on the bike (_switchhands unused [U]).
bool onRope(State s) { return s == State::GrappleSwing || s == State::GrappleRappel; }
constexpr std::string_view kRopeCols[5] = {"B", "RB", "R", "FR", "F"};
const float kRopeColYaw[] = {-180 * kDeg, -135 * kDeg, -90 * kDeg, -45 * kDeg, 0};
constexpr float kRopeUp = 45 * kDeg, kRopeDown = 45 * kDeg;
std::string ropeClip(State s, std::string_view name) {
    return std::string(s == State::GrappleRappel ? "combat_pistol_grapple_rappel_" : "combat_pistol_grapple_rappel_free_") + std::string(name);
}
std::string ropeCell(State s, int row, int col) { return ropeClip(s, std::string(row == 0 ? "1haimU" : row == 2 ? "1haimD" : "1haim") + std::string(kRopeCols[col])); }

// Clips and lengths of the equip / unequip / reload layer: under water (the speargun), on the bike (the motorcycle
// clips' own lengths [O]), else WeaponInfo.
struct Handling {
    std::string equipClip, unequipClip, reloadClip;
    float equip, unequip, reload;
};
Handling handling(const AnimLibrary& lib, const PlayerState& s, Weapon w) {
    const WeaponInfo& in = info(w);
    if (s.water >= 0 && w == Weapon::Speargun)
        return {std::string(in.equipClip) + "_submerged", std::string(in.unequipClip) + "_submerged", "combat_speargun_reload_submerged",
                kSubEquip, kSubUnequip, in.reload};
    if (s.water >= 0) {
        // Every other wp_* DRM has Lara's combat_water_submerged_* and combat_water_surface_* equip / unequip clips
        // (the pistols and uzi a reload of each too) [O data]; their own lengths. Treading / surface strokes use the
        // surface set [I: by the names].
        const bool surface = s.state == State::SwimTread || s.state == State::SwimSurface;
        const std::string set = surface ? "combat_water_surface_" : "combat_water_submerged_";
        const auto pick = [&](const char* part, std::string_view fallbackClip, float fallback) -> std::pair<std::string, float> {
            if (const anim::Clip* k = lib.weaponClip(w, set + part)) return {set + part, float(k->durationSeconds())};
            return {std::string(fallbackClip), fallback};
        };
        const auto e = pick("equip", in.equipClip, in.equip), u = pick("unequip", in.unequipClip, in.unequip),
                   r = pick("reload", in.reloadClip, in.reload);
        return {e.first, u.first, r.first, e.second, u.second, r.second};
    }
    if (s.state == State::Vehicle && bikeWeapon(w)) {
        const auto len = [&](std::string_view n, float fallback) {
            const anim::Clip* k = lib.weaponClip(w, n);
            return k ? float(k->durationSeconds()) : fallback;
        };
        return {"combat_pistol_motorcycle_arm", "combat_pistol_motorcycle_disarm", "combat_pistol_motorcycle_reload",
                len("combat_pistol_motorcycle_arm", in.equip), len("combat_pistol_motorcycle_disarm", in.unequip),
                len("combat_pistol_motorcycle_reload", in.reload)};
    }
    if (onRope(s.state) && bikeWeapon(w)) {
        const auto len = [&](const std::string& n, float fallback) {
            const anim::Clip* k = lib.weaponClip(w, n);
            return k ? float(k->durationSeconds()) : fallback;
        };
        const std::string e = ropeClip(s.state, "equip"), u = ropeClip(s.state, "unequip"), r = ropeClip(s.state, "reload");
        return {e, u, r, len(e, in.equip), len(u, in.unequip), len(r, in.reload)};
    }
    return {std::string(in.equipClip), std::string(in.unequipClip), std::string(in.reloadClip), in.equip, in.unequip, in.reload};
}

// Health packs (g_healthpack data; use = gadget fnC 0x6CBB20): heal +0xF0 = 640 through 0x6F4EF0 (clamped to max),
// cooldown +0xEA = 60 frames [I 30 Hz]; no full-health check in the exe.
constexpr float kHealthPackHeal = 640.0f, kHealthPackCooldown = 2.0f;
// Adrenaline numbers: AdrenalineTuning (Lara data). The head-shot window's length and the pip are [I].
constexpr float kSlowWindow = 3.0f;  // real seconds to steer the pip in and fire
constexpr float kPipStart = 2.5f, kPipSpeed = 2.0f; // pip start distance / stick speed, reticle radii (per second)

bool swimHit(State s) { return s == State::SwimUnder || s == State::Surfacing || s == State::SwimDive || s == State::SwimDodge || s == State::Hit; }
// Guns out on the ground (and in a hit reaction) and in the water. tru.exe's weapon stats carry no under-water flag
// (S+0x88: 0x80 infinite, 0x40, 0x20 dual, 0x08 two-handed) and every wp_* DRM has Lara's submerged and surface equip /
// unequip clips, a `fire_submerged` gun clip and (bullets) an under-water damage record (ammo +0x84) [O data]; the
// chart has under-water aim sets for one-handed guns, two-handed guns and Mjolnir (requests 924 / 926 / 925). So every
// weapon can be drawn and fired while swimming [I: no refusal found in the exe; the walkthrough calls the spear gun
// the best under-water choice, not the only one]. Not during the surface dive, the entry splash or the climb out.
bool swimArmed(State s) {
    return s == State::SwimUnder || s == State::Surfacing || s == State::SwimTread || s == State::SwimSurface || s == State::SwimDodge ||
           s == State::Hit;
}
bool canShoot(const PlayerState& s, Weapon) {
    if (s.water >= 0) return swimArmed(s.state);
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
    uint32_t base = 0;                // the ObjectBase section
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
        o.section = it->second.section, o.offset = it->second.offset, o.base = i;
        return o;
    }
    return std::nullopt;
}

template <class T>
T rd(std::span<const uint8_t> d, size_t o) {
    T v{};
    if (o + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + o, sizeof(T));
    return v;
}
// The ObjectBase animation list (+0x54 count, +0x58 {i16 anim, i16, u32 event-list DTP id, char* name}) -> each clip's
// event list (DTP: u32 header, records {u16 info, u16 frame, ...} of 8 + (info >> 12) * 4 bytes until 0xFFFF; EFFECTS.md):
// the command records (info & 3 == 2), {record +8, frame}, by clip name.
std::map<std::string, std::vector<std::pair<int, int>>, std::less<>> clipCommands(const ObjectData& o) {
    std::map<std::string, std::vector<std::pair<int, int>>, std::less<>> out;
    const auto& s = o.drm.sections[o.base];
    const auto ptrs = cdc::sectionPointers(o.drm, o.base);
    const auto list = ptrs.find(0x58);
    const uint32_t n = rd<uint32_t>(s.data, 0x54);
    if (list == ptrs.end() || n > 4096) return out;
    const auto& ls = o.drm.sections[list->second.section];
    const auto lptrs = list->second.section == o.base ? ptrs : cdc::sectionPointers(o.drm, list->second.section);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t e = list->second.offset + 12 * i;
        const auto name = lptrs.find(e + 8);
        if (name == lptrs.end()) continue;
        std::string nm;
        const auto& nd = o.drm.sections[name->second.section].data;
        for (size_t k = name->second.offset; k < nd.size() && nd[k]; ++k) nm += char(nd[k]);
        const uint32_t id = rd<uint32_t>(ls.data, e + 4);
        const auto ev = std::ranges::find_if(o.drm.sections, [&](const cdc::Section& x) { return x.type == cdc::SectionType::DtpData && x.id == id; });
        if (ev == o.drm.sections.end()) continue;
        std::vector<std::pair<int, int>> cmds;
        for (size_t at = 4; at + 8 <= ev->data.size();) {
            const auto info = rd<uint16_t>(ev->data, at);
            if (info == 0xFFFF) break;
            if ((info & 3) == 2) cmds.emplace_back(ev->data[at + 8], rd<uint16_t>(ev->data, at + 2));
            at += 8 + size_t(info >> 12) * 4;
        }
        if (!cmds.empty()) out.emplace(std::move(nm), std::move(cmds));
    }
    return out;
}
// Length in seconds of the clip `name` of the DRM's animation list; nullopt when missing.
std::optional<float> clipLength(const cdc::Drm& drm, std::string_view name) {
    for (const auto& e : anim::animList(drm))
        if (e.name == name)
            for (const auto& s : drm.sections)
                if (s.type == cdc::SectionType::Animation && s.id == e.animId) return float(anim::decodeClip(s.data).durationSeconds());
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
        d.muzzleMarker = -1;
        d.shortRange = 2000, d.maxRange = 4500, d.maxSpreadDeg = 5, d.spreadPerShot = 1, d.spreadRecovery = 0.1f;
        d.damage[0] = 25, d.damage[1] = 25, d.damage[2] = 10;
        d.wetDamage[0] = 25, d.wetDamage[1] = 25, d.wetDamage[2] = 10;
        d.refire = 3 / 30.0f; // wp_uzi combat_fire_overlay: 3 keys
        break;
    case Weapon::Shotgun:
        d.object = "wp_shotgun", d.ammo = "pr_shotgunbullet", d.ammoSlot = 1, d.maxReserve = {90, 72, 42}, d.pellets = 7, d.clip = 6;
        d.hand = {5, 7}, d.stow = {15, 6}, d.muzzleMarker = 0;
        d.infinite = d.dual = false, d.shortRange = 2000, d.maxRange = 3000, d.spreadDeg = d.maxSpreadDeg = 15;
        d.damage[0] = 50, d.damage[1] = 50, d.damage[2] = 40;
        d.wetDamage[0] = 50, d.wetDamage[1] = 50, d.wetDamage[2] = 40;
        d.hitClass = 2;
        d.refire = (13 + 25) / 30.0f;
        break;
    case Weapon::Speargun:
        d.object = "wp_speargun", d.ammo = "pr_spear", d.ammoSlot = 4, d.maxReserve = {40, 25, 15}, d.clip = 1, d.infinite = d.dual = false;
        d.hand = {5, 7}, d.stow = {15, 6};
        d.shortRange = 2500, d.maxRange = 5000, d.mode = 2, d.damage[0] = d.damage[1] = d.damage[2] = 250;
        d.wetDamage[0] = d.wetDamage[1] = d.wetDamage[2] = 550;
        d.hitClass = 4, d.hitType = 0, d.projectileSpeed = 1000 * 30.0f, d.projectileGravity = {2 * 900.0f, 6 * 900.0f};
        d.refire = 13 / 30.0f;
        break;
    case Weapon::ThorHammer: // pr_thorbolt: 1000 at every range on land, 1000 / 400 / 100 under water
        d.object = "wp_thorhammer", d.ammo = "pr_thorbolt", d.clip = -1, d.dual = false, d.shortRange = 1000, d.maxRange = 8000;
        d.hand = {5, 7}, d.stow = {31, 6}, d.muzzleMarker = 0;
        d.damage[0] = d.damage[1] = d.damage[2] = 1000;
        d.wetDamage[0] = 1000, d.wetDamage[1] = 400, d.wetDamage[2] = 100;
        d.hitClass = 13, d.hitType = 3;
        d.refire = 29 / 30.0f;
        break;
    case Weapon::AssaultRifle: // pr_akriflebullet (as wp_akrifle, the poachers' gun)
        d.object = "wp_assault_rifle", d.ammo = "pr_akriflebullet", d.ammoSlot = 2, d.maxReserve = {300, 210, 120}, d.clip = 30;
        d.hand = {5, 7}, d.stow = {15, 6}, d.muzzleMarker = -1;
        d.infinite = d.dual = false, d.shortRange = 5000, d.maxRange = 8000;
        d.maxSpreadDeg = 5, d.spreadPerShot = 1.2f, d.spreadRecovery = 0.125f;
        d.damage[0] = 27, d.damage[1] = 27, d.damage[2] = 21;
        d.wetDamage[0] = 23, d.wetDamage[1] = 23, d.wetDamage[2] = 18;
        d.refire = 3 / 30.0f;
        break;
    case Weapon::Tranquilizer: // pr_tranquilizerdart: a projectile (mode 2) whose +0x1C is the 500 dose
        d.object = "wp_tranq", d.ammo = "pr_tranquilizerdart", d.ammoSlot = 3, d.maxReserve = {25, 15, 10}, d.clip = 1;
        d.hand = {5, 7}, d.stow = {15, 6}, d.muzzleMarker = -15;
        d.infinite = d.dual = false, d.shortRange = 4000, d.maxRange = 6000, d.spreadDeg = d.maxSpreadDeg = 5, d.mode = 2;
        d.damage[0] = d.damage[1] = d.damage[2] = 500;
        d.wetDamage[0] = d.wetDamage[1] = d.wetDamage[2] = 500;
        d.hitClass = 4, d.hitType = 0, d.projectileSpeed = 1500 * 30.0f, d.projectileGravity = {1 * 900.0f, 3 * 900.0f};
        d.refire = 3 / 30.0f;
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
            if (bikeWeapon(Weapon(i))) {
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 5; ++c) o.push_back(bikeCell(r, c));
                o.insert(o.end(), {"combat_pistol_motorcycle_arm", "combat_pistol_motorcycle_disarm", "combat_pistol_motorcycle_reload"});
                for (const State st : {State::GrappleSwing, State::GrappleRappel}) {
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 5; ++c) o.push_back(ropeCell(st, r, c));
                    for (const char* n : {"equip", "unequip", "reload"}) o.push_back(ropeClip(st, n));
                }
            }
            if (Weapon(i) == Weapon::Speargun)
                o.insert(o.end(), {"combat_shotgun_equip_submerged", "combat_shotgun_unequip_submerged", "combat_speargun_reload_submerged"});
            else { // Lara's in-water handling clips (handling())
                o.insert(o.end(), {"combat_water_submerged_equip", "combat_water_submerged_unequip", "combat_water_surface_equip",
                                   "combat_water_surface_unequip"});
                if (!wi.rifle && Weapon(i) != Weapon::ThorHammer) o.insert(o.end(), {"combat_water_submerged_reload", "combat_water_surface_reload"});
            }
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

std::optional<int> WeaponDef::commandFrame(std::string_view clip, std::initializer_list<int> wanted) const {
    const auto it = commands.find(clip);
    if (it != commands.end())
        for (const auto& [cmd, frame] : it->second)
            if (std::ranges::find(wanted, cmd) != wanted.end()) return frame;
    return std::nullopt;
}

std::optional<AdrenalineTuning> loadAdrenalineTuning(const cdc::BigfileSet& bf) {
    const auto l = objectData(bf, "lara");
    if (!l) return std::nullopt;
    const auto max = l->at<float>(0x2D0), drain = l->at<float>(0x2D4), mode = l->at<float>(0x2D8), modeScale = l->at<float>(0x31C),
               shot = l->at<float>(0x2C8), ease = l->at<float>(0x2CC);
    if (!max || !drain || !mode || !modeScale || !shot || !ease || !(*max > 0)) return std::nullopt;
    return AdrenalineTuning{*max, *drain, *mode, *modeScale, *shot, std::round(*ease) / 30.0f};
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
    const auto muzzle = w->at<int16_t>(S + 0x14);
    if (!slot || !pellets || !clip || !ammoId || !maxRange || !spread || !shortRange || !maxSpread || !perShot || !recovery || !flags || !markers ||
        !muzzle)
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
    // the second record's hitscan damage; a projectile's at its +0x1C as the first's
    const bool wetScan = ammo->at<int32_t>(R2) == 1;
    const auto w0 = ammo->at<float>(*mode == 2 ? R2 + 0x1C : R2 + 4), w1 = ammo->at<float>(*mode == 2 ? R2 + 0x1C : R2 + 0x10),
               w2 = ammo->at<float>(*mode == 2 ? R2 + 0x1C : R2 + 0x14);
    if ((wetScan || *mode == 2) && (!w0 || !w1 || !w2)) return std::nullopt;
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
    else def.hitClass = 4, def.hitType = 0; // 0x6C9670 posts every projectile hit as class 4, type 0
    if (*mode == 2) {
        const auto speed = w->at<float>(S + 0x10), g0 = ammo->at<float>(R + 0x4C), g1 = ammo->at<float>(R2 + 0x4C);
        if (!speed || !g0 || !g1) return std::nullopt;
        def.projectileSpeed = *speed * 30, def.projectileGravity = {-*g0 * 900, -*g1 * 900};
    }
    def.commands = clipCommands(*w);
    const auto wi = std::ranges::find(kWeapons, object, &WeaponInfo::object);
    if (wi != std::end(kWeapons)) {
        const auto fire = clipLength(w->drm, wi->fireClip);
        const auto pump = wi->object == "wp_shotgun" ? clipLength(w->drm, "combat_shotgun_pump") : std::optional<float>(0.0f);
        if (!fire || !pump) return std::nullopt;
        def.refire = *fire + *pump;
    }
    def.hand = {(*markers)[0], (*markers)[2]}, def.stow = {(*markers)[1], (*markers)[3]};
    def.muzzleMarker = *muzzle;
    if (const auto f = physics::loadObjectPhysics(w->drm).frame(def.muzzleMarker)) def.muzzle = *f;
    def.damage[0] = *d0, def.damage[1] = *d1, def.damage[2] = *d2;
    if (wetScan || *mode == 2) def.wetDamage[0] = *w0, def.wetDamage[1] = *w1, def.wetDamage[2] = *w2;
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

std::optional<GrappleTuning> loadGrappleTuning(const cdc::BigfileSet& bf) {
    const auto g = objectData(bf, "g_grapple");
    if (!g) return std::nullopt;
    const auto range = g->at<float>(0x100), maxRope = g->at<float>(0x104), minRope = g->at<float>(0x108);
    const auto bends = g->at<int32_t>(0x10C);
    const auto markers = g->at<std::array<uint8_t, 4>>(0xB4);
    if (!range || !maxRope || !minRope || !bends || !markers || !(*range > 0) || !(*minRope > 0) || *maxRope < *minRope ||
        *bends <= 0)
        return std::nullopt;
    return GrappleTuning{*range, *maxRope, *minRope, std::min(int(*bends), kRopeBends), (*markers)[1]};
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
int hitReaction(int damageClass, float damage) {
    if (damageClass == 2) return 2;
    if (damageClass == 1) return 1;
    if (damageClass == 6) return damage >= kHeavyHit ? 2 : damage >= kLightHit ? 1 : 0;
    return 0;
}

void World::damageLara(float amount, vec3 from, int type) {
    const int react = hitReaction(type, amount);
    if (!health_.damage(amount, type) || health_.dead() || !react) return;
    const PlayerState& s = player_.state();
    const bool swim = s.water >= 0;
    if (swim ? !(swimHit(s.state) || s.state == State::SwimTread || s.state == State::SwimSurface)
             : !(s.state == State::Idle || s.state == State::Walk || s.state == State::Run || s.state == State::Stop ||
                 s.state == State::Turn || s.state == State::Land || s.state == State::Hit))
        return;
    const float rel = wrapAngle(std::atan2(from.y - s.pos.y, from.x - s.pos.x) - s.yaw), a = std::abs(rel);
    const int dir = a <= 45 * kDeg ? 0 : a >= 135 * kDeg ? 1 : 2;
    static constexpr std::string_view kLight[] = {"hit_light_front", "hit_light_back", "hit_light_side"};
    static constexpr std::string_view kHeavy[] = {"hit_mediumfront", "hit_mediumback", "hit_mediumright"};
    // 0x6EB320: the anim graph's mirror flag 0x20 is set when the attacker's angle from her heading is positive (her left)
    player_.hitReact(std::string(react == 2 ? kHeavy[dir] : kLight[dir]) + (swim ? "_swim" : ""), rel > 0);
}

void World::kill() { health_.damage(health_.value(), kDamageKill); }

// ---- World: guns --------------------------------------------------------------------------------
std::optional<std::pair<int, glm::mat4>> World::gunMount(Weapon w, int hand) const {
    const CombatState& c = combat_;
    const WeaponDef& d = anims_.weapons[size_t(w)];
    // [O] the gun goes into her hand on the equip clip's event 0x76 / 0x77 / 0x78 (0x6F64C0) and back on the unequip
    // clip's 0x74 (0x6E8FC0); without the data half way through [I].
    const bool inHand = [&] {
        if (w != c.weapon) return false;
        const Handling hd = handling(anims_, player_.state(), w);
        if (c.drawn) {
            const auto f = d.commandFrame(hd.equipClip, {0x76, 0x77, 0x78});
            return c.drawT * hd.equip >= (f ? float(*f) / 30.0f : 0.5f * hd.equip);
        }
        const auto f = d.commandFrame(hd.unequipClip, {0x74});
        return (1 - c.drawT) * hd.unequip < (f ? float(*f) / 30.0f : 0.5f * hd.unequip);
    }();
    // [O] the weapon model sits at one of Lara's HInfo markers (WeaponDef::hand / stow, data +0xB8..+0xBB) with the
    // marker's frame as its own: in hand the grips 7 (right, segment 96) / 5 (left, 71; two-handed guns and Mjolnir).
    // A library without markers (synthetic tests) draws no stowed weapon and holds the gun at the hand bone.
    const auto marker = [&](int id) -> std::optional<std::pair<int, glm::mat4>> {
        const auto it = anims_.markers.find(id);
        if (it == anims_.markers.end()) return std::nullopt;
        return it->second;
    };
    const auto grip = [&](int id, int bone) -> std::optional<std::pair<int, glm::mat4>> {
        if (anims_.markers.empty()) return std::pair{bone, glm::mat4(1)};
        return marker(id);
    };
    if (w == Weapon::ThorHammer) {
        // In hand marker 5 (segment 71, the left hand's grip child of 55), stowed 31 (chest segment 15, on her back):
        // the left hand draws it from the back (ground_equip) and holds it up while the right one points.
        if (hand != 0 || (w != c.weapon && !hasHammer())) return std::nullopt;
        return marker(inHand ? d.hand[0] : d.stow[0]);
    }
    if (!info(w).rifle) {
        // on the bike / rope only the right gun is drawn; the left one stays in its holster
        if (!inHand || (hand == 1 && (player_.state().state == State::Vehicle || onRope(player_.state().state))))
            return w == Weapon::Pistols || w == c.secondary ? marker(d.stow[size_t(hand)]) : std::nullopt;
        return grip(d.hand[size_t(hand)], kHandBone[hand]);
    }
    if (hand != 0 || (w != c.weapon && w != c.secondary)) return std::nullopt;
    return inHand ? grip(d.hand[0], kHandBone[1]) : marker(d.stow[0]); // stowed 15: across her back
}

glm::mat4 World::gunMatrix(int hand) const {
    const auto mount = gunMount(combat_.weapon, hand);
    const PlayerState& s = player_.state();
    glm::mat4 b(1), offset(1); // no mount / bone (synthetic skeletons): the gun at shoulder height
    if (mount) offset = mount->second;
    if (mount && size_t(mount->first) < bones_.size()) b = bones_[size_t(mount->first)];
    else b[3].z = kShoulderHeight;
    return player_.modelMatrix(s.pos, s.yaw, s.pitch, s.roll) * b * offset;
}

vec3 World::muzzle(int hand) const { return vec3(gunMatrix(hand) * anims_.weapons[size_t(combat_.weapon)].muzzle[3]); }

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
    // In hand: the draw, aim, lock, cooldowns, recoil clocks and the spread generator, so a load continues mid-fight
    // exactly (the target by enemy uid). Older saves load holstered.
    int32_t target = 0;
    for (const auto& e : enemies_.all())
        if (static_cast<const Damageable*>(e.get()) == c.target) target = e->uid();
    nlohmann::json ragdoll = nlohmann::json::array();
    for (const glm::mat4& m : c.ragdoll) ragdoll.push_back(std::vector<float>(&m[0][0], &m[0][0] + 16));
    const nlohmann::json state = {
        {"previous", weaponName(c.previous)}, {"want", weaponName(c.want)}, {"hammer", {c.hammer.clip, c.hammer.t, c.hammer.prevT}},
        {"drawn", c.drawn}, {"redraw", c.redraw}, {"drawT", c.drawT}, {"target", target}, {"locked", c.locked},
        {"aim", {c.aim.x, c.aim.y}}, {"aimPoint", {c.aimPoint.x, c.aimPoint.y, c.aimPoint.z}}, {"reload", c.reload},
        {"cooldown", c.cooldown}, {"hand", c.hand}, {"sinceShot", {c.sinceShot[0], c.sinceShot[1]}}, {"spread", c.spread},
        {"shotsFired", c.shotsFired}, {"rng", c.rng}, {"deathTime", c.deathTime}, {"ragdollPhase", c.ragdollPhase}, {"ragdoll", ragdoll},
        {"held", {c.drawHeld, c.switchHeld, c.weaponHeld, c.healHeld, c.interactHeld, c.fireHeld}}, {"healCooldown", c.healCooldown},
        {"healthPacksUsed", c.healthPacksUsed}, {"slowMo", c.slowMo}, {"pip", {c.pip.x, c.pip.y}}, {"timeScale", c.timeScale},
        {"adrenalineMode", c.adrenalineMode}, {"timeRamp", {c.timeTarget, c.timeRate}}, {"projectiles", [&] {
             nlohmann::json a = nlohmann::json::array();
             for (const Projectile& p : c.projectiles)
                 a.push_back({{p.from.x, p.from.y, p.from.z}, {p.pos.x, p.pos.y, p.pos.z}, {p.vel.x, p.vel.y, p.vel.z}, p.gravity,
                              p.travelled, weaponName(p.weapon), p.hand, p.wet});
             return a;
         }()}};
    return {{"weapon", weaponName(c.weapon)}, {"secondary", weaponName(c.secondary)}, {"guns", guns}, {"adrenaline", c.adrenaline},
            {"state", state}, {"healthClocks", [&] {
                const auto h = health_.clocks();
                return nlohmann::json{h.now, h.regenStart, h.regenBase, h.invuln, h.lastType};
            }()}};
}

void World::restoreCombat(const nlohmann::json& j) {
    if (!j.is_object()) return;
    const auto weapon = weaponByName(j.value("weapon", std::string{})), secondary = weaponByName(j.value("secondary", std::string{}));
    if (!weapon || !secondary) return;
    if (const auto h = j.find("healthClocks"); h != j.end() && h->is_array() && h->size() == 5) // older saves: idle clocks
        health_.setClocks({h->at(0).get<double>(), h->at(1).get<double>(), h->at(2).get<float>(), h->at(3).get<float>(), h->at(4).get<int>()});
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
    if (const auto s = j.find("state"); s != j.end() && s->is_object()) {
        try {
            CombatState d = c;
            const auto w = [&](const char* key) {
                const auto x = weaponByName(s->at(key).get<std::string>());
                if (!x) throw std::runtime_error("weapon");
                return *x;
            };
            d.previous = w("previous"), d.want = w("want");
            d.hammer = {s->at("hammer").at(0).get<std::string>(), s->at("hammer").at(1).get<float>(), s->at("hammer").at(2).get<float>()};
            d.drawn = s->at("drawn"), d.redraw = s->at("redraw"), d.drawT = s->at("drawT"), d.locked = s->at("locked");
            d.target = nullptr;
            for (const auto& e : enemies_.all())
                if (e->uid() == s->at("target").get<int32_t>()) d.target = e.get();
            d.aim = {s->at("aim").at(0).get<float>(), s->at("aim").at(1).get<float>()};
            d.aimPoint = {s->at("aimPoint").at(0).get<float>(), s->at("aimPoint").at(1).get<float>(), s->at("aimPoint").at(2).get<float>()};
            d.reload = s->at("reload"), d.cooldown = s->at("cooldown"), d.hand = s->at("hand");
            d.sinceShot[0] = s->at("sinceShot").at(0), d.sinceShot[1] = s->at("sinceShot").at(1);
            d.spread = s->at("spread"), d.shotsFired = s->at("shotsFired"), d.rng = s->at("rng"), d.deathTime = s->at("deathTime");
            d.ragdollPhase = s->value("ragdollPhase", 0);
            for (const auto& m : s->value("ragdoll", nlohmann::json::array())) {
                const auto f = m.get<std::vector<float>>();
                if (f.size() != 16) throw std::runtime_error("ragdoll frame");
                d.ragdoll.push_back(glm::make_mat4(f.data()));
            }
            const auto& h = s->at("held");
            d.drawHeld = h.at(0), d.switchHeld = h.at(1), d.weaponHeld = h.at(2), d.healHeld = h.at(3), d.interactHeld = h.at(4), d.fireHeld = h.at(5);
            d.healCooldown = s->at("healCooldown"), d.healthPacksUsed = s->at("healthPacksUsed"), d.slowMo = s->at("slowMo");
            d.pip = {s->at("pip").at(0).get<float>(), s->at("pip").at(1).get<float>()}, d.timeScale = s->at("timeScale");
            d.adrenalineMode = s->value("adrenalineMode", false);
            if (const auto r = s->find("timeRamp"); r != s->end()) d.timeTarget = r->at(0), d.timeRate = r->at(1);
            d.projectiles.clear();
            for (const auto& p : s->value("projectiles", nlohmann::json::array())) {
                const auto v = [&](int i) { return vec3(p.at(i).at(0).get<float>(), p.at(i).at(1).get<float>(), p.at(i).at(2).get<float>()); };
                const auto wpn = weaponByName(p.at(5).get<std::string>());
                if (!wpn) throw std::runtime_error("projectile weapon");
                d.projectiles.push_back({v(0), v(1), v(2), p.at(3), p.at(4), *wpn, p.at(6), p.at(7)});
            }
            c = d;
        } catch (const std::exception&) { // nlohmann errors too: holstered, as older saves
        }
    }
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

// Grapple target: Lara's grapple targeting slot, player data +0x464 [O lara.drm]: flags 0xEC000000 (line of sight from
// her eye, 0x04000000 as the combat slots), six target sets tried in order, the first one with a candidate wins
// (criteria 0x712090; d = 3D distance from the eye, horiz = horizontal angle from her facing, cam = angle from the
// camera's view, elevation = the target's angle over the horizontal; range = within GrappleTuning::range of her,
// criterion 15):
//   0: horiz <= 25, d <= 2880, cam <= 90       1: range, horiz <= 15, d <= 5120, cam <= 90
//   2: d <= 2880, cam <= 90                     3: d <= 2880, horiz <= 90, elevation >= -60
//   4: range, cam <= 90, elevation >= 5         5: range, horiz <= 90, elevation >= 5
// Candidates are the *_ei_grapple_* anchors only: no enemy or other object has a grapple clip or reaction in the data
// [O: lara / weapon / kraken DRMs alone name grapple clips], so grappling an enemy is not a thing in TRU. Within a set
// the one nearest the camera's view wins [I]; distances are from her eye (0x6ECE40) [O], "range" is the target's own
// (its 0x404 block) else g_grapple's, measured from the g_grapple instance (case 0xE: player message 0x175 -> grapple
// controller +0x34, the instance's +0x20) [O], stowed at her HInfo marker GrappleTuning::stowMarker (the eye without
// markers: synthetic skeletons).
std::optional<GrappleTarget> World::grappleTarget() const {
    const PlayerState& s = player_.state();
    if (s.grappleUid) return std::nullopt;
    const vec3 eye = s.pos + vec3(0, 0, targeting.eyeHeight);
    vec3 gadget = eye;
    if (const auto m = anims_.markers.find(anims_.grapple.stowMarker); m != anims_.markers.end() && size_t(m->second.first) < bones_.size())
        gadget = vec3(player_.modelMatrix(s.pos, s.yaw, s.pitch, s.roll) * bones_[size_t(m->second.first)] * m->second.second[3]);
    const vec3 camFwd = cam_.target - cam_.position;
    const vec3 laraFwd(std::cos(s.yaw), std::sin(s.yaw), 0);
    const float rad = kPi / 180;
    struct C { GrappleTarget t; float d, horiz, cam, elev; bool range; };
    std::vector<C> cs;
    for (const auto& u : units_)
        for (const auto& g : u.data.grapples) {
            if (script_.grappleOff(g.uid) || uberGrappleOff(g.uid)) continue; // `unit` 13 took its markers / uber action 0x1A
            GrappleTarget t = g;
            t.pos += u.offset;
            const vec3 v = t.pos - eye;
            const float d = glm::length(v);
            if (d < 1) continue;
            // Line of sight from her eye (0x713200 -> 0x713090): what tru.exe's targeting ray sees blocks (kSightMask:
            // terrain except the groups its filter skips, objects with oflags bit 8), not the target's own object.
            if (const auto hit = col_.raycast(eye, v / d, d - 60.0f, collision::kSightMask);
                hit && (hit->dynamic < 0 || col_.dynamics[size_t(hit->dynamic)].id != t.uid))
                continue;
            const float range = t.range > 0 ? t.range : anims_.grapple.range; // 0x698AC0: the target's own, else g_grapple's
            cs.push_back({t, d, angleBetween(laraFwd, vec3(vec2(v), 0)), angleBetween(camFwd, t.pos - cam_.position),
                          std::asin(std::clamp(v.z / d, -1.0f, 1.0f)), glm::length(t.pos - gadget) < range});
        }
    const auto pass = [&](const C& c, int set) {
        switch (set) {
        case 0: return c.horiz <= 25 * rad && c.d <= 2880 && c.cam <= 90 * rad;
        case 1: return c.range && c.horiz <= 15 * rad && c.d <= 5120 && c.cam <= 90 * rad;
        case 2: return c.d <= 2880 && c.cam <= 90 * rad;
        case 3: return c.d <= 2880 && c.horiz <= 90 * rad && c.elev >= -60 * rad;
        case 4: return c.range && c.cam <= 90 * rad && c.elev >= 5 * rad;
        default: return c.range && c.horiz <= 90 * rad && c.elev >= 5 * rad;
        }
    };
    for (int set = 0; set < 6; ++set) {
        const C* best = nullptr;
        for (const C& c : cs)
            if (pass(c, set) && (!best || c.cam < best->cam)) best = &c;
        if (best) return best->t;
    }
    return std::nullopt;
}

namespace {
// 32-bit LCG in [-0.5, 0.5]: spread and the adrenaline pip stay replay-exact
float rnd(uint64_t& rng) {
    rng = rng * 1664525u + 1013904223u;
    return float((rng >> 8) & 0xFFFF) / 65535.0f - 0.5f;
}
} // namespace

void World::gainAdrenaline(float dealt) {
    if (dealt > 0) combat_.adrenaline = std::min(1.0f, combat_.adrenaline + std::round(dealt) / anims_.adrenaline.max);
}

bool World::strike(Weapon wpn, vec3 from, vec3 dir, float len, float dist0, bool wet, Shot& shot) {
    const WeaponDef& w = anims_.weapons[size_t(wpn)];
    float best = len;
    bool hitAny = false;
    int32_t object = 0; // the dynamic collider (script object) it hit
    if (const auto g = col_.raycast(from, dir, best))
        best = g->t, hitAny = true, object = g->dynamic >= 0 ? col_.dynamics[size_t(g->dynamic)].id : 0;
    Damageable* struck = nullptr;
    for (Damageable* t : targets) {
        if (!t || !t->alive() || t->team() == Team::Player) continue;
        if (const auto d = rayCapsule(from, dir, t->position(), t->radius(), t->height(), best)) best = *d, struck = t, hitAny = true;
    }
    shot.to = from + dir * best, shot.hit = hitAny, shot.target = struck;
    const float damage = w.damageAt(dist0 + best, wet);
    if (struck) {
        shot.damage = damage;
        Hit hit{damage, shot.to, dir, Team::Player};
        hit.tranq = wpn == Weapon::Tranquilizer; // pr_tranquilizerdart: the damage is its dose
        hit.damageClass = w.hitClass, hit.damageType = w.hitType;
        const float before = struck->health();
        struck->onHit(hit);
        gainAdrenaline(before - std::max(0.0f, struck->health())); // message 0xD9: the health it lost (0x63AD50)
    }
    if (!struck && object) { // the hit message 0x11C reaches an anim-graph object too (docs/formats/ANIMGRAPH.md)
        uberShot(object, dir);
        script_.graphHit(object, uint32_t(w.hitClass), damage);
        damageObject(object, damage, w.hitClass); // breakable uber objects (urns)
    }
    return hitAny;
}

void World::fire(int hand) {
    CombatState& c = combat_;
    const WeaponDef& w = anims_.weapons[size_t(c.weapon)];
    const glm::mat4 gun = gunMatrix(hand);
    const vec3 from(gun * w.muzzle[3]);
    vec3 aim = c.aimPoint - from;
    if (glm::length(aim) < 1) aim = vec3(std::cos(player_.state().yaw), std::sin(player_.state().yaw), 0);
    aim = glm::normalize(aim);
    const vec3 right = glm::normalize(glm::cross(aim, vec3(0, 0, 1)) + vec3(1e-6f, 0, 0));
    const bool wet = submerged();
    for (int pellet = 0; pellet < std::max(1, w.pellets); ++pellet) {
        // spread: yaw and pitch offsets within +-spread/2 degrees (0x69E300), per pellet
        const float dy = rnd(c.rng) * c.spread * kDeg, dp = rnd(c.rng) * c.spread * kDeg;
        const vec3 dir = glm::normalize(glm::angleAxis(dy, vec3(0, 0, 1)) * (glm::angleAxis(dp, right) * aim));
        Shot shot{from, from, hand, false, nullptr, 0, gun};
        if (w.mode == 2 && w.projectileSpeed > 0) // a projectile (0x6C95F0): flies from the muzzle, World::tickProjectiles
            c.projectiles.push_back({from, from, dir * w.projectileSpeed, w.projectileGravity[wet ? 1 : 0], 0, c.weapon, hand, wet});
        else
            strike(c.weapon, from, dir, w.maxRange, 0, wet, shot);
        c.shots.push_back(shot);
    }
    c.spread = std::min(w.maxSpreadDeg, c.spread + w.spreadPerShot);
    c.sinceShot[hand] = 0;
    ++c.shotsFired;
}

// Projectiles in flight: gravity (pos += v t - g t^2 / 2, as the player integrator), the swept segment tested as a ray;
// a hit (world or target) ends it with an impact shot, as does flying past the weapon's max range [I].
void World::tickProjectiles(float dt) {
    CombatState& c = combat_;
    for (size_t i = 0; i < c.projectiles.size();) {
        Projectile& p = c.projectiles[i];
        const vec3 next = p.pos + p.vel * dt - vec3(0, 0, 0.5f * p.gravity * dt * dt);
        p.vel.z -= p.gravity * dt;
        const vec3 seg = next - p.pos;
        const float len = glm::length(seg);
        Shot shot{p.from, p.pos, p.hand, false, nullptr, 0, glm::mat4(1)};
        shot.impact = true;
        const bool hit = len > 0 && strike(p.weapon, p.pos, seg / len, len, p.travelled, p.wet, shot);
        p.pos = next, p.travelled += len;
        if (hit || p.travelled > anims_.weapons[size_t(p.weapon)].maxRange) {
            if (hit) c.shots.push_back(shot);
            c.projectiles.erase(c.projectiles.begin() + std::ptrdiff_t(i));
        } else {
            ++i;
        }
    }
}

float World::screenFade() const {
    const float death = combat_.deathTime >= 0 ? std::min(1.0f, combat_.deathTime / kDeathFade) : 0.0f;
    return std::max(script_.screenFade(), death);
}

// 0x6E68B0 (event 0x4B) runs one 0x69F1E0 area hit per ring of the current attack record (weapon controller +0xC4,
// Lara's object data: the record with damage class 13, rings at +0x24 / count +0x20; GAMEPLAY.md "Mjolnir").
// 0x69F1E0: every instance but Lara whose origin lies strictly between the inner and outer radius (3D) takes hit
// message 0x11C, damage lerped from `damage` at the inner to `edge` at the outer radius, class 13, type 0; rings with a
// knock-back speed (+0x14 > 0) also throw it off its feet (message 0x91; ours Hit::knock).
void World::groundPound() {
    const vec3 p = player_.state().pos;
    const auto ring = [&](vec3 at) -> const PoundRing* {
        const float d = glm::length(at - p);
        for (const PoundRing& r : kPoundRings)
            if (d > r.inner && d < r.outer) return &r;
        return nullptr;
    };
    const auto damage = [&](const PoundRing& r, vec3 at) {
        return glm::mix(r.damage, r.edge, (glm::length(at - p) - r.inner) / (r.outer - r.inner));
    };
    for (Damageable* t : targets) {
        if (!t || !t->alive() || t->team() != Team::Enemy) continue;
        const PoundRing* r = ring(t->position());
        if (!r) continue;
        const vec3 d = t->position() - p;
        Hit hit{damage(*r, t->position()), t->aimPoint(), glm::normalize(vec3(vec2(d), 0) + vec3(1e-4f, 0, 0)), Team::Player, false,
                r->knock > 0};
        hit.damageClass = 13, hit.damageType = 0;
        if (r->knock > 0) { // message 0x91: speed lerped inner -> outer (knockEdge >= 0), r->angle up, away from Lara
            const float dist = glm::length(t->position() - p);
            const float speed = r->knockEdge >= 0 ? glm::mix(r->knock, r->knockEdge, (dist - r->inner) / (r->outer - r->inner)) : r->knock;
            const float up = r->angle * kDeg;
            hit.knockVel = (hit.dir * std::cos(up) + vec3(0, 0, std::sin(up))) * speed;
        }
        t->onHit(hit);
    }
    for (const auto& o : script_.objects())
        if (const PoundRing* r = uberRuntime_.contains(o.uid) ? ring(o.pos) : nullptr) damageObject(o.uid, damage(*r, o.pos), 13);
}

void World::tickCombat(const Actions& a, float dt) {
    CombatState& c = combat_;
    c.shots.clear();
    tickProjectiles(dt);
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
    // adrenaline: the meter drains (0x6F2930), the mode ends with it; world time ramps to the window's / mode's scale
    const AdrenalineTuning& at = anims_.adrenaline;
    if (c.adrenaline > 0) c.adrenaline = std::max(0.0f, c.adrenaline - (c.adrenalineMode ? at.modeDrain : at.drain) / at.max * dt);
    if (c.adrenaline <= 0 && c.slowMo <= 0) c.adrenalineMode = false;
    const float timeTarget = c.slowMo > 0 ? at.headShotScale : c.adrenalineMode ? at.modeScale : 1.0f;
    if (timeTarget != c.timeTarget) c.timeTarget = timeTarget, c.timeRate = std::abs(timeTarget - c.timeScale) / at.ease;
    c.timeScale = approach(c.timeScale, c.timeTarget, c.timeRate * dt);
    if (s.state == State::Dead) {
        c.deathTime += dt;
        if (c.deathTime >= kDeathFade) respawn(); // the wipe is black (DeathState_Process 0x75AF90)
        return;
    }
    if (health_.dead() && !airborne(s.state)) { // [I] dies on the ground; fatal falls die on landing
        // out of breath (0x755230: periodic damage, death type 8) under water: death_drown [I: the type-8 clip]
        const bool drowned = health_.lastType() == kDamagePeriodic && s.water >= 0;
        // a fatal landing out of a swan dive: death_fall_swan (lara.drm request 104) [I: by the name]
        const bool swan = s.clip == "ng_swan_land" && anims_.find("death_fall_swan");
        player_.die(drowned ? "death_drown" : health_.lastType() == kDamageAbsolute ? (swan ? "death_fall_swan" : "death_fall_normal") : "death_toknee");
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
    // On the bike (riding, not mounting / dismounting) and on the rope (swinging, on a wall) only the pistols and the
    // uzi come out, one gun (Vehicle.cpp, Grapple.cpp)
    const bool bike = s.state == State::Vehicle, rope = onRope(s.state), oneHand = bike || rope;
    const auto allowed = [&](Weapon wpn) { return bike ? ridingBike() && bikeWeapon(wpn) : rope ? bikeWeapon(wpn) : canShoot(s, wpn); };
    if (weaponPressed && c.slowMo <= 0) {
        Weapon cycle[3] = {Weapon::Pistols};
        int n = 1;
        if (c.secondary != Weapon::Pistols && (!oneHand || bikeWeapon(c.secondary))) cycle[n++] = c.secondary;
        if (hasHammer() && !oneHand) cycle[n++] = Weapon::ThorHammer;
        const auto at = std::find(cycle, cycle + n, c.want);
        if (n > 1 || at == cycle + n) c.want = at == cycle + n || at + 1 == cycle + n ? cycle[0] : *(at + 1);
        c.redraw = c.redraw || (n > 1 && c.drawn);
    }
    // [I] drawing on the bike / rope with another weapon ready takes the pistols
    if (((bike && ridingBike()) || rope) && !bikeWeapon(c.want) && (drawPressed || a.fire || a.aim)) c.want = Weapon::Pistols, c.redraw = true;
    if (c.want != c.weapon) {
        c.drawn = false, c.reload = 0, c.hammer = {};
        if (c.drawT <= 0) {
            c.previous = c.weapon, c.weapon = c.want;
            c.drawn = c.redraw && allowed(c.weapon);
            c.redraw = false, c.cooldown = 0, c.hand = 0;
            c.spread = anims_.weapons[size_t(c.weapon)].spreadDeg;
        }
    } else if (!allowed(c.weapon)) c.drawn = false; // hanging, swimming (not the speargun under water), getting on / off the bike: holstered
    else if (c.redraw) c.drawn = true, c.redraw = false;
    else if (drawPressed) c.drawn = !c.drawn;
    else if ((a.fire || a.aim) && !c.drawn) c.drawn = true; // target lock draws (manual), fire too [I]
    const auto wi = size_t(c.weapon);
    const WeaponDef& w = anims_.weapons[wi];
    const WeaponInfo& in = info(c.weapon);
    const Handling hd = handling(anims_, s, c.weapon);
    const bool dual = w.dual && !oneHand; // one gun on the bike / rope
    c.drawT = approach(c.drawT, c.drawn ? 1.0f : 0.0f, dt / (c.drawn ? hd.equip : hd.unequip));
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
    float yaw = wrapAngle(std::atan2(v.y, v.x) - player_.state().yaw);
    const float pitch = std::atan2(v.z, glm::length(vec2(v)));
    if (bike) { // the bike grid: straight round her right side to behind (behind-left counts as behind)
        if (yaw > kPi / 2) yaw -= 2 * kPi;
        c.aim = {std::clamp(yaw, kBikeCols[0], kBikeCols[4]), std::clamp(pitch, -kBikeDown, kBikeUp)};
    } else if (rope) { // the rope grid: front round her right side to behind, the same way
        if (yaw > kPi / 2) yaw -= 2 * kPi;
        c.aim = {std::clamp(yaw, kRopeColYaw[0], kRopeColYaw[4]), std::clamp(pitch, -kRopeDown, kRopeUp)};
    } else {
        c.aim = {std::clamp(yaw, yawLo, yawHi), hammer ? 0.0f : std::clamp(pitch, -gridDown(in.rifle), gridUp(in.rifle))};
    }

    if (!c.locked && c.slowMo <= 0) c.adrenalineMode = false; // 0x718BD0: the mode needs her lock (flag 0x400)
    const int hand = dual ? c.hand : 0;
    if (hammer) { // Mjolnir: swings and ground pounds play out; no adrenaline head shot [I]
        HammerAction& h = c.hammer;
        if (!h.clip.empty()) {
            h.prevT = h.t, h.t += dt;
            const auto reached = [&](float at) { return h.prevT < at && h.t >= at; };
            if (h.clip == "ground_melee_idle") {
                const auto f = anims_.weapons[size_t(Weapon::ThorHammer)].commandFrame(h.clip, {0x4B});
                if (reached(f ? float(*f) / 30.0f : kHammerPound)) groundPound();
            } else if (const auto f = anims_.weapons[size_t(Weapon::ThorHammer)].commandFrame(h.clip, {0x75});
                       reached(f ? float(*f) / 30.0f : s.water >= 0 ? kHammerSwimShot : kHammerShot)) {
                fire(0);
            }
            const anim::Clip* k = anims_.weaponClip(Weapon::ThorHammer, h.clip);
            if (h.t >= (k ? float(k->durationSeconds()) : w.refire)) h = {};
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
                const Shot shot{from, to, hand, true, c.target, c.target->health(), gunMatrix(hand)};
                Hit hit{shot.damage, to, glm::normalize(to - from + vec3(1e-6f, 0, 0)), Team::Player, true};
                hit.damageClass = anims_.weapons[size_t(c.weapon)].hitClass, hit.damageType = anims_.weapons[size_t(c.weapon)].hitType;
                c.target->onHit(hit);
                c.shots.push_back(shot);
                c.sinceShot[hand] = 0;
                ++c.shotsFired;
            }
            c.slowMo = 0; // one shot per window [I]
        }
        if (c.slowMo <= 0) c.adrenaline = 0, c.adrenalineMode = false; // spent
    } else if (interactPressed && c.adrenalineMode && c.locked && c.target && c.drawT >= 1 && // in the mode, close [I]
               glm::length(c.target->aimPoint() - (s.pos + vec3(0, 0, targeting.eyeHeight))) <= targeting.closeRange) {
        // "full meter, lock on, get close and press X" (locals 1333/1334); close = the close lock tier [I]
        const float ang = (rnd(c.rng) + 0.5f) * 2 * kPi;
        c.slowMo = kSlowWindow;
        c.pip = kPipStart * vec2(std::cos(ang), std::sin(ang));
    } else if (interactPressed && c.locked && c.target && c.drawT >= 1 && (c.adrenalineMode || c.adrenaline > 0)) {
        c.adrenalineMode = !c.adrenalineMode; // 0x718BD0 toggles it (0x7189C0 / 0x718AD0); interact as the button [I]
    } else if (a.fire && c.drawT >= 1 && c.reload <= 0 && c.cooldown <= 0) {
        const bool canReload = w.infinite || c.reserve[wi] > 0;
        if (c.clip[wi][hand] <= 0) {
            if (canReload) c.reload = hd.reload;
            else if (c.weapon != Weapon::Pistols) c.want = Weapon::Pistols, c.redraw = true; // [I] dry: back to the pistols
        } else {
            fire(hand);
            --c.clip[wi][hand]; // the clip empties even with infinite reserve (0x6C9E50)
            if (dual) c.hand ^= 1;
            c.cooldown = w.refire;
            if ((dual ? c.clip[wi][0] <= 0 && c.clip[wi][1] <= 0 : c.clip[wi][0] <= 0) && canReload) c.reload = hd.reload;
        }
    }
}

// Upper body while a weapon is out (subtree of spine segment 14): its aim grid blended bilinearly by the aim
// angles; pistols / uzi add the fire clip of the hand that just fired (its motion relative to its first frame),
std::pair<std::string, double> World::combatLayerClip() const {
    const CombatState& c = combat_;
    const Handling hd = handling(anims_, player_.state(), c.weapon);
    if (c.drawT <= 0) return {};
    if (c.reload > 0) return {hd.reloadClip, hd.reload - c.reload};
    if (c.drawT >= 1) return {};
    return {c.drawn ? hd.equipClip : hd.unequipClip, (c.drawn ? c.drawT : 1 - c.drawT) * (c.drawn ? hd.equip : hd.unequip)};
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
    const bool rifle = in.rifle, hammer = wpn == Weapon::ThorHammer;
    // on the bike: its 5 x 3 grid (kBikeCols), each cell held at its last key (_straight_u moves into its pose); on the
    // rope its own (kRopeColYaw), looping as the ground grids [I]
    const State st = player_.state().state;
    const bool bike = st == State::Vehicle && bikeWeapon(wpn), rope = onRope(st) && bikeWeapon(wpn);
    const Handling hd = handling(anims_, player_.state(), wpn);
    const int cols = bike || rope ? 5 : gridCols(rifle);
    const auto yawOf = [&](int col) { return bike ? kBikeCols[col] : rope ? kRopeColYaw[col] : colYaw(rifle, col); };
    const auto aimCell = [&](int r, int col) { return bike ? bikeCell(r, col) : rope ? ropeCell(st, r, col) : cellName(in, false, r, col); };
    const auto clip = [&](std::string_view n) { return anims_.weaponClip(wpn, n); };
    const anim::Clip* centre = clip(hammer ? std::string("ground_aim_front") : aimCell(1, bike || rope ? 4 : cols / 2));
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
        while (i0 + 2 < cols && c.aim.x > yawOf(i0 + 1)) ++i0;
        const float u = std::clamp((c.aim.x - yawOf(i0)) / (yawOf(i0 + 1) - yawOf(i0)), 0.0f, 1.0f);
        const int row = c.aim.y >= 0 ? 0 : 2;
        const float up = bike ? kBikeUp : rope ? kRopeUp : gridUp(rifle), down = bike ? kBikeDown : rope ? kRopeDown : gridDown(rifle);
        const float v = std::clamp(c.aim.y >= 0 ? c.aim.y / up : -c.aim.y / down, 0.0f, 1.0f);
        const bool firing = rifle && c.sinceShot[0] < in.fire;
        const auto cell = [&](int r, int col) {
            if (const anim::Clip* k = firing ? clip(cellName(in, true, r, col)) : nullptr) return sample(k, c.sinceShot[0], false);
            const anim::Clip* k = clip(aimCell(r, col));
            if (!k) k = centre;
            return bike ? sample(k, k->durationSeconds(), false) : sample(k, aimTime_, true);
        };
        anim::blendPoses(cell(1, i0), cell(1, i0 + 1), u, p);
        anim::blendPoses(cell(row, i0), cell(row, i0 + 1), u, q);
        anim::blendPoses(p, q, v, p);
        if (!rifle) {
            for (int hand = 0; hand < (bike || rope ? 1 : 2); ++hand) // recoil (on the bike / rope the right gun only)
                if (const anim::Clip* k = clip(hand == 0 ? "combat_pistol_RfireE" : "combat_pistol_LfireE"); k && c.sinceShot[hand] < k->durationSeconds())
                    additive(p, k, c.sinceShot[hand]);
        } else if (const anim::Clip* k = wpn == Weapon::Shotgun ? clip("combat_shotgun_pump") : nullptr;
                   k && c.sinceShot[0] >= in.fire && c.sinceShot[0] < in.fire + kPump) {
            additive(p, k, c.sinceShot[0] - in.fire);
        }
        if (const anim::Clip* k = clip(hd.reloadClip); k && c.reload > 0)
            anim::blendPoses(p, sample(k, hd.reload - c.reload, false), 1.0f, p);
    }
    float layer = 1;
    if (c.drawT < 1) {
        const float dur = c.drawn ? hd.equip : hd.unequip;
        if (const anim::Clip* k = clip(c.drawn ? hd.equipClip : hd.unequipClip)) {
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
