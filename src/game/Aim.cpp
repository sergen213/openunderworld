// Aiming beyond the target lock (docs/GAMEPLAY.md "Manual aim", "Adrenaline (decoded)", "Dual targeting", "Time
// scales"): manual (precision) aim, the adrenaline head shot move, the second target of a two-gun weapon, the hit shapes
// only shots meet, and tru.exe's two time scales. World::tickCombat (Combat.cpp) calls in here.
#include "game/Game.hpp"

#include <algorithm>
#include <cmath>

namespace ou::game {

namespace {
constexpr float kPi = 3.14159265f;
// The head shot on a target without an enemy record (a plain Damageable: ou-game --target, synthetic tests): the
// record's numbers every enemy type shares (+0x10 = 316.06 units, +0x14 = 6 frames) [O] and an aim window in the
// move's seconds [I: no hosted clip to read events 5 / 4 from].
constexpr float kHeadShotDistance = 316.056f, kHeadShotOpen = 0.4f, kHeadShotClose = 1.0f, kHeadShotLength = 1.4f;
// The pip [I: 0x70DB20 starts the accurate aim on the target's head marker; its steering is not decoded]: it starts
// kPipStart reticle radii off in a random direction; the look input moves it kPipLook radii per radian turned, the
// move stick kPipStick radii per second; inside 1 radius fire is the head shot.
constexpr float kPipStart = 2.5f, kPipLook = 10.0f, kPipStick = 2.0f;
// Scripts may ask for any scale; a step of exactly 0 would stall every clip and timer for good [I: guard].
constexpr float kMinScale = 0.01f, kMaxScale = 4.0f;

bool onFeet(State s) { return s == State::Idle || s == State::Walk || s == State::Run || s == State::Stop || s == State::Turn; }
// 32-bit LCG in [-0.5, 0.5], as Combat.cpp's: replay-exact
float rnd(uint64_t& rng) {
    rng = rng * 1664525u + 1013904223u;
    return float((rng >> 8) & 0xFFFF) / 65535.0f - 0.5f;
}
// Lara's hosted clip per weapon (HeadShotMove::clips): the enemy DRMs' hosted_enemy_aa_handguns / _shotgun / _rifle /
// _hammer [I: by name; the request (type 2) the record holds is not resolved].
size_t hostedKind(Weapon w) {
    switch (w) {
    case Weapon::Shotgun: case Weapon::Speargun: return 1;
    case Weapon::AssaultRifle: case Weapon::Tranquilizer: return 2;
    case Weapon::ThorHammer: return 3;
    default: return 0;
    }
}
} // namespace

// ---- time scales --------------------------------------------------------------------------------
// 0x78C4E0: while the record runs (seconds > 0, or 0 for a jump), value = from + (target - from) x ease(u), u =
// elapsed / seconds, ease 0x78C120 = 2u^2 below a half, 1 - 2(1 - u)^2 above; at u >= 1 the target, and the record
// goes idle (-1). The exe's clock is its game timer 0xE7F15C; ours real seconds [I].
float CombatState::TimeRamp::step(float value, float dt) {
    if (!(seconds >= 0)) return value;
    elapsed += dt;
    const float u = seconds > 0 ? elapsed / seconds : 1.0f;
    if (u < 1) {
        const float e = u < 0.5f ? 2 * u * u : 1 - 2 * (1 - u) * (1 - u);
        return from + (target - from) * e;
    }
    seconds = -1;
    return target;
}

void World::tickTimeScales(float real) {
    CombatState& c = combat_;
    c.timeScale = c.timeRamp.step(c.timeScale, real);
    c.worldScale = c.worldRamp.step(c.worldScale, real);
}

void World::setTimeScale(bool world, float target, float seconds) {
    CombatState& c = combat_;
    target = std::clamp(target, kMinScale, kMaxScale);
    float& value = world ? c.worldScale : c.timeScale;
    if (seconds < 0) value = target; // 0x4A3DC0 / 0x4A3DD0 write the scale only: a ramp still running goes on from its record
    else (world ? c.worldRamp : c.timeRamp).start(target, seconds, value);
}

// ---- shots --------------------------------------------------------------------------------------
World::RayHit World::shotRay(vec3 from, vec3 dir, float len) const {
    RayHit r{len};
    if (const auto g = col_.raycast(from, dir, r.t))
        r.t = g->t, r.hit = true, r.object = g->dynamic >= 0 ? col_.dynamics[size_t(g->dynamic)].id : 0;
    for (Damageable* t : targets) {
        if (!t || !t->alive() || t->team() == Team::Player) continue;
        if (const auto d = rayCapsule(from, dir, t->position(), t->radius(), t->height(), r.t)) r = {*d, t, 0, true};
    }
    for (const auto& d : shotOnly_) {
        const script::Object* o = script_.find(d.id);
        if (!o || o->dead || o->hidden || !d.shape) continue;
        const glm::mat4 inv = glm::inverse(d.transform); // rigid: lengths are kept
        if (const auto h = d.shape->raycast(vec3(inv * glm::vec4(from, 1)), glm::mat3(inv) * dir, r.t)) r = {h->t, nullptr, d.id, true};
    }
    return r;
}

// ---- manual aim ---------------------------------------------------------------------------------
// Player state chart [O lara.drm]: on her feet (S711 and its ground siblings; also wading, a perch, a beam, carrying:
// not ours yet) Manual Aim pressed (input action 25 = virtual button 9) without Target Lock held (action 35) enters
// S718, whose camera mode is 23; there Manual Aim (T1887) or Target Lock (T1886) pressed, or the grapple attaching
// (T1885), leaves it. The guns come out with it (S718's combat stance). Ours also leaves when she is no longer on her
// feet [I].
void World::tickManualAim(const Actions& a, bool manualPressed, bool aimPressed) {
    CombatState& c = combat_;
    const PlayerState& s = player_.state();
    const bool can = (onFeet(s.state) || (c.manualAim && s.state == State::Land)) && s.water < 0 && !s.grappleUid && !fieldCamera_ &&
                     !splineCam_ && !health_.dead() && c.headShot < 0;
    if (c.manualAim) {
        if (!can || manualPressed || aimPressed) c.manualAim = false;
    } else if (manualPressed && !a.aim && can) {
        c.manualAim = true;
    }
    if (c.manualAim && !c.drawn && c.want == c.weapon && !c.redraw) c.drawn = true;
}

vec3 World::manualAimPoint() const {
    const WeaponDef& w = anims_.weapons[size_t(combat_.weapon)];
    const vec3 d = glm::normalize(cam_.target - cam_.position);
    // the weapon's range counted from her, not from the camera behind her
    const float len = std::max(0.0f, glm::dot(player_.state().pos - cam_.position, d)) + w.maxRange;
    return cam_.position + d * shotRay(cam_.position, d, len).t;
}

// ---- dual targeting -----------------------------------------------------------------------------
// 0x71E490, each frame: with a primary target, a two-gun weapon (stats S+0x88 bit 0x20), no paired move and outside
// adrenaline mode, the best candidate of targeting slot 4 (Lara data +0x470, our soft set) other than the primary
// becomes the second target (weapon controller +0x10, flag 0x4000); one already held stays for `Lara data +0x286`
// frames before another candidate (or none) replaces it. 0x7035F0 then hands the second target to one of the two arm
// aimers. Ours: the left gun fires at it (World::fire); the arms' pose still follows the primary [I].
void World::tickSecondTarget(bool dual, float dt) {
    CombatState& c = combat_;
    const auto valid = [&](Damageable* t) {
        return t && t != c.target && std::find(targets.begin(), targets.end(), t) != targets.end() && t->alive() &&
               glm::length(t->aimPoint() - player_.state().pos) <= targeting.lockRange;
    };
    if (!valid(c.second)) c.second = nullptr;
    if (!dual || !c.target || c.adrenalineMode || c.headShot >= 0 || c.manualAim) {
        c.second = nullptr;
        return;
    }
    c.secondHold += dt;
    if (c.second && c.secondHold < anims_.adrenaline.secondTargetHold) return;
    Damageable* best = nullptr;
    for (Damageable* t : candidates(false))
        if (t != c.target) {
            best = t;
            break;
        }
    if (best != c.second) c.second = best, c.secondHold = 0;
}

// ---- adrenaline head shot -----------------------------------------------------------------------
// State chart T1404 (from the ground states S711 / S724 / S730): Grapple held (input action 46) with condition 0x21A
// (weapon controller flag 0x400000 = the meter latched, 0x6E66C0) and a melee-range target (0x7D1) [I: ours the close
// lock tier, 1264 from her eye]. The target must answer query 0x165 (its HeadShotMove record).
bool World::headShotAvailable() const {
    const CombatState& c = combat_;
    const PlayerState& s = player_.state();
    if (!c.headShotReady || c.headShot >= 0 || !c.drawn || c.drawT < 1 || !c.locked || !c.target || !c.target->alive()) return false;
    if (!onFeet(s.state) || s.water >= 0 || health_.dead()) return false;
    if (glm::length(c.target->aimPoint() - (s.pos + vec3(0, 0, targeting.eyeHeight))) > targeting.closeRange) return false;
    if (const auto* e = dynamic_cast<const ai::Enemy*>(c.target)) return e->type().tuning.headShot.valid();
    return true;
}

// 0x71D4F0: the target's clip for her starts (0x444BA0 with the target's object), she is carried to `distance` from it
// facing it (0x438DC0 over the record's frames; ours at once [I]), the target gets message 0xBE (its reaction), the
// meter is spent (0x6E6670) and the move flag 0x200000 set.
bool World::startHeadShot() {
    CombatState& c = combat_;
    c.headShotRequest = false;
    if (!headShotAvailable()) return false;
    const PlayerState& s = player_.state();
    float distance = kHeadShotDistance;
    c.headShotClip.clear();
    c.headShotOpen = kHeadShotOpen, c.headShotClose = kHeadShotClose, c.headShotLength = kHeadShotLength;
    auto* enemy = dynamic_cast<ai::Enemy*>(c.target);
    if (enemy) {
        const ai::HeadShotMove& hs = enemy->type().tuning.headShot;
        const ai::HeadShotMove::Clip& hc = hs.clips[hostedKind(c.weapon)];
        distance = hs.distance;
        if (const auto k = enemy->type().clips.find(hc.name); !hc.name.empty() && k != enemy->type().clips.end()) {
            c.headShotClip = enemy->type().name + ":" + hc.name; // her clip lives in the enemy's DRM
            anims_.clips.try_emplace(c.headShotClip, k->second);
            c.headShotLength = float(k->second.durationSeconds());
            if (hc.open >= 0) c.headShotOpen = hc.open;
            c.headShotClose = hc.close > c.headShotOpen ? hc.close : c.headShotLength;
        }
    }
    vec3 to = c.target->position() - s.pos;
    to.z = 0;
    const float dist = glm::length(to);
    const vec3 dir = dist > 1 ? to / dist : vec3(std::cos(s.yaw), std::sin(s.yaw), 0);
    const float yaw = std::atan2(dir.y, dir.x);
    vec3 at = s.pos + dir * (dist - distance);
    // [I] she is not carried through a wall or off her floor: then she plays it where she stands
    const vec3 chest = s.pos + vec3(0, 0, targeting.eyeHeight);
    if (col_.raycast(chest, glm::normalize(at - s.pos + vec3(1e-4f, 0, 0)), glm::length(at - s.pos) + player_.tuning().radius, 0x0F) ||
        !col_.raycast(at + vec3(0, 0, 50), {0, 0, -1}, 150.0f, 0x0F))
        at = s.pos;
    if (c.headShotClip.empty() || !player_.use(c.headShotClip, at, yaw)) player_.face(yaw);
    if (enemy) enemy->headShotReact(player_.state().pos);
    c.adrenaline = 0, c.headShotReady = false, c.adrenalineMode = false;
    c.headShot = 0, c.slowMo = 0, c.manualAim = false, c.second = nullptr;
    return true;
}

// The move's clock runs on her (scaled) time, as the clip. At its event 5 (0x715020) the game's time scale ramps to
// Lara data +0x2C8 = 0.2 and the aim opens; at event 4 (0x718EA0), or once she has fired, it ramps back and the aim
// closes. In between: the pip (kPip*), and fire with it in the reticle kills (Hit::headshot).
void World::tickHeadShot(const Actions& a, bool firePressed, int hand, float dt) {
    CombatState& c = combat_;
    const AdrenalineTuning& at = anims_.adrenaline;
    const float before = c.headShot;
    c.headShot += dt;
    const bool target = c.target && c.target->alive();
    if (before < c.headShotOpen && c.headShot >= c.headShotOpen && c.headShot < c.headShotClose && target) {
        const float ang = (rnd(c.rng) + 0.5f) * 2 * kPi;
        c.pip = kPipStart * vec2(std::cos(ang), std::sin(ang));
        c.slowMo = c.headShotClose - c.headShot;
        c.timeRamp.start(at.headShotScale, at.ease, c.timeScale);
    } else if (c.slowMo > 0) {
        c.slowMo = target ? std::max(0.0f, c.headShotClose - c.headShot) : 0.0f;
        c.pip += (vec2(-a.look.x, a.look.y) * kPipLook + a.move * kPipStick) * float(kTickSeconds);
        if (firePressed && c.slowMo > 0) {
            if (glm::length(c.pip) <= 1) { // damage = the target's remaining health
                const vec3 from = muzzle(hand), to = c.target->aimPoint();
                const Shot shot{from, to, hand, true, c.target, c.target->health(), gunMatrix(hand)};
                Hit hit{shot.damage, to, glm::normalize(to - from + vec3(1e-6f, 0, 0)), Team::Player, true};
                hit.damageClass = anims_.weapons[size_t(c.weapon)].hitClass, hit.damageType = anims_.weapons[size_t(c.weapon)].hitType;
                c.target->onHit(hit);
                c.shots.push_back(shot);
                c.sinceShot[hand] = 0;
                ++c.shotsFired;
            }
            c.slowMo = 0, c.headShotClose = c.headShot; // one shot per move [I]
        }
        if (c.slowMo <= 0) c.timeRamp.start(1, at.ease, c.timeScale);
    }
    if (c.headShot >= c.headShotLength && c.slowMo <= 0) c.headShot = -1, c.headShotClip.clear();
}

// 0x718FD0 (a hit on her, 0x71F1D0 message 0x11C; her death): the aim closes, time returns.
void World::endHeadShot() {
    CombatState& c = combat_;
    if (c.headShot < 0) return;
    if (c.slowMo > 0) c.slowMo = 0, c.timeRamp.start(1, anims_.adrenaline.ease, c.timeScale);
    c.headShot = -1, c.headShotClip.clear();
}

} // namespace ou::game
