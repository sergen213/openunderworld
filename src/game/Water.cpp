// See Water.hpp. WaveField is Joan of Arc's water_waves.cpp with lengths x kWaterScale and z up; WaterSim is its
// world_view_water.cpp (what kicks the field) for TRU's bodies. Droplet randomness is a local xorshift, so a replay
// produces the same field and droplets.
#include "game/Water.hpp"

#include "game/Game.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ou::game {

namespace {
constexpr float S = kWaterScale;
constexpr float kGravity = 5.72443f * 900.0f; // as Tuning::gravity
constexpr size_t kMaxDrops = 256;
constexpr float kNoWater = -std::numeric_limits<float>::infinity();

// Velocity damping (1/s): amplitude falls as exp(-1.25 t), so with the 2D spreading a splash is gone in ~2-3 s and a
// small pool does not fill up with reverberating reflections (a busy, blotchy surface).
constexpr float kDamping = 2.5f;
// Viscous damping of the velocity Laplacian: a checkerboard loses 4% per substep (gone in ~0.4 s), an 8-cell wave 0.3%.
constexpr float kViscosity = 0.005f;
// Weak spring back to the rest level (omega ~0.6 rad/s, far below the ~8 rad/s of the waves): bodies wading on zero
// the cells they step into, and in a closed pool nothing else would ever drain the offset that leaves.
constexpr float kRelax = 1e-4f;
// Wading: body radius and the wake, a bow mound / stern trough pair (two kicks of width kWakeSigma just outside the
// body) deepened by kWake per unit walked each step, the speed capped at kMaxWade (a teleport is no sprint).
constexpr float kLegs = 10.0f * S, kWakeSigma = 6.0f * S, kWake = 0.3f, kMinWade = 30.0f * S, kMaxWade = 500.0f * S;
constexpr float kLaraHeight = 512.0f;
} // namespace

// ---- WaveField ----

WaveField::WaveField()
    : h_(kN * kN), prev_(kN * kN), open_(kN * kN), mask_(kN * kN), scratch_(kN * kN) {
    bodies_.reserve(64);
}

int WaveField::cellIndex(vec2 p) const {
    if (!placed_) return -1;
    const glm::ivec2 c = glm::ivec2(glm::floor(p / kCell)) - corner_;
    return c.x < 0 || c.y < 0 || c.x >= kN || c.y >= kN ? -1 : c.y * kN + c.x;
}

void WaveField::shift(std::vector<float>& a, int dx, int dy, float fill) {
    for (int y = 0; y < kN; ++y) {
        float* dst = scratch_.data() + y * kN;
        const int oy = y + dy;
        if (oy < 0 || oy >= kN) {
            std::fill(dst, dst + kN, fill);
            continue;
        }
        for (int x = 0; x < kN; ++x) dst[x] = x + dx >= 0 && x + dx < kN ? a[size_t(oy * kN + x + dx)] : fill;
    }
    a.swap(scratch_);
}

void WaveField::follow(vec2 centre, const Wet& wet) {
    const glm::ivec2 corner = glm::ivec2(glm::floor(centre / kCell)) - kN / 2;
    if (placed_ && corner == corner_) return;
    const glm::ivec2 d = corner - corner_;
    if (!placed_ || std::abs(d.x) >= kN || std::abs(d.y) >= kN) {
        std::fill(h_.begin(), h_.end(), 0.0f);
        std::fill(prev_.begin(), prev_.end(), 0.0f);
        std::fill(open_.begin(), open_.end(), -1.0f);
    } else {
        shift(h_, d.x, d.y, 0.0f);
        shift(prev_, d.x, d.y, 0.0f);
        shift(open_, d.x, d.y, -1.0f);
        // Rows 0 / kN-1 are never stepped (walls): clear what scrolled onto them.
        for (std::vector<float>* a : {&h_, &prev_}) {
            std::fill_n(a->begin(), kN, 0.0f);
            std::fill_n(a->end() - kN, kN, 0.0f);
        }
    }
    corner_ = corner, placed_ = true;
    for (int i = 0; i < kN * kN; ++i)
        if (open_[size_t(i)] < 0.0f)
            open_[size_t(i)] = wet((vec2(corner_ + glm::ivec2(i % kN, i / kN)) + 0.5f) * kCell) ? 1.0f : 0.0f;
}

void WaveField::kick(vec2 p, float amplitude, float sigma) {
    if (!placed_) return;
    if (idle_ > kQuiet) { // waking: the damped remainder is cleared
        std::fill(h_.begin(), h_.end(), 0.0f);
        std::fill(prev_.begin(), prev_.end(), 0.0f);
    }
    idle_ = 0.0f;
    const vec2 local = p / kCell - vec2(corner_); // in cells
    const float reach = 4.0f * sigma / kCell, k = kCell * kCell / (2.0f * sigma * sigma);
    const int x0 = std::max(1, int(std::floor(local.x - reach))), x1 = std::min(kN - 2, int(std::ceil(local.x + reach)));
    const int y0 = std::max(1, int(std::floor(local.y - reach))), y1 = std::min(kN - 2, int(std::ceil(local.y + reach)));
    // Free: open water, not under a body of this step (a kick at a wader's feet would otherwise lose its crater to the
    // body and leave the rim: the pool level creeps up).
    const auto free = [&](int x, int y) {
        if (open_[size_t(y * kN + x)] == 0.0f) return false;
        for (const vec3& b : bodies_)
            if (glm::distance(vec2(x, y) + 0.5f, vec2(b) / kCell - vec2(corner_)) <= b.z / kCell) return false;
        return true;
    };
    const auto shape = [&](int x, int y) {
        const vec2 o = vec2(x, y) + 0.5f - local;
        const float q = glm::dot(o, o) * k;
        return q > 8.0f ? 0.0f : -amplitude * (1.0f - q) * std::exp(-q);
    };
    // The Mexican hat clipped by walls and bodies keeps zero volume: what it lost comes off its free cells evenly.
    float volume = 0.0f;
    int cells = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (const float v = shape(x, y); v != 0.0f && free(x, y)) volume += v, ++cells;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (const float v = shape(x, y); v != 0.0f && free(x, y)) {
                // Pure displacement (h and prev alike): the crater and its rim fall back and run out as rings.
                const size_t i = size_t(y * kN + x);
                h_[i] += v - volume / float(cells), prev_[i] += v - volume / float(cells);
            }
}

void WaveField::wade(vec2 p, vec2 velocity, float dt) {
    bodies_.push_back({p, kLegs});
    const float speed = glm::length(velocity);
    if (speed < kMinWade) return;
    const vec2 ahead = velocity / speed * (kLegs + kWakeSigma);
    const float push = kWake * std::min(speed, kMaxWade) * dt;
    kick(p + ahead, -push, kWakeSigma);
    kick(p - ahead, push, kWakeSigma);
}

void WaveField::step(float dt) {
    idle_ += dt;
    if (idle_ > kQuiet || !placed_) {
        bodies_.clear();
        return;
    }
    owed_ = std::min(owed_ + dt, 0.25f); // a hitch does not run seconds of substeps
    if (owed_ >= kStep) {
        mask_ = open_;
        for (float* row : {mask_.data(), mask_.data() + (kN - 1) * kN}) std::fill_n(row, kN, 0.0f);
        for (int y = 0; y < kN; ++y) mask_[size_t(y * kN)] = mask_[size_t(y * kN + kN - 1)] = 0.0f;
        for (const vec3& b : bodies_) {
            const vec2 local = vec2(b) / kCell - vec2(corner_);
            const float r = b.z / kCell;
            for (int y = std::max(0, int(local.y - r)); y <= std::min(kN - 1, int(local.y + r)); ++y)
                for (int x = std::max(0, int(local.x - r)); x <= std::min(kN - 1, int(local.x + r)); ++x)
                    if (glm::distance(vec2(x, y) + 0.5f, local) <= r) mask_[size_t(y * kN + x)] = 0.0f;
        }
    }
    bodies_.clear();
    // Verlet: h' = h + v * damping + (c dt / cell)^2 * lap(h) + viscosity * lap(v) - relax * h, v = h - h_prev.
    // Neighbours behind a wall count as the cell itself (zero gradient into the wall), so waves reflect there without
    // a phase flip. The viscous term eats the cell-sized checkerboard that moving bodies stamp into the field.
    constexpr float a = (kSpeed * kStep / kCell) * (kSpeed * kStep / kCell), damp = 1.0f - kDamping * kStep;
    for (; owed_ >= kStep; owed_ -= kStep) {
        const float* h = h_.data();
        const float* p = prev_.data();
        const float* m = mask_.data();
        float* out = scratch_.data();
        std::fill_n(out, kN, 0.0f);
        std::fill_n(out + kN * (kN - 1), kN, 0.0f);
        for (int i = kN; i < kN * (kN - 1); ++i) {
            const float c = h[i], v = c - p[i];
            const float lap = m[i - 1] * (h[i - 1] - c) + m[i + 1] * (h[i + 1] - c) + m[i - kN] * (h[i - kN] - c) +
                              m[i + kN] * (h[i + kN] - c);
            const float lapv = m[i - 1] * (h[i - 1] - p[i - 1] - v) + m[i + 1] * (h[i + 1] - p[i + 1] - v) +
                               m[i - kN] * (h[i - kN] - p[i - kN] - v) + m[i + kN] * (h[i + kN] - p[i + kN] - v);
            out[i] = m[i] * (c + v * damp + a * lap + kViscosity * lapv - kRelax * c);
        }
        prev_.swap(h_);    // prev <- h
        h_.swap(scratch_); // h <- out
    }
}

float WaveField::height(vec2 p) const {
    const int i = cellIndex(p);
    return i < 0 ? 0.0f : h_[size_t(i)];
}

bool WaveField::wall(vec2 p) const {
    const int i = cellIndex(p);
    return i < 0 || i < kN || i >= kN * (kN - 1) || i % kN == 0 || i % kN == kN - 1 || open_[size_t(i)] == 0.0f;
}

std::span<const float> WaveField::heights() const {
    if (idle_ > kQuiet || !placed_) return {};
    return h_;
}

// ---- WaterSim: what kicks the field ----

void WaterSim::reset() {
    field_.clear();
    drops_.clear(), bodies_.clear();
    active_ = false;
}

float WaterSim::waterAt(vec2 p) const {
    float h = kNoWater;
    for (const collision::Markup* m : volumes_)
        if (p.x >= m->points[0].x && p.x <= m->points[1].x && p.y >= m->points[0].y && p.y <= m->points[1].y)
            h = std::max(h, waterSurface(*m, p, seconds_));
    return h;
}

void WaterSim::ripple(vec3 p, float strength) {
    // Strength 1 (splash): a 45-unit crater of width 64; 0.15 (a standing body's bob) 11.5 over 42; 0.6 (a weapon) 31
    // over 54 (^0.7: faint kicks still read, a crowd's bobbing does not churn the whole pool). Joan x kWaterScale.
    field_.kick(vec2(p), 7.0f * S * std::pow(strength, 0.7f), (6.0f + 4.0f * strength) * S);
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
    const PlayerState& ps = w.player().state();
    const PlayerState& prev = w.previous();
    const auto& col = w.collision();
    // The water near Lara: her own when she swims, else the closest volume she could reach. Its rest surface is
    // surfaceZ (underwater caustics); the field covers every volume in its window.
    int chosen = -1;
    float best = 1e30f;
    volumes_.clear();
    const float reach = WaveField::kN * 0.5f * WaveField::kCell;
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
    active_ = true;
    surfaceZ_ = col.markup[size_t(chosen)].points[1].z;
    // Walls: no water volume over the cell, or terrain within 0.75 cell of its rest surface (shores, rocks, hulls,
    // pool sides). Moving bodies are not walls here (they are bodies each step).
    field_.follow(vec2(ps.pos), [&](vec2 p) {
        float rest = kNoWater;
        for (const collision::Markup* m : volumes_)
            if (p.x >= m->points[0].x && p.x <= m->points[1].x && p.y >= m->points[0].y && p.y <= m->points[1].y)
                rest = std::max(rest, m->points[1].z);
        if (rest == kNoWater) return false;
        const auto hit = col.closestPoint(vec3(p, rest), WaveField::kCell * 0.75f);
        return !hit || hit->dynamic >= 0;
    });

    // A body with its feet in the water and not under it: an obstacle; moving, a wake; standing, a faint bob every
    // 1.2..1.5 s (phased per body so a group does not pulse together).
    const auto wade = [&](vec3 p, vec3 last, float height, float& bob, float phase) {
        const float h = waterAt(vec2(p));
        if (h == kNoWater || p.z > h + 2.0f * S || p.z <= h - height) return;
        const vec2 vel = dt > 0 ? vec2(p - last) / dt : vec2(0);
        field_.wade(vec2(p), vel, dt);
        if (glm::length(vel) > kMinWade) bob = time_;
        else if (time_ - bob >= 1.2f + 0.3f * phase) ripple(vec3(vec2(p), h), 0.15f), bob = time_;
    };
    wade(ps.pos, prev.pos, kLaraHeight, bob_, 0.0f);

    const float lara = waterAt(vec2(ps.pos));
    if (lara != kNoWater) {
        const vec3 at(ps.pos.x, ps.pos.y, lara);
        if (ps.state == State::SwimSplash && prev.state != State::SwimSplash) { // diving / falling in
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
        if (b.alive) wade(p, it == bodies_.end() ? p : it->second.pos, e->height(), b.bob, glm::fract(float(e->uid()) * 0.618f));
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
            else if (p != it->second.pos) wade(p, it->second.pos, kLaraHeight, b.bob, glm::fract(float(d.id) * 0.618f));
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

    field_.step(dt);
    ++version_;
}

} // namespace ou::game
