#include "fx/Weather.hpp"
#include "fx/Fx.hpp"

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

void Weather::setSnow(int32_t percent) {
    snow_ = percent * 0x400 / 100; // 0x52E130: snow clears the rain (music variable 2 to 0 when it was raining)
    if (percent) {
        if (rain_) musicVar_ = 0;
        rain_ = ramp_ = 0;
    }
}

void Weather::clear() {
    drops_ = {};
    live_ = 0;
    flakeSlots_ = {};
    flakes_ = 0;
    ramp_ = timer_ = kind_ = step_ = 0;
    rain_ = snow_ = 0; // 0x539A60: the next setScript / setSnow starts them afresh (rain from none runs event 0x2E)
    lightningMin_ = 600, lightningMax_ = 900;
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
    exeOrigin_ = world.exeOrigin;
    if (rain_) musicVar_ = uint8_t((world.unitFlags & 2) != 0); // 0x545950 every rain frame
    if (!def) return; // 0x546FC0: no weather object, no weather
    if (flakes_) snowFall(cam); // 0x546FC0's order: flakes fall, spawn, then the drops
    if (snow_) {
        musicVar_ = uint8_t((world.unitFlags & 2) != 0); // 0x539EF0 every snow frame
        snowSpawn(cam, world);
    }
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
    if (flakes_ && !cinematic) snowCull(cam); // the draw (0x534DF0, skipped in cinematics) right after this tick
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
    const size_t n = std::min<size_t>(kDrops - live_ - flakes_, 32); // drops and flakes share the 512 records
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

namespace {
// tru.exe's ftol (0x814D20, truncating) of a flake coordinate, kept through movsx as an s16 (0x539EF0, 0x539CD0).
int32_t flakeCoord(float v) { return int32_t(int16_t(int32_t(v))); }
// 0x52EF50: cos of a 4096-unit angle, x 4096 (the table at 0x8A0840).
int32_t cos12(int32_t a) { return int32_t(std::lround(std::cos(float(a & 0xFFF) * (6.2831853f / 4096.0f)) * 4096.0f)); }
uint8_t twinkle(uint8_t v) { const int32_t n = v + 8; return uint8_t(n > 0xFF ? n - 0xFF : n); }
} // namespace

// 0x539CD0, one 60 Hz frame: falling flakes drop by +0x14 and sway (the phase walks by rand & 63 - fall / 2); drifting
// ones move by (+0x10, +0x12). A flake whose z reaches its ground + 32 (0x534DF0 sets z = ground when it leaves the
// view) starts again at a random point of the view (0x52DF00 -> 0x539C00) while snow is on, else ends. tru.exe calls the
// precipitation height map there (0x5C32C0) but drops its result: only the point's own z must exceed -29968 in its
// frame, so flakes fall through roofs [O: asm 0x539E18 .. 0x539E2F].
void Weather::snowFall(const Camera& cam) {
    for (auto& f : flakeSlots_) {
        if (!f.life) continue;
        if (!f.drift) {
            f.z += f.vz;
            f.phase = int16_t(f.phase + (rand() & 0x3F) - f.vz / 2);
            if (!f.a) f.b = twinkle(f.b);
            else f.a = twinkle(f.a);
        } else {
            f.x += f.phase, f.y += f.drift;
        }
        if (f.z <= f.ground + 32) {
            if (!snow_) { f.life = 0, --flakes_; continue; }
            glm::vec3 p(0);
            bool placed = false;
            for (int attempt = 0; attempt < 2 && !placed; ++attempt) {
                const float sx = float(rand() & 0x1FF), sy = float(rand() % 0x1C0), depth = float(rand() % 0x3000 + 0x400);
                p = fromScreen(cam, sx, sy, depth) - exeOrigin_;
                placed = p.z >= -29968.0f;
            }
            if (!placed) { f.life = 0, --flakes_; continue; }
            f.x = flakeCoord(p.x), f.y = flakeCoord(p.y), f.z = flakeCoord(p.z), f.ground = -30000;
            f.variant = uint8_t(rand() & 7);
            if (f.drift) f.a = f.b = 0; // 0x52DF50
            else if ((rand() & 0xFFF) < 0xC00) f.a = 0, f.b = uint8_t((rand() & 0x7F) + 0x7F);
            else f.b = 0, f.a = uint8_t((rand() & 0x7F) + 0x7F);
        }
        if (f.life > 0) --f.life;
        if (f.life <= 0) f.life = 0, --flakes_;
    }
}

// 0x539EF0: up to snow / 2 flakes (512 at 100 %), at most 20 + |yaw change| x 4096 / 2 (<= 50) new a frame, at the top
// of the view (screen y 5; anywhere while the camera turns by more than 16 / 4096 rad a frame), 1024 .. 13311 deep,
// falling 14 .. 17 units a frame for 300 frames. 80 % of those nearer than 1536 (while the camera's z is above -29872)
// get screen coordinates and a screen drift instead; 0x534DF0 draws them unprojected, so they leave the +-1.1 view
// test at once and only hold their record (and turn up again as invisible respawns) [O: asm 0x53A17E .. 0x53A2A6].
void Weather::snowSpawn(const Camera& cam, const World& world) {
    const float yaw = std::atan2(cam.forward.x, cam.forward.y);
    float turn = std::abs(yaw - prevYaw_);
    if (turn > 3.14159265f) turn = 6.2831853f - turn;
    turn *= 4096.0f;
    prevYaw_ = yaw;
    int32_t n = snow_ / 2 - int32_t(flakes_);
    if (n <= 0) return;
    n = std::min(n, std::min(50, 20 - int32_t(turn * -0.5f)));
    const bool nearDrift = cam.eye.z - world.exeOrigin.z >= -29872.0f;
    for (int32_t k = 0; k < n; ++k) {
        int16_t phase = int16_t(rand() & 0xFFF), drift = 0, vz = int16_t(-14 - (rand() & 3));
        glm::vec3 p(0), screen(0);
        bool placed = false;
        for (int attempt = 0; attempt < 2 && !placed; ++attempt) {
            screen.x = float(rand() & 0x1FF);
            screen.y = turn < 16.0f ? 5.0f : float(rand() % 0x1C0);
            screen.z = float(rand() % 0x3000 + 0x400);
            p = fromScreen(cam, screen.x, screen.y, screen.z) - world.exeOrigin;
            placed = attempt == 0 ? p.z >= -29968.0f : p.z > -29968.0f;
        }
        if (!placed) continue;
        int32_t ground = -30000;
        if (nearDrift && screen.z < 1536.0f && (rand() & 0x3FF) > 200) {
            const float d = screen.z - 1024.0f;
            phase = int16_t(int32_t(d / 32.0f - 20.0f - float(rand() & 3)));
            drift = int16_t(int32_t(14.0f - d / 64.0f + float(rand() & 3)));
            vz = 0;
            p.x = float(rand() & 0x3FF), p.y = 0;
            if (p.x >= 512.0f) p.x = 511.0f, p.y = float(rand() % 0x1C0);
            p.z = float((rand() & 0x3FF) + 0x480);
            ground = int32_t(p.z - 448.0f);
        }
        if (live_ + flakes_ >= kDrops) return; // 0x52DE50: no free record
        Flake& f = *std::find_if(flakeSlots_.begin(), flakeSlots_.end(), [](const Flake& s) { return !s.life; });
        f.x = flakeCoord(p.x), f.y = flakeCoord(p.y), f.z = flakeCoord(p.z), f.ground = ground;
        f.phase = phase, f.drift = drift, f.vz = vz, f.life = kFlakeLife;
        f.variant = uint8_t(rand() & 7);
        if (f.drift) f.a = f.b = 0;
        else if ((rand() & 0xFFF) < 0xC00) f.a = 0, f.b = uint8_t((rand() & 0x7F) + 0x7F);
        else f.b = 0, f.a = uint8_t((rand() & 0x7F) + 0x7F);
        ++flakes_;
    }
}

// 0x534DF0's position and view test: a falling flake at (x + cos(phase) / 128, y - cos(phase) / 128, z), projected; a
// drifting one takes its coordinates as projected ones. Kept only within +-1.1 of the view and 1024 .. 12288 deep.
bool Weather::flakeView(const Flake& f, const Camera& cam, glm::vec3& view) const {
    if (f.drift) view = glm::vec3(float(f.x), float(f.y), float(f.z));
    else {
        const float c = float(cos12(f.phase)) / 128.0f;
        const glm::vec3 rel = glm::vec3(float(f.x) + c, float(f.y) - c, float(f.z)) + exeOrigin_ - cam.eye;
        const float depth = glm::dot(cam.forward, rel), proj = 192.0f / cam.tanHalfFovY;
        if (!(depth > 0)) return false;
        // [I] 0x49F730 is taken to project through the same scaled view space as the rain sheets (y x kViewY), so
        // y = +-1 is the screen's edge; the quad's own height uses 0x401910 = 1 [O]
        view = {viewX(cam.aspect) * glm::dot(cam.right, rel) * proj / (256.0f * depth),
                kViewY * glm::dot(cam.down, rel) * proj / (224.0f * depth), depth};
    }
    return !(std::abs(view.x) > 1.1f || std::abs(view.y) > 1.1f || view.z < 1024.0f || view.z > 12288.0f);
}

void Weather::snowCull(const Camera& cam) {
    glm::vec3 v;
    for (auto& f : flakeSlots_)
        if (f.life && !flakeView(f, cam, v)) f.z = f.ground;
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
    if (flakes_) { // 0x534DF0: a quad of particle.drm model 0 frame 3 (0x52B0B0(0x8AA77C, 0, 3): the glow texture) per flake
        const float proj = 192.0f / cam.tanHalfFovY, ax = viewX(cam.aspect);
        std::vector<render::FxVertex> quads;
        for (const Flake& f : flakeSlots_) {
            glm::vec3 v;
            if (!f.life || f.z <= f.ground + 32 || !flakeView(f, cam, v)) continue;
            // half size in virtual pixels: 12 x proj / depth + 0.5 (depth floored at 1024), at most 2.5; the twinkle bytes
            // scale x (+0x18) and y (+0x19) by their triangle wave / 128 (>= 16, +1/16 per unit nearer than 1536)
            const float s = std::min(2.5f, 12.0f * proj / std::max(v.z, 1024.0f) + 0.5f);
            const auto wave = [&](uint8_t b) {
                float w = b > 127 ? 255.0f - float(b) : float(b);
                if (v.z < 1536.0f) w = std::min(128.0f, (1536.0f - v.z) * 0.0625f + w);
                return std::max(w, 16.0f);
            };
            const float sx = f.a ? wave(f.a) * s / 128.0f : s, sy = f.b ? wave(f.b) * s / 128.0f : s;
            int32_t g = f.z - f.ground >= 128 ? 0x40 : (f.z - f.ground) / 2; // [I] the ftol below 128 is not decoded
            if (f.life < 2) g = f.life * g / 2;
            else if (kFlakeLife - f.life < 8) g = (kFlakeLife - f.life) * g / 8;
            const uint32_t rgba = rgbaOf(0x80000000u | uint32_t(g) * 0x10101u);
            const float w = v.z / proj; // world units per virtual pixel at this depth
            const glm::vec3 c = cam.eye + cam.forward * v.z + cam.right * (v.x * 256.0f / ax * w) + cam.down * (v.y * 224.0f / kViewY * w);
            const glm::vec3 r = cam.right * (sx * w), d = cam.down * (sy / kViewY * w);
            const glm::vec3 q[4] = {c - r - d, c + r - d, c + r + d, c - r + d};
            static constexpr float kQuv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            for (const int k : {0, 1, 2, 0, 2, 3}) quads.push_back({{q[k].x, q[k].y, q[k].z}, {kQuv[k][0], kQuv[k][1]}, rgba});
        }
        if (!quads.empty()) {
            render::FxBatch b;
            b.first = uint32_t(vertices.size()), b.count = uint32_t(quads.size());
            b.textureId = kGlowTexture, b.blend = 1;
            vertices.insert(vertices.end(), quads.begin(), quads.end());
            batches.push_back(b);
        }
    }
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
