// ou-asset: inspect and dump TRU assets (sections, textures, meshes, collision, animation, materials, scripts,
// cinematics, the UI font) from the bigfile, and check container round trips.
#include "anim/Anim.hpp"
#include "cine/Cine.hpp"
#include "collision/Collision.hpp"
#include "formats/Formats.hpp"
#include "formats/Shader.hpp"
#include "script/Script.hpp"
#include "ui/Art.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <array>
#include <map>
#include <set>
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
                 "  ou-asset dump-mesh <gameDir> <drm> -o out.obj [--textures] [--level|--deps]  (writes .mtl + tex_<id>.tga)\n"
                 "  ou-asset stats <gameDir> <drm> [--level|--deps]\n"
                 "  ou-asset render <gameDir> <drm> -o out.ppm [--view top|front|side] [--size N] [--textured] [--level|--deps]\n"
                 "  ou-asset shaders <gameDir> all|<drm>        survey ShaderLib (type 9) D3D9 shaders: counts, opcodes, registers\n"
                 "  ou-asset shader-dis <gameDir> <drm> <libId> [slot]   disassemble a ShaderLib section\n"
                 "  ou-asset collision <gameDir> <unit> [-o out.obj]    game BIH per terrain group, face/markup tables\n"
                 "  ou-asset anim <gameDir> <objectDrm> [clipId]         skeletons, anim list, clips; clipId: track dump\n"
                 "  ou-asset materials <gameDir> <drm>                   material sections: state, shaders, textures\n"
                 "  ou-asset script <gameDir> <drm> [typeId]             script types; typeId: members + bytecode listing\n"
                 "  ou-asset cine <gameDir> <name> [channel [count]]     cinematic header/summary; channel: per-frame values\n"
                 "  ou-asset font <gameDir> <drm> [dtpId]                font DTP glyph table (pc-w\\generalbank.drm, 46)\n"
                 "  ou-asset roundtrip <gameDir> all|<drm>              DRM serialise, CDRM re-wrap, PCD9 re-wrap identity\n"
                 "--level loads a unit DRM as a whole level (stream groups, IMFs, intros; transforms baked).\n"
                 "<drm> is a bigfile path such as pc-w\\lara.drm; <name> a pc-w\\cinstream stream (or a bigfile path);\n"
                 "ids accept 0x prefix.\n";
    return 2;
}

cdc::Drm loadDrm(const cdc::BigfileSet& bf, const std::string& path) {
    auto bytes = bf.read(path);
    if (!bytes) throw cdc::FormatError("not in bigfile: " + path);
    return cdc::parseDrm(std::move(*bytes));
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

// Survey of D3D9 shaders (docs/formats/SHADERS.md).
void shaderSurvey(const std::vector<formats::d3d9::InstallShader>& shaders) {
    using namespace formats::d3d9;
    struct Stat { size_t n = 0, ins = 0, minIns = SIZE_MAX, maxIns = 0, maxTemp = 0, rel = 0, pred = 0, flow = 0, failed = 0; };
    Stat st[2];
    std::map<std::string, std::array<size_t, 2>> ops, decls, consts;
    std::map<std::string, size_t> unsupported;
    for (const auto& sh : shaders) {
        Stat& s = st[sh.pixel];
        Program p;
        try {
            p = parse(std::span<const uint8_t>(sh.bytecode));
        } catch (const std::exception& e) {
            ++s.failed;
            std::printf("parse failure %016llx: %s\n", (unsigned long long)sh.hash, e.what());
            continue;
        }
        ++s.n;
        size_t n = 0;
        std::set<std::string> usedOps;
        bool rel = false, pred = false, flow = false;
        for (const auto& in : p.instructions) {
            if (in.op == Dcl) {
                std::string d = format(in.dst);
                if (in.dst.type == Reg::Sampler) d = in.textureType == 3 ? "sampler cube" : in.textureType == 4 ? "sampler volume" : "sampler 2d";
                else if (in.dst.type == Reg::Misc) d = in.dst.num ? "vFace" : "vPos";
                else {
                    static const char* u[] = {"position", "blendweight", "blendindices", "normal", "psize", "texcoord", "tangent",
                                              "binormal", "tessfactor", "positiont", "color", "fog", "depth", "sample"};
                    d = std::string(in.dst.type == Reg::Output ? "out " : "in ") + u[size_t(in.usage) % 14] + std::to_string(in.usageIndex);
                }
                ++decls[d][sh.pixel];
                continue;
            }
            if (in.op == Def || in.op == DefI || in.op == DefB) continue;
            ++n;
            usedOps.insert(opName(in.op));
            if (!opSupported(in.op)) ++unsupported[opName(in.op)];
            if (in.hasDst && in.dst.type == Reg::Temp) s.maxTemp = std::max<size_t>(s.maxTemp, in.dst.num + 1);
            for (const auto& x : in.src) rel |= x.relative;
            pred |= in.predicated;
            flow |= in.op == If || in.op == IfC || in.op == Loop || in.op == Rep || in.op == Call || in.op == CallNZ;
        }
        for (const auto& o : usedOps) ++ops[o][sh.pixel];
        for (const auto& c : p.constants)
            ++consts[c.name + " " + "bics"[c.registerSet & 3] + std::to_string(c.registerIndex) + "[" + std::to_string(c.registerCount) + "]"][sh.pixel];
        s.ins += n, s.minIns = std::min(s.minIns, n), s.maxIns = std::max(s.maxIns, n);
        s.rel += rel, s.pred += pred, s.flow += flow;
    }
    for (int k = 0; k < 2; ++k)
        std::printf("%s: %zu shaders (%zu failed), instructions min %zu avg %.1f max %zu, max temps %zu, relative addressing %zu, "
                    "predication %zu, flow control %zu\n",
                    k ? "ps_3_0" : "vs_3_0", st[k].n, st[k].failed, st[k].n ? st[k].minIns : 0, st[k].n ? double(st[k].ins) / st[k].n : 0.0,
                    st[k].maxIns, st[k].maxTemp, st[k].rel, st[k].pred, st[k].flow);
    auto table = [](const char* title, const std::map<std::string, std::array<size_t, 2>>& m) {
        std::printf("\n%s (shaders using it: vs ps)\n", title);
        for (const auto& [k, v] : m) std::printf("  %-48s %6zu %6zu\n", k.c_str(), v[0], v[1]);
    };
    table("opcodes", ops);
    table("declarations", decls);
    table("constants (CTAB)", consts);
    std::printf("\nunsupported opcodes:%s\n", unsupported.empty() ? " none" : "");
    for (const auto& [k, v] : unsupported) std::printf("  %s %zu\n", k.c_str(), v);
}

// ---- collision (docs/formats/COLLISION.md) ---------------------------------------------------------------
// Pointer chasing through DRM relocations, as collision/LevelCollision.cpp does.
struct Ptrs {
    const cdc::Drm& drm;
    std::map<uint32_t, std::unordered_map<uint32_t, cdc::Pointer>> cache;
    std::optional<cdc::Pointer> at(cdc::Pointer p) {
        auto it = cache.find(p.section);
        if (it == cache.end()) it = cache.emplace(p.section, cdc::sectionPointers(drm, p.section)).first;
        const auto f = it->second.find(p.offset);
        return f == it->second.end() ? std::nullopt : std::optional(f->second);
    }
    // Bytes past the section end read as 0 (the last face / BIH node is stored without its padding).
    template <class T> T rd(cdc::Pointer p, size_t extra = 0) const {
        const auto d = drm.sections.at(p.section).data;
        const size_t off = size_t(p.offset) + extra;
        T v{};
        if (off < d.size()) std::memcpy(&v, d.data() + off, std::min(sizeof v, d.size() - off));
        return v;
    }
};
cdc::Pointer plus(cdc::Pointer p, uint32_t off) { return {p.section, p.offset + off}; }

void collisionDump(const cdc::Drm& drm, const std::string& out) {
    Ptrs u{drm, {}};
    const auto terrain = u.at({0, 0});
    if (!terrain) throw cdc::FormatError("section 0 has no Terrain pointer (not a unit DRM?)");
    const uint32_t groups = u.rd<uint32_t>(*terrain, 0x08);
    const auto groupArray = u.at(plus(*terrain, 0x0C));
    std::printf("game BIH (16-byte nodes, depth-first; left = this + 1, right = this + value):\n");
    std::printf("%5s %10s %6s %7s %7s %6s %6s %6s %6s %9s %s\n", "group", "flags", "verts", "faces", "nodes", "height",
                "inner", "leaves", "depth", "leafFaces", "check");
    for (uint32_t g = 0; g < groups && groupArray; ++g) {
        const auto grp = plus(*groupArray, 0x50 * g);
        const auto mesh = u.at(plus(grp, 0x38));
        if (!mesh) continue;
        const auto extra = u.at(plus(grp, 0x30));
        const uint32_t flags = extra ? u.rd<uint32_t>(*extra) : 0;
        const uint32_t numNodes = u.rd<uint32_t>(*mesh, 0x40), numFaces = u.rd<uint32_t>(*mesh, 0x44),
                       numVerts = u.rd<uint32_t>(*mesh, 0x48);
        const uint16_t height = u.rd<uint16_t>(*mesh, 0x56);
        const auto nodes = u.at(plus(*mesh, 0x38));
        uint32_t inner = 0, leaves = 0, maxDepth = 0;
        uint64_t leafFaces = 0;
        std::string check = "ok";
        std::vector<uint8_t> seen(numNodes);
        std::vector<std::pair<uint32_t, uint32_t>> stack;
        if (nodes && numNodes) stack.push_back({0, 1});
        while (!stack.empty()) {
            const auto [n, depth] = stack.back();
            stack.pop_back();
            if (n >= numNodes || seen[n]++) { check = "bad child / revisit"; continue; }
            maxDepth = std::max(maxDepth, depth);
            const auto node = plus(*nodes, 16 * n);
            if (const uint8_t count = u.rd<uint8_t>(node, 0x0D)) {
                ++leaves;
                leafFaces += count;
                if (u.rd<uint32_t>(node, 0x08) + count > numFaces) check = "leaf past faces";
            } else {
                ++inner;
                stack.push_back({n + u.rd<uint32_t>(node, 0x08), depth + 1});
                stack.push_back({n + 1, depth + 1});
            }
        }
        if (inner + leaves != numNodes && check == "ok") check = "unreached nodes";
        if (leafFaces != numFaces && check == "ok") check = "leaf faces != faces";
        std::printf("%5u %10x %6u %7u %7u %6u %6u %6u %6u %9llu %s\n", g, flags, numVerts, numFaces, numNodes, height,
                    inner, leaves, maxDepth, (unsigned long long)leafFaces, check.c_str());
    }
    std::vector<std::string> warnings;
    const auto mesh = collision::loadLevelCollision(drm, &warnings);
    std::printf("loaded mesh: %zu vertices, %zu faces, %zu markup\n", mesh.vertices.size(), mesh.faces.size(), mesh.markup.size());
    std::map<uint32_t, size_t> flags, masks, materials, client, perGroup;
    for (const auto& f : mesh.faces) ++flags[f.flags], ++masks[f.mask], ++materials[f.material], ++client[f.clientFlags], ++perGroup[f.group];
    const auto hist = [](const char* name, const std::map<uint32_t, size_t>& h) {
        std::printf("%s:", name);
        for (const auto& [k, n] : h) std::printf(" 0x%x=%zu", k, n);
        std::printf("\n");
    };
    hist("face flags (+0x0C)", flags);
    hist("face mask (+0x0D)", masks);
    hist("face clientFlags (+0x0E)", client);
    hist("face material (+0x10)", materials);
    hist("faces per group", perGroup);
    std::map<std::string, size_t> types;
    for (const auto& m : mesh.markup) ++types[std::string(collision::markupTypeName(m.type))];
    std::printf("markup types:");
    for (const auto& [k, n] : types) std::printf(" %s=%zu", k.c_str(), n);
    std::printf("\n%6s %5s %-12s %4s %6s %s\n", "id", "type", "name", "pts", "intro", "first point / camera");
    for (const auto& m : mesh.markup) {
        std::printf("%6u %5u %-12s %4zu %6u", m.id, m.type, std::string(collision::markupTypeName(m.type)).c_str(),
                    m.points.size(), m.intro);
        if (!m.points.empty()) std::printf(" (%.1f %.1f %.1f)", m.points[0].x, m.points[0].y, m.points[0].z);
        if (m.camera) std::printf(" camera type %u, %zu spline points", m.camera->type, m.camera->pos.size());
        std::printf("\n");
    }
    for (const auto& w : warnings) std::printf("warning: %s\n", w.c_str());
    if (!out.empty()) {
        std::ofstream o(out);
        for (const auto& v : mesh.vertices) o << "v " << v.x << ' ' << v.y << ' ' << v.z << '\n';
        uint32_t group = UINT32_MAX;
        for (const auto& f : mesh.faces) {
            if (f.group != group) o << "g group" << (group = f.group) << '\n';
            o << "f " << f.v[0] + 1 << ' ' << f.v[1] + 1 << ' ' << f.v[2] + 1 << '\n';
        }
        std::printf("wrote %s\n", out.c_str());
    }
}

// ---- animation (docs/formats/ANIMATION.md) ---------------------------------------------------------------
void animDump(const cdc::Drm& drm, std::optional<uint32_t> clipId) {
    static const char* const channel[] = {"rotX", "rotY", "rotZ", "scaleX", "scaleY", "scaleZ", "transX", "transY", "transZ"};
    std::vector<std::string> warnings;
    if (!clipId) {
        for (const auto& sk : anim::decodeSkeletons(drm, warnings)) {
            std::printf("skeleton: render model %u, %zu bones\n", sk.renderModelId, sk.bones.size());
            for (size_t b = 0; b < sk.bones.size(); ++b)
                std::printf("  bone %3zu parent %3d pivot (%.2f %.2f %.2f)\n", b, sk.bones[b].parent, sk.bones[b].pivot[0],
                            sk.bones[b].pivot[1], sk.bones[b].pivot[2]);
        }
        const auto list = anim::animList(drm);
        std::printf("animation list: %zu entries\n", list.size());
        for (size_t i = 0; i < list.size(); ++i) std::printf("  %4zu anim %5u %s\n", i, list[i].animId, list[i].name.c_str());
        std::printf("animation patterns: %zu\n", anim::animPatterns(drm).size());
        std::printf("clips (type-2 sections):\n%6s %5s %7s %8s %5s %8s\n", "id", "keys", "tpk", "seconds", "segs", "animated");
    }
    size_t clips = 0, bad = 0;
    for (const auto& s : drm.sections) {
        if (s.type != cdc::SectionType::Animation || (clipId && s.id != *clipId)) continue;
        try {
            const auto c = anim::decodeClip(s.data);
            ++clips;
            size_t animated = 0;
            for (const auto& seg : c.tracks)
                for (const auto& t : seg) animated += !t.empty();
            if (!clipId) {
                std::printf("%6u %5u %7u %8.3f %5zu %8zu\n", c.id, c.keyCount, c.timePerKey, c.durationSeconds(), c.tracks.size(), animated);
                continue;
            }
            std::printf("clip %u: %u keys, %u time units per key (%.3f s), %zu segments, %zu animated tracks\n", c.id,
                        c.keyCount, c.timePerKey, c.durationSeconds(), c.tracks.size(), animated);
            for (size_t seg = 0; seg < c.tracks.size(); ++seg)
                for (int ch = 0; ch < anim::kChannels; ++ch) {
                    const auto& t = c.tracks[seg][size_t(ch)];
                    if (t.empty()) continue;
                    std::printf("seg %3zu %-6s", seg, channel[ch]);
                    for (float v : t) std::printf(" %.4g", v);
                    std::printf("\n");
                }
        } catch (const std::exception& e) {
            ++bad;
            std::printf("%6u error: %s\n", s.id, e.what());
        }
    }
    if (clipId && !clips && !bad) throw cdc::FormatError("no animation section " + std::to_string(*clipId));
    if (!clipId) std::printf("%zu clips decoded, %zu errors\n", clips, bad);
    for (const auto& w : warnings) std::printf("warning: %s\n", w.c_str());
}

// ---- materials (docs/formats/MATERIALS.md) ---------------------------------------------------------------
void materialsDump(const cdc::Drm& drm) {
    static const char* const blend[] = {"opaque", "alpha", "additive", "average"};
    std::printf("%6s %-8s %3s %3s %3s %3s %4s %8s %6s %6s %3s %3s %3s  textures (id:category:slot)\n", "id", "blend", "mod",
                "ref", "2s", "shd", "zw", "flags", "vsLib", "psLib", "vsC", "psC", "smp");
    size_t n = 0;
    for (const auto& s : drm.sections) {
        if (s.type != cdc::SectionType::Material) continue;
        ++n;
        const auto st = formats::materialState(s);
        formats::MaterialShaderLibs libs;
        const auto m = formats::decodeMaterial(s, &libs);
        std::printf("%6u %-8s %3u %3u %3d %3d %4d %8x %6u %6u %3zu %3zu %3zu ", s.id, blend[size_t(st.blend) & 3], m.blendMode,
                    st.alphaRef, st.doubleSided, st.castsShadow, st.depthWrite, m.flags, libs.vertex, libs.pixel,
                    m.vsParams.size() / 4, m.psParams.size() / 4, m.samplers.size());
        for (const auto& t : formats::materialTextures(s)) std::printf(" %u:%u:%u", t.textureId, t.category, t.slot);
        std::printf("\n");
    }
    std::printf("%zu material sections\n", n);
}

// ---- scripts (docs/formats/SCRIPTS.md) -------------------------------------------------------------------
// cdc SCMD_* names (cdcEngineDXHR cdcScript/ByteCodeEnum.h), opcode = index; same table as tests/test_gates.cpp.
constexpr const char* kOps[] = {
    "NOP", "POP", "PUSH", "PUSHI", "PUSHI8", "PUSHC", "PUSHL8", "PUSHL16", "PUSHL32", "PUSHLA", "PUSHLR", "PUSHS",
    "PUSHS8", "PUSHS16", "PUSHS32", "PUSHSA", "PUSHSR", "PUSHM8", "PUSHM16", "PUSHM32", "PUSHMA", "PUSHMR", "PUSHT8",
    "PUSHT16", "PUSHT32", "PUSHTA", "PUSHTR", "DREF8", "DREF16", "DREF32", "DREFA", "DREFR", "DREFD", "BOUNDS",
    "DBOUNDS", "MAP8", "MAP16", "MAP32", "MAPA", "MAPR", "MAPG", "IMAP8", "IMAP16", "IMAP32", "IMAPA", "IMAPR",
    "MOVE8", "MOVE32", "MOVER", "MOVEC", "MOVEM", "MOVED", "MOVEU", "MOVET", "MOVEN", "MOVEAR", "MOVEAC", "MOVEAM",
    "MOVEAD", "MOVEAU", "MOVEAT", "NEGI", "ADDI", "SUBI", "MULI", "DIVI", "MODI", "LTI", "LTEQI", "EQI", "EQI8",
    "NEGF", "ADDF", "SUBF", "MULF", "DIVF", "LTF", "LTEQF", "EQF", "EQC", "NOTB", "ANDB", "ORB", "XORB", "AND", "OR",
    "XOR", "SHL", "SHR", "INV", "FTOI", "FTOB", "ITOF", "ITOB", "BTOI", "BTOF", "CTOI", "CTOF", "CTOB", "JMP", "JMPF",
    "CALLU", "CALL1", "CALL2", "CALL3", "CALL4", "CALL5", "SSEQU", "SSEQ1", "SSEQ2", "SSEQ3", "SSEQ4", "SSEQ5",
    "PSEQU", "PSEQ1", "PSEQ2", "PSEQ3", "PSEQ4", "PSEQ5", "START", "TSSTART", "TPSTART", "TEND", "RET", "SLEEP",
    "STATE", "ISTYPE", "NEW", "WAIT", "PUSHD8", "PUSHD16", "PUSHD32", "PUSHDA", "MOVEP"};
static_assert(std::size(kOps) == script::kOpCount && std::string_view(kOps[script::kOpCallU]) == "CALLU" &&
              std::string_view(kOps[script::kOpRet]) == "RET");

void scriptDump(const cdc::Drm& drm, std::optional<uint32_t> typeId) {
    size_t types = 0, errors = 0;
    if (!typeId) std::printf("%6s %6s %-24s %5s %7s %9s %6s\n", "id", "super", "native", "size", "members", "functions", "states");
    for (uint32_t i = 0; i < drm.sections.size(); ++i) {
        const auto& s = drm.sections[i];
        if (s.type != cdc::SectionType::Script || (typeId && s.id != *typeId)) continue;
        script::ScriptType t;
        try {
            t = script::parseScriptType(drm, i);
        } catch (const std::exception& e) {
            ++errors;
            std::printf("%6u error: %s\n", s.id, e.what());
            continue;
        }
        ++types;
        if (!typeId) {
            std::printf("%6u %6u %-24s %5u %7zu %9zu %6zu\n", t.id, t.superId, t.nativeName.c_str(), t.size, t.members.size(),
                        t.functions.size(), t.states.size());
            continue;
        }
        std::printf("script type %u (section %u): super %u, native '%s', %u bytes, %zu states\n", t.id, i, t.superId,
                    t.nativeName.c_str(), t.size, t.states.size());
        for (const auto& m : t.members) {
            std::printf("  member +0x%03x type 0x%02x typeId %u argTypeId %u symbol %u", m.offset, m.type, m.typeId,
                        m.argTypeId, m.name);
            if (m.init) std::printf(" init 0x%x", *m.init);
            if (!m.initText.empty()) std::printf(" \"%s\"", m.initText.c_str());
            std::printf("\n");
        }
        for (size_t f = 0; f < t.functions.size(); ++f) {
            const auto& fn = t.functions[f];
            std::printf("  function %zu @%u:%x vt %d, %u args, %u locals, returns 0x%x%s, %u words\n", f, fn.self.section,
                        fn.self.offset, fn.vtIndex, fn.argCount, fn.localCount, fn.returnType, fn.native ? ", native" : "",
                        fn.wordCount);
            for (const auto& in : fn.code) {
                std::printf("    %5u  %-8s %d", in.word, in.op < std::size(kOps) ? kOps[in.op] : "?", in.arg);
                for (const auto& p : in.ptrs) std::printf(" ->%u:%x", p.section, p.offset);
                if (in.ref) std::printf(" type %u", in.ref);
                if (in.op == script::kOpPush) std::printf(" #0x%x", in.imm);
                if (!in.text.empty()) std::printf(" \"%s\"", in.text.c_str());
                std::printf("\n");
            }
        }
    }
    if (typeId && !types && !errors) throw cdc::FormatError("no script section " + std::to_string(*typeId));
    if (!typeId) std::printf("%zu script types, %zu errors\n", types, errors);
}

// ---- cinematics (docs/formats/CINEMATICS.md) -------------------------------------------------------------
void cineDump(const cdc::BigfileSet& bf, const std::string& name, std::optional<uint32_t> channel, uint32_t count) {
    const std::string path = name.find('\\') == std::string::npos ? "pc-w\\cinstream\\" + name + ".mul" : name;
    const auto mul = bf.read(path);
    if (!mul) throw cdc::FormatError("not in bigfile: " + path);
    const auto c = cine::demux(*mul);
    const auto sum = cine::scan(c);
    const auto& h = sum.header;
    std::printf("%s: CINE version %u, unit %s (%u), %u channels, %d frames (%.2f s), %zu subtitle entries, clean end %d\n",
                path.c_str(), h.version, h.unitName.c_str(), h.unitId, h.channels, sum.frames, sum.frames / cine::kFps,
                sum.subtitles.size(), sum.cleanEnd);
    for (const auto& m : h.markers) std::printf("  marker unit %u intro %d channels %u..%u\n", m.unit, m.intro, m.channel, m.channel + 8);
    for (const auto& in : h.instances) {
        std::printf("  instance intro %d: %u bones, channels %u.. (bone 0 at %u), %zu lights", in.intro, in.bones, in.channel,
                    in.boneChannel(0), in.lights.size());
        if (!in.lights.empty()) std::printf(" (channels %u..)", in.lightChannel);
        if (in.vars) std::printf(", %u vars at %u", in.vars, in.varChannel);
        if (in.endPos) std::printf(", ends at (%.0f %.0f %.0f) unit %u", in.endPos->x, in.endPos->y, in.endPos->z, in.endUnit);
        std::printf("\n");
    }
    for (const uint32_t cam : h.cameras) std::printf("  camera channels %u..%u\n", cam, cam + 15);
    for (const auto& e : h.events) std::printf("  event %u on channel %u (target %u)\n", e.id, e.channel, h.eventTarget);
    if (!channel) return;
    if (*channel >= h.channels) throw cdc::FormatError("channel past the header's " + std::to_string(h.channels));
    count = std::min(count, h.channels - *channel);
    cine::Decoder dec(c, h);
    while (dec.next()) {
        std::printf("%5d", dec.frame());
        for (uint32_t k = 0; k < count; ++k) std::printf(" %.5g", dec.values()[*channel + k]);
        std::printf("\n");
    }
}

// ---- UI font (docs/formats/UI.md) ------------------------------------------------------------------------
void fontDump(const cdc::Drm& drm, uint32_t dtpId) {
    std::string why;
    const auto f = ui::parseFont(drm, dtpId, &why);
    if (!f) throw cdc::FormatError("font DTP " + std::to_string(dtpId) + ": " + why);
    std::printf("font DTP %u: textures %u / %u, cell height %d, spacing %d, scale %.3f x %.3f, %zu glyphs\n", dtpId, f->texture,
                f->texture2, f->cellHeight, f->spacing, f->scaleX, f->scaleY, f->glyphs.size());
    std::vector<uint32_t> codes;
    for (const auto& [code, g] : f->glyphs) codes.push_back(code);
    std::ranges::sort(codes);
    for (const uint32_t code : codes) {
        const auto& g = f->glyphs.at(code);
        std::printf("  U+%04X %-4s width %2u at (%4u, %4u)%s\n", code, code >= 0x20 && code < 0x7F ? std::string(1, char(code)).c_str() : "",
                    g.width, g.x, g.y, g.second ? " texture2" : "");
    }
}

// ---- container round trip (docs/formats/CONTAINERS.md §8, TEXTURES.md "Round trip") ---------------------
struct RoundTripCounts {
    size_t drms = 0, cdrm = 0, cdrmSame = 0, drmSame = 0, textures = 0, pcdSame = 0;
    std::vector<std::string> mismatches;
};
void roundTrip(const std::string& label, std::vector<uint8_t> bytes, RoundTripCounts& n) {
    const bool wrapped = cdc::isCdrm(bytes);
    try {
        if (wrapped) {
            ++n.cdrm;
            if (cdc::writeCdrm(cdc::readCdrmBlocks(bytes)) == bytes) ++n.cdrmSame;
            else n.mismatches.push_back(label + ": CDRM re-wrap differs");
        }
        const auto drm = cdc::parseDrm(std::move(bytes));
        ++n.drms;
        if (cdc::serializeDrm(cdc::toDrmImage(drm)) == drm.bytes) ++n.drmSame;
        else n.mismatches.push_back(label + ": DRM serialise differs");
        for (const auto& s : drm.sections) {
            if (s.type != cdc::SectionType::Texture) continue;
            ++n.textures;
            const auto h = formats::parsePcdHeader(s.data);
            if (h && s.data.size() - 24 == h->dataSize &&
                formats::encodePcd(*h, s.data.subspan(24)) == std::vector<uint8_t>(s.data.begin(), s.data.end()))
                ++n.pcdSame;
            else
                n.mismatches.push_back(label + ": PCD9 re-wrap differs for texture " + std::to_string(s.id));
        }
    } catch (const std::exception& e) {
        n.mismatches.push_back(label + ": " + e.what());
    }
}

} // namespace

int main(int argc, char** argv) try {
    std::vector<std::string> a(argv + 1, argv + argc);
    std::string out, view = "front";
    int size = 1024;
    bool textured = false, deps = false, level = false;
    std::vector<std::string> pos;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == "-o" && i + 1 < a.size()) out = a[++i];
        else if (a[i] == "--view" && i + 1 < a.size()) view = a[++i];
        else if (a[i] == "--size" && i + 1 < a.size()) size = std::stoi(a[++i]);
        else if (a[i] == "--textured" || a[i] == "--textures") textured = true;
        else if (a[i] == "--deps") deps = true;
        else if (a[i] == "--level") level = true;
        else pos.push_back(a[i]);
    }
    if (pos.size() < 3) return usage();
    const std::string& cmd = pos[0];
    const cdc::BigfileSet bf(pos[1]);
    // The CPU tools work in world space: bake MeshData::transform (instances) into the vertices.
    auto scene = [&] {
        auto sc = level ? formats::loadLevelScene(bf, pos[2]) : formats::loadDrmScene(bf, pos[2], deps);
        for (auto& m : sc.meshes)
            for (auto& v : m.vertices) {
                const float* t = m.transform;
                float p[3], n[3];
                for (int r = 0; r < 3; ++r) {
                    p[r] = t[r] * v.position[0] + t[4 + r] * v.position[1] + t[8 + r] * v.position[2] + t[12 + r];
                    n[r] = t[r] * v.normal[0] + t[4 + r] * v.normal[1] + t[8 + r] * v.normal[2];
                }
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (int r = 0; r < 3; ++r) v.position[r] = p[r], v.normal[r] = len > 0 ? n[r] / len : n[r];
            }
        return sc;
    };
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
        const auto sc = scene();
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
        const auto sc = scene();
        printStats(sc.meshes);
        for (const auto& w : sc.warnings) std::printf("warning: %s\n", w.c_str());
    } else if (cmd == "render") {
        if (out.empty()) return usage();
        const auto sc = scene();
        render(sc, view, size, textured, out);
    } else if (cmd == "shaders") {
        using namespace formats::d3d9;
        std::vector<InstallShader> shaders;
        if (path == "all") {
            auto all = collectInstallShaders(pos[1]);
            std::printf("%zu DRMs, %zu ShaderLib sections, %zu entries, %zu distinct shaders, %zu hash mismatches, %zu lib errors\n",
                        all.drms, all.libs, all.entries, all.shaders.size(), all.hashMismatches, all.errors.size());
            for (const auto& e : all.errors) std::printf("  %s\n", e.c_str());
            shaders = std::move(all.shaders);
        } else {
            const auto drm = loadDrm(bf, path);
            std::set<std::pair<bool, uint64_t>> seen;
            for (const auto& s : drm.sections) {
                if (s.type != cdc::SectionType::ShaderLib) continue;
                const auto lib = parseShaderLib(s.data);
                for (const auto& e : lib.entries)
                    if (seen.insert({lib.pixel, e.hash}).second)
                        shaders.push_back({lib.pixel, e.hash, {e.bytecode.begin(), e.bytecode.end()}, s.id, e.index});
            }
        }
        shaderSurvey(shaders);
    } else if (cmd == "shader-dis") {
        using namespace formats::d3d9;
        if (pos.size() < 4) return usage();
        const auto drm = loadDrm(bf, path);
        const uint32_t id = uint32_t(std::stoul(pos[3], nullptr, 0));
        const int want = pos.size() > 4 ? std::stoi(pos[4], nullptr, 0) : -1;
        for (const auto& s : drm.sections) {
            if (s.type != cdc::SectionType::ShaderLib || s.id != id) continue;
            const auto lib = parseShaderLib(s.data);
            std::printf("shader lib %u: %s, %u slots, %zu present\n", id, lib.pixel ? "pixel" : "vertex", lib.slots, lib.entries.size());
            for (const auto& e : lib.entries) {
                if (want >= 0 && int(e.index) != want) continue;
                std::printf("== slot %u hash %016llx (%zu bytes)\n%s", e.index, (unsigned long long)e.hash, e.bytecode.size(),
                            disassemble(parse(e.bytecode)).c_str());
            }
        }
    } else if (cmd == "collision") {
        collisionDump(loadDrm(bf, path), out);
    } else if (cmd == "anim") {
        animDump(loadDrm(bf, path), pos.size() > 3 ? std::optional(uint32_t(std::stoul(pos[3], nullptr, 0))) : std::nullopt);
    } else if (cmd == "materials") {
        materialsDump(loadDrm(bf, path));
    } else if (cmd == "script") {
        scriptDump(loadDrm(bf, path), pos.size() > 3 ? std::optional(uint32_t(std::stoul(pos[3], nullptr, 0))) : std::nullopt);
    } else if (cmd == "cine") {
        cineDump(bf, path, pos.size() > 3 ? std::optional(uint32_t(std::stoul(pos[3], nullptr, 0))) : std::nullopt,
                 pos.size() > 4 ? uint32_t(std::stoul(pos[4], nullptr, 0)) : 1u);
    } else if (cmd == "font") {
        fontDump(loadDrm(bf, path), pos.size() > 3 ? uint32_t(std::stoul(pos[3], nullptr, 0)) : 46u);
    } else if (cmd == "roundtrip") {
        RoundTripCounts n;
        if (path == "all") {
            for (const char* archive : {"bigfile.000", "patch.000"}) {
                if (!fs::exists(fs::path(pos[1]) / archive)) continue;
                const cdc::Bigfile big(fs::path(pos[1]) / archive);
                for (const auto& r : big.records()) {
                    auto bytes = big.read(r);
                    uint32_t version = 0;
                    if (bytes.size() >= 4) std::memcpy(&version, bytes.data(), 4);
                    if (!cdc::isCdrm(bytes) && version != cdc::kDrmVersion) continue;
                    char label[48];
                    std::snprintf(label, sizeof label, "%s:%08x", archive, r.hash);
                    roundTrip(label, std::move(bytes), n);
                }
            }
        } else {
            auto bytes = bf.read(path);
            if (!bytes) throw cdc::FormatError("not in bigfile: " + path);
            roundTrip(path, std::move(*bytes), n);
        }
        for (const auto& m : n.mismatches) std::printf("MISMATCH %s\n", m.c_str());
        std::printf("%zu DRMs (%zu CDRM): DRM serialise identical %zu, CDRM re-wrap identical %zu, %zu PCD9 textures, "
                    "re-wrap identical %zu, %zu mismatches\n",
                    n.drms, n.cdrm, n.drmSame, n.cdrmSame, n.textures, n.pcdSame, n.mismatches.size());
        return n.mismatches.empty() ? 0 : 1;
    } else {
        return usage();
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
}
