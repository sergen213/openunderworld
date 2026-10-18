// ou-asset: inspect and dump TRU assets (sections, textures, meshes) from the bigfile.
#include "formats/Formats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace ou;
namespace fs = std::filesystem;

namespace {

int usage() {
    std::cerr << "usage:\n"
                 "  ou-asset sections <gameDir> <drm>\n"
                 "  ou-asset textures <gameDir> <drm>\n"
                 "  ou-asset dump-texture <gameDir> <drm> <textureId> -o out.tga|out.dds\n"
                 "  ou-asset dump-mesh <gameDir> <drm> -o out.obj [--textures]   (writes .mtl + tex_<id>.tga)\n"
                 "  ou-asset stats <gameDir> <drm>\n"
                 "  ou-asset render <gameDir> <drm> -o out.ppm [--view top|front|side] [--size N] [--textured]\n"
                 "<drm> is a bigfile path such as pc-w\\lara.drm; ids accept 0x prefix.\n";
    return 2;
}

cdc::Drm loadDrm(const cdc::Bigfile& bf, const std::string& path) {
    const auto rec = bf.find(path);
    if (!rec) throw cdc::FormatError("not in bigfile: " + path);
    return cdc::parseDrm(bf.read(*rec));
}

const char* formatName(scene::PixelFormat f) {
    switch (f) {
    case scene::PixelFormat::RGBA8: return "RGBA8";
    case scene::PixelFormat::BGRA8: return "BGRA8";
    case scene::PixelFormat::BC1: return "BC1";
    case scene::PixelFormat::BC2: return "BC2";
    case scene::PixelFormat::BC3: return "BC3";
    }
    return "?";
}

void put32(std::ostream& o, uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); }

void writeTga(const fs::path& p, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    std::ofstream o(p, std::ios::binary);
    const uint8_t hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(w), uint8_t(w >> 8), uint8_t(h), uint8_t(h >> 8), 32, 0x28};
    o.write(reinterpret_cast<const char*>(hdr), 18);
    std::vector<uint8_t> bgra(rgba);
    for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    o.write(reinterpret_cast<const char*>(bgra.data()), std::streamsize(bgra.size()));
}

void writeDds(const fs::path& p, const scene::TextureData& t) {
    std::ofstream o(p, std::ios::binary);
    const bool bc = t.format != scene::PixelFormat::RGBA8 && t.format != scene::PixelFormat::BGRA8;
    o.write("DDS ", 4);
    put32(o, 124);
    put32(o, 0x1007 | 0x20000 | (bc ? 0x80000 : 0x8)); // caps|height|width|pixelformat|mipcount|linear/pitch
    put32(o, t.mips[0].height);
    put32(o, t.mips[0].width);
    put32(o, bc ? uint32_t(t.mips[0].data.size()) : t.mips[0].width * 4);
    put32(o, 0);
    put32(o, uint32_t(t.mips.size()));
    for (int i = 0; i < 11; ++i) put32(o, 0);
    put32(o, 32);
    if (bc) {
        put32(o, 4); // DDPF_FOURCC
        o.write(t.format == scene::PixelFormat::BC1 ? "DXT1" : t.format == scene::PixelFormat::BC2 ? "DXT3" : "DXT5", 4);
        for (int i = 0; i < 5; ++i) put32(o, 0);
    } else {
        put32(o, 0x41); // RGB | ALPHAPIXELS
        put32(o, 0);
        put32(o, 32);
        const bool bgra = t.format == scene::PixelFormat::BGRA8;
        put32(o, bgra ? 0x00FF0000 : 0x000000FF);
        put32(o, 0x0000FF00);
        put32(o, bgra ? 0x000000FF : 0x00FF0000);
        put32(o, 0xFF000000);
    }
    put32(o, 0x1000 | 0x400000 | 0x8); // texture | mipmap | complex
    for (int i = 0; i < 4; ++i) put32(o, 0);
    for (const auto& m : t.mips) o.write(reinterpret_cast<const char*>(m.data.data()), std::streamsize(m.data.size()));
}

std::optional<scene::TextureData> findTexture(const cdc::Drm& drm, uint32_t id) {
    for (const auto& s : drm.sections)
        if (s.type == cdc::SectionType::Texture && s.id == id) return formats::decodeTexture(s);
    return std::nullopt;
}

// Plausibility metrics: bbox, index validity, normal length, normal vs. face winding, UV range.
void printStats(const std::vector<scene::MeshData>& meshes) {
    for (const auto& m : meshes) {
        float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
        float uvlo[2] = {INFINITY, INFINITY}, uvhi[2] = {-INFINITY, -INFINITY};
        double nmin = 1e9, nmax = 0;
        for (const auto& v : m.vertices) {
            for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v.position[k]), hi[k] = std::max(hi[k], v.position[k]);
            for (int k = 0; k < 2; ++k) uvlo[k] = std::min(uvlo[k], v.uv[k]), uvhi[k] = std::max(uvhi[k], v.uv[k]);
            const double n = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
            nmin = std::min(nmin, n), nmax = std::max(nmax, n);
        }
        size_t bad = 0, degenerate = 0, agree = 0, faces = 0;
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            const uint32_t a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
            if (a >= m.vertices.size() || b >= m.vertices.size() || c >= m.vertices.size()) { ++bad; continue; }
            const auto &A = m.vertices[a].position, &B = m.vertices[b].position, &C = m.vertices[c].position;
            const float e1[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, e2[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
            const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            if (n[0] == 0 && n[1] == 0 && n[2] == 0) { ++degenerate; continue; }
            const auto& vn = m.vertices[a].normal;
            agree += (n[0] * vn[0] + n[1] * vn[1] + n[2] * vn[2]) > 0;
            ++faces;
        }
        std::printf("%s: %zu verts, %zu tris, %zu submeshes\n  bbox (%.1f %.1f %.1f)-(%.1f %.1f %.1f)\n"
                    "  |n| %.3f..%.3f, uv (%.3f %.3f)-(%.3f %.3f)\n  bad idx %zu, degenerate %zu, "
                    "vertex normal agrees with CCW face normal: %.1f%%\n",
                    m.origin.c_str(), m.vertices.size(), m.indices.size() / 3, m.submeshes.size(), lo[0], lo[1], lo[2],
                    hi[0], hi[1], hi[2], nmin, nmax, uvlo[0], uvlo[1], uvhi[0], uvhi[1], bad, degenerate,
                    faces ? 100.0 * double(agree) / double(faces) : 0.0);
    }
}

// Tiny orthographic rasterizer: z-buffered, Lambert-shaded, optionally textured (mip 0, wrap).
void render(const scene::SceneData& sc, const std::string& view, int size, bool textured, const fs::path& out) {
    int ax = 0, ay = 2, az = 1; // front: x right, z up, looking along +y
    float sx = 1, sz = -1;
    if (view == "top") ay = 1, az = 2, sz = 1;  // x right, y up, looking down -z
    if (view == "side") ax = 1, ay = 2, az = 0, sz = 1;
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (const auto& m : sc.meshes)
        for (const auto& v : m.vertices)
            for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v.position[k]), hi[k] = std::max(hi[k], v.position[k]);
    const float span = std::max(hi[ax] - lo[ax], hi[ay] - lo[ay]) * 1.02f;
    const float scale = float(size) / span;
    std::vector<float> zb(size_t(size) * size, -INFINITY);
    std::vector<uint8_t> img(size_t(size) * size * 3, 32);
    std::map<uint32_t, const scene::TextureData*> texById;
    std::map<uint32_t, std::vector<uint8_t>> rgba;
    if (textured)
        for (const auto& t : sc.textures) {
            texById[t.id] = &t;
            rgba[t.id] = formats::toRgba8(t.format, t.mips[0].width, t.mips[0].height, t.mips[0].data);
        }
    const float L[3] = {0.4f, -0.7f, 0.6f};
    for (const auto& m : sc.meshes)
        for (const auto& sm : m.submeshes) {
            const auto tex = texById.find(sm.textureId);
            for (uint32_t i = sm.firstIndex; i + 2 < sm.firstIndex + sm.indexCount; i += 3) {
                const scene::Vertex* v[3] = {&m.vertices[m.indices[i]], &m.vertices[m.indices[i + 1]], &m.vertices[m.indices[i + 2]]};
                float px[3], py[3], pz[3];
                for (int k = 0; k < 3; ++k) {
                    px[k] = (v[k]->position[ax] - (lo[ax] + hi[ax]) / 2) * scale * sx + size / 2.f;
                    py[k] = (v[k]->position[ay] - (lo[ay] + hi[ay]) / 2) * scale * -1 + size / 2.f;
                    pz[k] = v[k]->position[az] * (sz);
                }
                const float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
                if (area == 0) continue;
                const int x0 = std::max(0, int(std::floor(std::min({px[0], px[1], px[2]})))),
                          x1 = std::min(size - 1, int(std::ceil(std::max({px[0], px[1], px[2]})))),
                          y0 = std::max(0, int(std::floor(std::min({py[0], py[1], py[2]})))),
                          y1 = std::min(size - 1, int(std::ceil(std::max({py[0], py[1], py[2]}))));
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x) {
                        const float qx = x + 0.5f, qy = y + 0.5f;
                        float w[3] = {((px[1] - qx) * (py[2] - qy) - (px[2] - qx) * (py[1] - qy)) / area,
                                      ((px[2] - qx) * (py[0] - qy) - (px[0] - qx) * (py[2] - qy)) / area, 0};
                        w[2] = 1 - w[0] - w[1];
                        if (w[0] < 0 || w[1] < 0 || w[2] < 0) continue;
                        const float z = w[0] * pz[0] + w[1] * pz[1] + w[2] * pz[2];
                        float& d = zb[size_t(y) * size + x];
                        if (z <= d) continue;
                        d = z;
                        float n[3], uv[2] = {0, 0};
                        for (int k = 0; k < 3; ++k) n[k] = w[0] * v[0]->normal[k] + w[1] * v[1]->normal[k] + w[2] * v[2]->normal[k];
                        for (int k = 0; k < 2; ++k) uv[k] = w[0] * v[0]->uv[k] + w[1] * v[1]->uv[k] + w[2] * v[2]->uv[k];
                        const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-6f;
                        const float shade = 0.35f + 0.65f * std::abs(n[0] * L[0] + n[1] * L[1] + n[2] * L[2]) / nl;
                        float c[3] = {200, 200, 200};
                        if (tex != texById.end()) {
                            const auto& t = *tex->second;
                            const int tw = int(t.mips[0].width), th = int(t.mips[0].height);
                            const int tx = ((int(std::floor(uv[0] * tw)) % tw) + tw) % tw;
                            const int ty = ((int(std::floor(uv[1] * th)) % th) + th) % th;
                            const uint8_t* p = &rgba[t.id][(size_t(ty) * tw + tx) * 4];
                            c[0] = p[0], c[1] = p[1], c[2] = p[2];
                        }
                        uint8_t* o = &img[(size_t(y) * size + x) * 3];
                        for (int k = 0; k < 3; ++k) o[k] = uint8_t(std::min(255.f, c[k] * shade));
                    }
            }
        }
    std::ofstream o(out, std::ios::binary);
    o << "P6\n" << size << " " << size << "\n255\n";
    o.write(reinterpret_cast<const char*>(img.data()), std::streamsize(img.size()));
}

} // namespace

int main(int argc, char** argv) try {
    std::vector<std::string> a(argv + 1, argv + argc);
    std::string out, view = "front";
    int size = 1024;
    bool textured = false, deps = false;
    std::vector<std::string> pos;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == "-o" && i + 1 < a.size()) out = a[++i];
        else if (a[i] == "--view" && i + 1 < a.size()) view = a[++i];
        else if (a[i] == "--size" && i + 1 < a.size()) size = std::stoi(a[++i]);
        else if (a[i] == "--textured" || a[i] == "--textures") textured = true;
        else if (a[i] == "--deps") deps = true;
        else pos.push_back(a[i]);
    }
    if (pos.size() < 3) return usage();
    const std::string& cmd = pos[0];
    const cdc::Bigfile bf(fs::path(pos[1]) / "bigfile.000");
    const std::string& path = pos[2];

    if (cmd == "sections") {
        const auto drm = loadDrm(bf, path);
        std::printf("version %u, %zu sections, dependencies:", drm.version, drm.sections.size());
        for (const auto& d : drm.dependencies) std::printf(" %s", d.c_str());
        std::printf("\n%5s %-15s %4s %8s %10s %9s\n", "idx", "type", "res", "id", "size", "relocB");
        for (size_t i = 0; i < drm.sections.size(); ++i) {
            const auto& s = drm.sections[i];
            std::printf("%5zu %-15s %4u %8u %10zu %9zu\n", i, std::string(cdc::sectionTypeName(s.type)).c_str(),
                        s.resourceType(), s.id, s.data.size(), s.relocations.size());
        }
    } else if (cmd == "textures") {
        const auto drm = loadDrm(bf, path);
        for (const auto& s : drm.sections) {
            if (s.type != cdc::SectionType::Texture) continue;
            const auto h = formats::parsePcdHeader(s.data);
            std::string why;
            const auto t = formats::decodeTexture(s, &why);
            std::printf("%6u (0x%x) fmt 0x%x %ux%u levels %u flags 0x%x %s%s\n", s.id, s.id, h ? h->format : 0,
                        h ? h->width : 0, h ? h->height : 0, h ? h->mipCount + 1u : 0, h ? h->flags : 0,
                        t ? formatName(t->format) : "UNSUPPORTED: ", t ? "" : why.c_str());
        }
    } else if (cmd == "dump-texture") {
        if (pos.size() < 4 || out.empty()) return usage();
        const auto drm = loadDrm(bf, path);
        const auto t = findTexture(drm, uint32_t(std::stoul(pos[3], nullptr, 0)));
        if (!t) throw cdc::FormatError("texture not found or unsupported");
        if (fs::path(out).extension() == ".dds") writeDds(out, *t);
        else writeTga(out, t->mips[0].width, t->mips[0].height, formats::toRgba8(t->format, t->mips[0].width, t->mips[0].height, t->mips[0].data));
    } else if (cmd == "dump-mesh") {
        if (out.empty()) return usage();
        const auto sc = formats::loadDrmScene(bf, path, deps);
        const fs::path obj(out), mtl = fs::path(out).replace_extension(".mtl");
        std::ofstream o(obj), mo(mtl);
        o << "# " << path << "\nmtllib " << mtl.filename().string() << "\n";
        size_t base = 1;
        for (size_t mi = 0; mi < sc.meshes.size(); ++mi) {
            const auto& m = sc.meshes[mi];
            o << "o mesh" << mi << "\n# " << m.origin << "\n";
            for (const auto& v : m.vertices) o << "v " << v.position[0] << ' ' << v.position[1] << ' ' << v.position[2] << '\n';
            for (const auto& v : m.vertices) o << "vt " << v.uv[0] << ' ' << 1 - v.uv[1] << '\n';
            for (const auto& v : m.vertices) o << "vn " << v.normal[0] << ' ' << v.normal[1] << ' ' << v.normal[2] << '\n';
            for (const auto& s : m.submeshes) {
                o << "usemtl tex_" << s.textureId << '\n';
                for (uint32_t i = s.firstIndex; i + 2 < s.firstIndex + s.indexCount; i += 3) {
                    o << 'f';
                    for (int k = 0; k < 3; ++k) {
                        const size_t idx = base + m.indices[i + k];
                        o << ' ' << idx << '/' << idx << '/' << idx;
                    }
                    o << '\n';
                }
            }
            base += m.vertices.size();
        }
        for (const auto& t : sc.textures) {
            const std::string name = "tex_" + std::to_string(t.id);
            mo << "newmtl " << name << "\nmap_Kd " << name << ".tga\n";
            if (textured)
                writeTga(obj.parent_path() / (name + ".tga"), t.mips[0].width, t.mips[0].height,
                         formats::toRgba8(t.format, t.mips[0].width, t.mips[0].height, t.mips[0].data));
        }
        mo << "newmtl tex_0\n";
        for (const auto& w : sc.warnings) std::cerr << "warning: " << w << '\n';
        std::printf("%zu meshes, %zu textures, %zu warnings\n", sc.meshes.size(), sc.textures.size(), sc.warnings.size());
    } else if (cmd == "stats") {
        const auto sc = formats::loadDrmScene(bf, path, deps);
        printStats(sc.meshes);
        for (const auto& w : sc.warnings) std::printf("warning: %s\n", w.c_str());
    } else if (cmd == "render") {
        if (out.empty()) return usage();
        const auto sc = formats::loadDrmScene(bf, path, deps);
        render(sc, view, size, textured, out);
    } else {
        return usage();
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
}
