// PDA 3D views on the CPU (see Pda.hpp); look: docs/GAMEPLAY.md "PDA".
#include "ui/Pda.hpp"

#include "formats/Formats.hpp"
#include "scene/Assets.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace ou::ui {

namespace {
constexpr float kPi = 3.14159265f;
uint8_t u8(float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }
void put(uint8_t* px, glm::vec3 c) { px[0] = u8(c.r), px[1] = u8(c.g), px[2] = u8(c.b), px[3] = 255; }
} // namespace

// ---- meshes -----------------------------------------------------------------------------------------------------
ViewMesh viewMesh(const scene::SceneData& sc) {
    ViewMesh v;
    std::map<uint32_t, int> tex;
    auto texture = [&](uint32_t id) {
        if (!id) return -1;
        if (const auto it = tex.find(id); it != tex.end()) return it->second;
        int slot = -1;
        for (const auto& t : sc.textures)
            if (t.id == id && !t.cube && !t.mips.empty()) {
                const auto& m = t.mips[0];
                v.textures.push_back({formats::toRgba8(t.format, m.width, m.height, m.data), int(m.width), int(m.height)});
                slot = int(v.textures.size()) - 1;
                break;
            }
        return tex[id] = slot;
    };
    v.min = glm::vec3(1e30f), v.max = glm::vec3(-1e30f);
    for (const auto& m : sc.meshes) {
        glm::mat4 xf;
        std::copy(m.transform, m.transform + 16, &xf[0][0]);
        const uint32_t base = uint32_t(v.pos.size());
        for (const auto& vert : m.vertices) {
            const glm::vec3 p(xf * glm::vec4(vert.position[0], vert.position[1], vert.position[2], 1));
            v.pos.push_back(p), v.uv.push_back({vert.uv[0], vert.uv[1]});
            v.min = glm::min(v.min, p), v.max = glm::max(v.max, p);
        }
        auto add = [&](uint32_t first, uint32_t count, int t) {
            ViewMesh::Part part{uint32_t(v.index.size()), 0, t};
            for (uint32_t i = first; i + 2 < first + count && i + 2 < m.indices.size(); i += 3)
                for (int k = 0; k < 3; ++k) v.index.push_back(base + m.indices[i + size_t(k)]);
            part.count = uint32_t(v.index.size()) - part.first;
            if (part.count) v.parts.push_back(part);
        };
        if (m.submeshes.empty()) add(0, uint32_t(m.indices.size()), -1);
        for (const auto& s : m.submeshes)
            if (s.blend == scene::Blend::Opaque || s.alphaRef) add(s.firstIndex, s.indexCount, texture(s.textureId));
    }
    if (v.pos.empty()) v.min = v.max = glm::vec3(0);
    return v;
}

// ---- rasteriser -------------------------------------------------------------------------------------------------
Raster::Raster(Image& target, int x, int y, int w, int h) : img_(target), x_(x), y_(y), w_(w), h_(h) {
    const int dw = std::min(target.width, x + w) - std::max(0, x), dh = std::min(target.height, y + h) - std::max(0, y);
    depth_.assign(size_t(std::max(0, dw)) * size_t(std::max(0, dh)), 2.0f);
}

template <class Shade> void Raster::triangle(const glm::mat4& m, const glm::vec3 (&p)[3], Shade&& shade) {
    const V in[3] = {{m * glm::vec4(p[0], 1), {1, 0, 0}}, {m * glm::vec4(p[1], 1), {0, 1, 0}}, {m * glm::vec4(p[2], 1), {0, 0, 1}}};
    V poly[4];
    int n = 0;
    for (int k = 0; k < 3; ++k) { // near plane z >= -w
        const V& a = in[k];
        const V& b = in[(k + 1) % 3];
        const float da = a.c.z + a.c.w, db = b.c.z + b.c.w;
        if (da >= 0) poly[n++] = a;
        if ((da >= 0) != (db >= 0)) {
            const float t = da / (da - db);
            poly[n++] = {glm::mix(a.c, b.c, t), glm::mix(a.b, b.b, t)};
        }
    }
    for (int i = 1; i + 1 < n; ++i) draw(poly[0], poly[i], poly[i + 1], shade);
}

template <class Shade> void Raster::draw(const V& a, const V& b, const V& c, Shade& shade) {
    const V* v[3] = {&a, &b, &c};
    float sx[3], sy[3], sz[3], iw[3];
    for (int k = 0; k < 3; ++k) {
        if (v[k]->c.w <= 1e-6f) return;
        iw[k] = 1.0f / v[k]->c.w;
        sx[k] = float(x_) + (v[k]->c.x * iw[k] * 0.5f + 0.5f) * float(w_);
        sy[k] = float(y_) + (0.5f - v[k]->c.y * iw[k] * 0.5f) * float(h_);
        sz[k] = v[k]->c.z * iw[k];
    }
    const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (std::abs(area) < 1e-8f) return;
    const int cx0 = std::max(0, x_), cy0 = std::max(0, y_);
    const int cx1 = std::min(img_.width, x_ + w_), cy1 = std::min(img_.height, y_ + h_);
    const int bx0 = std::max(cx0, int(std::floor(std::min({sx[0], sx[1], sx[2]}))));
    const int bx1 = std::min(cx1 - 1, int(std::ceil(std::max({sx[0], sx[1], sx[2]}))));
    const int by0 = std::max(cy0, int(std::floor(std::min({sy[0], sy[1], sy[2]}))));
    const int by1 = std::min(cy1 - 1, int(std::ceil(std::max({sy[0], sy[1], sy[2]}))));
    const float inv = 1.0f / area;
    const int dw = std::min(img_.width, x_ + w_) - cx0; // depth row stride: the clipped viewport
    for (int y = by0; y <= by1; ++y) {
        const float qy = float(y) + 0.5f;
        for (int x = bx0; x <= bx1; ++x) {
            const float qx = float(x) + 0.5f;
            const float w0 = ((sx[1] - qx) * (sy[2] - qy) - (sx[2] - qx) * (sy[1] - qy)) * inv;
            const float w1 = ((sx[2] - qx) * (sy[0] - qy) - (sx[0] - qx) * (sy[2] - qy)) * inv;
            const float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            const float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
            float& d = depth_[size_t(y - cy0) * size_t(dw) + size_t(x - cx0)];
            if (z >= d || z > 1) continue;
            const float p0 = w0 * iw[0], p1 = w1 * iw[1], p2 = w2 * iw[2], s = 1.0f / (p0 + p1 + p2);
            const glm::vec3 bary = (p0 * a.b + p1 * b.b + p2 * c.b) * s;
            if (shade(bary, &img_.rgba[(size_t(y) * size_t(img_.width) + size_t(x)) * 4])) d = z;
        }
    }
}

// ---- model panel ------------------------------------------------------------------------------------------------
// The panel [V captures of WEAPON SELECTION at 1920x1080, 1.5 screen pixels per PDA pixel]: near-black ground
// (3, 4, 5), a teal grid every 9 PDA pixels through the panel centre, brighter border lines and two inner vertical
// frame lines 12.7 pixels in from the sides.
void drawModelPanel(Image& img, int x0, int y0, int w, int h, const ViewMesh* mesh) {
    constexpr glm::vec3 kGround = glm::vec3(3, 4, 5) / 255.0f, kGridLine = glm::vec3(9, 32, 36) / 255.0f,
                        kFrame = glm::vec3(12, 45, 50) / 255.0f, kBorder = glm::vec3(13, 52, 58) / 255.0f;
    constexpr float kCell = 9.0f, kInset = 12.7f;
    const float cx = float(w) / 2, cy = float(h) / 2;
    const auto onLine = [](float d) { return std::abs(d - kCell * std::round(d / kCell)) < 0.5f; };
    for (int y = std::max(0, y0); y < std::min(img.height, y0 + h); ++y)
        for (int x = std::max(0, x0); x < std::min(img.width, x0 + w); ++x) {
            const int lx = x - x0, ly = y - y0;
            const float px = float(lx) + 0.5f, py = float(ly) + 0.5f;
            glm::vec3 c = kGround;
            if (lx == 0 || ly == 0 || lx == w - 1 || ly == h - 1) c = kBorder;
            else if (std::abs(px - kInset) < 0.5f || std::abs(px - (float(w) - kInset)) < 0.5f) c = kFrame;
            else if (onLine(px - cx) || onLine(py - cy)) c = kGridLine;
            put(&img.rgba[(size_t(y) * size_t(img.width) + size_t(x)) * 4], c);
        }
    if (!mesh || mesh->empty()) return;
    // Three-quarter view [V fits of the uzi / rifle / tranquilizer captures]: the barrels (model -Y) to the right, turned
    // 45 degrees towards the camera (the muzzles' openings show), seen from 10 degrees above through a 10 degree lens (near-flat: the fit holds for near parts); fitted so the model fills
    // 88% of the panel's width or height (the height, for every weapon: 88-90% in the captures).
    const glm::vec3 box = (mesh->min + mesh->max) * 0.5f;
    const float yaw = glm::radians(45.0f), elev = glm::radians(10.0f);
    const glm::vec3 back(-std::cos(yaw) * std::cos(elev), -std::sin(yaw) * std::cos(elev), std::sin(elev)); // centre -> eye
    const glm::mat4 view0 = glm::lookAt(box + back, box, {0, 0, 1});
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const glm::vec3& p : mesh->pos) { // the model's extent across / along the view
        const glm::vec3 v(view0 * glm::vec4(p, 1));
        lo = glm::min(lo, v), hi = glm::max(hi, v);
    }
    const float fov = glm::radians(10.0f), aspect = float(w) / float(h), t = std::tan(fov / 2);
    const glm::vec2 half = glm::max(glm::vec2(hi - lo) / 2.0f, glm::vec2(1)) / 0.88f;
    const float dist = std::max(half.y / t, half.x / (t * aspect)); // fitted at the centre's depth: near parts may reach out
    const glm::vec3 centre = glm::vec3(glm::inverse(view0) * glm::vec4((lo + hi) / 2.0f, 1)), eye = centre + back * dist;
    const glm::mat4 vp = glm::perspective(fov, aspect, dist * 0.2f, dist * 3) * glm::lookAt(eye, centre, {0, 0, 1});
    const glm::vec3 light = glm::normalize(back + glm::vec3(0, -0.6f, 0.8f)); // [I] key light from the camera's upper right
    Raster r(img, x0, y0, w, h);
    for (const auto& part : mesh->parts) {
        const Image* tex = part.texture >= 0 ? &mesh->textures[size_t(part.texture)] : nullptr;
        for (uint32_t i = part.first; i + 2 < part.first + part.count; i += 3) {
            const uint32_t ia = mesh->index[i], ib = mesh->index[i + 1], ic = mesh->index[i + 2];
            const glm::vec3 p[3] = {mesh->pos[ia], mesh->pos[ib], mesh->pos[ic]};
            glm::vec3 n = glm::cross(p[1] - p[0], p[2] - p[0]);
            if (glm::dot(n, n) < 1e-12f) continue;
            n = glm::normalize(n);
            if (glm::dot(n, eye - p[0]) < 0) n = -n; // two-sided
            const float lit = 0.6f + 1.0f * std::max(0.0f, glm::dot(n, light)); // [V] brighter than the textures
            const glm::vec2 uv[3] = {mesh->uv[ia], mesh->uv[ib], mesh->uv[ic]};
            r.triangle(vp, p, [&](const glm::vec3& b, uint8_t* px) {
                glm::vec3 c(0.55f);
                if (tex && tex->width > 0) {
                    const glm::vec2 t = b.x * uv[0] + b.y * uv[1] + b.z * uv[2];
                    const int tx = ((int(std::floor(t.x * float(tex->width))) % tex->width) + tex->width) % tex->width;
                    const int ty = ((int(std::floor(t.y * float(tex->height))) % tex->height) + tex->height) % tex->height;
                    const uint8_t* s = &tex->rgba[(size_t(ty) * size_t(tex->width) + size_t(tx)) * 4];
                    if (s[3] < 96) return false; // alpha-tested parts (sights, grips)
                    c = glm::vec3(s[0], s[1], s[2]) / 255.0f;
                }
                put(px, c * lit);
                return true;
            });
        }
    }
}

// ---- sonar ------------------------------------------------------------------------------------------------------
glm::vec3 SonarCamera::dir() const {
    return {std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), std::sin(pitch)};
}

void SonarCamera::reset(glm::vec3 lara, float laraYaw) {
    // Behind and above Lara, looking down at her [V captures: the view opens behind her, ~25 degrees down].
    const glm::vec3 forward(std::sin(laraYaw), -std::cos(laraYaw), 0); // model -Y rotated by yaw
    yaw = std::atan2(forward.y, forward.x);
    pitch = glm::radians(-25.0f);
    eye = lara + glm::vec3(0, 0, 300) - dir() * 2200.0f;
}

void SonarCamera::fly(glm::vec3 move, float dt) {
    constexpr float kTurn = glm::radians(90.0f), kSpeed = 2500.0f; // [I] per second
    yaw += move.x * kTurn * dt;
    const glm::vec3 flat(std::cos(yaw), std::sin(yaw), 0);
    eye += flat * move.y * kSpeed * dt + glm::vec3(0, 0, move.z * kSpeed * dt);
}

std::vector<uint8_t> Sonar::revealTriangles(const SonarMap& map, std::span<const glm::vec3> points) {
    std::vector<uint8_t> near(map.vertices.size(), 0);
    const float r2 = kSonarRevealRadius * kSonarRevealRadius;
    for (size_t i = 0; i < map.vertices.size(); ++i) {
        const glm::vec3 v(map.vertices[i][0], map.vertices[i][1], map.vertices[i][2]);
        for (const auto& p : points)
            if (glm::dot(v - p, v - p) <= r2) { near[i] = 1; break; }
    }
    std::vector<uint8_t> shown(map.triangles.size(), 0);
    for (size_t t = 0; t < map.triangles.size(); ++t)
        shown[t] = near[map.triangles[t][0]] | near[map.triangles[t][1]] | near[map.triangles[t][2]];
    return shown;
}

const std::vector<uint8_t>* Sonar::revealed(const std::string& unit) const {
    const auto it = cache_.find(unit);
    return it == cache_.end() ? nullptr : &it->second.shown;
}

const Image& Sonar::render(int w, int h, const SonarCamera& cam, std::span<const SonarUnit> units, std::span<const SonarPawn> pawns) {
    image_.width = w, image_.height = h;
    image_.rgba.assign(size_t(w) * size_t(h) * 4, 0);
    for (size_t i = 3; i < image_.rgba.size(); i += 4) image_.rgba[i] = 255;
    if (w <= 0 || h <= 0) return image_;
    const glm::vec3 eye = cam.eye, dir = cam.dir();
    const float fov = glm::radians(kSonarFovDeg);
    const glm::mat4 vp = glm::perspective(fov, float(w) / float(h), 20.0f, 200000.0f) * glm::lookAt(eye, eye + dir, {0, 0, 1});
    const float pixel = 2 * std::tan(fov / 2) / float(h); // world size of one pixel per unit of distance
    Raster r(image_, 0, 0, w, h);
    // Look [V captures]: grey surfaces, darker facing the camera and lighter at grazing angles; a teal world-space grid,
    // brighter and thicker near the camera; black beyond the revealed triangles.
    constexpr glm::vec3 kTeal(0.13f, 0.95f, 0.72f);
    for (const auto& u : units) {
        if (!u.map) continue;
        auto& cache = cache_[u.name];
        const size_t points = u.reveal ? u.reveal->size() : 0;
        if (cache.map != u.map || cache.points != points || cache.shown.size() != u.map->triangles.size()) {
            cache.shown = revealTriangles(*u.map, u.reveal ? std::span<const glm::vec3>(*u.reveal) : std::span<const glm::vec3>());
            cache.map = u.map, cache.points = points;
        }
        for (size_t t = 0; t < u.map->triangles.size(); ++t) {
            if (!cache.shown[t]) continue;
            glm::vec3 p[3];
            for (int k = 0; k < 3; ++k) {
                const auto& v = u.map->vertices[u.map->triangles[t][size_t(k)]];
                p[k] = glm::vec3(v[0], v[1], v[2]) + u.offset;
            }
            glm::vec3 n = glm::cross(p[1] - p[0], p[2] - p[0]);
            if (glm::dot(n, n) < 1e-12f) continue;
            n = glm::normalize(n);
            const glm::vec3 axis = 1.0f - glm::abs(n); // grid lines run across the planes the face is not parallel to
            r.triangle(vp, p, [&](const glm::vec3& b, uint8_t* px) {
                const glm::vec3 q = b.x * p[0] + b.y * p[1] + b.z * p[2];
                const glm::vec3 to = eye - q;
                const float dist = glm::length(to);
                const float facing = std::abs(glm::dot(n, to)) / std::max(dist, 1e-3f);
                const float fog = std::clamp(1.0f - dist / 30000.0f, 0.25f, 1.0f);
                glm::vec3 c(0.07f + 0.36f * std::pow(1.0f - facing, 1.3f));
                c *= fog;
                const float foot = dist * pixel;
                const float half = std::max(7.0f, foot * 0.6f);
                float line = 0;
                for (int k = 0; k < 3; ++k) {
                    if (axis[k] < 0.15f) continue;
                    const float g = q[k] / kSonarGrid;
                    const float d = std::abs(g - std::round(g)) * kSonarGrid;
                    line = std::max(line, std::clamp(1.0f - (d - half) / std::max(foot, 1.0f), 0.0f, 1.0f) * std::min(1.0f, axis[k] * 1.5f));
                }
                const float bright = std::clamp(1.15f - dist / 9000.0f, 0.35f, 1.0f);
                put(px, glm::mix(c, kTeal * bright, line * 0.9f));
                return true;
            });
        }
    }
    for (const auto& pawn : pawns) {
        if (!pawn.mesh) continue;
        for (size_t i = 0; i + 2 < pawn.mesh->index.size(); i += 3) {
            glm::vec3 p[3];
            for (int k = 0; k < 3; ++k) p[k] = glm::vec3(pawn.model * glm::vec4(pawn.mesh->pos[pawn.mesh->index[i + size_t(k)]], 1));
            glm::vec3 n = glm::cross(p[1] - p[0], p[2] - p[0]);
            if (glm::dot(n, n) < 1e-12f) continue;
            n = glm::normalize(n);
            const float facing = std::abs(glm::dot(n, glm::normalize(eye - p[0])));
            const glm::vec3 c = pawn.color * (0.35f + 0.45f * facing + 0.5f * std::pow(1.0f - facing, 2.0f));
            r.triangle(vp, p, [&](const glm::vec3&, uint8_t* px) { put(px, c); return true; });
        }
    }
    return image_;
}

} // namespace ou::ui
