#include "fx/Weather.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ou::fx {
namespace {

template <class T> T rd(std::span<const uint8_t> d, size_t o) {
    T v;
    std::memcpy(&v, d.data() + o, sizeof v);
    return v;
}
constexpr int32_t kNone = -8000000;
// 0x53A550's lightning light intensities (0x8A3804, x 1/4096 per 30 Hz frame; -1 ends).
constexpr int16_t kFlicker[26] = {1300, 800, 1400, 1500, 1400, 1300, 0, 2600, 1700, 1400, 1000, 2800, 4000,
                                  3100, 1400, 0, 1100, 1400, 1300, 1000, 600, 800, 500, 400, 0, -1};
// tru.exe's view space (0xE804A0, 0x49F900) scales x by 0x5C59F0 = (4 / 3) / aspect and y by 7 / 6 [I: the y scale
// from one 16:9 Wine read; its aspect dependence is not checked]. Screen y points down.
constexpr float kViewY = 224.0f / 192.0f;
float viewX(float aspect) { return (4.0f / 3.0f) / aspect; }
uint32_t rgbaOf(uint32_t argb) { // D3D 0xAARRGGBB -> FxVertex bytes R, G, B, A
    return ((argb >> 16) & 0xFF) | (argb & 0xFF00) | ((argb & 0xFF) << 16) | (argb & 0xFF000000);
}

} // namespace

std::optional<WeatherDef> parseWeatherDef(std::span<const uint8_t> d) {
    if (d.size() < kWeatherDefSize) return std::nullopt;
    WeatherDef w;
    for (int k = 0; k < 3; ++k) w.textures[size_t(k)] = rd<uint32_t>(d, 4 * size_t(k));
    for (int k = 0; k < 4; ++k) w.levels[size_t(k)] = rd<float>(d, 0x0C + 4 * size_t(k));
    w.minWidth = rd<float>(d, 0x20), w.width = rd<float>(d, 0x24), w.length = rd<float>(d, 0x28);
    w.nearZ = rd<float>(d, 0x2C), w.farZ = rd<float>(d, 0x30), w.wind = rd<float>(d, 0x34);
    w.colourA = rd<uint32_t>(d, 0x38), w.colourB = rd<uint32_t>(d, 0x3C), w.farColour = rd<uint32_t>(d, 0x40);
    w.alpha = rd<uint32_t>(d, 0x44), w.flags = d[0x48];
    w.flashColour = rd<uint32_t>(d, 0x4C), w.flashLight = rd<uint32_t>(d, 0x50), w.flashRadius = rd<float>(d, 0x54);
    w.flashDistance = float(rd<int32_t>(d, 0x64)), w.flashHeight = float(rd<int32_t>(d, 0x68));
    return w;
}

int32_t Weather::rand() {
    seed_ = seed_ * 214013u + 2531011u;
    return int32_t((seed_ >> 16) & 0x7FFF);
}

void Weather::setScript(int32_t rainPercent, int32_t lightningMin, int32_t lightningMax) {
    // 0x53AB30: rain stopping sets music variable 2 to 0; rain starting from none sets it to 1 (overwritten by the same
    // frame's 0x545950), restarts the camera-motion reference and runs event 0x2E on every instance (started()).
    const int32_t rain = rainPercent * 0x400 / 100;
    started_ = rain && !rain_;
    if (started_) ticks_ = 0;
    if (!rain && rain_) musicVar_ = 0;
    rain_ = rain;
    if (!rain_) ramp_ = 0;
    if (lightningMin * 30 != lightningMin_ || lightningMax * 30 != lightningMax_) { // 0x52E080
        if (lightningMin) timer_ = step_ = kind_ = 0;
        lightningMin_ = lightningMin * 30, lightningMax_ = lightningMax * 30;
    }
}

void Weather::clear() {
    drops_ = {};
    live_ = 0;
    ramp_ = timer_ = kind_ = step_ = 0;
    flashOn_ = false;
    ticks_ = 0;
    events_.clear();
}

// 0x539C00: a point of the 512 x 448 virtual screen at view depth `depth` (projection distance 192 / tan(fov / 2)).
glm::vec3 Weather::fromScreen(const Camera& cam, float sx, float sy, float depth) const {
    const float f = depth * cam.tanHalfFovY / 192.0f;
    return cam.eye + cam.right * ((sx - 256.0f) / viewX(cam.aspect) * f) + cam.down * ((sy - 224.0f) * f) + cam.forward * depth;
}

void Weather::tick(const WeatherDef* def, const Camera& cam, const World& world, bool cinematic) {
    events_.clear();
    eyeDelta_ = ticks_++ ? cam.eye - prevEye_ : glm::vec3(0);
    prevEye_ = cam.eye;
    if (rain_) musicVar_ = uint8_t((world.unitFlags & 2) != 0); // 0x545950 every rain frame
    if (!def) return; // 0x546FC0: no weather object, no weather
    if (live_) // 0x52DFA0, one 60 Hz step
        for (auto& d : drops_) {
            if (!d.life) continue;
            const int16_t life = int16_t(std::max(0, d.life - (d.life > 0)));
            d.z += d.vz;
            if (d.z > d.ground && life) d.life = life, d.x += d.vx, d.y += d.vy;
            else d.life = 0, --live_;
        }
    if (rain_) { // 0x545950: fade in, lightning timer, refill
        if (ramp_ < 0x1000) ramp_ = std::min(0x1000, ramp_ + 16); // timeMult x 4096 x 32 / 4096 per 60 Hz frame
        lightning(def, cam, cinematic);
        spawn(*def, cam, world);
    }
    if (kind_ && (ticks_ & 1)) { // 0x53A550 on 30 Hz frames
        int32_t v = -1;
        switch (kind_) {
        case 5: v = step_ < 24 ? kFlicker[24 - step_] : -1; break;
        case 6: v = step_ <= 5 ? kFlicker[step_] : -1; break;
        case 2: case 3: case 4: v = kFlicker[step_ > 24 ? step_ - 25 : step_]; break; // twice
        default: v = kFlicker[step_]; break;
        }
        if (!flashOn_ || v == -1) kind_ = step_ = 0, flashOn_ = false;
        else intensity_ = float(v) / 4096.0f, ++step_;
    }
    if (rain_) splashes(cam, world);
}

// 0x545950: refill the 512 drops at up to 32 a frame, at the top of the view (screen y 5), below the precipitation
// height map only; a drop that would start under a roof retries once anywhere in view.
void Weather::spawn(const WeatherDef& def, const Camera& cam, const World& world) {
    const int32_t pitch = int32_t(std::asin(std::clamp(cam.forward.z, -1.0f, 1.0f)) * 4096.0f); // camera rot.x x 4096
    float depthScale = 1;
    if (pitch < -1500) depthScale = std::max(0.28f, (float(pitch) + 4100.0f) / 2600.0f);
    if (pitch > 1500) depthScale = std::max(0.6f, (4100.0f - float(pitch)) / 2600.0f);
    const int32_t spread = pitch < -256 ? std::min(0x4B0, (-256 - pitch) * 0x4B0 / 0xF00) : 0; // looking down: wider
    // Unit flag 0x20000: drops start up to 18000 deep (rand % 18000) and speed up beyond 8192 (x depth / 8192), else
    // up to 0x1FFF and beyond 4096 (x depth / 4096).
    const bool deep = world.unitFlags & 0x20000;
    const auto depthRand = [&] { return float(deep ? rand() % 18000 : rand() & 0x1FFF) * depthScale; };
    const size_t n = std::min<size_t>(kDrops - live_, 32);
    for (size_t k = 0; k < n; ++k) {
        float sx = float(rand() % (spread + 512) - spread / 2), depth = depthRand();
        glm::vec3 p = fromScreen(cam, sx, 5, depth);
        int32_t ground = world.ceiling ? world.ceiling(p) : kNone;
        if (const auto w = world.water ? world.water(p) : std::nullopt; w && int32_t(*w) > ground) ground = int32_t(*w);
        if (ground == kNone || float(ground) > p.z) {
            sx = float(rand() & 0x1FF);
            const float sy = float(rand() % 0x1C0);
            depth = depthRand();
            p = fromScreen(cam, sx, sy, depth);
            ground = world.ceiling ? world.ceiling(p) : kNone;
            if (ground == kNone || float(ground) > p.z) continue;
        }
        int32_t vz = (-14 - (rand() & 3)) * 8;
        if (const float far = deep ? 8192.0f : 4096.0f; depth > far) vz = int32_t(depth) * vz / int32_t(far); // far drops fall faster
        const auto slot = std::find_if(drops_.begin(), drops_.end(), [](const Drop& d) { return !d.life; });
        if (slot == drops_.end()) return;
        const glm::vec2 wind = world.wind ? world.wind(p) * def.wind : glm::vec2(0);
        *slot = {int32_t(p.x), int32_t(p.y), int32_t(p.z), ground, int16_t(wind.x), int16_t(wind.y), int16_t(vz / 3),
                 int16_t(30 - int32_t(depth * -45.0f / 8000.0f)), uint8_t(rand() & 3)};
        ++live_;
    }
}

// 0x545950's lightning timer -> 0x53A650: a far light (and the weather object's event 0x30) every lightningMin..Max.
void Weather::lightning(const WeatherDef* def, const Camera& cam, bool cinematic) {
    if (timer_ >= 1 || flashOn_ || !lightningMin_) {
        timer_ -= int32_t(ticks_ & 1); // 30 Hz frames
        return;
    }
    kind_ = rand() % 6 + 1, step_ = 0;
    if (!cinematic) {
        flashOn_ = (def->flags >> 1) & 1;
        if (flashOn_ && def->flashLight) {
            // [I] the light stands flashDistance ahead of the camera (horizontally) and flashHeight above it.
            const glm::vec2 ahead = glm::length(glm::vec2(cam.forward)) > 1e-4f ? glm::normalize(glm::vec2(cam.forward)) : glm::vec2(0, 1);
            flashPos_ = cam.eye + glm::vec3(ahead * def->flashDistance, def->flashHeight);
            flashColour_ = glm::vec3(def->flashColour & 0xFF, (def->flashColour >> 8) & 0xFF, (def->flashColour >> 16) & 0xFF) / 255.0f;
            flashRadius_ = def->flashRadius, intensity_ = 0;
            events_.push_back({0x30, flashPos_, 0});
        }
    }
    const uint32_t r = uint32_t(rand()) << 15 | uint32_t(rand()); // [I] 0x597780 is another generator
    timer_ = int32_t(r % uint32_t(std::max(1, lightningMax_ - lightningMin_ + 1))) + lightningMin_;
}

std::optional<Weather::Flash> Weather::flash() const {
    if (!flashOn_ || flashRadius_ <= 0) return std::nullopt;
    return Flash{flashPos_, flashColour_ * intensity_, flashRadius_};
}

// 0x53A860: rain hitting the ground in view (event 0x2E) and, half as often, behind the camera (0x2F): a vertical
// screen line at a random depth meets the height map, a segment there finds the surface material.
void Weather::splashes(const Camera& cam, const World& world) {
    int32_t n = int32_t((int64_t(ramp_) * rain_ * 10) >> 22);
    for (int pass = 0; pass < 2; ++pass, n /= 2) {
        const float sign = pass ? -1.0f : 1.0f, y1 = pass ? 448.0f : 0.0f, y2 = pass ? 0.0f : 448.0f;
        const uint8_t ev = pass ? 0x2F : 0x2E;
        for (int32_t k = 0; k < n; ++k) {
            const float depth = float((rand() & 0xFFF) + 0x200) * sign, sx = float(rand() & 0x1FF);
            const glm::vec3 q = fromScreen(cam, sx, y1, depth), p = fromScreen(cam, sx, y2, depth);
            const int32_t ground = world.ceiling ? world.ceiling(p) : kNone;
            if (const auto w = world.water ? world.water(p) : std::nullopt; w && int32_t(*w) > ground) {
                events_.push_back({ev, {p.x, p.y, *w}, 0});
                continue;
            }
            if (ground == kNone || q.z < float(ground) || !world.ray) continue;
            if (const auto hit = world.ray({q.x, q.y, float(ground) + 128}, {p.x, p.y, float(ground) - 128}))
                events_.push_back({ev, hit->first, hit->second});
        }
    }
}

// 0x5346A0: each drop a quad from its head to its tail (velocity x length back, stretched by the camera's motion)
// in tru.exe's view space, max(width x 512 / depth, minWidth) wide. Drops below the split index use the denser tier's
// texture, the rest the next one: the split moves with the rain amount (flags & 1, or the lightest tier: half and
// half, cross-faded by alpha). Additive [I: device draw flags 0x611045 not decoded; black sheets].
void Weather::build(const WeatherDef& def, const Camera& cam, std::vector<render::FxVertex>& vertices,
                    std::vector<render::FxBatch>& batches) const {
    if (!live_) return;
    const float t = float(rain_) / 1024.0f;
    const auto& L = def.levels;
    uint32_t split = 0, rest = 0;
    float lo = 0, hi = 0, frac = 1, range = 1;
    if (t > L[3]) split = rest = def.textures[0];
    else if (t > L[2]) split = def.textures[1], rest = def.textures[0], lo = L[2], hi = L[3];
    else if (t > L[1]) split = def.textures[2], rest = def.textures[1], lo = L[1], hi = L[2];
    else if (t > L[0]) rest = def.textures[2], lo = L[0], hi = L[1];
    else return;
    if (t <= L[3]) range = hi - lo > 0 ? hi - lo : 1, frac = t - lo;
    const uint32_t a = def.alpha >> 24, low = def.alpha & 0xFFFFFF;
    uint32_t aA = a, aB = a;
    size_t splitCount = 0;
    if (!split || (def.flags & 1)) {
        splitCount = split ? live_ / 2 : 0;
        const float f = frac / range;
        aA = uint32_t(float(a) * (1 - f)), aB = uint32_t(float(a) * f);
    } else {
        splitCount = size_t(std::max(0.0f, (1 - frac / range) * float(live_)));
    }
    const uint32_t colourA = def.colourA | aA << 24 | low, colourB = def.colourB | aB << 24 | low;
    const float ax = viewX(cam.aspect);
    auto toView = [&](glm::vec3 p) {
        p -= cam.eye;
        return glm::vec3(ax * glm::dot(cam.right, p), kViewY * glm::dot(cam.down, p), glm::dot(cam.forward, p));
    };
    auto toWorld = [&](glm::vec3 v) { return cam.eye + cam.right * (v.x / ax) + cam.down * (v.y / kViewY) + cam.forward * v.z; };
    static constexpr float kUv[4][4][2] = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}, {{1, 0}, {0, 0}, {0, 1}, {1, 1}},
                                           {{0, 1}, {1, 1}, {1, 0}, {0, 0}}, {{1, 1}, {0, 1}, {0, 0}, {1, 0}}};
    std::vector<render::FxVertex> sheets[2]; // A (split texture) then B (rest), as the exe draws them
    size_t remaining = live_;
    for (size_t i = 0; i < kDrops && remaining; ++i) {
        const Drop& d = drops_[i];
        if (!d.life) continue;
        --remaining;
        if (!d.vz) continue;
        const glm::vec3 head(float(d.x), float(d.y), float(d.z));
        const glm::vec3 tail = head - glm::vec3(d.vx, d.vy, d.vz) * def.length + eyeDelta_;
        const glm::vec3 h = toView(head), tv = toView(tail);
        if (!(tv.z > def.nearZ) || !(def.farZ > 0)) continue;
        const bool useA = i < splitCount || !rest;
        uint32_t colour = useA ? colourA : colourB;
        const float f = tv.z / def.farZ;
        colour |= uint32_t(float((def.farColour >> 16) & 0xFF) * f) << 16 | uint32_t(float((def.farColour >> 8) & 0xFF) * f) << 8 |
                  uint32_t(float(def.farColour & 0xFF) * f) | (def.farColour & 0xFF000000);
        const glm::vec3 dir = glm::normalize(tv - h);
        // ponytail: a head behind the camera takes minWidth (the exe divides by its depth as it is)
        const glm::vec3 off = dir * (h.z > 0 ? std::max(def.width * 512.0f / h.z, def.minWidth) : def.minWidth);
        const glm::vec3 side(off.y, off.x, 0); // the exe's (y, x) swap, not a true perpendicular
        const glm::vec3 q[4] = {toWorld(h - side), toWorld(h + side), toWorld(tv + side), toWorld(tv - side)};
        const uint32_t rgba = rgbaOf(colour);
        auto& out = sheets[useA ? 0 : 1];
        for (const int k : {0, 1, 2, 0, 2, 3})
            out.push_back({{q[k].x, q[k].y, q[k].z}, {kUv[d.variant & 3][k][0], kUv[d.variant & 3][k][1]}, rgba});
    }
    for (int s = 0; s < 2; ++s) {
        if (sheets[s].empty()) continue;
        render::FxBatch b;
        b.first = uint32_t(vertices.size()), b.count = uint32_t(sheets[s].size());
        b.textureId = s == 0 && split ? split : rest, b.blend = 1;
        vertices.insert(vertices.end(), sheets[s].begin(), sheets[s].end());
        batches.push_back(b);
    }
}

} // namespace ou::fx
