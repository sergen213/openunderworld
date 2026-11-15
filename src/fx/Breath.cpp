#include "fx/Breath.hpp"

#include <algorithm>
#include <cmath>

namespace ou::fx {

namespace {
float hash01(uint32_t x) { // lowbias32 -> [0, 1)
    x ^= x >> 16, x *= 0x7FEB352Du, x ^= x >> 15, x *= 0x846CA68Bu, x ^= x >> 16;
    return float(x >> 8) / 16777216.0f;
}
// [I] breathing: a puff every 3.2 s holding her breath (a diver lets a little out now and then), every 2.4 s on the
// regulator; 5-8 / 8-12 bubbles 50 ms apart; radii 4-12 units (1.4-4 cm at 290 per metre: an exhale's big bubbles, and
// still a few pixels from the follow camera ~4 m away); rising at 75-135 units/s (0.26-0.47 m/s, bigger ones faster)
// after a 0.25 s start-up, drifting up to 20 units/s sideways at first.
constexpr double kPeriod = 3.2, kScubaPeriod = 2.4, kGap = 0.05;
constexpr float kSpinUp = 0.25f, kDriftTime = 0.4f, kMaxAge = 12.0f;
constexpr size_t kMaxLive = 256;
glm::vec3 position(const glm::vec3& from, glm::vec2 drift, float speed, float age) {
    const float rise = speed * (age - kSpinUp * (1.0f - std::exp(-age / kSpinUp)));
    const glm::vec2 side = drift * (kDriftTime * (1.0f - std::exp(-age / kDriftTime)));
    return from + glm::vec3(side, rise);
}
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
                live_.push_back({mouth + jitter, born, 4.0f + 8.0f * a * a, hash01(pj + 41u), 75.0f + 60.0f * a * (0.5f + 0.5f * d),
                                 surfaceZ, glm::vec2(c - 0.5f, b - 0.5f) * 40.0f});
            }
        }
    }
    last_ = t;
    // Gone at the surface or after kMaxAge; the oldest first when there are too many.
    std::erase_if(live_, [&](const Live& l) {
        const float age = float(t - l.born);
        return age > kMaxAge || position(l.from, l.drift, l.speed, age).z + l.radius >= l.surface;
    });
    while (live_.size() > kMaxLive) live_.pop_front();
}

void Breath::append(double t, std::vector<render::Bubble>& out) const {
    for (const Live& l : live_) {
        const float age = std::max(0.0f, float(t - l.born));
        const glm::vec3 p = position(l.from, l.drift, l.speed, age);
        const float alpha = std::clamp((l.surface - l.radius - p.z) / 60.0f, 0.0f, 1.0f); // thins out under the surface
        if (alpha <= 0) continue;
        out.push_back({{p.x, p.y, p.z}, l.radius * std::min(1.0f, 0.6f + age * 4.0f), alpha, l.seed}); // grows as it leaves
    }
}

} // namespace ou::fx
