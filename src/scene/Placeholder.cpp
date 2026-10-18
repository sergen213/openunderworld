#include "scene/Placeholder.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace ou::scene {
namespace {

struct Rgb { uint8_t r, g, b; };

uint32_t packRgba(Rgb c, uint8_t a = 255) { return c.r | (c.g << 8) | (c.b << 16) | (uint32_t(a) << 24); }

// Checker of `square`-pixel cells at mip 0, full mip chain; cells that shrink below
// one pixel collapse to the average colour (what a box filter would converge to).
TextureData makeChecker(uint32_t id, PixelFormat format, uint32_t size, uint32_t square, Rgb a, Rgb b) {
    TextureData t;
    t.id = id;
    t.format = format;
    const Rgb avg{uint8_t((a.r + b.r) / 2), uint8_t((a.g + b.g) / 2), uint8_t((a.b + b.b) / 2)};
    for (uint32_t level = 0; (size >> level) > 0; ++level) {
        const uint32_t w = size >> level, s = square >> level;
        auto at = [&](uint32_t x, uint32_t y) { return s ? (((x / s + y / s) & 1) ? b : a) : avg; };
        TextureMip mip{w, w, {}};
        if (format == PixelFormat::BC1) {
            const uint32_t blocks = (w + 3) / 4;
            for (uint32_t by = 0; by < blocks; ++by)
                for (uint32_t bx = 0; bx < blocks; ++bx) {
                    Rgb c = s >= 4 ? at(bx * 4, by * 4) : avg; // keep each 4x4 block uniform
                    uint16_t c565 = uint16_t(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
                    // color0 = c, color1 = 0, all indices 0 -> every texel is color0
                    mip.data.insert(mip.data.end(), {uint8_t(c565), uint8_t(c565 >> 8), 0, 0, 0, 0, 0, 0});
                }
        } else {
            for (uint32_t y = 0; y < w; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    Rgb c = at(x, y);
                    if (format == PixelFormat::BGRA8) mip.data.insert(mip.data.end(), {c.b, c.g, c.r, 255});
                    else mip.data.insert(mip.data.end(), {c.r, c.g, c.b, 255});
                }
        }
        t.mips.push_back(std::move(mip));
    }
    return t;
}

// Corners in order p0..p3 are counter-clockwise seen from the front unless `clockwise`.
void addQuad(MeshData& m, const float (&p)[4][3], const float (&n)[3], float uvScale, bool clockwise = false) {
    const float uv[4][2] = {{0, 0}, {uvScale, 0}, {uvScale, uvScale}, {0, uvScale}};
    uint32_t base = uint32_t(m.vertices.size());
    for (int i = 0; i < 4; ++i)
        m.vertices.push_back({{p[i][0], p[i][1], p[i][2]}, {n[0], n[1], n[2]}, {uv[i][0], uv[i][1]}});
    if (clockwise) m.indices.insert(m.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    else m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

MeshData makeUnitCube(uint32_t textureId, uint32_t color) {
    MeshData m;
    for (int axis = 0; axis < 3; ++axis)
        for (float sign : {-1.0f, 1.0f}) {
            const int u = (axis + 1) % 3, v = (axis + 2) % 3;
            float n[3] = {}, p[4][3] = {};
            n[axis] = sign;
            const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (int i = 0; i < 4; ++i) {
                p[i][axis] = 0.5f * sign;
                p[i][u] = 0.5f * corners[i][0];
                p[i][v] = 0.5f * corners[i][1];
            }
            addQuad(m, p, n, 1.0f, sign < 0); // (u, v, axis) is right-handed, so -axis faces flip
        }
    for (auto& vtx : m.vertices) vtx.color = color;
    m.submeshes.push_back({0, uint32_t(m.indices.size()), textureId});
    return m;
}

void setTransform(MeshData& m, float scaleX, float scaleY, float scaleZ, float tx, float ty, float tz) {
    std::fill(std::begin(m.transform), std::end(m.transform), 0.0f);
    m.transform[0] = scaleX;
    m.transform[5] = scaleY;
    m.transform[10] = scaleZ;
    m.transform[12] = tx;
    m.transform[13] = ty;
    m.transform[14] = tz;
    m.transform[15] = 1.0f;
}

} // namespace

SceneData makePlaceholderScene() {
    SceneData s;
    s.placeholder = true;
    s.textures.push_back(makeChecker(1, PixelFormat::RGBA8, 256, 32, {200, 200, 200}, {60, 60, 70}));
    s.textures.back().origin = "placeholder:checker-rgba8";
    s.textures.push_back(makeChecker(2, PixelFormat::BGRA8, 128, 16, {230, 120, 40}, {40, 40, 40}));
    s.textures.back().origin = "placeholder:checker-bgra8";
    s.textures.push_back(makeChecker(3, PixelFormat::BC1, 128, 16, {60, 150, 230}, {240, 240, 240}));
    s.textures.back().origin = "placeholder:checker-bc1";

    MeshData ground;
    const float e = 2000.0f;
    addQuad(ground, {{-e, -e, 0}, {e, -e, 0}, {e, e, 0}, {-e, e, 0}}, {0, 0, 1}, 16.0f);
    ground.submeshes.push_back({0, uint32_t(ground.indices.size()), 1});
    ground.origin = "placeholder:ground";
    s.meshes.push_back(std::move(ground));

    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) {
            const uint32_t tex = uint32_t(1 + (i + j) % 3);
            const Rgb tint{uint8_t(155 + 25 * i), uint8_t(155 + 25 * j), 230};
            MeshData cube = makeUnitCube(tex, packRgba(tint));
            const float z = 50.0f + float((i * 3 + j) % 3) * 60.0f;
            setTransform(cube, 100, 100, 100, (i - 2) * 300.0f, (j - 2) * 300.0f, z);
            cube.origin = "placeholder:cube";
            s.meshes.push_back(std::move(cube));
        }
    return s;
}

std::vector<MeshData> makeTextureGallery(const std::vector<TextureData>& textures) {
    std::vector<MeshData> out;
    const int cols = std::max(1, int(std::ceil(std::sqrt(double(textures.size())))));
    const float cell = 300.0f, size = 256.0f;
    for (size_t k = 0; k < textures.size(); ++k) {
        const TextureData& t = textures[k];
        float w = size, h = size; // keep aspect ratio of mip 0
        if (!t.mips.empty() && t.mips[0].width && t.mips[0].height) {
            float aspect = float(t.mips[0].width) / float(t.mips[0].height);
            if (aspect >= 1) h = size / aspect; else w = size * aspect;
        }
        const float cx = float(int(k) % cols) * cell, cz = -float(int(k) / cols) * cell;
        MeshData m;
        // v = 0 is the first stored row (D3D convention: top of the image) -> top edge.
        addQuad(m, {{cx - w / 2, 0, cz + h / 2}, {cx + w / 2, 0, cz + h / 2}, {cx + w / 2, 0, cz - h / 2},
                    {cx - w / 2, 0, cz - h / 2}},
                {0, -1, 0}, 1.0f, /*clockwise=*/true);
        m.submeshes.push_back({0, 6, t.id});
        m.origin = "placeholder:texture-gallery-quad for " + t.origin;
        out.push_back(std::move(m));
    }
    return out;
}

} // namespace ou::scene
