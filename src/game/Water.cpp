// See Water.hpp. WaterSim decides what makes waves (after Wars and Warriors: Joan of Arc's world_view_water.cpp, same
// author) for TRU's bodies; the waves run on the GPU (render/WaterField.hpp). Droplet randomness is a local xorshift,
// so a replay produces the same kicks, bodies and droplets.
#include "game/Water.hpp"

#include "game/Game.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ou::game {

namespace {
constexpr float kGravity = 5.72443f * 900.0f; // as Tuning::gravity
constexpr size_t kMaxDrops = 256;
constexpr float kNoWater = -std::numeric_limits<float>::infinity();
constexpr float kM = 290.0f; // TRU units per metre (shaders/waves.glsl)

// Bodies (WaveBody): a wader's legs / a swimmer's torso as pressure patches [I]. Draft 7 cm still (the dip under a
// still body stays hidden under it), up to kStrokeDraft more moving: kicking legs and pulling arms push far more water
// aside than the floating body, and that churn is what draws a swimmer's or wader's Kelvin wake.
constexpr float kLegs = 0.22f * kM, kTorso = 0.28f * kM, kDraft = 0.07f * kM;
constexpr float kStrokeDraft = 0.15f * kM, kStrokeSpeed = 1.5f * kM;
// A body's core reflects waves while it is slow (a standing wader, a treading swimmer) and stops doing so as it speeds
// up (a moving wall would stamp noise into the field; its pressure wake takes over).
constexpr float kWallSlow = 0.2f * kM, kWallFast = 1.0f * kM;
// Strokes: a swimmer ringing the water once per stroke (kStroke s, ring strength kStrokeRing) on top of her wake [I].
constexpr float kStroke = 0.4f, kStrokeRing = 0.5f, kSwimRing = 1.0f * kM;
// A standing / treading body's bob: a kick as wide as the body, every 1.2..1.5 s (a ~3-unit ring) [I].
constexpr float kBob = 25.6f, kBobSigma = 1.4f * kLegs;
constexpr float kLaraHeight = 512.0f;
} // namespace

// ---- WaveWindow ----

WaveWindow::WaveWindow() : mask_(kMaskN * kMaskN), scratch_(kMaskN * kMaskN), asked_(kMaskN * kMaskN) {}

void WaveWindow::follow(vec2 centre, const Wet& wet) {
    const glm::ivec2 corner = glm::ivec2(glm::floor(centre / kMaskCell)) - kMaskN / 2;
    if (placed_ && corner == corner_) return;
    const glm::ivec2 d = corner - corner_;
    if (!placed_ || std::abs(d.x) >= kMaskN || std::abs(d.y) >= kMaskN) {
        std::fill(asked_.begin(), asked_.end(), uint8_t(0));
    } else {
        for (std::vector<uint8_t>* a : {&mask_, &asked_}) {
            for (int y = 0; y < kMaskN; ++y) {
                uint8_t* dst = scratch_.data() + y * kMaskN;
                const int oy = y + d.y;
                if (oy < 0 || oy >= kMaskN) {
                    std::fill(dst, dst + kMaskN, uint8_t(0));
                    continue;
                }
                for (int x = 0; x < kMaskN; ++x)
                    dst[x] = x + d.x >= 0 && x + d.x < kMaskN ? (*a)[size_t(oy * kMaskN + x + d.x)] : uint8_t(0);
            }
            a->swap(scratch_);
        }
    }
    corner_ = corner, placed_ = true;
    for (int i = 0; i < kMaskN * kMaskN; ++i)
        if (!asked_[size_t(i)]) {
            mask_[size_t(i)] = wet((vec2(corner_ + glm::ivec2(i % kMaskN, i / kMaskN)) + 0.5f) * kMaskCell) ? 255 : 0;
            asked_[size_t(i)] = 1;
        }
    ++version_;
}

bool WaveWindow::wall(vec2 p) const {
    if (!placed_) return true;
    const glm::ivec2 c = glm::ivec2(glm::floor(p / kMaskCell)) - corner_;
    return c.x < 0 || c.y < 0 || c.x >= kMaskN || c.y >= kMaskN || mask_[size_t(c.y * kMaskN + c.x)] == 0;
}

// ---- WaterSim: what makes waves ----

void WaterSim::reset() {
    window_.clear();
    drops_.clear(), bodies_.clear(), waveBodies_.clear();
    kickBase_ += kicks_.size(), kicks_.clear(), kickTicks_.clear();
    if (active_) ++generation_;
    active_ = false, chosen_ = -1;
}

float WaterSim::waterAt(vec2 p) const {
    float h = kNoWater;
    for (const collision::Markup* m : volumes_)
        if (p.x >= m->points[0].x && p.x <= m->points[1].x && p.y >= m->points[0].y && p.y <= m->points[1].y)
            h = std::max(h, waterSurface(*m, p, seconds_));
    return h;
}

void WaterSim::kick(vec2 p, float amplitude, float sigma) {
    if (!active_ || window_.wall(p)) return;
    if (kicks_.size() >= kMaxKicks) { // a flood of kicks within kKeepTicks: the oldest go
        kickBase_ += 1;
        kicks_.erase(kicks_.begin()), kickTicks_.erase(kickTicks_.begin());
    }
    kicks_.push_back({p.x, p.y, amplitude, sigma});
    kickTicks_.push_back(tick_);
}

void WaterSim::ripple(vec3 p, float strength) {
    // Strength 1 (splash): a 45-unit crater of width 64; 0.6 (a stroke, a weapon) 31 over 54; 0.05 (a droplet) 5.5 over
    // 40 (^0.7: faint kicks still read) [I].
    kick(vec2(p), -45.0f * std::pow(strength, 0.7f), 38.4f + 25.6f * strength);
}

void WaterSim::emit(vec3 pos, int count, float speed, float up, float radius) {
    auto rnd = [&] { // xorshift32, 0..1
        rng_ ^= rng_ << 13, rng_ ^= rng_ >> 17, rng_ ^= rng_ << 5;
        return float(rng_ & 0xFFFFFF) / float(0x1000000);
    };
    for (int n = 0; n < count; ++n) {
        const float a = rnd() * 6.2831853f, s = speed * (0.35f + 0.65f * rnd());
        SplashDrop d;
        d.pos = pos;
        d.vel = {std::cos(a) * s, std::sin(a) * s, up * (0.55f + 0.45f * rnd())};
        d.maxLife = d.life = 0.35f + 0.4f * rnd();
        d.radius = radius * (0.7f + 0.6f * rnd());
        if (drops_.size() < kMaxDrops) drops_.push_back(d);
        else drops_[rng_ % kMaxDrops] = d;
    }
}

void WaterSim::splash(vec3 pos, float strength) {
    if (!active_ || !world_) return;
    const float h = waterAt(vec2(pos));
    if (h == kNoWater || pos.z > h + 100.0f) return; // only at / below the surface
    ripple(vec3(pos.x, pos.y, h), strength);
    emit(vec3(pos.x, pos.y, h + 10.0f), int(8 + 6 * strength), 220.0f * strength, 550.0f * strength);
}

void WaterSim::tick(const World& w, float dt) {
    world_ = &w;
    seconds_ = double(w.ticks()) * kTickSeconds;
    time_ += dt;
    ++tick_;
    { // kicks older than kKeepTicks ticks: every frame since has taken them
        size_t old = 0;
        while (old < kickTicks_.size() && kickTicks_[old] + kKeepTicks <= tick_) ++old;
        kicks_.erase(kicks_.begin(), kicks_.begin() + std::ptrdiff_t(old));
        kickTicks_.erase(kickTicks_.begin(), kickTicks_.begin() + std::ptrdiff_t(old));
        kickBase_ += old;
    }
    waveBodies_.clear();
    const PlayerState& ps = w.player().state();
    const PlayerState& prev = w.previous();
    const auto& col = w.collision();
    // The water near Lara: her own when she swims, else the closest volume she could reach. Its rest surface is
    // surfaceZ (underwater caustics); the field covers every volume in its window.
    int chosen = -1;
    float best = 1e30f;
    volumes_.clear();
    const float reach = WaveWindow::kN * 0.5f * WaveWindow::kCell;
    for (size_t i = 0; i < col.markup.size(); ++i) {
        const auto& m = col.markup[i];
        if (m.type != 10 || m.points.size() < 2) continue;
        volumes_.push_back(&m);
        if (int(i) == ps.water) { chosen = int(i), best = -1; continue; }
        if (best < 0) continue;
        const vec3& lo = m.points[0];
        const vec3& hi = m.points[1];
        const float dx = std::max({lo.x - ps.pos.x, 0.0f, ps.pos.x - hi.x});
        const float dy = std::max({lo.y - ps.pos.y, 0.0f, ps.pos.y - hi.y});
        if (dx > reach || dy > reach || std::abs(hi.z - ps.pos.z) > 8000.0f) continue;
        const float score = dx + dy + 0.01f * std::abs(hi.z - ps.pos.z);
        if (score < best) best = score, chosen = int(i);
    }
    if (chosen < 0) {
        reset();
        return;
    }
    const auto& vol = col.markup[size_t(chosen)];
    const float surface = vol.points[1].z;
    // Another surface (a different rest height): its waves start from rest. The stacked boxes of one sea (l1_seatop
    // over l1_seafloor) share the height and keep the field.
    if (!active_ || std::abs(surface - surfaceZ_) > 1.0f) {
        if (active_) ++generation_, window_.clear();
        active_ = true;
    }
    chosen_ = chosen, surfaceZ_ = surface;
    depth_ = std::max(surface - vol.points[0].z, 1.0f);
    // Walls: no water volume over the cell, or terrain within 0.75 cell of its rest surface (shores, rocks, hulls,
    // pool sides). Moving bodies are not walls here (they are bodies).
    window_.follow(vec2(ps.pos), [&](vec2 p) {
        float rest = kNoWater;
        for (const collision::Markup* m : volumes_)
            if (p.x >= m->points[0].x && p.x <= m->points[1].x && p.y >= m->points[0].y && p.y <= m->points[1].y)
                rest = std::max(rest, m->points[1].z);
        if (rest == kNoWater) return false;
        const auto hit = col.closestPoint(vec3(p, rest), WaveWindow::kMaskCell * 0.75f);
        return !hit || hit->dynamic >= 0;
    });

    // A body with its feet in the water and not under it: a pressure body (its wake); swimming fast, a ring per stroke;
    // standing, a faint bob every 1.2..1.5 s (phased per body so a group does not pulse together).
    const auto wade = [&](vec3 p, vec3 last, float height, float radius, float& bob, float phase) {
        const float h = waterAt(vec2(p));
        if (h == kNoWater || p.z > h + 13.0f || p.z <= h - height) return;
        const vec2 vel = dt > 0 ? vec2(p - last) / dt : vec2(0);
        const float speed = glm::length(vel);
        if (waveBodies_.size() < 64)
            waveBodies_.push_back({p.x, p.y, radius, kDraft + kStrokeDraft * glm::smoothstep(0.0f, kStrokeSpeed, speed), vel.x, vel.y,
                                   1.0f - glm::smoothstep(kWallSlow, kWallFast, speed), 0.0f});
        if (speed >= kSwimRing) {
            if (time_ - bob >= kStroke) ripple(vec3(vec2(p), h), kStrokeRing), bob = time_;
        } else if (speed > kWallSlow) bob = time_;
        else if (time_ - bob >= 1.2f + 0.3f * phase) kick(vec2(p), -kBob, kBobSigma), bob = time_;
    };
    const bool swimming = ps.water >= 0;
    wade(ps.pos, prev.pos, kLaraHeight, swimming ? kTorso : kLegs, bob_, 0.0f);
    const float lara = waterAt(vec2(ps.pos));
    if (lara != kNoWater) {
        const vec3 at(ps.pos.x, ps.pos.y, lara);
        if (ps.water >= 0 && prev.water < 0) { // diving / falling in (also landing in it dead: no SwimSplash then)
            const float s = std::clamp(glm::length(prev.vel) / 1500.0f, 0.4f, 2.0f);
            ripple(at, s);
            emit(at + vec3(0, 0, 10), int(10 + 8 * s), 260.0f * s, 700.0f);
        }
        if (ps.state == State::SwimClimbOut && prev.state != State::SwimClimbOut) {
            ripple(at, 0.6f);
            emit(at + vec3(0, 0, 40), 6, 120.0f, 300.0f, 7.0f);
        }
        const float speed = dt > 0 ? glm::length(vec2(ps.pos - prev.pos)) / dt : 0.0f;
        if (ps.state == State::SwimSurface && speed > 200.0f && w.ticks() % 5 == 0) // spray off the strokes
            emit(at + vec3(0, 0, 8), 1, 60.0f, 120.0f, 6.0f);
    }
    // Shots through the surface (hitscan rays of this tick): a projectile falling in.
    for (const Shot& s : w.combat().shots) {
        const float h = waterAt(vec2(s.to));
        if (h == kNoWater || s.from.z <= h || s.to.z > h) continue;
        splash(s.from + (s.to - s.from) * ((s.from.z - h) / (s.from.z - s.to.z)), 1.0f);
    }
    // Enemies and animals wade / swim at the surface; one dying in the water splashes. Dynamic bodies (crates, debris)
    // splash falling in and wade floating or pushed.
    std::map<int32_t, Body> now;
    for (const auto& e : w.enemies().all()) {
        const vec3 p = e->position();
        const auto it = bodies_.find(e->uid());
        Body b{p, e->alive(), it == bodies_.end() ? -1e9f : it->second.bob};
        if (b.alive) wade(p, it == bodies_.end() ? p : it->second.pos, e->height(), kLegs, b.bob, glm::fract(float(e->uid()) * 0.618f));
        else if (it != bodies_.end() && it->second.alive) splash(p, 1.0f);
        now[e->uid()] = b;
    }
    for (const auto& d : col.dynamics) {
        const vec3 p(d.transform[3]);
        const auto it = bodies_.find(d.id);
        Body b{p, true, it == bodies_.end() ? -1e9f : it->second.bob};
        if (it != bodies_.end()) {
            const float h = waterAt(vec2(p));
            if (h != kNoWater && it->second.pos.z > h && p.z <= h) splash(vec3(vec2(p), h), 0.8f);
            else if (p != it->second.pos) wade(p, it->second.pos, kLaraHeight, kTorso, b.bob, glm::fract(float(d.id) * 0.618f));
        }
        now[d.id] = b;
    }
    bodies_ = std::move(now);

    // Droplets: ballistic; one falling back into the water leaves a pinprick.
    for (auto& d : drops_) {
        d.vel.z -= kGravity * dt;
        d.pos += d.vel * dt;
        d.life -= dt;
        if (d.vel.z < 0 && d.pos.z < surfaceZ_ - 4.0f) {
            if (d.life > 0) ripple(d.pos, 0.05f);
            d.life = 0;
        }
    }
    std::erase_if(drops_, [](const SplashDrop& d) { return d.life <= 0; });
}

} // namespace ou::game
