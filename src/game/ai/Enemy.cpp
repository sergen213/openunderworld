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
constexpr float kSleepSeconds = 20.0f; // [I] sleep_loop time (EnemyNapData +4 not tied to the cats' data)
constexpr float kFaceToAttack = 0.5f; // rad: attack only roughly facing Lara [I]
constexpr float kBlowCone = 1.05f;    // rad: half angle a blow reaches [I]
constexpr float kCount = 1.0f / 30.0f; // [I] s per record timer count (enemy clock unit not found; 30 Hz frames)
constexpr float kMissChance = 0.35f;  // [I] fraction of enemy shots aimed off Lara
constexpr float kShotRange = 12000.0f; // [I] a shot travels this far past nothing
constexpr float kTipSeconds = 0.5f;   // [I] a body whose death clip ends upright tips onto its side this fast
constexpr float kClingUpZ = 0.5f;     // [I] crawler intros whose up axis is flatter than 60° from vertical cling
constexpr float kSpitRange = 2528.45f; // [I] a spit reaches as far as the lose-track distance (8 m)
constexpr float kZoneHeight = 320.0f;  // [O] 0x61F810: a zone holds points within 320 along the up axis
constexpr float kZoneClamp = 0.8f;     // [O] 0x61F990 callers: goals pulled to 0.8 x the outer radius
constexpr float kCoverReach = 1500.0f; // [I] cover nodes this close (flat) are considered
constexpr float kCoverLow = 150.0f;    // [I] crouched eye height: its ray to Lara must hit the cover
constexpr float kCoverSeconds = 2.5f;  // [I] crouched behind cover before standing up to fire
constexpr float kCoverRest = 6.0f;     // [I] s before seeking cover again
constexpr float kCoverRunSeconds = 4.0f; // [I] gives up a cover it has not reached by then
constexpr float kPatrolGiveUp = 20.0f;   // [I] s walking to a patrol point before taking the next one
constexpr float kSearchWalkSeconds = 8.0f; // [I] s walking to a search position before idling there anyway
constexpr int kSearchTries = 4;          // [I] random possible positions tried per step
constexpr float kFlankRest = 600 * kCount; // [I] s between flank tries (0x62D930 re-evaluates its side 600 counts on)
constexpr float kFlankRunSeconds = 8.0f;   // [I] gives up a flank node it has not reached by then
constexpr float kInvestigateSeconds = 40.0f; // [I] gives up walking to a disturbance by then
constexpr float kLinkTakeoff = 60.0f;  // [I] a link starts within its radius + this (flat) of the take-off (ledge avoidance stops it a radius short)
constexpr float kLinkLevel = 158.0f;   // [I] a link whose ends differ less in height is a jump across
constexpr float kLinkHop = 158.0f;     // [I] the flight's top above the higher end

glm::mat3 tiltMatrix(glm::vec2 tilt) { // Rx(x) * Ry(y), as formats::introMatrix before the heading
    return glm::mat3(glm::rotate(glm::rotate(glm::mat4(1), tilt.x, vec3(1, 0, 0)), tilt.y, vec3(0, 1, 0)));
}

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
    static constexpr std::string_view k[] = {"idle", "alert", "chase", "attack", "hit", "dead", "spawning", "fire", "reload", "sleep",
                                             "cling", "drop", "down", "rise", "return", "cover", "crawl", "patrol", "search",
                                             "flank", "dodge", "investigate"};
    return size_t(m) < std::size(k) ? k[size_t(m)] : "unknown";
}

Enemy::Enemy(std::shared_ptr<const EnemyType> type, int32_t uid, vec3 pos, float yaw, bool spawning, int healthLevel, glm::vec2 tilt)
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
        if (type_->crawler() && (tiltMatrix(tilt) * vec3(0, 0, 1)).z < kClingUpZ) s_.mode = Mode::Cling, s_.tilt = tilt;
    }
    prev_ = start_ = s_;
    updatePose(0, true);
}

glm::mat4 Enemy::modelMatrix(vec3 pos, float yaw, float tip, glm::vec2 tilt) {
    const glm::mat4 m = glm::rotate(glm::translate(glm::mat4(1), pos) * glm::mat4(tiltMatrix(tilt)), yaw + kPi / 2, vec3(0, 0, 1));
    return tip == 0 ? m : glm::rotate(m, tip, vec3(0, 1, 0)); // rolls about the model's forward axis, at the feet
}

void Enemy::play(Role r, bool loop) {
    const std::string* name = &type_->clipName(r);
    if (name->empty()) name = &type_->clipName(r == Role::Walk ? Role::Run : r == Role::Run ? Role::Walk : Role::Idle);
    playClip(*name, loop);
}

void Enemy::playClip(const std::string& name, bool loop) {
    if (s_.clip == name && s_.loop == loop && loop) return;
    ++clipSerial_; // updatePose restarts the controller (onHit changes clips between ticks, after prev_ was taken)
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
    if (m == Mode::Dead || m == Mode::Down || m == Mode::Sleep) s_.leap = 0; // falls from wherever it is
    s_.mode = m;
    s_.modeTime = 0;
    switch (m) {
    case Mode::Idle: play(Role::Idle, true); break;
    case Mode::Alert: play(Role::Alert, true); break;
    case Mode::Chase: play(Role::Run, true); break;
    case Mode::Attack:
        s_.blowLanded = false;
        if (s_.melee >= 0) playClip(type_->tuning.attacks[size_t(s_.melee)].clip, false);
        else play(Role::Attack, false);
        break;
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
    case Mode::Sleep: s_.sleep = kSleepSeconds, s_.drowsy = 0, playClip(type_->sleepClips[0], false); break;
    case Mode::Cling: play(Role::Idle, true); break;
    case Mode::Drop: // lets go of the wall: off it by a radius, upright, falling
        s_.pos += up() * type_->tuning.radius;
        s_.tilt = glm::vec2(0), s_.vz = 0;
        if (!type_->fallClip.empty()) playClip(type_->fallClip, true);
        break;
    case Mode::Down: { // 0x67ED20: full health at once, the fall clip, then lies for the record's time
        const Regenerate& r = type_->tuning.regenerate;
        s_.health = start_.health, s_.damageSinceReaction = 0, s_.down = r.time;
        if (r.clip.empty()) play(Role::Death, false);
        else playClip(r.clip, false);
        break;
    }
    case Mode::Rise: s_.unseenTime = 0, playClip(type_->tuning.regenerate.up, false); break; // hunts on afterwards
    case Mode::Return: play(Role::Walk, true); break;
    case Mode::Cover: play(Role::Run, true); break;
    case Mode::Crawl: play(Role::Run, true); break;
    case Mode::Patrol: s_.phase = 0, s_.phaseTime = 0; break; // walks on in tick (PatrolMove)
    case Mode::Search: startSearch(); break;
    case Mode::Flank: // MoveNav with the flank set's clip (soldiers' is an anim request: the run clip)
        if (type_->clips.count(type_->flank.run)) playClip(type_->flank.run, true);
        else play(Role::Run, true);
        break;
    case Mode::Dodge: break; // dodge() plays the side's clip
    case Mode::Investigate: // InvestigateStart: the record's +0x0C clip, then InvestigateMove
        s_.phase = 1, s_.phaseTime = 0;
        if (!type_->investigate.start.empty()) s_.phase = 0, playClip(type_->investigate.start, false);
        break;
    }
}

bool GuardZone::contains(vec3 p, bool outerRadius) const {
    return std::abs(p.z - point.z) < kZoneHeight && flatDist(p, point) < (outerRadius ? outer : inner);
}

vec3 Enemy::zoneGoal(vec3 p) const {
    if (zones.empty()) return p;
    const GuardZone& z = zones[0];
    const vec3 d = p - z.point;
    const float l = glm::length(d);
    return l > z.outer ? z.point + d * (z.outer * kZoneClamp / l) : p;
}

bool Enemy::inZone(bool outerRadius) const {
    return zones.empty() || std::any_of(zones.begin(), zones.end(), [&](const GuardZone& z) { return z.contains(s_.pos, outerRadius); });
}

// [I] the spotter barks (combat_bark_01 / _02; the line itself: Enemies::barks); squad mates alerted by it do not.
void Enemy::notice() {
    setMode(Mode::Alert);
    const char* bark = (lcg(s_.rng) >> 16) & 1 ? "combat_bark_02" : "combat_bark_01";
    if (type_->clips.count(bark)) playClip(bark, false);
}

static bool calm(Mode m) { return m == Mode::Idle || m == Mode::Return || m == Mode::Patrol || m == Mode::Search || m == Mode::Investigate; }

void Enemy::alert(vec3 at) {
    if (!calm(s_.mode)) return;
    s_.lastSeen = at, s_.unseenTime = 0;
    setMode(Mode::Alert);
}

// 0x61DD10 (message 0xBF): a disturbance more than 1580 from the one it investigates (0x611BA0) restarts it; the
// behaviour runs when its time is within 30 counts of the message (0x61DA60) [I: at once].
void Enemy::hear(vec3 at) {
    const InvestigateStyle& iv = type_->investigate;
    if (!alive() || iv.walk.empty() || !calm(s_.mode) || flatDist(s_.pos, at) > iv.hear) return;
    if (s_.mode == Mode::Investigate && glm::distance(s_.goal, at) <= 1580.25f) return;
    s_.goal = at;
    setMode(Mode::Investigate);
}

const std::vector<PatrolPoint>* Enemy::patrolPoints() const {
    return s_.path >= 0 && size_t(s_.path) < patrolPaths.size() && !patrolPaths[size_t(s_.path)].empty() ? &patrolPaths[size_t(s_.path)] : nullptr;
}

// 0x63AC40 -> 0x63A410: the first point (modes 3 / 4: 0x637870 mode 3, a random free one). Also the placement state,
// so a respawn patrols again [I].
void Enemy::setPatrol(int path) {
    s_.path = path, s_.patrolIndex = -1, s_.patrolStep = 1, s_.visited = 0;
    if (patrolPoints()) {
        if (patrolMode == 3 || patrolMode == 4) nextPatrolPoint(3);
        else s_.patrolIndex = 0;
    }
    if (s_.mode == Mode::Patrol) setMode(Mode::Idle);
    start_.path = s_.path, start_.patrolIndex = s_.patrolIndex, start_.patrolStep = s_.patrolStep, start_.visited = s_.visited;
}

// 0x637870 by the path's order mode: 0 loop, 1 once (then done), 2 ping-pong (a one-point path is done), 3 a random
// other point no other enemy has claimed (0x633F10; ours: one another enemy walks to or idles at), 4 a random point
// not visited this round ([I] done when all are).
bool Enemy::nextPatrolPoint(uint32_t mode) {
    const auto* p = patrolPoints();
    const int n = p ? int(p->size()) : 0, i = s_.patrolIndex;
    s_.patrolIndex = -1;
    if (!n) return false;
    const auto pick = [&](auto&& ok) {
        std::vector<int> c;
        for (int k = 0; k < n; ++k)
            if (ok(k)) c.push_back(k);
        return c.empty() ? -1 : c[lcg(s_.rng) % c.size()];
    };
    switch (mode) {
    case 0: s_.patrolIndex = (i + 1) % n; break;
    case 1: s_.patrolIndex = i < n - 1 ? i + 1 : -1; break;
    case 2:
        if (i == n - 1) s_.patrolStep = -1;
        else if (i == 0) s_.patrolStep = 1;
        s_.patrolIndex = i + s_.patrolStep;
        break;
    case 3:
        s_.patrolIndex = n < 2 ? -1 : pick([&](int k) {
            return k != i && std::none_of(claimed.begin(), claimed.end(), [&](vec3 c) { return glm::distance(c, (*p)[size_t(k)].pos) < 1.0f; });
        });
        break;
    case 4:
        if (i >= 0 && i < 32) s_.visited |= 1u << i;
        s_.patrolIndex = pick([&](int k) { return k >= 32 || !(s_.visited >> k & 1); });
        break;
    default: break;
    }
    return s_.patrolIndex >= 0;
}

void Enemy::playIdle(const std::vector<IdleAnim>& idles) {
    uint32_t total = 0;
    for (const auto& a : idles) total += a.weight;
    s_.phaseLength = 0;
    if (!total) return play(Role::Idle, true);
    uint32_t r = lcg(s_.rng) % total;
    for (const auto& a : idles) {
        if (r >= a.weight) {
            r -= a.weight;
            continue;
        }
        playClip(a.clip, true);
        s_.phaseLength = a.minTime + (a.maxTime - a.minTime) * frand(s_.rng);
        return;
    }
}

const EnemyType::LinkClips* EnemyType::linkClips(vec3 from, vec3 to) const {
    const float dz = to.z - from.z; // 0x647CA0: a link segment rising is kind 1, else kind 2
    const LinkClips* c = std::abs(dz) <= kLinkLevel && jumpAcross.valid() ? &jumpAcross : dz > 0 ? &jumpUp : &jumpDown;
    return c->valid() ? c : nullptr;
}

vec3 Enemy::steer(vec3 goal) {
    if (!nav) return goal;
    const bool jumps = type_->jumpUp.valid() || type_->jumpDown.valid() || type_->jumpAcross.valid();
    const NavStep step = nav->steer(s_.pos, goal, jumps ? type_->linkUsage : uint16_t(0));
    if (step.link < 0 || flatDist(s_.pos, step.at) > type_->tuning.radius + kLinkTakeoff || !type_->linkClips(step.at, step.land)) return step.at;
    takeoff_ = step, takeoffMode_ = s_.mode;
    return step.land;
}

// [I] NavPower hands the link to the game, which runs it as a jump (the path's link segments keep no length for
// walking, 0x649F50): faces the landing, the kind's take-off clip in place, a ballistic flight (the agent's gravity,
// the top kLinkHop above the higher end) with the in-air loop, the landing clip; then the mode carries on.
void Enemy::startLeap(const NavStep& step) {
    const auto* c = type_->linkClips(step.at, step.land);
    s_.leap = 1, s_.leapFrom = s_.pos, s_.leapTo = step.land, s_.leapTime = 0, s_.vz = 0;
    s_.yaw = std::atan2(step.land.y - s_.pos.y, step.land.x - s_.pos.x);
    if (!c->start.empty()) playClip(c->start, false);
    else s_.leap = 2, playClip(c->air, true);
}

bool Enemy::leapTick(float dt) {
    if (!s_.leap) return false;
    const auto* c = type_->linkClips(s_.leapFrom, s_.leapTo);
    advanceClip(dt);
    const float g = type_->tuning.gravity;
    const auto flight = [&] { // from where it stands now
        s_.leapFrom = s_.pos, s_.leapTime = 0;
        const float top = std::max(s_.pos.z, s_.leapTo.z) + kLinkHop;
        s_.leapLength = std::sqrt(2 * (top - s_.pos.z) / g) + std::sqrt(2 * (top - s_.leapTo.z) / g);
        s_.leap = 2, playClip(c->air, true);
    };
    if (!c) { // the type changed under a save: land at once
        s_.pos = s_.leapTo, s_.leap = 0;
    } else if (s_.leap == 1) {
        if (clipDone()) flight();
    } else if (s_.leap == 2) {
        s_.leapTime += dt;
        const float u = std::min(1.0f, s_.leapTime / s_.leapLength), t = u * s_.leapLength;
        const float top = std::max(s_.leapFrom.z, s_.leapTo.z) + kLinkHop, v0 = std::sqrt(2 * g * (top - s_.leapFrom.z));
        s_.pos = vec3(glm::mix(glm::vec2(s_.leapFrom), glm::vec2(s_.leapTo), u), s_.leapFrom.z + v0 * t - 0.5f * g * t * t);
        if (u >= 1) {
            s_.pos = s_.leapTo;
            if (!c->land.empty()) s_.leap = 3, playClip(c->land, false);
            else s_.leap = 0;
        }
    } else if (clipDone()) {
        s_.leap = 0;
    }
    return true;
}

bool Enemy::walkTo(vec3 goal, float arrive, const std::string& clip, float dt, vec3* flight) {
    if (type_->flyer()) { // [I] straight through the air / water at half the chase speed
        const vec3 to = goal - s_.pos;
        const float d = glm::length(to);
        if (d <= arrive) return true;
        if (clip.empty()) play(Role::Walk, true);
        else playClip(clip, true);
        turnTowards(goal, dt);
        if (flight) *flight = to / d * std::min(d, type_->tuning.flySpeed * 0.5f * dt);
        return false;
    }
    if (flatDist(s_.pos, goal) <= arrive) return true;
    if (clip.empty()) play(Role::Walk, true);
    else playClip(clip, true);
    turnTowards(steer(goal), dt);
    return false;
}

// SearchStart (0x61E8F0: the record's +0x04 clip once), then SearchToLastKnownPos.
void Enemy::startSearch() {
    s_.phase = 0, s_.phaseTime = 0, s_.searchTime = 0, s_.goal = s_.lastSeen;
    if (type_->clips.count(type_->search.start)) playClip(type_->search.start, false);
    else s_.phase = 1;
}

// [I] EnemyBehavior_Cover picks the AI's current planner node +0x4E68 with cover flags (+0x84 bits 6 / 12..14) that
// tru.exe derives from 14 probes of 200 around each node (0x669D00); ours: the nearest planner node within reach (and
// the guard zone), not towards her, reachable in a straight run, where a crouched body's view of Lara is blocked. It stands up after a while and
// hunts on (a tall cover: it steps out, as AtCoverStepOutOfCover).
bool Enemy::seekCover(const collision::CollisionMesh& col, const Target& lara) {
    if (s_.coverCooldown > 0 || nodes.empty() || !type_->clips.count("combat_idle_crouch")) return false;
    const vec3 chest = lara.pos + vec3(0, 0, lara.height * 0.6f);
    const auto clear = [&](vec3 from) {
        const float d = glm::length(chest - from);
        const auto hit = d < 1 ? std::nullopt : col.raycast(from, (chest - from) / d, d);
        return !hit || hit->t >= d - 1.0f;
    };
    std::optional<vec3> best;
    float bestDist = kCoverReach;
    for (const auto& node : nodes) {
        const vec3 n = node.pos();
        const float d = flatDist(s_.pos, n);
        if (d >= bestDist || d < type_->tuning.radius || std::abs(n.z - s_.pos.z) >= kZoneHeight) continue;
        if (!zones.empty() && !std::any_of(zones.begin(), zones.end(), [&](const GuardZone& z) { return z.contains(n, true); })) continue;
        if (flatDist(n, lara.pos) < flatDist(s_.pos, lara.pos) * 0.5f) continue; // not towards her
        if (clear(n + vec3(0, 0, kCoverLow))) continue;
        const vec3 knee = s_.pos + vec3(0, 0, type_->tuning.stepUp + type_->tuning.radius), to = n + vec3(0, 0, type_->tuning.stepUp + type_->tuning.radius);
        if (col.raycast(knee, glm::normalize(to - knee), glm::distance(knee, to))) continue; // [I] a straight run there
        best = n, bestDist = d;
    }
    if (!best) return false;
    s_.cover = *best;
    setMode(Mode::Cover);
    return true;
}

// EnemyBehavior_Flank (0x639B20 -> 0x637D10 -> 0x632F80). Lara must be inside [min, max) of it (R+0x04 bit 0; else
// outside). The side (0x62D930): out of the side range (s16 R+0x3A) 3; closer than R+0x1C and more than R+0x28 degrees
// off its facing 4; else 1 / 2 by the sign. The query direction (0x615090) = towards Lara, turned +45 (1) / -45 (2)
// degrees, kept (3) or reversed (4). Candidates: planner nodes in a box of `gather` around it (0x660CE0), not claimed
// (0x61F5D0), in a guard zone (0x622C80); cost (0x622670):
//   |dist(node, it) - travel| travelW + |dist(node, Lara) - lara| laraW + (1 - dir . toward(node)) dirW / 2
//   + closeCost (closer than closeDist to Lara) + nearCost (closer than nearDist to it) + coneW (Lara outside the
//   node's cover cone, 0x623610) + 2000 (one of its last 8 destinations within 640, 0x61F6B0);
// the cheapest under maxCost wins (strictly: the first of equals [I: nearest first, for tru.exe's kd-tree order]),
// if it can get there (0x6498D0: its nav mesh). Every soldier type reads the same record: all weights 0, closeCost /
// nearCost 1e11 (4 m from Lara, 0.8 m from itself), so it takes the nearest free node at least 4 m from her.
bool Enemy::flank(const Target& lara) {
    const FlankStyle& f = type_->flank;
    if (!f.valid || s_.flankWait > 0 || nodes.empty()) return false;
    s_.flankWait = kFlankRest;
    const vec3 ai = s_.pos, to = lara.pos - ai;
    const float d = glm::length(to), lo = std::min(f.minRange, f.maxRange), hi = std::max(f.minRange, f.maxRange);
    if (f.inside ? d < lo || d >= hi : d >= lo || d < hi) return false;
    const vec3 facing(std::cos(s_.yaw), std::sin(s_.yaw), 0);
    const float off = std::atan2(facing.x * to.y - facing.y * to.x, facing.x * to.x + facing.y * to.y); // + = Lara on its left [I]
    const int side = f.lara * f.lara <= d * d ? 3 : d < f.frontRange && std::abs(off) > glm::radians(f.frontAngle) ? 4 : off < 0 ? 1 : 2;
    const glm::vec2 base = glm::length(glm::vec2(to)) > 1e-3f ? glm::normalize(glm::vec2(to)) : glm::vec2(facing);
    const float turn = side == 1 ? kPi / 4 : side == 2 ? -kPi / 4 : side == 4 ? kPi : 0;
    const glm::vec2 dir(base.x * std::cos(turn) - base.y * std::sin(turn), base.x * std::sin(turn) + base.y * std::cos(turn));
    std::vector<std::pair<float, size_t>> order; // nearest first
    for (size_t i = 0; i < nodes.size(); ++i) {
        const vec3 p = nodes[i].pos();
        if (glm::any(glm::greaterThan(glm::abs(p - ai), vec3(f.gather)))) continue;
        order.push_back({flatDist(ai, p), i});
    }
    std::sort(order.begin(), order.end());
    const float eye = type_->tuning.height * 0.5f; // [I] 0x6746C0: crouched half height (or the eye marker)
    bool best = false;
    float bestCost = f.maxCost;
    for (const auto& [travel, i] : order) {
        const formats::PlannerNode& n = nodes[i];
        const vec3 p = n.pos();
        if (std::any_of(claimed.begin(), claimed.end(), [&](vec3 c) { return glm::distance(c, p) < 1.0f; })) continue;
        if (!zones.empty() && !std::any_of(zones.begin(), zones.end(), [&](const GuardZone& z) { return z.contains(p, true); })) continue;
        const vec3 local(glm::inverse(n.frame) * glm::vec4(lara.pos, 1)); // 0x623610: Lara in the node's cover cone
        const float ahead = -local.y, across = glm::length(glm::vec2(local));
        const bool cone = ahead > n.depth + 632.1f && std::abs(std::atan2(local.x, ahead)) < glm::radians(n.angle) && across > 0 &&
                          (local.z - eye) / across <= (n.height - eye) / (n.depth + 632.1f);
        const bool recent = std::any_of(s_.recent.begin(), s_.recent.begin() + s_.recentCount,
                                        [&](vec3 r) { return glm::all(glm::lessThan(glm::abs(r - p), vec3(640.0f))); });
        const float fromLara = flatDist(p, lara.pos);
        float cost = std::abs(travel - f.travel) * f.travelW + std::abs(fromLara - f.lara) * f.laraW + (cone ? 0 : f.coneW) + (recent ? 2000.0f : 0);
        if (travel > 0) cost += (1 - glm::dot(dir, glm::vec2(p - ai) / travel)) * f.dirW * 0.5f;
        if (fromLara < f.closeDist) cost += f.closeCost;
        if (travel < f.nearDist) cost += f.nearCost;
        if (cost < bestCost) best = true, bestCost = cost, s_.goal = p;
    }
    if (!best || (nav && nav->locate(ai) >= 0 && nav->path(ai, s_.goal).empty())) return false;
    s_.recent[size_t(s_.recentNext)] = s_.goal;
    s_.recentNext = (s_.recentNext + 1) % int(s_.recent.size()), s_.recentCount = std::min(s_.recentCount + 1, int(s_.recent.size()));
    setMode(Mode::Flank);
    return true;
}

// EnemyBehavior_Sidestep (0x677D70: Lara closer than `trigger`, its timer run out; 0x678530: a random side, `step`
// away, checked on the nav mesh by 0x67A560; 0x678B50 / 0x674320 re-arm the timer with +0x10 after a failed try, +0x0C
// after a step). Ours: it faces her and plays the side's clip (root motion carries it).
bool Enemy::dodge(const collision::CollisionMesh& col, const Target& lara) {
    const DodgeStyle& d = type_->dodge;
    if (d.left.empty() || s_.dodgeWait > 0 || flatDist(s_.pos, lara.pos) >= d.trigger) return false;
    const bool left = (lcg(s_.rng) >> 16) & 1;
    const float b = bearing(s_.pos, lara.pos), a = b + (left ? kPi / 2 : -kPi / 2);
    const vec3 to = s_.pos + vec3(std::cos(a), std::sin(a), 0) * d.step;
    const vec3 knee(0, 0, type_->tuning.stepUp + type_->tuning.radius);
    const bool ok = nav && nav->locate(s_.pos) >= 0
                        ? nav->locate(to) >= 0
                        : !col.raycast(s_.pos + knee, glm::normalize(to - s_.pos), d.step) &&
                              col.groundProbe(to + vec3(0, 0, type_->tuning.stepUp), type_->tuning.stepUp + type_->tuning.stepDown).has_value();
    if (!ok) {
        s_.dodgeWait = d.retry;
        return false;
    }
    setMode(Mode::Dodge);
    s_.yaw = b;
    playClip(left ? d.left : d.right, false);
    return true;
}

// [I] wall walking (tru.exe EnemyState_CornerWalk* / the spiders' concave_* / convex_* clips walk the planner mesh's
// wall faces; not decoded): the crawler walks its run clip's root speed along its surface, down a wall (towards Lara
// sideways) or across a ceiling towards her, held on by a ray along -up. It stands up on the floor at a wall's foot,
// lets go above Lara on a ceiling, and drops when the surface ends or after 8 s. False: drop now.
bool Enemy::crawl(const collision::CollisionMesh& col, const Target& lara, float step) {
    const EnemyTuning& t = type_->tuning;
    const vec3 n = up();
    if (s_.modeTime > 8.0f) return false;
    vec3 along = lara.pos - s_.pos;
    along -= n * glm::dot(along, n);
    vec3 dir;
    if (n.z < -0.5f) { // ceiling: to above her, then lets go
        if (flatDist(s_.pos, lara.pos) < t.radius * 2) return false;
        dir = along;
    } else { // wall: downwards, drifting towards her
        vec3 down = vec3(0, 0, -1) - n * -n.z;
        const float dl = glm::length(down);
        if (dl < 1e-3f) return false; // a floor-like surface: stands up through Drop
        down /= dl;
        const float al = glm::length(along - down * glm::dot(along, down));
        dir = down + (al > 1 ? (along - down * glm::dot(along, down)) / al * 0.5f : vec3(0));
        // the foot of the wall: a floor within a body radius of where it stands off the wall
        if (const auto g = col.groundProbe(s_.pos + n * (t.radius * 2), t.radius * 3); g && g->normal.z >= t.minFloorNormalZ) {
            s_.pos = g->point, s_.tilt = glm::vec2(0), s_.vz = 0;
            setMode(Mode::Chase);
            return true;
        }
    }
    const float l = glm::length(dir);
    if (l < 1e-3f) return true;
    dir /= l;
    const vec3 local = glm::transpose(tiltMatrix(s_.tilt)) * dir; // heading so the body faces where it walks
    s_.yaw = std::atan2(local.y, local.x);
    const vec3 next = s_.pos + dir * std::max(step, 1.0f);
    const auto hold = col.raycast(next + n * t.radius, -n, t.radius * 2);
    if (!hold) return false;
    s_.pos = hold->point;
    return true;
}

vec3 Enemy::up() const { return tiltMatrix(s_.tilt) * vec3(0, 0, 1); }

// 0x67F040: an entry {class (-1 any), type (0 any), flag} matching the hit message's +0x50 / +0x64 / +0x6E.
// ponytail: flag bit 0 compared with false (our hits never set +0x6E).
bool Enemy::getsUp(const Hit& hit) const {
    for (const auto& k : type_->tuning.regenerate.kills)
        if ((k.source == -1 || k.source == hit.damageClass) && (k.type == 0 || k.type == hit.damageType) && !k.flag) return true;
    return false;
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
    const vec3 at = lara.pos + vec3(0, 0, lara.height * 0.75f);
    if (s_.mode == Mode::Cling || s_.mode == Mode::Crawl) { // [I] on a wall or ceiling the cones' heading means nothing: eye range and the ray
        const vec3 eye = s_.pos + up() * (type_->tuning.radius * 2);
        const float d = glm::length(at - eye);
        if (d > se.eye.range) return false;
        const auto hit = d < 1 ? std::nullopt : col.raycast(eye, (at - eye) / d, d);
        return !hit || hit->t >= d - 1.0f;
    }
    const vec3 eye = s_.pos + vec3(0, 0, type_->tuning.height * 0.8f);
    // [I] body cones measured from the eye too (the exe's 0x60DBA0 uses the body matrix)
    if (!inCone(se.eye, eye, s_.yaw, at) && !inCone(se.body[0], eye, s_.yaw, at) && !inCone(se.body[1], eye, s_.yaw, at)) return false;
    const float d = glm::length(at - eye);
    if (!se.ray || d < 1) return true;
    const auto hit = col.raycast(eye, (at - eye) / d, d);
    return !hit || hit->t >= d - 1.0f;
}

void Enemy::onHit(const Hit& hit) {
    if (!alive()) return;
    if (hit.knockVel != vec3(0)) s_.knock = hit.knockVel, s_.vz = std::max(s_.vz, hit.knockVel.z);
    s_.lastSeen = s_.pos - hit.dir * 1000.0f; // [I] turns towards where the shot came from
    s_.unseenTime = 0;
    if (s_.mode == Mode::Down || s_.mode == Mode::Rise) { // 0x682490: hits it would get up from are absorbed
        if (getsUp(hit)) return;
        if ((s_.health -= hit.damage) <= 0) die(hit); // anything else (Mjolnir) hurts the full-health body
        return;
    }
    if (hit.tranq && type_->sleeps) { // darts dose the animal instead of hurting it; asleep at its health [I]
        if (s_.mode == Mode::Sleep) return;
        if ((s_.drowsy += hit.damage) >= s_.health) setMode(Mode::Sleep);
        else if (s_.mode == Mode::Idle) setMode(Mode::Alert);
        return;
    }
    s_.health -= hit.damage;
    s_.damageSinceReaction += hit.damage;
    if (s_.health <= 0) {
        // 0x620380: a lethal hit the regenerate record lists (guns, blows, explosions) only knocks a thrall down
        if (type_->regenerates() && getsUp(hit)) setMode(Mode::Down);
        else die(hit);
    } else if (s_.leap) {
        // [I] mid-jump: no reaction, it lands first
    } else if (s_.mode == Mode::Cling || s_.mode == Mode::Crawl) {
        setMode(Mode::Drop); // [I] shot on the wall: lets go
    } else if (hit.knock || (type_->tuning.staggerDamage > 0 && s_.damageSinceReaction >= type_->tuning.staggerDamage &&
                             s_.mode != Mode::Hit)) {
        // [I] stagger 0 (thralls, spiders, sharks): no reaction (thralls have additive hitreact_*_add clips; a full
        // one per pistol round pushed them back for good); a reaction plays out, hits during it add up
        s_.damageSinceReaction = 0;
        setMode(Mode::Hit);
    } else if (calm(s_.mode)) {
        setMode(Mode::Alert); // shot from out of sight: turns to the shooter
    }
}

void Enemy::headShotReact(vec3 lara) {
    if (!alive() || s_.mode == Mode::Down || s_.mode == Mode::Rise) return;
    s_.leap = 0;
    s_.yaw = std::atan2(lara.y - s_.pos.y, lara.x - s_.pos.x);
    setMode(Mode::Hit);
    if (const auto& clip = type_->tuning.headShot.reaction; !clip.empty() && type_->clips.count(clip)) playClip(clip, false);
}

// Death clip as FUN_00642210: the attacker's quadrant relative to the heading (1 front ±45°, 3 left, 5 back, 7
// right) picks a record entry (category, flag) at random among the matching ones (FUN_00642170); none -> front/back by
// ±90° -> front. The heading then turns so the clip falls away from the attacker. Flag [I]: 3 on a head shot
// (death_adren / death_placed_shot), 5 when hit while running (death_running), else 0. A type-2 entry (anim request,
// not resolved) plays the role death clip. The entry's min / max set when physics takes the body (0x66C500 ->
// ragdollAt): min + rand(max - min) counts; no entry: at once. [O] 0x66C500 hands over at once instead when the
// hits' summed impulses / damage reach death record +8 / +0xC; those are 1e9 / 101138 (soldiers, cats) and 2.8e6
// (thralls), beyond any hit, so ours always waits.
void Enemy::die(const Hit& hit) {
    const bool running = s_.clip == type_->clipName(Role::Run) && s_.mode == Mode::Chase;
    if (s_.tilt != glm::vec2(0)) s_.pos += up() * type_->tuning.radius, s_.tilt = glm::vec2(0); // falls off the wall
    setMode(Mode::Dead);
    s_.killAt = hit.point - s_.pos, s_.killDir = hit.dir;
    s_.ragdollAt = 0;
    const float a = std::atan2(-hit.dir.y, -hit.dir.x), rel = wrapAngle(a - s_.yaw); // bearing to the attacker
    const int quadrant = std::abs(rel) <= kPi / 4 ? 1 : std::abs(rel) > 3 * kPi / 4 ? 5 : rel > 0 ? 3 : 7;
    const auto& deaths = type_->tuning.deaths;
    for (const int flag : {hit.headshot ? 3 : running ? 5 : 0, 0})
        for (const int cat : {quadrant, std::abs(rel) > kPi / 2 ? 5 : 1, 1}) {
            const auto match = [&](const DeathClip& d) { return d.valid && d.category == cat && d.flag == flag; };
            const auto n = std::ranges::count_if(deaths, match);
            if (!n) continue;
            auto pick = lcg(s_.rng) % uint32_t(n);
            const DeathClip* d = nullptr;
            for (const auto& x : deaths)
                if (match(x) && !pick--) {
                    d = &x;
                    break;
                }
            if (type_->clips.count(d->clip)) playClip(d->clip, false);
            if (cat != 1) s_.yaw = wrapAngle(a + (cat == 3 ? -kPi / 2 : cat == 5 ? kPi : kPi / 2));
            s_.ragdollAt = float(d->min + (d->max > d->min ? int(lcg(s_.rng) % uint32_t(d->max - d->min)) : 0)) * kCount;
            return;
        }
}

int Enemy::readyFire() const {
    const auto& as = type_->tuning.attacks;
    for (size_t i = 0; i < as.size() && i < s_.fireCooldown.size(); ++i)
        if (as[i].ranged() && type_->clips.count(as[i].shotClip) && s_.fireCooldown[i] <= 0) return int(i);
    return -1;
}

// One shot of the current fire record: a ray from the gun's shot origin (EnemyType::muzzle on the gun riding its
// marker; [I] eye height, one radius ahead without one) at Lara's chest, [I] kMissChance of them aimed 1.5..3 of her
// radii aside; it stops at the first wall (line of fire) or Lara's capsule, which takes rec+0x154.
void Enemy::shoot(const collision::CollisionMesh& col, const Target& lara, const HurtFn& hurt, std::vector<Shot>* shots) {
    const Attack& a = type_->tuning.attacks[size_t(s_.fire)];
    const EnemyTuning& t = type_->tuning;
    const int seg = type_->gunSegment;
    // no gun marker: a gun facing the heading with its shot origin there
    const glm::mat4 gun = seg >= 0 && size_t(seg) < bones_.size()
                              ? modelMatrix(s_.pos, s_.yaw, s_.tip, s_.tilt) * bones_[size_t(seg)] * type_->gunMount
                              : modelMatrix(s_.pos + vec3(0, 0, t.height * 0.8f) + vec3(std::cos(s_.yaw), std::sin(s_.yaw), 0) * t.radius, s_.yaw) *
                                    glm::inverse(type_->muzzle);
    const vec3 from(gun * type_->muzzle[3]);
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
    if (shots) shots->push_back({uid_, from, from + dir * len, hit, gun});
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
    s_.coverCooldown = std::max(0.0f, s_.coverCooldown - dt);
    s_.flankWait = std::max(0.0f, s_.flankWait - dt), s_.dodgeWait = std::max(0.0f, s_.dodgeWait - dt);
    if (s_.ragdollPhase) { // physics owns the body (World::stepRagdolls)
        updatePose(dt, false);
        return;
    }
    const bool sees = s_.mode != Mode::Dead && lara.alive && perceives(col, lara);
    if (sees) {
        s_.unseenTime = 0, s_.lastSeen = lara.pos;
    } else {
        s_.unseenTime += dt;
        if (s_.mode != Mode::Idle && s_.mode != Mode::Patrol && s_.mode != Mode::Investigate && s_.mode != Mode::Dead && lara.alive &&
            (lara.inWater || !aquatic) && glm::distance(s_.pos, lara.pos) < senses().loseTrack)
            s_.lastSeen = lara.pos; // [I] senses +0x20: still tracked this close, unseen
    }
    // Ground enemies measure Lara flat, centre to centre; flyers from their centre to her head.
    const vec3 head = lara.pos + vec3(0, 0, lara.height * 0.85f), centre = s_.pos + vec3(0, 0, t.height * 0.5f);
    const float dist = flyer ? glm::distance(centre, head) : flatDist(s_.pos, lara.pos);
    takeoff_.reset();
    if (leapTick(dt)) {
        updatePose(dt, false);
        return;
    }
    vec3 delta = advanceClip(dt);
    vec3 flight = aquatic ? delta : vec3(0); // aquatic clips keep their 3D root motion; bats hover except when chasing

    switch (s_.mode) {
    case Mode::Cling: // holds on, no gravity, until it notices Lara, then crawls (or lets go)
        if (sees) setMode(Mode::Crawl);
        updatePose(dt, false);
        return;
    case Mode::Crawl:
        if (!crawl(col, lara, glm::length(delta))) setMode(Mode::Drop);
        updatePose(dt, false);
        return;
    case Mode::Drop: // falls (fall clip) to the floor, lands (land clip), hunts
        delta = vec3(0);
        if (!type_->landClip.empty() && s_.clip == type_->landClip) {
            if (clipDone()) setMode(Mode::Chase);
        } else if (s_.vz == 0 && s_.modeTime > dt) {
            if (type_->landClip.empty()) setMode(Mode::Chase);
            else playClip(type_->landClip, false);
        }
        break;
    case Mode::Down: // lies for the record's time, then gets up (0x682510 counts it down)
        delta = vec3(0);
        if ((s_.down -= dt) <= 0) setMode(type_->tuning.regenerate.up.empty() ? Mode::Chase : Mode::Rise);
        break;
    case Mode::Rise:
        delta = vec3(0);
        if (clipDone()) setMode(Mode::Chase);
        break;
    case Mode::Spawning:
        if (clipDone()) setMode(Mode::Chase);
        break;
    case Mode::Idle: // NonCombat: Patrol (priority 15) before MoveToGuardZone (10) before Idle (soldier tree 0x612030)
        if (sees) {
            notice();
        } else if (patrolPoints() && s_.patrolIndex >= 0) {
            setMode(Mode::Patrol);
        } else if (!inZone(false)) {
            setMode(Mode::Return);
        }
        break;
    case Mode::Patrol: { // PatrolMove to the point (pulled into the guard zone, 0x61F990), PatrolLookTurn, PatrolAnim, PatrolIncr
        const auto* p = patrolPoints();
        if (!p || s_.patrolIndex < 0 || size_t(s_.patrolIndex) >= p->size()) {
            if (sees) notice();
            else setMode(Mode::Idle);
            break;
        }
        const PatrolPoint& point = (*p)[size_t(s_.patrolIndex)];
        const PatrolStyle& ps = type_->patrol;
        if (sees) { // PatrolAttack (priority 20, 0x635920): a point of mode 2 / 3 attacks with record +0x10 / +0x14 at once
            const int a = point.mode == 2 || point.mode == 3 ? ps.attack[size_t(point.mode - 2)] : -1;
            const Attack* rec = a >= 0 ? &t.attacks[size_t(a)] : nullptr;
            if (rec && rec->ranged() && type_->clips.count(rec->shotClip) && dist <= senses().eye.range && size_t(a) < s_.fireCooldown.size()) {
                s_.fire = a, s_.lastSeen = lara.pos;
                setMode(Mode::Fire);
            } else if (rec && !rec->ranged() && type_->clips.count(rec->clip) && dist <= t.attackRange) { // [I] the 0x634B20 range
                s_.melee = a;
                setMode(Mode::Attack);
            } else {
                notice();
            }
            break;
        }
        s_.phaseTime += dt;
        if (s_.phase == 0) {
            if (walkTo(flyer ? point.pos : zoneGoal(point.pos), ps.arrive > 0 ? ps.arrive : 2 * t.radius, ps.walk, dt, &flight) ||
                s_.phaseTime > kPatrolGiveUp) {
                s_.phase = point.look && !flyer ? 2 : 1, s_.phaseTime = 0;
                if (s_.phase == 1) playIdle(ps.idles); // none (cats): straight on
                delta = flight = vec3(0);
            }
        } else if (s_.phase == 2) { // PatrolLookTurn (0x628800): faces the node with the set's turn clips
            delta = vec3(0);
            const vec3 look = s_.pos + point.facing * 100.0f;
            const float off = wrapAngle(bearing(s_.pos, look) - s_.yaw);
            if (const std::string& c = off > 0 ? ps.turnLeft : ps.turnRight; !c.empty()) playClip(c, true);
            turnTowards(look, dt);
            if (std::abs(off) < 0.05f) s_.phase = 1, s_.phaseTime = 0, playIdle(ps.idles);
        } else {
            delta = vec3(0);
            if (!aquatic) flight = vec3(0);
            if (s_.phaseTime >= s_.phaseLength) {
                s_.phase = 0, s_.phaseTime = 0;
                if (!nextPatrolPoint(patrolMode)) setMode(Mode::Idle); // a finished path: stays where it is
            }
        }
        break;
    }
    case Mode::Search: { // 0x61E570 steps 0 start, 1 to last known, 2 idle, 3 to a possible position, then 2 / 3 again
        if (sees) {
            notice();
            break;
        }
        const SearchStyle& se = type_->search;
        s_.phaseTime += dt, s_.searchTime += dt;
        if (s_.searchTime >= se.time) { // [I] the record's search time: back to its patrol, its zone or idle
            setMode(Mode::Idle);
            break;
        }
        const auto possible = [&] { // [I] the step's distance within ±angle of its heading, a straight walk onto floor
            const vec3 lift(0, 0, t.stepUp + t.radius);
            for (int k = 0; k < kSearchTries && se.step > 0; ++k) {
                const float a = s_.yaw + (2 * frand(s_.rng) - 1) * se.angle;
                const vec3 to = s_.pos + vec3(std::cos(a), std::sin(a), 0) * se.step;
                if (col.raycast(s_.pos + lift, glm::normalize(to - s_.pos), se.step)) continue;
                if (const auto g = col.groundProbe(to + vec3(0, 0, t.stepUp), t.stepUp + t.maxDrop)) return g->point;
            }
            return s_.pos;
        };
        const auto advance = [&] {
            const int next = s_.phase == 0 ? 1 : s_.phase == 2 || se.idles.empty() ? 3 : 2;
            s_.phase = next, s_.phaseTime = 0;
            if (next == 2) playIdle(se.idles);
            if (next == 3) s_.goal = possible();
        };
        if (s_.phase == 1 || s_.phase == 3) {
            if (walkTo(zoneGoal(s_.goal), se.arrive > 0 ? se.arrive : 2 * t.radius, se.walk, dt) || s_.phaseTime > kSearchWalkSeconds) advance();
        } else {
            delta = vec3(0);
            if ((s_.phase == 0 && clipDone()) || (s_.phase == 2 && s_.phaseTime >= s_.phaseLength)) advance();
        }
        break;
    }
    case Mode::Return: // EnemyBehavior_MoveToGuardZone: walks back to zone 0 until inside 0.8 x its inner radius
        if (sees) {
            notice();
            break;
        }
        play(Role::Walk, true);
        turnTowards(zones.empty() ? s_.pos : steer(zones[0].point), dt);
        if (zones.empty() || flatDist(s_.pos, zones[0].point) < zones[0].inner * kZoneClamp) setMode(Mode::Idle);
        break;
    case Mode::Alert:
        turnTowards(s_.lastSeen, dt);
        if (s_.modeTime >= kAlertSeconds && (s_.loop || clipDone())) setMode(lara.alive ? Mode::Chase : Mode::Idle);
        break;
    case Mode::Cover: {
        const vec3 run = delta; // this tick's root motion: kept only while running there
        delta = vec3(0);
        s_.unseenTime = 0; // [I] hidden on purpose: still knows where she is (lastSeen stays)
        if (s_.clip == "combat_stand_to_crouch") {
            turnTowards(lara.pos, dt);
            if (clipDone()) playClip("combat_idle_crouch", true), s_.modeTime = 0;
        } else if (s_.clip == "combat_idle_crouch") {
            turnTowards(lara.pos, dt);
            if (s_.modeTime >= kCoverSeconds) playClip(type_->clips.count("combat_crouch_to_stand") ? "combat_crouch_to_stand" : type_->clipName(Role::Alert), false);
        } else if (s_.clip == "combat_crouch_to_stand" || s_.modeTime >= kCoverRunSeconds) {
            if (s_.clip != "combat_crouch_to_stand" || clipDone()) s_.coverCooldown = kCoverRest, setMode(Mode::Chase);
        } else if (flatDist(s_.pos, s_.cover) < t.radius) { // arrived: crouches (reloaded meanwhile [I])
            playClip(type_->clips.count("combat_stand_to_crouch") ? "combat_stand_to_crouch" : "combat_idle_crouch", false);
            for (float& c : s_.fireCooldown) c = 0;
        } else { // runs there
            play(Role::Run, true);
            turnTowards(steer(s_.cover), dt);
            delta = run;
        }
        break;
    }
    case Mode::Chase: {
        const vec3 goal = zoneGoal(s_.lastSeen); // the guard zone bounds the chase (0x61F990)
        const bool held = flatDist(s_.pos, goal) < t.radius; // at the last seen spot, or the zone's edge
        const bool stands = sees && (dist <= t.attackRange || (type_->ranged() && dist <= senses().eye.range));
        turnTowards(stands || held || (sees && flyer) ? s_.lastSeen : flyer ? goal : steer(goal), dt);
        const bool facing = aimError(lara.pos) < kFaceToAttack;
        const int fire = type_->ranged() ? readyFire() : -1;
        if (!lara.alive) {
            setMode(Mode::Idle);
        } else if (s_.unseenTime > t.loseTime) { // lost her: searches (ground enemies with a search record), else idle
            setMode(!flyer && type_->search.time > 0 ? Mode::Search : Mode::Idle);
        } else if (sees && dist <= t.attackRange && facing && s_.cooldown <= 0 && type_->clip(Role::Attack)) {
            s_.melee = -1;
            setMode(Mode::Attack);
        } else if (type_->spit >= 0 && sees && dist > t.attackRange && dist <= kSpitRange && facing && s_.cooldown <= 0 &&
                   s_.fireCooldown[0] <= 0) { // [I] spits at Lara out of reach (its record's cooldown)
            s_.melee = type_->spit;
            s_.fireCooldown[0] = t.attacks[size_t(type_->spit)].cooldown;
            setMode(Mode::Attack);
        } else if (!flyer && sees && dodge(col, lara)) { // CloseCombat: Sidestep (priority 30) under Melee / Close
        } else if (stands) {
            play(Role::Alert, true); // in reach, or in rifle range with a clear line: stand (and aim)
            delta = vec3(0);
            flight = vec3(0);
            if (fire >= 0 && dist > t.attackRange && aimError(lara.pos) <= t.attacks[size_t(fire)].maxYaw) {
                s_.fire = fire;
                setMode(Mode::Fire);
            }
        } else if (held) {
            play(Role::Alert, true); // waits there
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
        const auto ci = type_->clips.find(s_.clip);
        const double f = ci != type_->clips.end() ? s_.clipTime / ci->second.durationSeconds() : 1.0;
        const Attack* rec = s_.melee >= 0 ? &t.attacks[size_t(s_.melee)] : nullptr;
        const bool spit = rec && s_.melee == type_->spit; // reaches as far as it sees
        if (f < t.hitStart) turnTowards(lara.pos, dt);
        if (!s_.blowLanded && lara.alive && f >= t.hitStart && f <= t.hitEnd && dist <= (spit ? kSpitRange : t.attackReach) &&
            aimError(lara.pos) <= kBlowCone && (!spit || sees)) {
            s_.blowLanded = true;
            if (hurt) hurt(rec ? rec->damage : t.attackDamage, s_.pos, rec ? (rec->type == 2 ? kDamageHeavy : kDamageLight) : t.attackType);
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
            if (!seekCover(col, lara) && !(!flyer && flank(lara))) setMode(type_->clips.count(a.followClip) ? Mode::Reload : Mode::Chase);
        }
        break;
    }
    case Mode::Reload:
        if (clipDone()) setMode(Mode::Chase);
        break;
    case Mode::Flank: // runs (nav path) to the node, then fights on from there; [I] gives up after kFlankRunSeconds
        if (walkTo(s_.goal, std::max(type_->flank.arrive, t.radius), type_->clips.count(type_->flank.run) ? type_->flank.run : type_->clipName(Role::Run), dt) ||
            s_.modeTime > kFlankRunSeconds)
            setMode(Mode::Chase), delta = vec3(0);
        break;
    case Mode::Dodge: // the side clip's root motion; then the step's timer (+0x0C)
        if (clipDone()) s_.dodgeWait = type_->dodge.rest, setMode(Mode::Chase);
        break;
    case Mode::Investigate: { // InvestigateStart clip, InvestigateMove (pulled into its zone: 0x61F990 x 0.8), InvestigateEnd
        if (sees) {
            notice();
            break;
        }
        const InvestigateStyle& iv = type_->investigate;
        s_.phaseTime += dt;
        if (s_.phase == 1) {
            if (walkTo(zoneGoal(s_.goal), iv.arrive > 0 ? iv.arrive : 2 * t.radius, iv.walk, dt) || s_.phaseTime > kInvestigateSeconds) {
                s_.phase = 2, s_.phaseTime = 0, delta = vec3(0);
                if (!iv.end.empty()) playClip(iv.end, false);
            }
        } else {
            delta = vec3(0);
            if (s_.phase == 0 && clipDone()) s_.phase = 1, s_.phaseTime = 0;
            else if (s_.phase == 2 && (iv.end.empty() || clipDone())) setMode(Mode::Idle); // back to its patrol or post
        }
        break;
    }
    case Mode::Sleep: // sleep_start, sleep_loop for kSleepSeconds, sleep_end, then awake and alert
        if (s_.clip == "sleep_start" && clipDone()) playClip("sleep_loop", true);
        else if (s_.clip == "sleep_loop" && (s_.sleep -= dt) <= 0) playClip("sleep_end", false);
        else if (s_.clip == "sleep_end" && clipDone()) setMode(Mode::Alert);
        delta = vec3(0), flight = vec3(0);
        break;
    case Mode::Hit:
        if (clipDone()) setMode(Mode::Chase);
        break;
    case Mode::Dead:
        if (aquatic && clipDone() && s_.clip != "death_float_loop" && type_->clips.count("death_float_loop"))
            playClip("death_float_loop", true);
        // The death timer (0x66C950, armed by die(): ragdollAt) hands the body to physics mid-clip: the chunk object
        // when it has one and fewer than 4 sets are live (0x696F90), else its ragdoll when fewer than 6 are live
        // (0x617EE0 -> 0x60EF50 -> 0x4F1920), else the timer re-arms for death record +0x1C. A pure ragdoll from the
        // pose and segment velocities the clip had: no blend into it (0x4F1920 builds it unpowered). Without either it
        // tips onto its side once the clip is done.
        if ((type_->ragdoll || type_->chunks) && !aquatic && s_.modeTime >= s_.ragdollAt) {
            if (type_->chunks && chunkSets < 4) {
                s_.ragdollPhase = 4;
                s_.pieces.clear();
            } else if (type_->ragdoll && ragdolls < 6) {
                ragdollOffsets(bones_);
                s_.ragdoll.clear();
                for (const auto& b : type_->ragdoll->bones) s_.ragdoll.push_back(bones_[size_t(b.segment)]);
                s_.ragdollPhase = 1;
            } else {
                s_.ragdollAt = s_.modeTime + t.deathTimer;
            }
        } else if (clipDone() && !flyer && !type_->ragdoll && !type_->lyingDeaths.count(s_.clip)) {
            s_.tip = std::min(kPi / 2, s_.tip + dt * kPi / 2 / kTipSeconds);
        }
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
    if (takeoff_ && s_.mode == takeoffMode_ && s_.vz == 0 && s_.knock == vec3(0)) { // at a link's take-off: jumps
        startLeap(*takeoff_);
        updatePose(dt, false);
        return;
    }
    if (s_.knock != vec3(0)) delta += vec3(vec2(s_.knock) * dt, 0); // thrown: flies on until it lands
    move(col, delta, dt);
    if (s_.vz == 0 && s_.mode != Mode::Dead) s_.knock = vec3(0);
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
    if (s_.ragdollPhase && !reset && !bones_.empty() && !ragdollOffset_.empty()) {
        prevBones_ = std::move(bones_);
        bones_ = ragdollBones();
        return;
    }
    if (reset || bones_.empty()) {
        anim_ = anim::AnimController(type_->skeleton);
        anim_.play(type_->clips.count(s_.clip) ? &type_->clips.find(s_.clip)->second : nullptr, 0, s_.loop, 1, s_.clipTime);
    } else {
        anim_.update(dt);
        if (clipSerial_ != poseSerial_) {
            const auto it = type_->clips.find(s_.clip);
            anim_.play(it == type_->clips.end() ? nullptr : &it->second, 0.1f, s_.loop, 1, s_.clipTime); // [I] 3-frame fade (Lara's default)
        } else {
            anim_.setTime(s_.clipTime);
        }
    }
    poseSerial_ = clipSerial_;
    auto local = anim_.pose();
    if (!local.empty()) local[0] = {{0, 0, 0}, {1, 1, 1}, glm::make_vec3(type_->skeleton.bones[0].pivot)}; // root motion is in pos
    auto m = anim::worldMatrices(type_->skeleton, local);
    if (s_.ragdollPhase && type_->ragdoll && s_.ragdoll.size() == type_->ragdoll->bones.size()) // loaded: the death pose's offsets
        ragdollOffsets(m), m = ragdollBones();
    else if (reset)
        ragdollOffset_.clear();
    prevBones_ = reset || bones_.empty() ? m : std::move(bones_);
    bones_ = std::move(m);
}

void Enemy::ragdollOffsets(const std::vector<glm::mat4>& pose) {
    const auto& sk = type_->skeleton.bones;
    const auto& rb = type_->ragdoll->bones;
    ragdollOwner_.assign(sk.size(), 0);
    ragdollOffset_.assign(sk.size(), glm::mat4(1));
    for (size_t s = 0; s < sk.size(); ++s) {
        const auto own = std::ranges::find(rb, int(s), &physics::RagdollBone::segment);
        ragdollOwner_[s] = own != rb.end() ? int(own - rb.begin()) : sk[s].parent >= 0 ? ragdollOwner_[size_t(sk[s].parent)] : 0;
        ragdollOffset_[s] = glm::inverse(pose[size_t(rb[size_t(ragdollOwner_[s])].segment)]) * pose[s];
    }
}

std::vector<glm::mat4> Enemy::ragdollBones() const {
    std::vector<glm::mat4> out(ragdollOffset_.size());
    for (size_t s = 0; s < out.size(); ++s) out[s] = s_.ragdoll[size_t(ragdollOwner_[s])] * ragdollOffset_[s];
    return out;
}

void Enemy::setRagdoll(std::span<const glm::mat4> world, bool asleep) {
    const glm::mat4 toModel = glm::inverse(modelMatrix(s_.pos, s_.yaw, 0, s_.tilt));
    s_.ragdoll.assign(world.size(), glm::mat4(1));
    for (size_t i = 0; i < world.size(); ++i) s_.ragdoll[i] = toModel * world[i];
    s_.ragdollPhase = asleep || s_.modeTime > kRagdollSeconds ? 3 : 2;
}

void Enemy::setPieces(std::span<const glm::mat4> world, std::span<const vec3> vel) {
    const glm::mat4 toModel = glm::inverse(modelMatrix(s_.pos, s_.yaw, 0, s_.tilt));
    s_.pieces.assign(world.size(), glm::mat4(1));
    for (size_t i = 0; i < world.size(); ++i) s_.pieces[i] = toModel * world[i];
    s_.pieceVel.assign(vel.begin(), vel.end());
}

std::vector<glm::mat4> Enemy::pose(double alpha) const {
    // ponytail: per-element matrix lerp between ticks (60 Hz; slerp as World::pose if it ever shows)
    std::vector<glm::mat4> out(bones_.size());
    for (size_t i = 0; i < out.size(); ++i) out[i] = prevBones_[i] + (bones_[i] - prevBones_[i]) * float(alpha);
    return out;
}

// ---- Enemies ----------------------------------------------------------------------------------

// Kept in uid order: the tick order (claims, shots) then does not depend on the order units streamed in, so a load
// (which streams its units afresh) ticks them as the saving run did.
Enemy& Enemies::add(std::shared_ptr<const EnemyType> type, int32_t uid, vec3 pos, float yaw, bool spawning, glm::vec2 tilt) {
    const auto at = std::upper_bound(enemies_.begin(), enemies_.end(), uid, [](int32_t u, const auto& e) { return u < e->uid(); });
    Enemy& e = **enemies_.insert(at, std::make_unique<Enemy>(std::move(type), uid, pos, yaw, spawning, healthLevel_, tilt));
    ++version_;
    sync();
    return e;
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
    // Claims (0x633F10 / 0x633F60 / 0x61F5D0 reserve a planner node): the patrol point each patrolling enemy walks to
    // or idles at, a flanker's node; kept current within the tick so two enemies never pick the same one.
    const auto claim = [](const Enemy& e) {
        const auto& s = e.state();
        if (e.alive() && s.mode == Mode::Flank) return s.goal;
        const bool on = e.alive() && (s.mode == Mode::Patrol || s.mode == Mode::Idle) && s.patrolIndex >= 0 && s.path >= 0 &&
                        size_t(s.path) < e.patrolPaths.size() && size_t(s.patrolIndex) < e.patrolPaths[size_t(s.path)].size();
        return on ? e.patrolPaths[size_t(s.path)][size_t(s.patrolIndex)].pos : vec3(std::nanf(""));
    };
    claims_.clear();
    for (const auto& e : enemies_) claims_.push_back(claim(*e));
    for (size_t k = 0; k < enemies_.size(); ++k) {
        auto& e = enemies_[k];
        if (const auto g = guards_.find(e->uid()); g != guards_.end() && e->zones.size() != g->second.size()) resolveZones(*e);
        e->nodes = worldNodes_;
        e->nav = navFor(e->type());
        e->claimed = claims_;
        e->ragdolls = e->chunkSets = 0;
        for (const auto& o : enemies_)
            if (o != e) e->ragdolls += o->state().ragdollPhase == 1 || o->state().ragdollPhase == 2, e->chunkSets += o->state().ragdollPhase == 4;
        e->tick(col, lara, hurt, dt, &shots);
        claims_[k] = claim(*e);
    }
    // [I] EnemyFireTeam (0x62E590) links soldiers; ours: a soldier that spots Lara alerts the idle soldiers within
    // kSquadRadius of it. Barks (the ship units' vo\enemy\merc lines, category = the file names' third token) on
    // spotting her (EAR), taking cover (CBR), a squad mate's death (FDY) and losing her (SWP) [I: the categories'
    // meaning; tru.exe's BarkTimer / NtBark choice is not decoded]; one at a time, kBarkGap apart.
    constexpr float kSquadRadius = 3000.0f, kBarkGap = 4.0f;
    barkWait_ = std::max(0.0f, barkWait_ - dt);
    barks.clear();
    const auto bark = [&](const Enemy& e, const char* category) {
        if (barkWait_ > 0) return;
        barks.push_back({e.uid(), category, e.position()});
        barkWait_ = kBarkGap;
    };
    std::vector<const Enemy*> spotters; // alerted by their own senses this tick (not by a squad mate)
    for (const auto& e : enemies_) {
        if (!e->type().ranged()) continue;
        const Mode was = e->previous().mode, now = e->state().mode;
        if (now == Mode::Alert && calm(was)) {
            if (!e->state().loop) bark(*e, "EAR"); // the spotter (its bark clip)
            spotters.push_back(e.get());
        } else if (now == Mode::Cover && was != Mode::Cover) {
            bark(*e, "CBR");
        } else if ((now == Mode::Idle || now == Mode::Search) && was == Mode::Chase) {
            bark(*e, "SWP");
        } else if (now == Mode::Dead && was != Mode::Dead) {
            for (const auto& o : enemies_)
                if (o != e && o->alive() && o->type().ranged() && glm::distance(o->position(), e->position()) < kSquadRadius) {
                    bark(*o, "FDY");
                    break;
                }
        }
    }
    for (const Enemy* e : spotters)
        for (auto& o : enemies_)
            if (o.get() != e && o->type().ranged() && glm::distance(o->position(), e->position()) < kSquadRadius) o->alert(e->state().lastSeen);
}

void Enemies::disturb(vec3 at) {
    for (auto& e : enemies_) e->hear(at);
}

// [I] the smallest loaded layer built for at least its radius (soldiers 94.8 -> 100), else the widest; tru.exe picks
// the layer from AI +0x1C50 (not traced). Flyers swim / fly free (NavPower's VolumeGraph is not decoded).
const NavMesh* Enemies::navFor(const EnemyType& t) const {
    if (t.flyer()) return nullptr;
    const NavMesh* best = nullptr;
    for (const auto& [layer, m] : navs_) {
        const bool fits = m.radius >= t.tuning.radius, bestFits = best && best->radius >= t.tuning.radius;
        if (!best || (fits && (!bestFits || m.radius < best->radius)) || (!fits && !bestFits && m.radius > best->radius)) best = &m;
    }
    return best;
}

void Enemies::guard(int32_t uid, uint32_t record, uint32_t node, float r1, float r2) {
    auto& g = guards_[uid];
    const Guard z{record, node, r1, std::max(r1, r2)};
    if (g.size() < 6 && std::find(g.begin(), g.end(), z) == g.end()) g.push_back(z);
    for (auto& e : enemies_)
        if (e->uid() == uid) resolveZones(*e);
}

// 0x63AC40: n < 1 the intro's own path, 1..count an alternative, else none (the point list is cleared first).
bool Enemies::selectPatrol(int32_t uid, int path) {
    bool found = false;
    for (auto& e : enemies_)
        if (e->uid() == uid) e->setPatrol(std::max(path, 0)), found = true;
    return found;
}

void Enemies::resolveZones(Enemy& e) const {
    e.zones.clear();
    const auto g = guards_.find(e.uid());
    const auto n = nodes_.find(e.unit);
    const auto o = offsets_.find(e.unit);
    if (g == guards_.end() || n == nodes_.end() || o == offsets_.end()) return;
    for (const Guard& z : g->second)
        if (z.record < n->second.size() && z.node < n->second[z.record].size())
            e.zones.push_back({n->second[z.record][z.node].pos() + o->second, z.inner, z.outer});
}

void Enemies::spawn(Generator& g) {
    g.fired = true;
    if (auto type = typeOf(g.object)) {
        auto& e = add(std::move(type), g.uid, g.pos, g.yaw, true);
        e.unit = g.unit;
        e.generator = g.uid;
        e.patrolPaths = g.patrolPaths, e.patrolMode = g.patrolMode; // the generator intro's own enemy data
        e.setPatrol(0);
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
