#include "fx/Precip.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ou::fx {
namespace {

// Rain / snow cells: horizontal tile, vertical layer, particles per cell at intensity 1, fall speed (units / s) and its
// spread, draw radius. [I] tuned by eye against real rain (6-9 m/s, 100 units = 1 m) and light snowfall (~1 m/s).
struct Params { float tile, layer; int perCell; float speed, spread, radius; };
constexpr Params kRain{500, 1000, 320, 900, 120, 1800};
constexpr Params kSnow{400, 800, 240, 75, 25, 1300};

uint32_t hash(uint32_t a, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) {
    uint32_t h = a * 0x8DA6B343u ^ b * 0xD8163841u ^ c * 0xCB1AB31Fu ^ d * 0x9E3779B1u;
    h ^= h >> 16, h *= 0x7FEB352Du, h ^= h >> 15, h *= 0x846CA68Bu, h ^= h >> 16;
    return h;
}
float unit(uint32_t h) { return float(h >> 8) * (1.0f / 16777216.0f); } // [0, 1)
float wrap(float v, float m) { return v - m * std::floor(v / m); }

uint32_t pack(glm::vec3 rgb, float alpha) { // FxVertex bytes R, G, B, A; fx.vert: 0x80 = 1
    auto b = [](float v) { return uint32_t(std::clamp(v * 128.0f, 0.0f, 255.0f)); };
    return b(rgb.r) | b(rgb.g) << 8 | b(rgb.b) << 16 | b(alpha) << 24;
}

void quad(std::vector<render::FxVertex>& out, const glm::vec3 q[4], uint32_t rgba) {
    static constexpr float kUv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (const int k : {0, 1, 2, 0, 2, 3}) out.push_back({{q[k].x, q[k].y, q[k].z}, {kUv[k][0], kUv[k][1]}, rgba});
}

// Camera-facing sprite of half size `s`.
void sprite(std::vector<render::FxVertex>& out, const glm::vec3& c, const glm::vec3& eye, float s, uint32_t rgba) {
    const glm::vec3 view = glm::normalize(c - eye);
    glm::vec3 r = glm::cross(view, glm::vec3(0, 0, 1));
    r = glm::dot(r, r) > 1e-6f ? glm::normalize(r) * s : glm::vec3(s, 0, 0);
    const glm::vec3 u = glm::normalize(glm::cross(r, view)) * s;
    const glm::vec3 q[4] = {c - r + u, c + r + u, c + r - u, c - r - u};
    quad(out, q, rgba);
}

scene::TextureData texture(uint32_t id, uint32_t w, uint32_t h, const char* name, auto&& texel) {
    scene::TextureData t;
    t.id = id, t.format = scene::PixelFormat::RGBA8, t.origin = std::string("procedural:") + name;
    std::vector<glm::vec4> base(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) base[size_t(y) * w + x] = texel((float(x) + 0.5f) / float(w), (float(y) + 0.5f) / float(h));
    // `texel` returns premultiplied colour; mips are box-filtered premultiplied (bright rims do not bleed dark), stored straight.
    uint32_t lw = w, lh = h;
    std::vector<glm::vec4> level = base;
    for (;;) {
        scene::TextureMip m{lw, lh, std::vector<uint8_t>(size_t(lw) * lh * 4)};
        for (size_t i = 0; i < level.size(); ++i) {
            const glm::vec4 v = level[i];
            const glm::vec3 c = v.a > 1e-4f ? glm::vec3(v) / v.a : glm::vec3(0);
            for (int k = 0; k < 3; ++k) m.data[i * 4 + k] = uint8_t(std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
            m.data[i * 4 + 3] = uint8_t(std::clamp(v.a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
        t.mips.push_back(std::move(m));
        if (lw == 1 && lh == 1) break;
        const uint32_t nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2);
        std::vector<glm::vec4> next(size_t(nw) * nh);
        for (uint32_t y = 0; y < nh; ++y)
            for (uint32_t x = 0; x < nw; ++x) {
                glm::vec4 s{0};
                for (uint32_t dy = 0; dy < 2; ++dy)
                    for (uint32_t dx = 0; dx < 2; ++dx)
                        s += level[size_t(std::min(lh - 1, y * 2 + dy)) * lw + std::min(lw - 1, x * 2 + dx)];
                next[size_t(y) * nw + x] = s * 0.25f;
            }
        level = std::move(next), lw = nw, lh = nh;
    }
    return t;
}

} // namespace

std::vector<scene::TextureData> precipTextures() {
    std::vector<scene::TextureData> out;
    // Drop: across u a narrow core with brighter rims (the refraction highlights of a falling drop), along v a soft head
    // and a tapering tail. Straight colour premultiplied for the mips.
    out.push_back(texture(kPrecipDropTexture, 16, 64, "rain drop", [](float u, float v) {
        const float x = std::abs(u - 0.5f) * 2.0f;
        const float body = std::clamp(1.0f - x, 0.0f, 1.0f);
        const float rim = std::exp(-std::pow((x - 0.6f) / 0.18f, 2.0f));
        const float along = std::clamp(v / 0.15f, 0.0f, 1.0f) * std::clamp((1.0f - v) / 0.55f, 0.0f, 1.0f);
        const float a = std::clamp((body * 0.7f + rim * 0.6f) * along, 0.0f, 1.0f);
        const float bright = 0.5f + 0.6f * rim; // darker core, bright rims: visible on light and dark backgrounds
        return glm::vec4(glm::vec3(bright) * a, a);
    }));
    out.push_back(texture(kPrecipFlakeTexture, 32, 32, "snow flake", [](float u, float v) {
        const float r = glm::length(glm::vec2(u - 0.5f, v - 0.5f)) * 2.0f;
        const float a = std::clamp(1.0f - r, 0.0f, 1.0f);
        const float core = a * a * (3.0f - 2.0f * a);
        return glm::vec4(glm::vec3(core), core);
    }));
    out.push_back(texture(kPrecipRingTexture, 64, 64, "splash ring", [](float u, float v) {
        const float r = glm::length(glm::vec2(u - 0.5f, v - 0.5f)) * 2.0f;
        const float a = std::exp(-std::pow((r - 0.78f) / 0.09f, 2.0f)) * (r < 1.0f ? 1.0f : 0.0f);
        return glm::vec4(glm::vec3(a), a);
    }));
    return out;
}

void Precip::clear() { std::fill(cover_.begin(), cover_.end(), Cell{}); }

void Precip::updateCover(const glm::vec3& eye, const Query& query, size_t budget) {
    const int ex = int(std::floor(eye.x / kCoverCell)), ey = int(std::floor(eye.y / kCoverCell));
    std::vector<glm::vec3> from, to;
    std::vector<Cell*> cells;
    from.reserve(budget), to.reserve(budget), cells.reserve(budget);
    const auto want = [&](int cx, int cy, bool stale) {
        Cell& c = cover_[size_t(wrap(float(cy), kCoverSize)) * kCoverSize + size_t(wrap(float(cx), kCoverSize))];
        const bool missing = c.x != cx || c.y != cy;
        if (!missing && !(stale && std::abs(c.fromZ - eye.z) > kRequery)) return;
        if (cells.size() >= budget) return;
        const glm::vec2 p = (glm::vec2(cx, cy) + 0.5f) * kCoverCell;
        // From well above the eye (cave ceilings a few rooms up still count: Jolt skips back faces, so a start inside rock
        // hits the rock's underside) down to well below it.
        from.push_back({p, eye.z + 6000.0f}), to.push_back({p, eye.z - 4000.0f});
        c.x = cx, c.y = cy, c.height = std::numeric_limits<float>::infinity(), c.fromZ = eye.z; // pending: hidden
        cells.push_back(&c);
    };
    // Rings outward from the eye so the near columns fill first.
    const int half = kCoverSize / 2 - 1;
    for (int pass = 0; pass < 2 && cells.size() < budget; ++pass)
        for (int r = 0; r <= half && cells.size() < budget; ++r)
            for (int i = -r; i <= r && cells.size() < budget; ++i) {
                want(ex + i, ey - r, pass == 1), want(ex + i, ey + r, pass == 1);
                if (i != -r && i != r) want(ex - r, ey + i, pass == 1), want(ex + r, ey + i, pass == 1);
            }
    if (cells.empty()) return;
    std::vector<std::optional<float>> hits(cells.size());
    query(from, to, hits);
    for (size_t i = 0; i < cells.size(); ++i) cells[i]->height = hits[i] ? *hits[i] : -std::numeric_limits<float>::infinity();
}

std::optional<float> Precip::cover(glm::vec2 p) const {
    const int cx = int(std::floor(p.x / kCoverCell)), cy = int(std::floor(p.y / kCoverCell));
    const Cell& c = cover_[size_t(wrap(float(cy), kCoverSize)) * kCoverSize + size_t(wrap(float(cx), kCoverSize))];
    if (c.x != cx || c.y != cy || (std::isinf(c.height) && c.height > 0)) return std::nullopt;
    return c.height;
}

void Precip::build(const Frame& f, std::vector<render::FxVertex>& vertices, std::vector<render::FxBatch>& batches) const {
    count_ = 0;
    if (f.kind == Kind::None || f.intensity <= 0.001f) return;
    const bool snow = f.kind == Kind::Snow;
    const Params& P = snow ? kSnow : kRain;
    const int n = std::clamp(int(std::lround(float(P.perCell) * std::min(f.intensity, 1.0f))), 0, P.perCell);
    if (!n) return;
    const float R = P.radius, pxScale = 2.0f * f.tanHalfFovY / std::max(f.viewportHeight, 1.0f);
    const glm::vec3 fwd = glm::normalize(f.forward);
    std::vector<render::FxVertex> main, splash, drops;
    const int x0 = int(std::floor((f.eye.x - R) / P.tile)), x1 = int(std::floor((f.eye.x + R) / P.tile));
    const int y0 = int(std::floor((f.eye.y - R) / P.tile)), y1 = int(std::floor((f.eye.y + R) / P.tile));
    const int z0 = int(std::floor((f.eye.z - R) / P.layer)), z1 = int(std::floor((f.eye.z + R) / P.layer));
    for (int cz = z0; cz <= z1; ++cz)
        for (int cy = y0; cy <= y1; ++cy)
            for (int cx = x0; cx <= x1; ++cx) {
                // The cell's nearest point to the eye: skip cells wholly outside the radius or wholly behind the camera.
                const glm::vec3 lo(float(cx) * P.tile, float(cy) * P.tile, float(cz) * P.layer);
                const glm::vec3 hi = lo + glm::vec3(P.tile, P.tile, P.layer);
                if (glm::length(glm::clamp(f.eye, lo, hi) - f.eye) > R) continue;
                for (int i = 0; i < n; ++i) {
                    const uint32_t h0 = hash(uint32_t(cx), uint32_t(cy), uint32_t(cz) ^ (snow ? 0x51u << 24 : 0u), uint32_t(i));
                    const float speed = P.speed + P.spread * (unit(hash(h0, 1)) * 2.0f - 1.0f);
                    const double fall = double(unit(h0)) * P.layer + double(speed) * f.time;
                    const double cycle = std::floor(fall / P.layer);
                    const float u = float(fall - cycle * P.layer); // fallen this cycle
                    const float tc = u / speed;                    // seconds since the cycle's top
                    const uint32_t h1 = hash(h0, uint32_t(int64_t(cycle)), 7);
                    glm::vec2 xy(wrap(unit(h1) * P.tile + f.wind.x * tc, P.tile), wrap(unit(hash(h1, 2)) * P.tile + f.wind.y * tc, P.tile));
                    if (snow) { // sway on two slow sines (flutter)
                        const float ph = unit(hash(h1, 3)) * 6.2831853f, w = 0.6f + unit(hash(h1, 4)) * 0.8f;
                        xy += glm::vec2(std::sin(float(f.time) * w + ph), std::cos(float(f.time) * w * 0.77f + ph * 1.3f)) * 14.0f;
                    }
                    const glm::vec3 p(lo.x + xy.x, lo.y + xy.y, hi.z - u);
                    const glm::vec3 d = p - f.eye;
                    const float dist = glm::length(d);
                    if (dist > R || dist < 8.0f || glm::dot(d, fwd) < -10.0f) continue;
                    const auto top = cover(glm::vec2(p));
                    if (!top) continue; // column not queried yet
                    const float fade = (1.0f - glm::smoothstep(0.65f * R, R, dist)) * glm::smoothstep(8.0f, 30.0f, dist);
                    if (p.z < *top) { // below the column's top surface: hidden, a rain drop splashes for a moment
                        if (snow || *top < lo.z || *top >= hi.z) continue;
                        const float age = (*top - p.z) / speed;
                        if (age >= kSplashLife) continue;
                        const float k = age / kSplashLife;
                        const glm::vec3 at(p.x, p.y, *top + 0.6f);
                        const float r = 1.5f + 7.0f * std::sqrt(k);
                        const glm::vec3 q[4] = {at + glm::vec3(-r, -r, 0), at + glm::vec3(r, -r, 0), at + glm::vec3(r, r, 0), at + glm::vec3(-r, r, 0)};
                        quad(splash, q, pack(f.light * 0.9f, 0.8f * (1.0f - k) * fade));
                        // two droplets thrown up and out
                        for (uint32_t j = 0; j < 2; ++j) {
                            const float a = unit(hash(h1, 10 + j)) * 6.2831853f, v = 40.0f + 50.0f * unit(hash(h1, 20 + j));
                            const glm::vec3 dp = at + glm::vec3(std::cos(a) * v * 0.35f * age, std::sin(a) * v * 0.35f * age, v * age - 490.0f * age * age);
                            if (dp.z < *top) continue;
                            sprite(drops, dp, f.eye, std::max(0.35f, 0.5f * dist * pxScale), pack(f.light, 0.7f * (1.0f - k) * fade));
                        }
                        ++count_;
                        continue;
                    }
                    if (snow) {
                        const float s = 1.2f + 1.8f * unit(hash(h0, 5));
                        const float px = dist * pxScale, half = std::max(s, 0.7f * px);
                        sprite(main, p, f.eye, half, pack(f.light * 0.97f, 0.85f * fade * std::min(1.0f, s / half)));
                    } else {
                        const glm::vec3 v(f.wind.x, f.wind.y, -speed);
                        const glm::vec3 along = glm::normalize(v);
                        glm::vec3 side = glm::cross(along, d / dist);
                        if (glm::dot(side, side) < 1e-8f) continue;
                        const float px = dist * pxScale;
                        const float width = std::max(0.35f, 0.7f * px), halfLen = std::max(4.0f, speed * 0.009f);
                        side = glm::normalize(side) * width;
                        const glm::vec3 a = along * halfLen;
                        const glm::vec3 q[4] = {p - a - side, p - a + side, p + a + side, p + a - side}; // v 0 = the head (leading)
                        quad(main, q, pack(f.light * 0.8f + 0.1f, 0.85f * fade * std::min(1.0f, 0.35f / width)));
                    }
                    ++count_;
                }
            }
    const auto add = [&](std::vector<render::FxVertex>& v, uint32_t texture, uint8_t blend) {
        if (v.empty()) return;
        render::FxBatch b;
        b.first = uint32_t(vertices.size()), b.count = uint32_t(v.size()), b.textureId = texture, b.blend = blend;
        vertices.insert(vertices.end(), v.begin(), v.end());
        batches.push_back(b);
    };
    add(main, snow ? kPrecipFlakeTexture : kPrecipDropTexture, 0);
    add(splash, kPrecipRingTexture, 1);
    add(drops, kPrecipFlakeTexture, 1);
}

// ---- snow on Lara ----

void SnowCoat::clear() { amount_ = {}, carry_ = {}, clumps_.clear(), lastVel_ = {}, lastGrounded_ = true; }

void SnowCoat::shed(const Input& in, int region, float amount, const Ground& ground) {
    // Region heights (fraction of the model height) and spread: head + shoulders, arms, torso, legs. [I]
    static constexpr float kZ[kRegions] = {0.86f, 0.62f, 0.66f, 0.32f}, kR[kRegions] = {14, 24, 14, 12};
    const int count = std::clamp(int(amount * 14.0f + 0.5f), 0, 8);
    for (int i = 0; i < count && clumps_.size() < 160; ++i) {
        auto rnd = [&] { return unit(seed_ = hash(seed_, uint32_t(i), uint32_t(region))); };
        Clump c;
        const float a = rnd() * 6.2831853f;
        c.pos = in.position + glm::vec3(std::cos(a) * kR[region] * rnd(), std::sin(a) * kR[region] * rnd(), kZ[region] * in.height);
        c.vel = in.velocity * 0.6f + glm::vec3(std::cos(a), std::sin(a), 0.5f + rnd()) * (20.0f + 30.0f * rnd());
        c.size = 0.8f + 1.4f * rnd();
        c.life = 2.0f + 2.0f * rnd();
        const auto g = ground(c.pos);
        c.rest = g ? *g : c.pos.z - 400.0f;
        clumps_.push_back(c);
    }
}

void SnowCoat::tick(const Input& in, const Ground& ground) {
    const float dt = std::clamp(in.dt, 0.0f, 0.1f);
    const float speed = glm::length(glm::vec2(in.velocity));
    const glm::vec3 accel = dt > 0 ? (in.velocity - lastVel_) / dt : glm::vec3(0);
    const bool landed = in.grounded && !lastGrounded_ && lastVel_.z < -250.0f;
    // Build-up while standing in snowfall: ~a minute to full on the upward-facing parts [I]; the arms and legs collect
    // less (they hang), moving collects less still.
    static constexpr float kCollect[kRegions] = {1.0f, 0.45f, 0.75f, 0.35f};
    for (int r = 0; r < kRegions; ++r) {
        const float still = 1.0f - std::clamp(speed / 300.0f, 0.0f, 0.8f);
        amount_[r] = std::min(1.0f, amount_[r] + in.snowfall * kCollect[r] * still * dt / 60.0f);
        // Fading with motion: walking loses it over ~half a minute, running faster; out of the snow it melts in ~2 min.
        const float fade = speed / 300.0f * dt / 25.0f + (in.snowfall < 0.05f ? dt / 120.0f : 0.0f);
        amount_[r] = std::max(0.0f, amount_[r] - fade * amount_[r] * 2.0f);
    }
    // Strong motion sheds clumps: running, jumping / falling, landing, climbing.
    float strong = 0;
    if (speed > 330.0f) strong = std::max(strong, (speed - 330.0f) / 300.0f);
    if (!in.grounded || in.climbing) strong = std::max(strong, 0.6f);
    if (landed) strong = 1.5f;
    strong = std::max(strong, std::clamp((glm::length(accel) - 1200.0f) / 3000.0f, 0.0f, 1.0f));
    if (strong > 0) {
        for (int r = 0; r < kRegions; ++r) {
            const float lose = std::min(amount_[r], amount_[r] * std::min(1.0f, strong * dt * (landed ? 30.0f : 1.6f)));
            if (lose <= 0) continue;
            amount_[r] -= lose;
            // a clump per ~4 % lost, at most a few per tick
            carry_[r] += lose * 25.0f;
            if (carry_[r] >= 1.0f) {
                const float k = std::floor(carry_[r]);
                carry_[r] -= k;
                shed(in, r, k / 14.0f, ground);
            }
        }
    }
    // Clumps fall under gravity and settle on the ground they were above; then melt.
    for (Clump& c : clumps_) {
        c.age += dt;
        if (!c.landed) {
            c.vel.z -= 980.0f * dt;
            c.vel *= std::pow(0.4f, dt); // air drag on a loose clump
            c.pos += c.vel * dt;
            if (c.pos.z <= c.rest) c.pos.z = c.rest + 0.4f, c.landed = true, c.vel = {};
        }
    }
    std::erase_if(clumps_, [](const Clump& c) { return c.age > c.life; });
    lastVel_ = in.velocity, lastGrounded_ = in.grounded;
}

void SnowCoat::build(const Precip::Frame& f, std::vector<render::FxVertex>& vertices, std::vector<render::FxBatch>& batches) const {
    if (clumps_.empty()) return;
    std::vector<render::FxVertex> v;
    for (const Clump& c : clumps_) {
        const float melt = 1.0f - glm::smoothstep(c.life * 0.6f, c.life, c.age);
        const float s = c.size * (c.landed ? 1.0f + 0.4f * melt : 1.0f);
        sprite(v, c.pos, f.eye, s, pack(f.light * 0.98f, 0.95f * melt));
    }
    render::FxBatch b;
    b.first = uint32_t(vertices.size()), b.count = uint32_t(v.size()), b.textureId = kPrecipFlakeTexture, b.blend = 0;
    vertices.insert(vertices.end(), v.begin(), v.end());
    batches.push_back(b);
}

} // namespace ou::fx
