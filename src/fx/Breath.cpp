#include "fx/Breath.hpp"

#include <algorithm>
#include <cmath>

namespace ou::fx {

namespace {
float hash01(uint32_t x) { // lowbias32 -> [0, 1)
    x ^= x >> 16, x *= 0x7FEB352Du, x ^= x >> 15, x *= 0x846CA68Bu, x ^= x >> 16;
    return float(x >> 8) / 16777216.0f;
}
float smooth(float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
// [I] breathing: a puff every 3.2 s holding her breath (a diver lets a little out now and then), every 2.4 s on the
// regulator; 5-8 / 8-12 bubbles 50 ms apart. Each bubble: abyssal-garden world.js's per-bubble attributes (MIT) — size
// rr(0.025, 0.08) m (the demo's small branch; its 24 % of 0.1-0.3 m vent bubbles would be wider than Lara's head),
// rise speed rr(1.1, 2.4) (0.6 + 3 size) m/s, wobble rr(0.05, 0.16) m (the shader's sway, uw_bubble.vert) — rising
// straight up from the mouth (vsBub: a.xyz + h) until 0.2 m under the surface (b.z = -y0 - 0.2), its size
// a.w (0.55 + 0.7 life) ss(0, 0.03, life) over life = h / b.z.
constexpr double kPeriod = 3.2, kScubaPeriod = 2.4, kGap = 0.05;
constexpr float kMetre = 290.0f; // TRU units per metre (render/Ocean.hpp ocean::kMetre)
constexpr size_t kMaxLive = 256;
} // namespace

void Breath::tick(double t, const glm::vec3& mouth, bool submerged, bool scuba, float surfaceZ) {
    if (last_ < 0 || t < last_ || t - last_ > 1.0) { // first step, load or a long stall: no backlog
        if (t < last_) live_.clear();
        last_ = t;
        return;
    }
    if (submerged && mouth.z < surfaceZ) {
        const double period = scuba ? kScubaPeriod : kPeriod;
        const uint32_t salt = scuba ? 0x5C0BA000u : 0u;
        for (int64_t k = int64_t(std::floor(last_ / period)) - 1; k <= int64_t(std::floor(t / period)); ++k) {
            const uint32_t pk = uint32_t(k) * 0x9E3779B9u + salt;
            const double at = double(k) * period + period * 0.25 * hash01(pk);
            const int n = scuba ? 8 + int(hash01(pk + 1u) * 5.0f) : 5 + int(hash01(pk + 1u) * 4.0f);
            for (int j = 0; j < n; ++j) {
                const double born = at + double(j) * kGap;
                if (born <= last_ || born > t) continue;
                const uint32_t pj = pk * 31u + uint32_t(j) * 0x85EBCA6Bu;
                const float a = hash01(pj), b = hash01(pj + 7u), c = hash01(pj + 13u), d = hash01(pj + 29u);
                const glm::vec3 jitter(8.0f * (b - 0.5f), 8.0f * (c - 0.5f), 4.0f * (d - 0.5f));
                const float size = 0.025f + 0.055f * a, speed = (1.1f + 1.3f * hash01(pj + 53u)) * (0.6f + size * 3.0f);
                const glm::vec3 from = mouth + jitter;
                live_.push_back({from, born, size, hash01(pj + 41u), speed, 0.05f + 0.11f * hash01(pj + 67u),
                                 (surfaceZ - from.z) / kMetre - 0.2f});
            }
        }
    }
    last_ = t;
    // Gone 0.2 m under the surface (life 1); the oldest first when there are too many.
    std::erase_if(live_, [&](const Live& l) { return l.range <= 0.0f || l.speed * float(t - l.born) >= l.range; });
    while (live_.size() > kMaxLive) live_.pop_front();
}

void Breath::append(double t, std::vector<render::Bubble>& out) const {
    for (const Live& l : live_) {
        const float h = l.speed * std::max(0.0f, float(t - l.born)), life = h / l.range;
        if (life >= 1.0f) continue;
        const float radius = l.size * (0.55f + 0.7f * life) * smooth(0.0f, 0.03f, life) * kMetre;
        out.push_back({{l.from.x, l.from.y, l.from.z + h * kMetre}, radius, 1.0f, l.seed, h, l.wobble});
    }
}

} // namespace ou::fx
