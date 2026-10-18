// Enemy state machine, perception, root-motion movement with capsule collide-and-slide and ledge
// avoidance. Numbers and provenance: docs/GAMEPLAY.md "Enemies".
#include "game/ai/Enemy.hpp"

#include "game/Game.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>

namespace ou::game::ai {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kSkin = 1.0f;       // gap kept to surfaces after a sweep (as Player)
constexpr float kAlertSeconds = 0.4f; // [I] pause facing Lara before the chase starts
constexpr float kFaceToAttack = 0.5f; // rad: attack only roughly facing Lara [I]
constexpr float kBlowCone = 1.05f;    // rad: half angle a blow reaches [I]

float wrapAngle(float a) {
    a = std::fmod(a + kPi, 2 * kPi);
    return (a < 0 ? a + 2 * kPi : a) - kPi;
}
float lerpAngle(float a, float b, float t) { return a + wrapAngle(b - a) * t; }
float flatDist(vec3 a, vec3 b) { return glm::length(glm::vec2(b - a)); }
float bearing(vec3 from, vec3 to) { return std::atan2(to.y - from.y, to.x - from.x); }

} // namespace

const anim::Clip* EnemyType::clip(Role r) const {
    const auto it = clips.find(roles[size_t(r)]);
    return it == clips.end() ? nullptr : &it->second;
}

std::string_view modeName(Mode m) {
    static constexpr std::string_view k[] = {"idle", "alert", "chase", "attack", "hit", "dead", "spawning"};
    return size_t(m) < std::size(k) ? k[size_t(m)] : "unknown";
}

Enemy::Enemy(std::shared_ptr<const EnemyType> type, int32_t uid, vec3 pos, float yaw, bool spawning)
    : type_(std::move(type)), uid_(uid), anim_(type_->skeleton) {
    s_.pos = pos;
    s_.yaw = yaw;
    s_.health = type_->tuning.health;
    s_.lastSeen = pos;
    if (spawning && type_->clip(Role::Spawn)) {
        s_.mode = Mode::Spawning;
        play(Role::Spawn, false);
    } else {
        play(Role::Idle, true);
    }
    prev_ = s_;
    updatePose(0, true);
}

glm::mat4 Enemy::modelMatrix(vec3 pos, float yaw) {
    return glm::rotate(glm::translate(glm::mat4(1), pos), yaw + kPi / 2, vec3(0, 0, 1));
}

void Enemy::play(Role r, bool loop) {
    const std::string* name = &type_->clipName(r);
    if (name->empty()) name = &type_->clipName(r == Role::Walk ? Role::Run : r == Role::Run ? Role::Walk : Role::Idle);
    if (s_.clip == *name && s_.loop == loop && loop) return;
    s_.clip = *name;
    s_.loop = loop;
    s_.clipTime = 0;
}

bool Enemy::clipDone() const {
    const auto it = type_->clips.find(s_.clip);
    return it == type_->clips.end() || (!s_.loop && s_.clipTime >= it->second.durationSeconds());
}

vec3 Enemy::advanceClip(float dt) {
    const auto it = type_->clips.find(s_.clip);
    if (it == type_->clips.end()) return vec3(0);
    const double t0 = s_.clipTime;
    double t1 = t0 + dt;
    if (!s_.loop) t1 = std::min(t1, it->second.durationSeconds());
    s_.clipTime = t1;
    const vec3 d = rootTranslation(it->second, t1, s_.loop) - rootTranslation(it->second, t0, s_.loop);
    // model -> world by heading (model -Y = forward)
    const float c = std::cos(s_.yaw + kPi / 2), s = std::sin(s_.yaw + kPi / 2);
    return {c * d.x - s * d.y, s * d.x + c * d.y, d.z};
}

void Enemy::setMode(Mode m) {
    s_.mode = m;
    s_.modeTime = 0;
    switch (m) {
    case Mode::Idle: play(Role::Idle, true); break;
    case Mode::Alert: play(Role::Alert, true); break;
    case Mode::Chase: play(Role::Run, true); break;
    case Mode::Attack: s_.blowLanded = false; play(Role::Attack, false); break;
    case Mode::Hit: play(Role::Hit, false); break;
    case Mode::Dead: play(Role::Death, false); break;
    case Mode::Spawning: play(Role::Spawn, false); break;
    }
}

void Enemy::turnTowards(vec3 p, float dt) {
    if (flatDist(s_.pos, p) < 1) return;
    const float d = wrapAngle(bearing(s_.pos, p) - s_.yaw), step = type_->tuning.turnRate * dt;
    s_.yaw = wrapAngle(s_.yaw + std::clamp(d, -step, step));
}

bool Enemy::perceives(const collision::CollisionMesh& col, const Target& lara) const {
    const EnemyTuning& t = type_->tuning;
    const vec3 eye = s_.pos + vec3(0, 0, t.height * 0.8f), at = lara.pos + vec3(0, 0, lara.height * 0.75f);
    const float d = glm::length(at - eye);
    if (d > t.sightRange) return false;
    if (flatDist(s_.pos, lara.pos) > t.senseRange &&
        std::abs(wrapAngle(bearing(s_.pos, lara.pos) - s_.yaw)) > t.sightHalfAngle)
        return false;
    if (d < 1) return true;
    const auto hit = col.raycast(eye, (at - eye) / d, d);
    return !hit || hit->t >= d - 1.0f;
}

void Enemy::onHit(const Hit& hit) {
    if (!alive()) return;
    s_.health -= hit.damage;
    s_.lastSeen = s_.pos - hit.dir * 1000.0f; // [I] turns towards where the shot came from
    s_.unseenTime = 0;
    s_.damageSinceReaction += hit.damage;
    if (s_.health <= 0) {
        setMode(Mode::Dead);
    } else if (s_.damageSinceReaction >= type_->tuning.staggerDamage) {
        s_.damageSinceReaction = 0;
        setMode(Mode::Hit);
    } else if (s_.mode == Mode::Idle) {
        setMode(Mode::Alert); // shot from out of sight: turns to the shooter
    }
}

// Collide-and-slide of the body capsule (as Player::slide; walls push sideways only), ground snap within
// stepUp/stepDown, gravity off larger drops, and ledge avoidance: a step that would leave the enemy with
// no floor within maxDrop below is not taken.
void Enemy::move(const collision::CollisionMesh& col, vec3 delta, float dt) {
    const EnemyTuning& t = type_->tuning;
    const float r = t.radius, top = std::max(t.height - r, t.stepUp + r);
    vec3 p = s_.pos;
    delta.z = 0;
    for (int iter = 0; iter < 4 && glm::dot(delta, delta) > 1e-8f; ++iter) {
        const auto hit = col.sweepCapsule(p + vec3(0, 0, t.stepUp + r), p + vec3(0, 0, top), r, delta);
        vec3 n = hit ? hit->normal : vec3(0);
        n.z = 0;
        const float nl = glm::length(n);
        if (!hit || nl < 1e-4f || (hit->t == 0 && glm::dot(delta, n) >= 0)) {
            p += delta;
            break;
        }
        n /= nl;
        p += delta * hit->t + n * kSkin;
        const vec3 rest = delta * (1 - hit->t);
        delta = rest - n * std::min(0.0f, glm::dot(rest, n));
    }
    const glm::vec2 moved(p - s_.pos);
    if (glm::dot(moved, moved) > 1e-6f && s_.vz == 0) {
        const vec3 ahead = p + vec3(glm::normalize(moved) * r, 0);
        if (!col.groundProbe(ahead + vec3(0, 0, t.stepUp), t.stepUp + t.maxDrop)) p = vec3(s_.pos.x, s_.pos.y, p.z);
    }
    const float fall = std::max(0.0f, -(s_.vz * dt - 0.5f * t.gravity * dt * dt));
    const auto g = col.groundProbe(p + vec3(0, 0, t.stepUp), t.stepUp + std::max(t.stepDown, fall));
    if (s_.vz == 0 && g && s_.pos.z - g->point.z <= t.stepDown && g->normal.z >= t.minFloorNormalZ) {
        p.z = g->point.z;
    } else {
        // tru.exe integrator (as Player): pos += v t + a t^2 / 2; v += a t
        const float z = p.z + s_.vz * dt - 0.5f * t.gravity * dt * dt;
        s_.vz -= t.gravity * dt;
        if (g && z <= g->point.z) p.z = g->point.z, s_.vz = 0;
        else p.z = z;
    }
    s_.pos = p;
}

void Enemy::tick(const collision::CollisionMesh& col, const Target& lara, const HurtFn& hurt, float dt) {
    prev_ = s_;
    const EnemyTuning& t = type_->tuning;
    s_.modeTime += dt;
    s_.cooldown = std::max(0.0f, s_.cooldown - dt);
    const bool sees = s_.mode != Mode::Dead && lara.alive && perceives(col, lara);
    if (sees) s_.unseenTime = 0, s_.lastSeen = lara.pos;
    else s_.unseenTime += dt;
    const float dist = flatDist(s_.pos, lara.pos);
    vec3 delta = advanceClip(dt);

    switch (s_.mode) {
    case Mode::Spawning:
        if (clipDone()) setMode(Mode::Chase);
        break;
    case Mode::Idle:
        if (sees) setMode(Mode::Alert);
        break;
    case Mode::Alert:
        turnTowards(s_.lastSeen, dt);
        if (s_.modeTime >= kAlertSeconds) setMode(lara.alive ? Mode::Chase : Mode::Idle);
        break;
    case Mode::Chase: {
        turnTowards(s_.lastSeen, dt);
        const bool facing = std::abs(wrapAngle(bearing(s_.pos, lara.pos) - s_.yaw)) < kFaceToAttack;
        if (!lara.alive || s_.unseenTime > t.loseTime) {
            setMode(Mode::Idle);
        } else if (sees && dist <= t.attackRange && facing && s_.cooldown <= 0) {
            setMode(Mode::Attack);
        } else if (sees && dist <= t.attackRange) {
            play(Role::Alert, true); // in reach but turning / cooling down: stand
            delta = vec3(0);
        } else {
            play(Role::Run, true);
        }
        break;
    }
    case Mode::Attack: {
        const anim::Clip* c = type_->clip(Role::Attack);
        const double f = c ? s_.clipTime / c->durationSeconds() : 1.0;
        if (f < t.hitStart) turnTowards(lara.pos, dt);
        if (!s_.blowLanded && lara.alive && f >= t.hitStart && f <= t.hitEnd && dist <= t.attackReach &&
            std::abs(wrapAngle(bearing(s_.pos, lara.pos) - s_.yaw)) <= kBlowCone) {
            s_.blowLanded = true;
            if (hurt) hurt(t.attackDamage, s_.pos);
        }
        if (clipDone()) {
            s_.cooldown = t.attackCooldown;
            setMode(Mode::Chase);
        }
        break;
    }
    case Mode::Hit:
        if (clipDone()) setMode(Mode::Chase);
        break;
    case Mode::Dead: break;
    }
    // Root motion never carries the body into Lara's (both capsules keep apart).
    if (s_.mode != Mode::Dead && lara.alive) {
        const float minGap = t.radius + 100.0f; // Lara's capsule radius (game::Tuning::radius)
        const glm::vec2 to(lara.pos - s_.pos), d2(delta);
        const float along = glm::length(to) > 1e-3f ? glm::dot(d2, glm::normalize(to)) : 0;
        if (along > 0 && dist - along < minGap) delta -= vec3(glm::normalize(to) * std::min(along, along - (dist - minGap)), 0);
    }
    move(col, delta, dt);
    updatePose(dt, false);
}

EnemyState Enemy::interpolated(double alpha) const {
    EnemyState s = s_;
    const float a = float(alpha);
    s.pos = glm::mix(prev_.pos, s_.pos, a);
    s.yaw = lerpAngle(prev_.yaw, s_.yaw, a);
    return s;
}

void Enemy::updatePose(float dt, bool reset) {
    if (reset || bones_.empty()) {
        anim_ = anim::AnimController(type_->skeleton);
        anim_.play(type_->clips.count(s_.clip) ? &type_->clips.find(s_.clip)->second : nullptr, 0, s_.loop, 1, s_.clipTime);
    } else {
        anim_.update(dt);
        if (s_.clip != prev_.clip || s_.loop != prev_.loop || s_.clipTime < prev_.clipTime) {
            const auto it = type_->clips.find(s_.clip);
            anim_.play(it == type_->clips.end() ? nullptr : &it->second, 0.1f, s_.loop, 1, s_.clipTime); // [I] 3-frame fade (Lara's default)
        } else {
            anim_.setTime(s_.clipTime);
        }
    }
    auto local = anim_.pose();
    if (!local.empty()) local[0] = {{0, 0, 0}, {1, 1, 1}, glm::make_vec3(type_->skeleton.bones[0].pivot)}; // root motion is in pos
    auto m = anim::worldMatrices(type_->skeleton, local);
    prevBones_ = reset || bones_.empty() ? m : std::move(bones_);
    bones_ = std::move(m);
}

std::vector<glm::mat4> Enemy::pose(double alpha) const {
    // ponytail: per-element matrix lerp between ticks (60 Hz; slerp as World::pose if it ever shows)
    std::vector<glm::mat4> out(bones_.size());
    for (size_t i = 0; i < out.size(); ++i) out[i] = prevBones_[i] + (bones_[i] - prevBones_[i]) * float(alpha);
    return out;
}

// ---- Enemies ----------------------------------------------------------------------------------

Enemy& Enemies::add(std::shared_ptr<const EnemyType> type, int32_t uid, vec3 pos, float yaw, bool spawning) {
    enemies_.push_back(std::make_unique<Enemy>(std::move(type), uid, pos, yaw, spawning));
    ++version_;
    sync();
    return *enemies_.back();
}

void Enemies::sync() {
    if (targets_)
        std::erase_if(*targets_, [&](Damageable* d) { return std::find(registered_.begin(), registered_.end(), d) != registered_.end(); });
    registered_.clear();
    for (const auto& e : enemies_) registered_.push_back(e.get());
    if (targets_) targets_->insert(targets_->end(), registered_.begin(), registered_.end());
}

void Enemies::tick(const collision::CollisionMesh& col, const Target& lara, const HurtFn& hurt, float dt) {
    for (auto& g : generators_) {
        if (g.fired || flatDist(g.pos, lara.pos) > g.range || std::abs(g.pos.z - lara.pos.z) > 2000.0f) continue;
        g.fired = true;
        if (auto type = typeOf(g.object)) {
            auto& e = add(std::move(type), g.uid, g.pos, g.yaw, true);
            e.unit = g.unit;
            e.generator = g.uid;
        }
    }
    for (auto& e : enemies_) e->tick(col, lara, hurt, dt);
}

} // namespace ou::game::ai
