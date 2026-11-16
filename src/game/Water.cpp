// See Water.hpp. WaterSim decides what makes waves (after Wars and Warriors: Joan of Arc's world_view_water.cpp, same
// author) for TRU's bodies, plus the splash bursts handed to the renderer's fluid; the waves run on the GPU
// (render/WaterField.hpp). Everything here is deterministic, so a replay produces the same kicks, bodies and bursts.
#include "game/Water.hpp"

#include "game/Game.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ou::game {

namespace {
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

// ---- splash bursts (render::fluid emits them) ----

namespace {
// A crown's sheet starts kSheetLayers particles thick over `spread`: the start height that puts `count` particles of
// spacing kSplashSpacing on its ring near rest density (a denser start would burst apart under the fluid's pressure).
constexpr float kSheetLayers = 2.0f;
float sheetHeight(int count, float radius) {
    return float(count) * kSplashSpacing * kSplashSpacing / (6.2831853f * radius * kSheetLayers);
}
SplashBurst crown(vec3 at, vec3 carry, float radius, int count, float speed, float up) {
    SplashBurst b{SplashBurst::Crown, at, at.z, carry, radius, speed, up, sheetHeight(count, radius), count};
    return b;
}
} // namespace

std::vector<SplashBurst> entryBursts(vec3 at, vec3 velocity, bool headFirst) {
    // e: entry speed in units of a running jump's landing (~1500 units/s down). [I] numbers tuned by eye on l4_pool:
    // e 1 throws a ~40 cm crown (up 1070 units/s under the game's gravity), e 2 ~75 cm; a swan dive cuts the crown to a
    // ring around her hands and the cavity behind her feet throws a ~1 m jet (real dives: Worthington jets of ~1 m).
    const float e = std::clamp(-velocity.z / 1500.0f, 0.3f, 3.0f);
    const vec3 carry(vec2(velocity) * 0.3f, 0.0f);
    const float horizontal = glm::length(vec2(velocity));
    const float shrink = headFirst ? 0.6f : 1.0f, fewer = headFirst ? 0.45f : 1.0f, lower = headFirst ? 0.75f : 1.0f;
    const float radius = (90.0f + 25.0f * e) * shrink;
    std::vector<SplashBurst> out;
    out.push_back(crown(at, carry, radius, int(std::lround((500.0f + 900.0f * e) * fewer)),
                        (180.0f + 120.0f * e + 0.15f * horizontal) * (headFirst ? 0.8f : 1.0f), (650.0f + 420.0f * e) * lower));
    SplashBurst spray{SplashBurst::Spray, at, at.z, carry, radius * 0.5f, 700.0f + 300.0f * e, 0.0f, 0.6f,
                      int(std::lround(150.0f * e * fewer))};
    out.push_back(spray);
    if (e >= 0.7f) {
        SplashBurst jet{SplashBurst::Jet, at, at.z, carry * 0.3f, 22.0f, 60.0f, (800.0f + 350.0f * e) * (headFirst ? 1.15f : 1.0f),
                        60.0f, int(std::lround((250.0f + 350.0f * e) * (headFirst ? 1.6f : 1.0f)))};
        jet.delay = 0.18f + 0.06f * e; // the cavity's collapse
        out.push_back(jet);
    }
    return out;
}

std::vector<SplashBurst> impactBursts(vec3 at, float strength) {
    const float s = std::clamp(strength, 0.1f, 3.0f);
    std::vector<SplashBurst> out;
    out.push_back(crown(at, vec3(0), 30.0f + 40.0f * s, int(std::lround(120.0f + 380.0f * s)), 120.0f + 80.0f * s,
                        500.0f + 300.0f * s));
    out.push_back({SplashBurst::Spray, at, at.z, vec3(0), 10.0f + 10.0f * s, 500.0f, 0.0f, 0.5f, int(std::lround(40.0f * s))});
    if (s >= 1.0f) {
        SplashBurst jet{SplashBurst::Jet, at, at.z, vec3(0), 12.0f + 6.0f * s, 30.0f, 650.0f + 100.0f * s, 30.0f,
                        int(std::lround(80.0f * s))};
        jet.delay = 0.12f;
        out.push_back(jet);
    }
    return out;
}

// ---- WaterSim: what makes waves ----

void WaterSim::reset() {
    window_.clear();
    bursts_.clear(), pending_.clear(), bodies_.clear(), waveBodies_.clear();
    plunged_ = false;
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

void WaterSim::release(std::vector<SplashBurst> bursts) {
    for (SplashBurst& b : bursts) {
        if (b.count <= 0) continue;
        if (b.delay > 0) {
            b.delay += time_; // release time
            pending_.push_back(b);
            continue;
        }
        b.serial = ++serial_;
        bursts_.push_back(b);
    }
    if (bursts_.size() > kBurstKeep) bursts_.erase(bursts_.begin(), bursts_.end() - ptrdiff_t(kBurstKeep));
}

void WaterSim::splash(vec3 pos, float strength) {
    if (!active_ || !world_) return;
    const float h = waterAt(vec2(pos));
    if (h == kNoWater || pos.z > h + 100.0f) return; // only at / below the surface
    ripple(vec3(pos.x, pos.y, h), strength);
    release(impactBursts(vec3(pos.x, pos.y, h), strength));
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
        // Entering the water: the tick her water volume becomes set (diving / falling in; also landing in it dead, as a
        // fatal plunge into l4_pool's eitr does, with no SwimSplash). Once per entry (plunged_ until she is out again);
        // only a plunge (falling faster than 300 units/s) throws water, a wade-in just rings it.
        if (ps.water >= 0 && prev.water < 0 && !plunged_) {
            const float s = std::clamp(glm::length(prev.vel) / 1500.0f, 0.4f, 2.0f);
            ripple(at, s);
            if (prev.vel.z < -300.0f) release(entryBursts(at, prev.vel, prev.clip.starts_with("ng_inair_swan")));
            plunged_ = true;
        }
        if (ps.state == State::SwimClimbOut && prev.state != State::SwimClimbOut) {
            ripple(at, 0.6f);
            release({{SplashBurst::Spray, at + vec3(0, 0, 40), lara, vec3(0), 40.0f, 150.0f, 0.0f, 1.2f, 160}});
        }
        const vec2 swim = dt > 0 ? vec2(ps.pos - prev.pos) / dt : vec2(0);
        if (ps.state == State::SwimSurface && glm::length(swim) > 200.0f && w.ticks() % 5 == 0) // spray off the strokes
            release({{SplashBurst::Spray, at + vec3(0, 0, 8), lara, vec3(swim * 0.5f, 0.0f), 20.0f, 300.0f, 0.0f, 0.7f, 24}});
    }
    if (ps.water < 0) plunged_ = false;
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

    // Delayed bursts (jets) whose time has come.
    std::vector<SplashBurst> due;
    std::erase_if(pending_, [&](SplashBurst& b) {
        if (b.delay > time_) return false;
        b.delay = 0;
        due.push_back(b);
        return true;
    });
    release(std::move(due));
}

} // namespace ou::game
