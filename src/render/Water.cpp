// See Water.hpp for the technique and the uniform layout. Shader sources live in
// src/render/shaders/ (embedded below for GL runtime compilation; Vulkan compiles the same
// files with glslc -DVULKAN=1 at build time).
#include "render/Water.hpp"

#include "render/OriginalShading.hpp"
#include "render/RenderCommon.hpp"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ou::render {

namespace {
const char* kWaterVert =
#include "water.vert.inc"
    ;
const char* kWaterFrag =
#include "water.frag.inc"
    ;
const char* kSplashVert =
#include "splash.vert.inc"
    ;
const char* kSplashFrag =
#include "splash.frag.inc"
    ;
const char* kUnderwater =
#include "underwater.glsl.inc"
    ;
const char* kAtmosphere =
#include "atmosphere.glsl.inc"
    ;
const char* kFilter =
#include "filter.glsl.inc"
    ;
const char* kCaustics =
#include "caustics.glsl.inc"
    ;
const char* kWaves =
#include "waves.glsl.inc"
    ;
} // namespace

bool isWaterSurface(std::string_view meshOrigin) { return meshOrigin.find("watersurface") != std::string_view::npos; }

bool reflectsWater(glm::vec3 center, float radius, float reflectZ, std::span<const WaterVolume> volumes) {
    if (std::isnan(reflectZ)) return false;
    for (const auto& v : volumes)
        if (center.x + radius >= v.min[0] && center.x - radius <= v.max[0] && center.y + radius >= v.min[1] &&
            center.y - radius <= v.max[1] && reflectZ >= v.min[2] && reflectZ <= v.max[2] + 500.0f)
            return true;
    return false;
}

glm::mat4 waterWaves(glm::vec3 center, float radius, std::span<const WaterVolume> volumes) {
    sm3::ObjectBlock o{};
    original::fillWaves(o, volumes, center, radius);
    glm::mat4 m;
    std::memcpy(&m, o.waves, sizeof m);
    return m;
}

bool cameraUnderwater(const Camera& camera, std::span<const WaterVolume> volumes) {
    for (const auto& v : volumes) {
        const glm::vec3 p = camera.position;
        if (p.x >= v.min[0] && p.x <= v.max[0] && p.y >= v.min[1] && p.y <= v.max[1] && p.z >= v.min[2] &&
            p.z <= v.max[2])
            return true;
    }
    return false;
}

WaterBlock waterBlock(const Camera& camera, Viewport vp, const glm::mat4& viewProj, const scene::Environment& env,
                      const FrameOptions& options) {
    WaterBlock w{};
    w.viewProj = viewProj;
    w.invViewProj = glm::inverse(viewProj);
    w.eye = glm::vec4(camera.position, options.time);
    w.screen = {float(vp.width), float(vp.height), 1.0f / vp.width, 1.0f / vp.height};
    w.sunDir = glm::vec4(sunToward(env), 1.0f);
    w.sunColor = glm::vec4(env.sun ? glm::vec3(env.sunColor[0], env.sunColor[1], env.sunColor[2]) : glm::vec3(1), env.sun ? 0.8f : 0.0f);
    const glm::vec3 fog = env.valid ? glm::vec3(env.fogColor[0], env.fogColor[1], env.fogColor[2]) : glm::vec3(0.08f, 0.09f, 0.11f);
    w.fogColor = glm::vec4(fog, cameraUnderwater(camera, options.waterVolumes) ? 1.0f : 0.0f);
    w.fogRange = {env.valid ? env.fogNear : 1000.0f, env.valid && env.fogFar > env.fogNear ? env.fogFar : 30000.0f, 1.0f, 1.0f};
    if (const Underwater* u = options.underwater) {
        if (u->fog) {
            w.fogColor = glm::vec4(u->fogColor[0], u->fogColor[1], u->fogColor[2], w.fogColor.w);
            w.fogRange.x = u->fogNear, w.fogRange.y = std::max(u->fogFar, u->fogNear + 1.0f);
        }
        // The colours always (the Enhanced surface tints and clears by them, water.frag ouWaterLook); the full-screen
        // pass only runs with a strength (post: waterPass[2].x > 0).
        if (u->pass[1][3] > 0) {
            w.waterPass[0] = glm::vec4(u->pass[0][0], u->pass[0][1], u->pass[0][2], u->pass[0][3]);
            w.waterPass[1] = glm::vec4(u->pass[1][0], u->pass[1][1], u->pass[1][2], u->pass[1][3]);
            w.waterPass[2].x = u->strength;
        }
        if (u->rayWeight > 0) {
            w.waterPass[2].y = u->rayWeight;
            std::memcpy(w.rays, u->rays.data(), u->rays.size());
        }
    }
    if (const ScreenFilter* f = options.screenFilter; f && f->active()) {
        w.filter[0] = {f->alphaA, f->blurA, f->dof ? 1.0f : 0.0f, f->colourMatrix ? 1.0f : 0.0f};
        w.filter[1] = {f->tintA[0], f->tintA[1], f->tintA[2], f->finishMatrix ? 1.0f : 0.0f};
        w.filter[2] = {f->amountC, f->blurC, f->dofScale, f->dofBias};
        w.filter[3] = {f->tintAdd[0], f->tintAdd[1], f->tintAdd[2], 0};
        w.filter[4] = {f->tintMix[0], f->tintMix[1], f->tintMix[2], f->tintMix[3]};
        w.filter[5] = {f->feedbackKeep, f->feedbackAmount, f->feedbackBlur, f->feedbackZoom[0]};
        w.filter[6] = {f->feedbackColour[0], f->feedbackColour[1], f->feedbackColour[2], f->feedbackAngle};
        w.filter[7] = {f->feedbackZoom[1], f->noiseAlpha, f->noiseCells, 0};
        w.filter[8] = {f->noiseColour[0], f->noiseColour[1], f->noiseColour[2], f->noiseSeed};
        std::memcpy(w.filterLut, f->lut.data(), sizeof w.filterLut);
    } else {
        w.filter[5] = {1, 0, 0, 1}, w.filter[7] = {1, 0, 0, 0}; // feedback off: keep the scene
    }
    const auto& r = options.ripples;
    w.ripple = {r.originX, r.originY, r.cellSize, r.surfaceZ};
    w.rippleInfo = {float(r.size), r.cellSize > 0 && r.size > 0 && !r.heights.empty() ? 1.0f : 0.0f, 0, 0};
    w.seaWaves = waterWaves(camera.position, 1.0f, options.waterVolumes); // the sea the caustics look through
    w.ambient = env.valid ? glm::vec4(env.ambient[0], env.ambient[1], env.ambient[2], 0) : glm::vec4(0.35f, 0.35f, 0.38f, 0);

    const glm::vec3 eye = camera.position;
    // Water volumes, nearest first (box distance): the atmosphere ends its rays on their surfaces (the Enhanced
    // water pass does not write depth) and the debug view marks them.
    std::vector<std::pair<float, const WaterVolume*>> vols;
    for (const auto& v : options.waterVolumes) {
        const glm::vec3 lo(v.min[0], v.min[1], v.min[2]), hi(v.max[0], v.max[1], v.max[2]);
        vols.push_back({glm::distance(glm::clamp(eye, lo, hi), eye), &v});
    }
    std::sort(vols.begin(), vols.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const size_t nv = std::min<size_t>(vols.size(), 8);
    for (size_t i = 0; i < nv; ++i) {
        w.volumes[2 * i] = {vols[i].second->min[0], vols[i].second->min[1], vols[i].second->min[2], 0};
        w.volumes[2 * i + 1] = {vols[i].second->max[0], vols[i].second->max[1], vols[i].second->max[2], 0};
    }
    const float debug = float(std::clamp(options.waterDebug, 0, kWaterDebugViews - 1));
    w.detail = {options.waterDetail ? 1.0f : 0.0f, debug, 0.12f * 290.0f, float(nv)}; // z: Godot heightstrength 0.12 m
    if (options.waterDetail) { // the four lamps nearest the camera, reflected by the water [I radius cut]
        std::vector<std::pair<float, const scene::Light*>> lamps;
        for (const auto& l : env.lights)
            if (l.radius > 0) lamps.push_back({glm::distance(glm::vec3(l.position[0], l.position[1], l.position[2]), eye), &l});
        std::sort(lamps.begin(), lamps.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < std::min<size_t>(lamps.size(), 4); ++i) {
            const auto& l = *lamps[i].second;
            w.lamps[i] = {l.position[0], l.position[1], l.position[2], l.radius};
            w.lampColor[i] = {l.color[0], l.color[1], l.color[2], 0};
        }
    }
    // Atmosphere [I values, tuned on l2 jungle / l5 Jan Mayen / l4 pool]: haze over the whole view thinning with
    // height above the camera's floor, plus a denser ground mist hugging the lowest water surface or the ground.
    const int level = int(options.atmosphere);
    if (level > 0) {
        const float floorZ = nv ? std::min(eye.z, w.volumes[1].z) : eye.z - 250.0f;
        w.haze = {1.0f / 60000.0f, 1.0f / 2200.0f, floorZ, float(level)};
        const bool sun = env.valid && env.sun; // mist.w: 0 no sun, 1 sun scattering, 2 + shafts
        const float flags = !sun ? 0.0f : options.lightShafts && level >= 2 ? 2.0f : 1.0f;
        w.mist = {1.0f / 30000.0f, 1.0f / 300.0f, floorZ, flags};
    }
    using K = Weather::Kind;
    if (options.weather.kind != K::None && options.weather.intensity > 0)
        w.weather = {options.weather.kind == K::Rain ? 1.0f : 2.0f, std::clamp(options.weather.intensity, 0.0f, 1.0f),
                     options.weather.wind[0], options.weather.wind[1]};
    return w;
}

namespace {
float rayRandom(uint32_t& s) { // tru.exe: (rand() & 0x7FFFFF) / 8388607 [I: generator]
    s = s * 1664525u + 1013904223u;
    return float(s >> 9) / 8388607.0f;
}
} // namespace

void UnderwaterRays::reset(const scene::Environment::WaterRays& r) {
    rec = r;
    const size_t n = size_t(r.grid) * size_t(r.grid);
    life.resize(n), darkUntil.resize(n);
    time = 0, cursor = 0;
    for (size_t i = 0; i < n; ++i) // 0x6D64D0 (texcoord u random, v 0), 0x6D6EA0 (dark for up to 20 s)
        life[i] = {rayRandom(rng), 0.0f}, darkUntil[i] = rayRandom(rng) * 20.0f;
}

void UnderwaterRays::step(float dt) {
    time += dt;
    for (int k = 0; k < 100 && !life.empty(); ++k, cursor = (cursor + 1) % life.size()) {
        const float a = rayRandom(rng);
        if (life[cursor].y >= time || darkUntil[cursor] >= time) continue;
        const float spell = rec.dark[0] * rayRandom(rng);
        if (spell > rec.dark[1]) darkUntil[cursor] = time + spell;
        life[cursor] = {time, time + rec.life[0] + a * rec.life[1] * rayRandom(rng)};
    }
}

bool UnderwaterRays::draw(const Camera& camera, float aspect, const float (&wv)[4][4], std::array<uint8_t, 2500>& out) {
    const int g = rec.grid;
    if (g < 2 || life.size() != size_t(g) * size_t(g)) return false;
    // 0x6D8C20: the rays fade in between the two depth markers (camera z), and are drawn only above 0
    float depth = (camera.position.z - rec.depthZ[0]) / (rec.depthZ[1] - rec.depthZ[0]);
    if (!(depth <= 1.0f)) depth = 1.0f;
    if (!(depth > 0.0f)) return false;
    const size_t n = life.size();
    const glm::vec3 o(rec.origin[0], rec.origin[1], rec.origin[2]), sun(rec.sun[0], rec.sun[1], rec.sun[2]), eye = camera.position;
    const float facing = std::clamp(1.0f + camera.forward().z, 0.0f, 1.0f); // c63.x [I: camera matrix row 2 = forward]
    const auto height = [&](float x, float y) { // c61.z + 100 (sin a0 - 1) + 75 (sin a1 - 1), a = x c57 + y c58 + c59
        float h = o.z;
        for (int i = 0; i < 2; ++i) h += (i ? 75.0f : 100.0f) * (std::sin(x * wv[0][i] + y * wv[1][i] + wv[2][i]) - 1.0f);
        return h;
    };
    const glm::mat4 vp = camera.projection(aspect) * camera.view();
    struct V {
        glm::vec4 c;
        float v;
    };
    std::vector<V> verts(2 * n); // top (surface) corners, then their bottom ends
    for (size_t k = 0; k < verts.size(); ++k) {
        const size_t i = k % n;
        const glm::vec2 p = glm::vec2(o) + rec.halfSize * glm::vec2(-1.0f + 2.0f * float(i % size_t(g)) / float(g),
                                                                   -1.0f + 2.0f * float(i / size_t(g)) / float(g));
        const float h = height(p.x, p.y);
        const glm::vec3 nrm = glm::normalize(glm::vec3(-2.0f * (height(p.x + 2, p.y) - h), -2.0f * (height(p.x, p.y + 2) - h), 4.0f));
        const float tilt = (1.0f - nrm.z) * (1.0f - nrm.z) * 0.99f + 0.01f;
        const glm::vec3 in = glm::normalize(glm::vec3(p, o.z) - sun);
        glm::vec3 pos(p, h);
        float ext, scatter = 0;
        if (k < n) { // forward scattering toward the eye, extinction over the view distance (+0xAC)
            const float c = glm::dot(glm::normalize(glm::vec3(p, o.z) - eye), -in);
            scatter = std::min(2.0f * std::clamp(0.0795774683f * (std::pow((1.0f + c) * 0.4f, 5.0f) * 4.5f + 0.8f), 0.0f, 1.0f), 1.0f);
            ext = std::exp(-glm::distance(pos, eye) * rec.vsExtinction * rec.extinction[1]);
        } else { // refracted (eta 0.8) and extended; extinction over depth + view distance (+0xA8)
            pos += glm::refract(in, nrm, 0.8f) * rec.length;
            ext = std::exp(-(o.z - eye.z + glm::distance(pos, eye)) * rec.vsExtinction * rec.extinction[0]);
        }
        const float t = (time - life[i].x) / (life[i].y - life[i].x);
        // ponytail: negative envelopes (a corner's first life) clamp per vertex, tru.exe clamps the blended pixels
        const float env = std::max(t <= 0.25f ? 4.0f * t : t <= 0.5f ? 2.0f - 4.0f * t : 0.0f, 0.0f);
        const float nearFade = std::clamp(glm::distance(glm::vec2(pos), glm::vec2(eye)) * 12.5f, 0.0f, 1.0f); // murk 0
        verts[k] = {vp * glm::vec4(pos, 1.0f), env * facing * depth * nearFade * ext * (tilt + scatter) * 0.07f};
    }
    std::array<float, kSize * kSize> img{};
    const auto fill = [&](const V& a, const V& b, const V& c) { // perspective-correct, pixel centres, rows down
        const V* v[3] = {&a, &b, &c};
        glm::vec2 s[3];
        float iw[3], vw[3];
        for (int i = 0; i < 3; ++i) {
            iw[i] = 1.0f / v[i]->c.w, vw[i] = v[i]->v * iw[i];
            s[i] = {(v[i]->c.x * iw[i] * 0.5f + 0.5f) * kSize, (0.5f - v[i]->c.y * iw[i] * 0.5f) * kSize};
        }
        const auto edge = [](glm::vec2 p, glm::vec2 q, glm::vec2 r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
        const float area = edge(s[0], s[1], s[2]);
        if (area >= 0) return; // back face (counter-clockwise in NDC is front; rows run down here)
        const int x0 = std::max(0, int(std::floor(std::min({s[0].x, s[1].x, s[2].x})))), x1 = std::min(kSize - 1, int(std::max({s[0].x, s[1].x, s[2].x})));
        const int y0 = std::max(0, int(std::floor(std::min({s[0].y, s[1].y, s[2].y})))), y1 = std::min(kSize - 1, int(std::max({s[0].y, s[1].y, s[2].y})));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const glm::vec2 p(float(x) + 0.5f, float(y) + 0.5f);
                const float b0 = edge(s[1], s[2], p) / area, b1 = edge(s[2], s[0], p) / area, b2 = edge(s[0], s[1], p) / area;
                if (b0 < 0 || b1 < 0 || b2 < 0) continue;
                img[size_t(y * kSize + x)] += (b0 * vw[0] + b1 * vw[1] + b2 * vw[2]) / (b0 * iw[0] + b1 * iw[1] + b2 * iw[2]);
            }
    };
    const auto tri = [&](size_t a, size_t b, size_t c) { // clipped against the near plane (z > -w) first
        const V in[3] = {verts[a], verts[b], verts[c]};
        if (in[0].v <= 0 && in[1].v <= 0 && in[2].v <= 0) return;
        V poly[4];
        int m = 0;
        for (int i = 0; i < 3; ++i) {
            const V &p = in[i], &q = in[(i + 1) % 3];
            const float dp = p.c.z + p.c.w, dq = q.c.z + q.c.w;
            if (dp >= 0) poly[m++] = p;
            if ((dp >= 0) != (dq >= 0)) {
                const float f = dp / (dp - dq);
                poly[m++] = {glm::mix(p.c, q.c, f), p.v + (q.v - p.v) * f};
            }
        }
        for (int i = 1; i + 1 < m; ++i) fill(poly[0], poly[i], poly[i + 1]);
    };
    for (size_t ca = 0; ca + 2 <= size_t(g); ++ca) // 0x6D6640: the four side walls of every masked cell
        for (size_t cb = 0; cb + 2 <= size_t(g); ++cb) {
            if (!rec.mask[cb * size_t(g) + ca]) continue;
            const size_t A = size_t(g) * ca + cb, B = A + size_t(g);
            tri(B, B + 1, B + 1 + n), tri(B, B + 1 + n, B + n), tri(A, B, B + n), tri(A, B + n, A + n);
            tri(A, A + n, A + 1 + n), tri(A, A + 1 + n, A + 1), tri(A + 1, B + 1 + n, B + 1), tri(A + 1, A + 1 + n, B + 1 + n);
        }
    for (size_t i = 0; i < img.size(); ++i) out[i] = uint8_t(std::lround(std::clamp(img[i], 0.0f, 1.0f) * 255.0f)); // A8R8G8B8
    return true;
}

namespace {
std::string withVersion(const char* source, bool vulkan) {
    (void)vulkan; // the files carry #version 460; glslc defines VULKAN for the descriptor syntax
    return source;
}
// GL has no #include: splice the shared files in place of their #include lines (glslc resolves them for Vulkan); every
// occurrence, in this order, so caustics.glsl's own waves include is spliced too (waves.glsl has an include guard).
std::string expandIncludes(std::string s) {
    const std::pair<const char*, const char*> files[] = {{"#include \"sun.glsl\"", sunGlsl()},
                                                         {"#include \"shaders/caustics.glsl\"", kCaustics},
                                                         {"#include \"shaders/waves.glsl\"", kWaves}};
    for (const auto& [line, text] : files)
        for (size_t at; (at = s.find(line)) != std::string::npos;) s.replace(at, std::strlen(line), text);
    return s;
}
} // namespace

std::string waterVertexSource(bool vulkan) { return expandIncludes(withVersion(kWaterVert, vulkan)); }
std::string waterFragmentSource(bool vulkan) { return expandIncludes(withVersion(kWaterFrag, vulkan)); }

std::vector<SplashVertex> splashVertices(const Camera& camera, std::span<const WaterParticle> splashes) {
    std::vector<SplashVertex> out;
    out.reserve(splashes.size() * 6);
    const glm::vec3 right = camera.right(), up = glm::cross(camera.forward(), right);
    for (const auto& p : splashes) {
        if (p.alpha <= 0 || p.radius <= 0) continue;
        const glm::vec3 c(p.pos[0], p.pos[1], p.pos[2]);
        const glm::vec3 r = right * p.radius, u = up * p.radius;
        const glm::vec3 corners[4] = {c - r - u, c + r - u, c + r + u, c - r + u};
        const float uvs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int i : {0, 1, 2, 0, 2, 3}) {
            const glm::vec3& q = corners[i];
            out.push_back({{q.x, q.y, q.z}, {uvs[i][0], uvs[i][1]}, p.alpha});
        }
    }
    return out;
}

std::string splashVertexSource(bool vulkan) { return withVersion(kSplashVert, vulkan); }
std::string splashFragmentSource(bool vulkan) { return withVersion(kSplashFrag, vulkan); }

std::string underwaterPostSource(bool vulkan) {
    return expandIncludes(std::string(vulkan ? "#define VULKAN 1\n" : "") + kUnderwater + kAtmosphere + kFilter);
}

} // namespace ou::render
