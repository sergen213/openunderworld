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

// Pistol timing [O clip lengths, I use]: one shot per hand's fire clip (combat_pistol_RfireE/LfireE, 10 frames),
// hands alternate; the upper-body layer ramps in over combat_pistols_equip (0.2 s) and out over
// combat_pistols_unequip (0.633 s); a reload lasts combat_pistols_reload (0.7 s). Respawn 1 s after the
// death clip ends [I].
constexpr float kRefire = 10.0f / 30.0f, kDrawSeconds = 0.2f, kHolsterSeconds = 19.0f / 30.0f, kReloadSeconds = 0.7f;
constexpr float kRespawnDelay = 1.0f;
constexpr float kShoulderHeight = 418.0f; // bind-pose shoulder joints 53 / 78 above the feet
// Aim grid of combat_pistol_pose[A-I], measured from each pose's shoulder->hand directions (both arms,
// model space): columns left (A D G) +109 deg, centre (B E H) 0, right (C F I) -104; rows up +40, down -57.
constexpr float kGridLeft = 109.0f * kPi / 180, kGridRight = 104.0f * kPi / 180, kGridUp = 40.0f * kPi / 180,
                kGridDown = 57.0f * kPi / 180;
// Lara's bones: arms 52-55 (left, model +X) / 77-80 (right), thighs 4 (left) / 9 (right); upper body = the
// subtree of spine segment 14.
constexpr int kHandBone[2] = {80, 55}, kThighBone[2] = {9, 4}, kSpineBone = 14;
// Gun model (wp_handgun mesh: barrel along -Y, top +Z) in the bone frames; bind rotations are identity, so
// bone axes are model axes: arms extend along +-X, thumbs point -Y (forward) in the T pose. Offsets [I], fitted
// on screenshots.
const glm::vec3 kMuzzle(0, -64, 22); // barrel tip in the gun model [I] from its bounds (y -64.2 .. 15.4)

bool canShoot(State s) {
    switch (s) {
    case State::Idle: case State::Walk: case State::Run: case State::Stop: case State::Turn: case State::Land:
    case State::JumpPrep: case State::Jump: case State::Fall: return true;
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
float WeaponDef::damageAt(float d) const {
    if (d <= shortRange) return glm::mix(damage[0], damage[1], std::max(0.0f, d) / shortRange);
    return glm::mix(damage[1], damage[2], std::min(1.0f, (d - shortRange) / (maxRange - shortRange)));
}

std::optional<WeaponDef> loadWeapon(const cdc::BigfileSet& bf, std::string_view object) {
    const auto w = objectData(bf, object);
    if (!w) return std::nullopt;
    constexpr uint32_t S = 0xE0; // stats block (query 0x36, 0x6C1CF0)
    const auto pellets = w->at<int8_t>(S + 0x05), clip = w->at<int8_t>(S + 0x0B);
    const auto ammoId = w->at<int16_t>(S + 0x06);
    const auto maxRange = w->at<float>(S + 0x18), spread = w->at<float>(S + 0x1C), shortRange = w->at<float>(S + 0x20);
    const auto maxSpread = w->at<float>(S + 0x2C), perShot = w->at<float>(S + 0x30);
    const auto flags = w->at<uint8_t>(S + 0x88);
    if (!pellets || !clip || !ammoId || !maxRange || !spread || !shortRange || !maxSpread || !perShot || !flags) return std::nullopt;
    const auto list = bf.read("\\tr8\\pc-w\\objectlist.txt");
    if (!list) return std::nullopt;
    const auto names = formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()});
    const auto ammoName = names.find(uint16_t(*ammoId));
    if (ammoName == names.end()) return std::nullopt;
    const auto ammo = objectData(bf, ammoName->second);
    if (!ammo) return std::nullopt;
    constexpr uint32_t R = 0x18; // fire-mode record (0x69E770; +0x84 upgraded)
    const auto mode = ammo->at<int32_t>(R);
    const auto d0 = ammo->at<float>(R + 4), d1 = ammo->at<float>(R + 0x10), d2 = ammo->at<float>(R + 0x14);
    if (!mode || !d0 || !d1 || !d2) return std::nullopt;
    WeaponDef def;
    def.object = std::string(object), def.ammo = ammoName->second;
    def.pellets = *pellets, def.clip = *clip;
    def.infinite = *flags & 0x80, def.dual = *flags & 0x20;
    def.shortRange = *shortRange, def.maxRange = *maxRange;
    def.spreadDeg = *spread, def.maxSpreadDeg = *maxSpread, def.spreadPerShot = *perShot;
    def.mode = *mode;
    def.damage[0] = *d0, def.damage[1] = *d1, def.damage[2] = *d2;
    return def;
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

// ---- World: damage, death -----------------------------------------------------------------------
void World::damageLara(float amount, vec3, int type) { health_.damage(amount, type); } // [U] hit reactions: direction unused

void World::kill() { health_.damage(health_.value(), kDamageKill); }

// ---- World: guns --------------------------------------------------------------------------------
std::pair<int, glm::mat4> World::gunMount(int hand) const {
    const float side = hand == 0 ? -1.0f : 1.0f; // right arm extends along -X, left along +X
    glm::mat4 m(1);
    if (combat_.drawT >= 0.5f) {
        // barrel (gun -Y) along the arm, gun top (+Z) along the thumb (-Y)
        m[0] = glm::vec4(0, 0, side, 0);
        m[1] = glm::vec4(-side, 0, 0, 0);
        m[2] = glm::vec4(0, -1, 0, 0);
        m[3] = glm::vec4(side * 18.0f, 0, 0, 1);
        return {kHandBone[hand], m};
    }
    // thigh holster: barrel down, gun top forward, on the outside of the thigh
    m[0] = glm::vec4(1, 0, 0, 0);
    m[1] = glm::vec4(0, 0, 1, 0);
    m[2] = glm::vec4(0, -1, 0, 0);
    m[3] = glm::vec4(side * 22.0f, 0, -60.0f, 1);
    return {kThighBone[hand], m};
}

vec3 World::muzzle(int hand) const {
    const PlayerState& s = player_.state();
    const auto [bone, offset] = gunMount(hand);
    glm::mat4 b(1); // no such bone (synthetic skeletons): the gun at shoulder height
    if (size_t(bone) < bones_.size()) b = bones_[size_t(bone)];
    else b[3].z = kShoulderHeight;
    return vec3(player_.modelMatrix(s.pos, s.yaw) * b * offset * glm::vec4(kMuzzle, 1));
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

void World::fire(int hand) {
    CombatState& c = combat_;
    const WeaponDef& w = anims_.pistols;
    const vec3 from = muzzle(hand);
    vec3 dir = c.aimPoint - from;
    if (glm::length(dir) < 1) dir = vec3(std::cos(player_.state().yaw), std::sin(player_.state().yaw), 0);
    dir = glm::normalize(dir);
    // spread: yaw and pitch offsets within +-spread/2 degrees (0x69E300); a 32-bit LCG keeps replays exact
    const auto rnd = [&] {
        c.rng = c.rng * 1664525u + 1013904223u;
        return float((c.rng >> 8) & 0xFFFF) / 65535.0f - 0.5f;
    };
    const float dy = rnd() * c.spread * kPi / 180, dp = rnd() * c.spread * kPi / 180;
    const vec3 right = glm::normalize(glm::cross(dir, vec3(0, 0, 1)) + vec3(1e-6f, 0, 0));
    dir = glm::normalize(glm::angleAxis(dy, vec3(0, 0, 1)) * (glm::angleAxis(dp, right) * dir));
    float best = w.maxRange;
    bool hitAny = false;
    if (const auto g = col_.raycast(from, dir, best)) best = g->t, hitAny = true;
    Damageable* struck = nullptr;
    for (Damageable* t : targets) {
        if (!t || !t->alive() || t->team() == Team::Player) continue;
        if (const auto d = rayCapsule(from, dir, t->position(), t->radius(), t->height(), best)) best = *d, struck = t, hitAny = true;
    }
    Shot shot{from, from + dir * best, hand, hitAny, struck, 0};
    if (struck) {
        shot.damage = w.damageAt(best);
        struck->onHit({shot.damage, shot.to, dir, Team::Player});
    }
    c.shots.push_back(shot);
    c.spread = std::min(w.maxSpreadDeg, c.spread + w.spreadPerShot);
    c.sinceShot[hand] = 0;
    ++c.shotsFired;
}

void World::tickCombat(const Actions& a, float dt) {
    CombatState& c = combat_;
    c.shots.clear();
    const PlayerState& s = player_.state();
    if (s.fallHeight > 0) health_.fall(s.fallHeight); // landing (tru.exe 0x75D080 -> 0x702FC0)
    health_.tick(dt);
    if (s.state == State::Dead) {
        c.deathTime += dt;
        const anim::Clip* clip = anims_.find(s.clip);
        if (c.deathTime >= (clip ? clip->durationSeconds() : 0) + kRespawnDelay) respawn();
        return;
    }
    if (health_.dead() && !airborne(s.state)) { // [I] dies on the ground; fatal falls die on landing
        player_.die(health_.lastType() == kDamageAbsolute ? "death_fall_normal" : "death_toknee");
        const uint64_t rng = c.rng;
        c = {};
        c.rng = rng, c.deathTime = 0;
        return;
    }
    const WeaponDef& w = anims_.pistols;
    const bool drawPressed = a.draw && !c.drawHeld, switchPressed = a.switchTarget && !c.switchHeld;
    c.drawHeld = a.draw, c.switchHeld = a.switchTarget;
    if (!canShoot(s.state)) c.drawn = false; // hanging, swimming: holstered
    else if (drawPressed) c.drawn = !c.drawn;
    else if (a.fire && !c.drawn) c.drawn = true; // [I] fire draws
    if (!c.drawn) c.spread = w.spreadDeg;        // reset on draw (0x6E6C60)
    c.drawT = approach(c.drawT, c.drawn ? 1.0f : 0.0f, dt / (c.drawn ? kDrawSeconds : kHolsterSeconds));
    for (float& t : c.sinceShot) t += dt;
    c.cooldown = std::max(0.0f, c.cooldown - dt);
    if (c.reload > 0 && (c.reload -= dt) <= 0) c.reload = 0, c.clip[0] = c.clip[1] = w.clip; // infinite reserve
    const bool known = std::find(targets.begin(), targets.end(), c.target) != targets.end();
    if (!c.drawn || !known || !c.target->alive() ||
        glm::length(c.target->aimPoint() - s.pos) > targeting.lockRange) // [I] the lock drops past the tier range
        c.target = nullptr, c.locked = false;
    if (!c.drawn) return;

    // targeting: aim held = hard lock (kept, switchable), else the soft target in front of Lara
    if (a.aim) {
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
    c.aim = {std::clamp(wrapAngle(std::atan2(v.y, v.x) - player_.state().yaw), -kGridRight, kGridLeft),
             std::clamp(std::atan2(v.z, glm::length(vec2(v))), -kGridDown, kGridUp)};

    if (a.fire && c.drawT >= 1 && c.reload <= 0 && c.cooldown <= 0) {
        if (c.clip[c.hand] <= 0) {
            c.reload = kReloadSeconds;
        } else {
            fire(c.hand);
            --c.clip[c.hand]; // the clip empties even with infinite reserve (0x6C9E50)
            c.hand ^= 1;
            c.cooldown = kRefire;
            if (c.clip[0] <= 0 && c.clip[1] <= 0) c.reload = kReloadSeconds;
        }
    }
}

// Upper body while the pistols are out: the 3x3 aim grid blended bilinearly by the aim angles, the fire
// clip of the hand that just fired added on top (its motion relative to its first frame), the reload clip
// while reloading; faded in/out with drawT over the subtree of spine segment 14.
void World::aimLayer(std::vector<anim::LocalTransform>& local, float dt) {
    aimTime_ += dt;
    const CombatState& c = combat_;
    const auto& sk = anims_.skeleton;
    const auto pose = [&](char k) { return anims_.find(std::string("combat_pistol_pose") + k); };
    if (c.drawT <= 0 || local.size() != sk.bones.size() || sk.bones.size() <= size_t(kHandBone[0]) || !pose('E')) return;
    const auto sample = [&](const anim::Clip* clip, double t, bool loop) { return anim::sample(*clip, sk, t, loop); };
    const char cols[3][3] = {{'A', 'B', 'C'}, {'D', 'E', 'F'}, {'G', 'H', 'I'}}; // [row up/mid/down][col left/centre/right]
    const float u = c.aim.x >= 0 ? c.aim.x / kGridLeft : -c.aim.x / kGridRight;
    const float v = c.aim.y >= 0 ? c.aim.y / kGridUp : -c.aim.y / kGridDown;
    const int col = c.aim.x >= 0 ? 0 : 2, row = c.aim.y >= 0 ? 0 : 2;
    const auto centre = sample(pose('E'), aimTime_, true), side = sample(pose(cols[1][col]), aimTime_, true);
    const auto vert = sample(pose(cols[row][1]), aimTime_, true), corner = sample(pose(cols[row][col]), aimTime_, true);
    std::vector<anim::LocalTransform> p(local.size()), q(local.size());
    anim::blendPoses(centre, side, std::clamp(u, 0.0f, 1.0f), p);
    anim::blendPoses(vert, corner, std::clamp(u, 0.0f, 1.0f), q);
    anim::blendPoses(p, q, std::clamp(v, 0.0f, 1.0f), p);
    for (int hand = 0; hand < 2; ++hand) { // recoil
        const anim::Clip* fireClip = anims_.find(hand == 0 ? "combat_pistol_RfireE" : "combat_pistol_LfireE");
        if (!fireClip || c.sinceShot[hand] >= fireClip->durationSeconds()) continue;
        const auto f0 = sample(fireClip, 0, false), ft = sample(fireClip, c.sinceShot[hand], false);
        for (size_t i = 0; i < p.size(); ++i) {
            const glm::quat q0 = glm::quat_cast(anim::rotationFromVector(f0[i].rot)), qt = glm::quat_cast(anim::rotationFromVector(ft[i].rot));
            const glm::quat r = glm::normalize(glm::quat_cast(anim::rotationFromVector(p[i].rot)) * glm::inverse(q0) * qt);
            p[i].rot = glm::angle(r) * glm::axis(r);
            p[i].trans += ft[i].trans - f0[i].trans;
        }
    }
    if (const anim::Clip* rl = anims_.find("combat_pistols_reload"); rl && c.reload > 0)
        anim::blendPoses(p, sample(rl, kReloadSeconds - c.reload, false), 1.0f, p);
    // upper-body mask: bones descending from the spine (parents precede children)
    std::vector<char> upper(sk.bones.size(), 0);
    upper[size_t(kSpineBone)] = 1;
    for (size_t i = size_t(kSpineBone) + 1; i < sk.bones.size(); ++i)
        upper[i] = sk.bones[i].parent >= 0 && upper[size_t(sk.bones[i].parent)];
    const float wgt = c.drawT * c.drawT * (3 - 2 * c.drawT); // smoothstep
    for (size_t i = 0; i < local.size(); ++i)
        if (upper[i]) local[i] = anim::blend(local[i], p[i], wgt);
}

} // namespace ou::game
