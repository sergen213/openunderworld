// Gadgets: the utility light (flashlight) and sticky grenades. Evidence and numbers: docs/GAMEPLAY.md "Flashlight",
// "Grenades".
#include "game/Game.hpp"

#include <algorithm>
#include <cmath>

namespace ou::game {

namespace {
constexpr int kChestBone = 15, kRightHand = 80, kSpineBone = 14;
constexpr float kLightForward = 25.0f;     // [I] light origin in front of the chest bone, model -Y = forward
constexpr float kLightCooldown = 4 / 30.0f; // g_personal_light data +0xEA = 4 frames [O]
constexpr float kGrenadeRadius = 15.0f;    // [I] collision sphere of a flying grenade
constexpr float kMaxFlight = 3.0f;         // [I] a grenade that never sticks goes off this long after the throw
constexpr float kRelease = 0.45f;          // [I] fraction of combat_grenade_Throw1 at which it leaves the hand
constexpr float kLayerFade = 0.1f;         // [I] throw layer fade in / out, seconds
constexpr const char* kThrowClip = "g_grenade_thrower:combat_grenade_Throw1";

// [I] throws from the ground (the original also has crouch / perch / under-water placing clips we do not use)
bool canThrow(State s) {
    switch (s) {
    case State::Idle: case State::Walk: case State::Run: case State::Stop: case State::Turn: case State::Land: case State::Crouch:
        return true;
    default: return false;
    }
}
} // namespace

bool World::flashlightSpot() const { return !units_.empty() && currentUnit().data.flashlightSpot; }

std::pair<vec3, vec3> World::flashlightPose(double alpha) const {
    const auto s = interpolated(alpha);
    const auto bones = pose(alpha);
    const glm::mat4 m = player_.modelMatrix(s.pos, s.yaw, s.pitch) * (size_t(kChestBone) < bones.size() ? bones[kChestBone] : glm::mat4(1));
    const vec3 dir = glm::normalize(vec3(m * glm::vec4(0, -1, 0, 0)));
    return {vec3(m * glm::vec4(0, -kLightForward, 0, 1)), dir};
}

void World::tickGadgets(const Actions& a, float dt) {
    const PlayerState& s = player_.state();
    const bool alive = !health_.dead();
    // Flashlight (gadget "Personal Light", use 0x6CBDC0): the press toggles it after the cooldown. Its battery never
    // drains: data +0xF0 / +0xF2 (drain / recharge frames) are -1, which 0x6B1810 skips [O].
    lightCooldown_ = std::max(0.0f, lightCooldown_ - dt);
    if (a.light && !lightHeld_ && lightCooldown_ <= 0 && alive) flashlight = !flashlight, lightCooldown_ = kLightCooldown;
    lightHeld_ = a.light;

    // Grenades: press = throw at the aim point (if one is carried and Lara can), released part-way through the clip.
    const GrenadeTuning& g = anims_.grenade;
    explosions_.clear();
    grenadeCooldown_ = std::max(0.0f, grenadeCooldown_ - dt);
    if (a.grenade && !grenadeHeld_ && grenadeCooldown_ <= 0 && grenades > 0 && throwT_ < 0 && alive && canThrow(s.state)) {
        vec3 aim;
        if (combat_.target) aim = combat_.target->aimPoint();
        else { // the camera's view ray, as free aim
            const vec3 d = glm::normalize(cam_.target - cam_.position);
            const auto hit = col_.raycast(cam_.position, d, 20000.0f);
            aim = cam_.position + d * (hit ? hit->t : 20000.0f);
        }
        vec2 flat(aim - s.pos);
        const float raw = glm::length(flat), dist = std::clamp(raw, g.minRange, g.maxRange);
        flat = raw > 1 ? flat / raw : vec2(std::cos(cam_.yaw), std::sin(cam_.yaw));
        throwAt_ = vec3(vec2(s.pos) + flat * dist, dist == raw ? aim.z : s.pos.z); // clamped: lands at her height [I]
        player_.face(std::atan2(flat.y, flat.x)); // [I] she turns to the throw
        throwT_ = 0;
        grenadeCooldown_ = g.cooldown;
    }
    grenadeHeld_ = a.grenade;
    if (throwT_ >= 0) {
        const anim::Clip* clip = anims_.find(kThrowClip);
        const float dur = clip ? float(clip->durationSeconds()) : 0.5f, release = kRelease * dur;
        const float before = throwT_;
        throwT_ += dt;
        if (before < release && throwT_ >= release && grenades > 0) {
            // Launch: fixed vertical speed, horizontal speed so the arc lands on throwAt_ [I].
            const glm::mat4 model = player_.modelMatrix(s.pos, s.yaw, s.pitch);
            const vec3 from = size_t(kRightHand) < bones_.size() ? vec3(model * bones_[kRightHand][3]) : s.pos + vec3(0, 0, 400);
            const float dz = throwAt_.z - from.z, disc = g.launchVz * g.launchVz - 2 * g.gravity * dz;
            const float t = disc > 0 ? (g.launchVz + std::sqrt(disc)) / g.gravity : g.launchVz / g.gravity;
            Grenade n;
            n.pos = from;
            n.vel = vec3((vec2(throwAt_) - vec2(from)) / t, g.launchVz);
            grenades_.push_back(n);
            --grenades;
        }
        if (throwT_ >= dur) throwT_ = -1;
    }

    // Flight: gravity, then stick to the first target or surface hit; the fuse runs once stuck.
    for (auto& n : grenades_) {
        n.age += dt;
        if (n.stuck) {
            if (n.target) {
                if (std::find(targets.begin(), targets.end(), n.target) == targets.end()) n.target = nullptr; // gone: stays put
                else n.pos = n.target->position() + n.offset;
            }
            n.fuse -= dt;
            continue;
        }
        n.vel.z -= g.gravity * dt;
        const vec3 step = n.vel * dt;
        const float len = glm::length(step);
        float best = 1;
        Damageable* hitTarget = nullptr;
        if (len > 1e-4f)
            for (Damageable* t : targets)
                if (t->alive())
                    if (const auto d = rayCapsule(n.pos, step / len, t->position(), t->radius(), t->height(), len); d && *d / len < best)
                        best = *d / len, hitTarget = t;
        const auto hit = col_.sweepSphere(n.pos, kGrenadeRadius, step);
        if (hit && hit->t <= best) {
            n.pos += step * hit->t, n.stuck = true, n.fuse = g.fuse;
        } else if (hitTarget) {
            n.pos += step * best, n.stuck = true, n.fuse = g.fuse;
            n.target = hitTarget, n.offset = n.pos - hitTarget->position();
        } else {
            n.pos += step;
            if (n.age >= kMaxFlight) n.stuck = true, n.fuse = 0;
        }
    }
    for (size_t i = 0; i < grenades_.size();)
        if (grenades_[i].stuck && grenades_[i].fuse <= 0) {
            const vec3 at = grenades_[i].pos;
            grenades_.erase(grenades_.begin() + std::ptrdiff_t(i));
            explode(at);
        } else ++i;
}

// Area damage after 0x69F1E0: every target and Lara inside the radius, linear from the centre damage to the edge damage,
// no occlusion test [O]; the knock speed (centre -> edge) pushes physics bodies away [I as velocity].
void World::explode(vec3 at) {
    const GrenadeTuning& g = anims_.grenade;
    explosions_.push_back(at);
    const auto falloff = [&](float d) { return std::clamp(d / g.radius, 0.0f, 1.0f); };
    for (Damageable* t : std::vector<Damageable*>(targets)) { // onHit may unregister a target
        if (!t->alive()) continue;
        const vec3 p = t->aimPoint();
        const float d = glm::distance(at, p);
        if (d >= g.radius) continue;
        const vec3 dir = d > 1e-3f ? (p - at) / d : vec3(0, 0, 1);
        t->onHit({glm::mix(g.damage, g.edgeDamage, falloff(d)), p, dir, Team::Player});
    }
    const vec3 chest = player_.state().pos + vec3(0, 0, targeting.eyeHeight);
    if (const float d = glm::distance(at, chest); d < g.radius) damageLara(glm::mix(g.damage, g.edgeDamage, falloff(d)), at, kDamageHeavy);
    for (auto& b : physics_.bodies) {
        if (b.invMass <= 0) continue;
        const float d = glm::distance(at, b.pos);
        if (d >= g.radius) continue;
        b.vel += (d > 1e-3f ? (b.pos - at) / d : vec3(0, 0, 1)) * glm::mix(g.impulse, g.edgeImpulse, falloff(d));
    }
}

void World::throwLayer(std::vector<anim::LocalTransform>& local) const {
    if (throwT_ < 0) return;
    const anim::Clip* clip = anims_.find(kThrowClip);
    const auto& sk = anims_.skeleton;
    if (!clip || local.size() != sk.bones.size() || sk.bones.size() <= size_t(kSpineBone)) return;
    const double dur = clip->durationSeconds();
    const float w = std::clamp(float(std::min(double(throwT_), dur - throwT_)) / kLayerFade, 0.0f, 1.0f);
    const auto p = anim::sample(*clip, sk, throwT_, false);
    std::vector<char> upper(sk.bones.size(), 0); // spine subtree, as the weapon layer
    upper[size_t(kSpineBone)] = 1;
    for (size_t i = size_t(kSpineBone) + 1; i < sk.bones.size(); ++i)
        upper[i] = sk.bones[i].parent >= 0 && upper[size_t(sk.bones[i].parent)];
    for (size_t i = 0; i < local.size(); ++i)
        if (upper[i]) local[i] = anim::blend(local[i], p[i], w);
}

} // namespace ou::game
