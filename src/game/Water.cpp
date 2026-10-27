// See Water.hpp. The field is a damped wave equation on a 256x256 grid that follows Lara in whole
// cells; impulses depress the surface and the rebound makes the rings. All randomness is a local
// xorshift, so a replay produces the same field and droplets.
#include "game/Water.hpp"

#include "game/Game.hpp"

#include <algorithm>
#include <cmath>

namespace ou::game {

namespace {
constexpr float kGravity = 5.72443f * 900.0f; // as Tuning::gravity
constexpr size_t kMaxDrops = 256;
} // namespace

void WaterSim::reset() {
    h_.clear(), prev_.clear(), next_.clear();
    drops_.clear(), moved_.clear();
    active_ = false;
    tick_ = 0;
}

bool WaterSim::inside(vec3 p, float margin) const {
    return p.x >= boxLo_.x - margin && p.x <= boxHi_.x + margin && p.y >= boxLo_.y - margin &&
           p.y <= boxHi_.y + margin;
}

void WaterSim::impulse(float x, float y, float radius, float amount) {
    if (h_.empty()) return;
    const int cx = int((x - origin_.x) / kCell), cy = int((y - origin_.y) / kCell);
    const int r = std::max(1, int(radius / kCell) + 1);
    for (int j = std::max(1, cy - r); j < std::min(kSize - 1, cy + r + 1); ++j)
        for (int i = std::max(1, cx - r); i < std::min(kSize - 1, cx + r + 1); ++i) {
            const float dx = (origin_.x + (i + 0.5f) * kCell) - x, dy = (origin_.y + (j + 0.5f) * kCell) - y;
            const float d2 = (dx * dx + dy * dy) / (radius * radius);
            if (d2 < 4.0f) h_[size_t(j) * kSize + i] += amount * std::exp(-2.0f * d2);
        }
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
    if (!active_ || !inside(pos, 100.0f)) return;
    impulse(pos.x, pos.y, 90.0f + 40.0f * strength, -45.0f * strength);
    emit(vec3(pos.x, pos.y, surfaceZ_ + 10.0f), int(8 + 6 * strength), 220.0f * strength, 550.0f * strength);
}

void WaterSim::tick(const World& w, float dt) {
    const PlayerState& ps = w.player().state();
    const PlayerState& prev = w.previous();
    const auto& markup = w.collision().markup;
    // The surface to simulate: Lara's own water when she swims, else the closest volume she could splash.
    int chosen = -1;
    float best = 1e30f;
    const float reach = kSize * 0.5f * kCell;
    for (size_t i = 0; i < markup.size(); ++i) {
        const auto& m = markup[i];
        if (m.type != 10 || m.points.size() != 2) continue;
        if (int(i) == ps.water) { chosen = int(i); break; }
        const vec3& lo = m.points[0];
        const vec3& hi = m.points[1];
        const float dx = std::max({lo.x - ps.pos.x, 0.0f, ps.pos.x - hi.x});
        const float dy = std::max({lo.y - ps.pos.y, 0.0f, ps.pos.y - hi.y});
        if (dx > reach || dy > reach || std::abs(hi.z - ps.pos.z) > 8000.0f) continue;
        const float score = dx + dy + 0.01f * std::abs(hi.z - ps.pos.z);
        if (score < best) best = score, chosen = int(i);
    }
    if (chosen < 0) {
        active_ = false;
        drops_.clear(), moved_.clear();
        return;
    }
    const vec3 lo = markup[size_t(chosen)].points[0], hi = markup[size_t(chosen)].points[1];
    if (!active_ || surfaceZ_ != hi.z) { // another surface: start calm
        h_.assign(size_t(kSize) * kSize, 0.0f);
        prev_ = next_ = h_;
        drops_.clear();
        origin_ = {1e30f, 1e30f}; // force the scroll below
    }
    active_ = true;
    boxLo_ = lo, boxHi_ = hi;
    surfaceZ_ = hi.z;

    // Follow Lara in whole cells; content scrolls with the grid.
    const vec2 want = glm::floor((vec2(ps.pos.x, ps.pos.y) - kSize * 0.5f * kCell) / kCell) * kCell;
    if (h_.empty()) {
        h_.assign(size_t(kSize) * kSize, 0.0f);
        prev_ = next_ = h_;
        origin_ = want;
    } else if (want != origin_) {
        const int sx = int((want.x - origin_.x) / kCell), sy = int((want.y - origin_.y) / kCell);
        auto scroll = [&](std::vector<float>& a) {
            std::vector<float> out(a.size(), 0.0f);
            for (int j = 0; j < kSize; ++j) {
                const int sj = j + sy;
                if (sj < 0 || sj >= kSize) continue;
                for (int i = 0; i < kSize; ++i) {
                    const int si = i + sx;
                    if (si >= 0 && si < kSize) out[size_t(j) * kSize + i] = a[size_t(sj) * kSize + si];
                }
            }
            a = std::move(out);
        };
        scroll(h_), scroll(prev_);
        origin_ = want;
    }

    // Wave step (interior cells; the border absorbs).
    const float k = (kWaveSpeed * dt / kCell) * (kWaveSpeed * dt / kCell);
    for (int j = 1; j < kSize - 1; ++j)
        for (int i = 1; i < kSize - 1; ++i) {
            const size_t n = size_t(j) * kSize + i;
            const float lap = h_[n - 1] + h_[n + 1] + h_[n - kSize] + h_[n + kSize] - 4.0f * h_[n];
            next_[n] = (2.0f * h_[n] - prev_[n] + k * lap) * kDamping;
        }
    std::swap(prev_, h_);
    std::swap(h_, next_);
    ++version_;
    ++tick_;

    const float speed = dt > 0 ? glm::length(ps.pos - prev.pos) / dt : 0.0f;
    const float surf = surfaceZ_;
    // Entry splash and climb-out drips.
    if (ps.state == State::SwimSplash && prev.state != State::SwimSplash) {
        const float s = std::clamp(glm::length(prev.vel) / 1500.0f, 0.4f, 2.0f);
        impulse(ps.pos.x, ps.pos.y, 130.0f, -55.0f * s);
        emit(vec3(ps.pos.x, ps.pos.y, surf + 10.0f), int(10 + 8 * s), 260.0f * s, 700.0f);
    }
    if (ps.state == State::SwimClimbOut && prev.state != State::SwimClimbOut) {
        impulse(ps.pos.x, ps.pos.y, 70.0f, -8.0f);
        emit(vec3(ps.pos.x, ps.pos.y, surf + 40.0f), 6, 120.0f, 300.0f, 7.0f);
    }
    switch (ps.state) {
    case State::SwimSurface:
        impulse(ps.pos.x, ps.pos.y, 70.0f, -3.0f * std::clamp(speed / 600.0f, 0.2f, 1.5f));
        if (speed > 200.0f && tick_ % 5 == 0) emit(vec3(ps.pos.x, ps.pos.y, surf + 8.0f), 1, 60.0f, 120.0f, 6.0f);
        break;
    case State::SwimTread:
        if (tick_ % 18 == 0) impulse(ps.pos.x, ps.pos.y, 60.0f, -2.0f);
        break;
    case State::SwimUnder:
        if (surf - ps.pos.z < 250.0f && speed > 60.0f && tick_ % 3 == 0) impulse(ps.pos.x, ps.pos.y, 60.0f, -1.5f);
        break;
    case State::Idle:
    case State::Walk:
    case State::Run: // wading: feet below the surface but not swimming
        if (ps.water < 0 && inside(ps.pos) && ps.pos.z < surf && surf - ps.pos.z < 400.0f && speed > 30.0f) {
            impulse(ps.pos.x, ps.pos.y, 55.0f, -2.5f * std::clamp(speed / 700.0f, 0.3f, 1.5f));
            if (tick_ % 8 == 0) emit(vec3(ps.pos.x, ps.pos.y, surf + 6.0f), 1, 70.0f, 160.0f, 6.0f);
        }
        break;
    default: break;
    }
    // Shots crossing the surface (hitscan rays of this tick).
    for (const Shot& s : w.combat().shots) {
        if ((s.from.z - surf) * (s.to.z - surf) >= 0 || s.from.z == s.to.z) continue;
        const vec3 p = s.from + (s.to - s.from) * ((surf - s.from.z) / (s.to.z - s.from.z));
        splash(p, 0.7f);
    }
    // Enemies and moving solids disturbing the surface.
    std::map<int32_t, vec3> now;
    auto body = [&](int32_t id, vec3 p) {
        now[id] = p;
        if (!inside(p, 100.0f) || std::abs(p.z - surf) > 200.0f) return;
        const auto it = moved_.find(id);
        const float moved = it == moved_.end() ? 0.0f : glm::length(p - it->second);
        if (moved > 0.5f) impulse(p.x, p.y, 60.0f, -1.5f * std::clamp(moved / 10.0f, 0.3f, 2.0f));
    };
    for (const auto& e : w.enemies().all())
        if (e->alive()) body(e->uid(), e->position());
    for (const auto& d : w.collision().dynamics) body(d.id, vec3(d.transform[3]));
    moved_ = std::move(now);

    // Droplets: ballistic, killed by the surface or old age.
    for (auto& d : drops_) {
        d.vel.z -= kGravity * dt;
        d.pos += d.vel * dt;
        d.life -= dt;
        if (d.vel.z < 0 && d.pos.z < surf - 4.0f) d.life = 0;
    }
    std::erase_if(drops_, [](const SplashDrop& d) { return d.life <= 0; });
}

} // namespace ou::game
