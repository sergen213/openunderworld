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
constexpr float kCount = 1.0f / 30.0f; // [I] s per record timer count (enemy clock unit not found; 30 Hz frames)
constexpr float kMissChance = 0.35f;  // [I] fraction of enemy shots aimed off Lara
constexpr float kShotRange = 12000.0f; // [I] a shot travels this far past nothing
constexpr float kTipSeconds = 0.5f;   // [I] a body whose death clip ends upright tips onto its side this fast

float wrapAngle(float a) {
    a = std::fmod(a + kPi, 2 * kPi);
    return (a < 0 ? a + 2 * kPi : a) - kPi;
}
float lerpAngle(float a, float b, float t) { return a + wrapAngle(b - a) * t; }
float flatDist(vec3 a, vec3 b) { return glm::length(glm::vec2(b - a)); }
float bearing(vec3 from, vec3 to) { return std::atan2(to.y - from.y, to.x - from.x); }
bool inCone(const Cone& c, vec3 eye, float yaw, vec3 at) {
    const vec3 d = at - eye;
    const float h = glm::length(glm::vec2(d));
    return glm::length(d) < c.range && std::abs(wrapAngle(std::atan2(d.y, d.x) - yaw)) <= c.yaw &&
           std::abs(std::atan2(d.z, h)) <= c.pitch;
}
// Ray (unit dir) against a vertical capsule's cylinder [z0, z1], radius r: entry distance or -1.
// ponytail: cylinder only (no end caps); Lara's 100-radius caps never decide a shot.
float rayCylinder(vec3 o, vec3 dir, vec3 base, float z1, float r) {
    const glm::vec2 p(o - base), d(dir);
    const float a = glm::dot(d, d), b = glm::dot(p, d), c = glm::dot(p, p) - r * r;
    if (a < 1e-8f || b * b - a * c < 0) return -1;
    const float t = (-b - std::sqrt(b * b - a * c)) / a;
    const float z = o.z + dir.z * t;
    return t >= 0 && z >= base.z && z <= z1 ? t : -1;
}
uint32_t lcg(uint32_t& s) { return s = s * 1664525u + 1013904223u; }
float frand(uint32_t& s) { return float(lcg(s) >> 8) / float(1 << 24); } // [0, 1)

} // namespace

const anim::Clip* EnemyType::clip(Role r) const {
    const auto it = clips.find(roles[size_t(r)]);
    return it == clips.end() ? nullptr : &it->second;
}

std::string_view modeName(Mode m) {
    static constexpr std::string_view k[] = {"idle", "alert", "chase", "attack", "hit", "dead", "spawning", "fire", "reload"};
    return size_t(m) < std::size(k) ? k[size_t(m)] : "unknown";
}

Enemy::Enemy(std::shared_ptr<const EnemyType> type, int32_t uid, vec3 pos, float yaw, bool spawning, int healthLevel)
    : type_(std::move(type)), uid_(uid), anim_(type_->skeleton) {
    s_.pos = pos;
    s_.yaw = yaw;
    s_.health = type_->tuning.healthAt(healthLevel);
    s_.lastSeen = pos;
    s_.rng = uint32_t(uid) * 2654435761u + 1u;
    if (spawning && type_->clip(Role::Spawn)) {
        s_.mode = Mode::Spawning;
        play(Role::Spawn, false);
    } else {
        play(Role::Idle, true);
    }
    prev_ = start_ = s_;
    updatePose(0, true);
}

glm::mat4 Enemy::modelMatrix(vec3 pos, float yaw, float tip) {
    const glm::mat4 m = glm::rotate(glm::translate(glm::mat4(1), pos), yaw + kPi / 2, vec3(0, 0, 1));
    return tip == 0 ? m : glm::rotate(m, tip, vec3(0, 1, 0)); // rolls about the model's forward axis, at the feet
}

void Enemy::play(Role r, bool loop) {
    const std::string* name = &type_->clipName(r);
    if (name->empty()) name = &type_->clipName(r == Role::Walk ? Role::Run : r == Role::Run ? Role::Walk : Role::Idle);
    playClip(*name, loop);
}

void Enemy::playClip(const std::string& name, bool loop) {
    if (s_.clip == name && s_.loop == loop && loop) return;
    s_.clip = name;
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
    case Mode::Fire: { // aim (combat idle) for the start delay; the shot clip starts with the first shot
        const Attack& a = type_->tuning.attacks[size_t(s_.fire)];
        s_.shotsLeft = a.shots;
        s_.shotTimer = float(a.startDelay) * kCount;
        play(Role::Alert, true);
        break;
    }
    case Mode::Reload: playClip(type_->tuning.attacks[size_t(s_.fire)].followClip, false); break;
    }
}

void Enemy::turnTowards(vec3 p, float dt) {
    if (flatDist(s_.pos, p) < 1) return;
    const float d = wrapAngle(bearing(s_.pos, p) - s_.yaw), step = type_->tuning.turnRate * dt;
    s_.yaw = wrapAngle(s_.yaw + std::clamp(d, -step, step));
}

float Enemy::aimError(vec3 p) const { return std::abs(wrapAngle(bearing(s_.pos, p) - s_.yaw)); }

bool Enemy::perceives(const collision::CollisionMesh& col, const Target& lara) const {
    // [I] sharks hunt a swimming Lara only: on a deck or shore she is out of their medium (l2_cliffclimb's three
    // sharks swim under the start boat; the original leaves them there).
    if (type_->aquatic() && !lara.inWater) return false;
    const Senses& se = senses();
    const vec3 eye = s_.pos + vec3(0, 0, type_->tuning.height * 0.8f), at = lara.pos + vec3(0, 0, lara.height * 0.75f);
    // [I] body cones measured from the eye too (the exe's 0x60DBA0 uses the body matrix)
    if (!inCone(se.eye, eye, s_.yaw, at) && !inCone(se.body[0], eye, s_.yaw, at) && !inCone(se.body[1], eye, s_.yaw, at)) return false;
    const float d = glm::length(at - eye);
    if (!se.ray || d < 1) return true;
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
        die(hit);
    } else if (s_.damageSinceReaction >= type_->tuning.staggerDamage) {
        s_.damageSinceReaction = 0;
        setMode(Mode::Hit);
    } else if (s_.mode == Mode::Idle) {
        setMode(Mode::Alert); // shot from out of sight: turns to the shooter
    }
}

// Death clip as FUN_00642210: the attacker's quadrant relative to the heading (1 front ±45°, 3 left, 5 back, 7
// right) picks a record entry (category, flag); not found -> front/back by ±90° -> front. The heading then turns so
// the clip falls away from the attacker. Flag [I]: 3 on a head shot (death_adren / death_placed_shot), 5 when hit
// while running (death_running), else 0. ponytail: first match, not the exe's random pick among equals. Nothing
// usable (type-2 refs): the role death clip.
void Enemy::die(const Hit& hit) {
    const bool running = s_.clip == type_->clipName(Role::Run) && s_.mode == Mode::Chase;
    setMode(Mode::Dead);
    const float a = std::atan2(-hit.dir.y, -hit.dir.x), rel = wrapAngle(a - s_.yaw); // bearing to the attacker
    const int quadrant = std::abs(rel) <= kPi / 4 ? 1 : std::abs(rel) > 3 * kPi / 4 ? 5 : rel > 0 ? 3 : 7;
    const auto find = [&](int cat, int flag) -> const DeathClip* {
        for (const auto& d : type_->tuning.deaths)
            if (d.category == cat && d.flag == flag && type_->clips.count(d.clip)) return &d;
        return nullptr;
    };
    for (const int flag : {hit.headshot ? 3 : running ? 5 : 0, 0})
        for (const int cat : {quadrant, std::abs(rel) > kPi / 2 ? 5 : 1, 1})
            if (const DeathClip* d = find(cat, flag)) {
                playClip(d->clip, false);
                if (cat != 1) s_.yaw = wrapAngle(a + (cat == 3 ? -kPi / 2 : cat == 5 ? kPi : kPi / 2));
                return;
            }
}

int Enemy::readyFire() const {
    const auto& as = type_->tuning.attacks;
    for (size_t i = 0; i < as.size() && i < s_.fireCooldown.size(); ++i)
        if (as[i].ranged() && type_->clips.count(as[i].shotClip) && s_.fireCooldown[i] <= 0) return int(i);
    return -1;
}

// One shot of the current fire record: a ray from the muzzle ([I] eye height, one radius ahead) at Lara's chest,
// [I] kMissChance of them aimed 1.5..3 of her radii aside; it stops at the first wall (line of fire) or Lara's
// capsule, which takes rec+0x154.
void Enemy::shoot(const collision::CollisionMesh& col, const Target& lara, const HurtFn& hurt, std::vector<Shot>* shots) {
    const Attack& a = type_->tuning.attacks[size_t(s_.fire)];
    const EnemyTuning& t = type_->tuning;
    const vec3 from = s_.pos + vec3(0, 0, t.height * 0.8f) + vec3(std::cos(s_.yaw), std::sin(s_.yaw), 0) * t.radius;
    const vec3 target = lara.alive ? lara.pos : s_.lastSeen;
    vec3 aim = target + vec3(0, 0, lara.height * 0.6f);
    if (frand(s_.rng) < kMissChance) {
        vec3 side = glm::cross(aim - from, vec3(0, 0, 1));
        side = glm::length(side) > 1e-3f ? glm::normalize(side) : vec3(1, 0, 0);
        const float off = lara.radius * (1.5f + 1.5f * frand(s_.rng));
        aim += side * (frand(s_.rng) < 0.5f ? off : -off) + vec3(0, 0, (frand(s_.rng) - 0.5f) * lara.radius);
    }
    const vec3 dir = glm::normalize(aim - from);
    float len = kShotRange;
    if (const auto w = col.raycast(from, dir, len)) len = w->t;
    const float tl = lara.alive ? rayCylinder(from, dir, lara.pos, lara.pos.z + lara.height, lara.radius) : -1;
    const bool hit = tl >= 0 && tl < len;
    if (hit) {
        len = tl;
        if (hurt) hurt(a.damage, from, a.type == 2 ? kDamageHeavy : kDamageLight);
    }
    if (shots) shots->push_back({uid_, from, from + dir * len, hit});
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
    auto g = col.groundProbe(p + vec3(0, 0, t.stepUp), t.stepUp + std::max(t.stepDown, fall));
    // [I] a floor inside the lower half of a standing body lifts it (intros sunk below their floor: ballcourt
    // poacher 6541 stands 96 under it) instead of dropping it through
    if (!g && s_.vz == 0) g = col.groundProbe(p + vec3(0, 0, t.height * 0.5f), t.height * 0.5f + t.stepDown);
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

// Flyers: a sphere (radius, at half height) slides through the level in 3D; no gravity while alive.
void Enemy::fly(const collision::CollisionMesh& col, vec3 delta) {
    const float r = type_->tuning.radius, half = type_->tuning.height * 0.5f;
    vec3 c = s_.pos + vec3(0, 0, half);
    const vec3 from = c;
    for (int iter = 0; iter < 4 && glm::dot(delta, delta) > 1e-8f; ++iter) {
        const auto hit = col.sweepSphere(c, r, delta);
        if (!hit || (hit->t == 0 && glm::dot(delta, hit->normal) >= 0)) {
            c += delta;
            break;
        }
        c += delta * hit->t + hit->normal * kSkin;
        const vec3 rest = delta * (1 - hit->t);
        delta = rest - hit->normal * std::min(0.0f, glm::dot(rest, hit->normal));
    }
    if (type_->aquatic())
        for (const auto& water : col.markup) {
            if (water.type != 10 || water.points.size() < 2) continue;
            const vec3 lo = glm::min(water.points[0], water.points[1]), hi = glm::max(water.points[0], water.points[1]);
            if (glm::any(glm::lessThan(from, lo)) || glm::any(glm::greaterThan(from, hi))) continue;
            for (int axis = 0; axis < 3; ++axis)
                c[axis] = hi[axis] - lo[axis] < 2 * r ? (lo[axis] + hi[axis]) * 0.5f
                                                      : std::clamp(c[axis], lo[axis] + r, hi[axis] - r);
            break;
        }
    s_.pos = c - vec3(0, 0, half);
}

void Enemy::tick(const collision::CollisionMesh& col, const Target& lara, const HurtFn& hurt, float dt, std::vector<Shot>* shots) {
    prev_ = s_;
    const EnemyTuning& t = type_->tuning;
    const bool flyer = type_->flyer();
    const bool aquatic = type_->aquatic();
    s_.modeTime += dt;
    s_.cooldown = std::max(0.0f, s_.cooldown - dt);
    for (float& c : s_.fireCooldown) c = std::max(0.0f, c - dt);
    const bool sees = s_.mode != Mode::Dead && lara.alive && perceives(col, lara);
    if (sees) {
        s_.unseenTime = 0, s_.lastSeen = lara.pos;
    } else {
        s_.unseenTime += dt;
        if (s_.mode != Mode::Idle && s_.mode != Mode::Dead && lara.alive && (lara.inWater || !aquatic) && glm::distance(s_.pos, lara.pos) < senses().loseTrack)
            s_.lastSeen = lara.pos; // [I] senses +0x20: still tracked this close, unseen
    }
    // Ground enemies measure Lara flat, centre to centre; flyers from their centre to her head.
    const vec3 head = lara.pos + vec3(0, 0, lara.height * 0.85f), centre = s_.pos + vec3(0, 0, t.height * 0.5f);
    const float dist = flyer ? glm::distance(centre, head) : flatDist(s_.pos, lara.pos);
    vec3 delta = advanceClip(dt);
    vec3 flight = aquatic ? delta : vec3(0); // aquatic clips keep their 3D root motion; bats hover except when chasing

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
        const bool facing = aimError(lara.pos) < kFaceToAttack;
        const int fire = type_->ranged() ? readyFire() : -1;
        if (!lara.alive || s_.unseenTime > t.loseTime) {
            setMode(Mode::Idle);
        } else if (sees && dist <= t.attackRange && facing && s_.cooldown <= 0 && type_->clip(Role::Attack)) {
            setMode(Mode::Attack);
        } else if (sees && (dist <= t.attackRange || (type_->ranged() && dist <= senses().eye.range))) {
            play(Role::Alert, true); // in reach, or in rifle range with a clear line: stand (and aim)
            delta = vec3(0);
            flight = vec3(0);
            if (fire >= 0 && dist > t.attackRange && aimError(lara.pos) <= t.attacks[size_t(fire)].maxYaw) {
                s_.fire = fire;
                setMode(Mode::Fire);
            }
        } else if (!sees && flatDist(s_.pos, s_.lastSeen) < t.radius) {
            play(Role::Alert, true); // at the last seen spot: waits
            delta = vec3(0);
            flight = vec3(0);
        } else {
            play(Role::Run, true);
            if (flyer && dist > 1) {
                const vec3 to = (sees ? head : s_.lastSeen + vec3(0, 0, lara.height * 0.85f)) - centre;
                const float l = glm::length(to), go = std::min(t.flySpeed * dt, l - t.attackRange * 0.8f);
                if (l > 1 && go > 0) flight = to / l * go;
            }
        }
        break;
    }
    case Mode::Attack: {
        const anim::Clip* c = type_->clip(Role::Attack);
        const double f = c ? s_.clipTime / c->durationSeconds() : 1.0;
        if (f < t.hitStart) turnTowards(lara.pos, dt);
        if (!s_.blowLanded && lara.alive && f >= t.hitStart && f <= t.hitEnd && dist <= t.attackReach && aimError(lara.pos) <= kBlowCone) {
            s_.blowLanded = true;
            if (hurt) hurt(t.attackDamage, s_.pos, t.attackType);
        }
        if (clipDone()) {
            s_.cooldown = t.attackCooldown;
            setMode(Mode::Chase);
        }
        break;
    }
    case Mode::Fire: {
        const Attack& a = t.attacks[size_t(s_.fire)];
        turnTowards(s_.lastSeen, dt);
        if (s_.shotsLeft == a.shots && !sees) { // lost her before the first shot
            setMode(Mode::Chase);
            break;
        }
        s_.shotTimer -= dt;
        for (; s_.shotsLeft > 0 && s_.shotTimer <= 0; --s_.shotsLeft, s_.shotTimer += float(a.gap) * kCount) {
            if (s_.shotsLeft == a.shots) playClip(a.shotClip, false); // ponytail: one burst clip, the shots spaced by the gap
            shoot(col, lara, hurt, shots);
        }
        if (s_.shotsLeft == 0 && clipDone()) {
            s_.fireCooldown[size_t(s_.fire)] = a.cooldown;
            setMode(type_->clips.count(a.followClip) ? Mode::Reload : Mode::Chase);
        }
        break;
    }
    case Mode::Reload:
        if (clipDone()) setMode(Mode::Chase);
        break;
    case Mode::Hit:
        if (clipDone()) setMode(Mode::Chase);
        break;
    case Mode::Dead:
        if (aquatic && clipDone() && s_.clip != "death_float_loop" && type_->clips.count("death_float_loop"))
            playClip("death_float_loop", true);
        if (clipDone() && !flyer && !type_->lyingDeaths.count(s_.clip)) s_.tip = std::min(kPi / 2, s_.tip + dt * kPi / 2 / kTipSeconds);
        break;
    }
    if (flyer && (s_.mode != Mode::Dead || aquatic)) {
        fly(col, flight); // bats hover unless chasing; sharks keep clip motion and never use ground gravity
        updatePose(dt, false);
        return;
    }
    // Root motion never carries the body into Lara's (both capsules keep apart).
    if (s_.mode != Mode::Dead && lara.alive) {
        const float minGap = t.radius + lara.radius;
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
    s.tip = glm::mix(prev_.tip, s_.tip, a);
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
    enemies_.push_back(std::make_unique<Enemy>(std::move(type), uid, pos, yaw, spawning, healthLevel_));
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

void Enemies::checkpoint() {
    deadAtCheckpoint_.clear();
    for (const auto& e : enemies_)
        if (!e->alive()) deadAtCheckpoint_.insert(e->uid());
    for (const auto& [uid, s] : stash_)
        if (s.health <= 0) deadAtCheckpoint_.insert(uid);
}

void Enemies::reset() {
    const auto keep = [&](int32_t uid) { return deadAtCheckpoint_.contains(uid); };
    std::erase_if(enemies_, [&](const auto& e) { return e->generator && !keep(e->uid()); });
    for (auto& e : enemies_)
        if (!keep(e->uid())) e->restore(e->start());
    for (auto& g : generators_) g.fired = g.fired && keep(g.uid);
    std::erase_if(firedGenerators_, [&](int32_t uid) { return !keep(uid); });
    std::erase_if(stash_, [&](const auto& kv) { return !keep(kv.first); }); // streamed back in from their intros
    ++version_;
    sync();
}

// tru.exe 0x781510: an enabled generator spawns on its own; ours when Lara is within range [I].
void Enemies::tick(const collision::CollisionMesh& col, const Target& lara, const HurtFn& hurt, float dt) {
    for (auto& g : generators_)
        if (!g.fired && g.enabled && flatDist(g.pos, lara.pos) <= g.range && std::abs(g.pos.z - lara.pos.z) <= 2000.0f)
            spawn(g);
    shots.clear();
    for (auto& e : enemies_) e->tick(col, lara, hurt, dt, &shots);
}

void Enemies::spawn(Generator& g) {
    g.fired = true;
    if (auto type = typeOf(g.object)) {
        auto& e = add(std::move(type), g.uid, g.pos, g.yaw, true);
        e.unit = g.unit;
        e.generator = g.uid;
    }
}

// 1/2 Spawn (0x780EB0; ours: once per generator), 3 Enable, 4 Disable (flag 0x84 bit 0, read by the update
// 0x781510), 5 kill every spawn (0x779350 flags them 0x20), 6 spawn count (+0x70).
bool Enemies::generatorNative(int32_t uid, uint32_t function, uint32_t& result) {
    const auto g = std::find_if(generators_.begin(), generators_.end(), [&](const Generator& x) { return x.uid == uid; });
    if (g == generators_.end()) return false;
    const auto spawned = [&](const auto& e) { return e->generator == uid && e->alive(); };
    switch (function) {
    case 1: case 2: if (!g->fired) spawn(*g); return true;
    case 3: g->enabled = true; return true;
    case 4: g->enabled = false; return true;
    case 5:
        for (auto& e : enemies_)
            if (spawned(e)) e->onHit({e->health() + 1, e->position(), {0, 0, -1}});
        return true;
    case 6: result = uint32_t(std::count_if(enemies_.begin(), enemies_.end(), spawned)); return true;
    default: return false;
    }
}

bool Enemies::damage(int32_t uid, float amount) {
    for (auto& e : enemies_)
        if (e->uid() == uid) {
            e->onHit({amount, e->position(), {0, 0, -1}});
            return true;
        }
    return false;
}

} // namespace ou::game::ai
