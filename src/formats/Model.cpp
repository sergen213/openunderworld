// Render models (RenderResource type 26, "Mesh") and terrain (RenderResource type 24).
// Layouts: docs/formats/MESHES.md (offsets verified against tru.exe PCRenderModel /
// PCRenderTerrain resource constructors and PCStreamDecl creation).
#include "formats/Formats.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

namespace ou::formats {
namespace {

using cdc::FormatError;

struct Reader {
    std::span<const uint8_t> d;
    template <class T> T at(size_t off) const {
        if (off > d.size() || d.size() - off < sizeof(T))
            throw FormatError("read past section end at " + std::to_string(off));
        T v;
        std::memcpy(&v, d.data() + off, sizeof v);
        return v;
    }
    uint32_t u32(size_t o) const { return at<uint32_t>(o); }
    uint16_t u16(size_t o) const { return at<uint16_t>(o); }
    void need(size_t off, size_t n) const {
        if (off > d.size() || d.size() - off < n) throw FormatError("range past section end");
    }
};

// Vertex attribute name hashes (same values as cdc VertexElem constants in DXHR; the
// usage of each is confirmed by the shader input specs stored in TRU material sections).
constexpr uint32_t kPosition = 0xD2F7D823, kNormal = 0x36F5E414, kTerrainNormal = 0x09B1D4EA,
                   kTexcoord1 = 0x8317902A, kTexcoord2 = 0x8E54B6F3, kColor1 = 0x7E7DD623, kTangent = 0xF1ED11C3,
                   kBinormal = 0x64A86F01;
// Skin attributes (D3DCOLOR). Byte k of the weights pairs with byte k of the indices; indices
// address the batch bone palette (docs/formats/ANIMATION.md).
constexpr uint32_t kSkinWeights = 0x48E691C0, kSkinIndices = 0x5156D8D3;

constexpr uint32_t kResourceTerrain = 24, kResourceModel = 26;
constexpr uint32_t kMeshMagic = 0x6873654D; // "Mesh"

struct Elem { uint32_t hash; uint16_t offset; uint16_t format; };
struct Decl { uint16_t stride = 0; std::vector<Elem> elems; size_t bytes = 0; };

// cdc VertexDecl: {u64 hash, u16 numAttr, u8 stride, u8, u32, {u32 hash, u16 offset, u8 format, u8 stream}[]}
// (u8 widths per tru.exe decl builder 0x529020, reported by GhidraRE).
Decl readDecl(const Reader& r, size_t off) {
    Decl d;
    const uint16_t n = r.u16(off + 8);
    d.stride = r.at<uint8_t>(off + 10);
    for (uint16_t i = 0; i < n; ++i) {
        const size_t e = off + 16 + 8 * size_t(i);
        d.elems.push_back({r.u32(e), r.u16(e + 4), r.at<uint8_t>(e + 6)});
    }
    d.bytes = 16 + 8 * size_t(n);
    return d;
}

// Reads one attribute as D3D9 would feed it to the shader. Format codes are cdc's; the
// mapping to D3DDECLTYPE is from tru.exe (switch at 0x5289d0). Returns false if unsupported.
bool readAttr(const uint8_t* p, uint16_t fmt, float out[4]) {
    out[0] = out[1] = out[2] = 0;
    out[3] = 1;
    auto f32 = [&](int n) { std::memcpy(out, p, 4 * size_t(n)); };
    auto s16 = [&](int n, float scale) {
        for (int i = 0; i < n; ++i) { int16_t v; std::memcpy(&v, p + 2 * i, 2); out[i] = v * scale; }
    };
    auto u16n = [&](int n) {
        for (int i = 0; i < n; ++i) { uint16_t v; std::memcpy(&v, p + 2 * i, 2); out[i] = v / 65535.f; }
    };
    switch (fmt) {
    case 0: f32(1); return true;
    case 1: f32(2); return true;
    case 2: f32(3); return true;
    case 3: f32(4); return true;
    case 4: case 5: case 6: case 7: // D3DCOLOR: bytes B,G,R,A -> x=R
        out[0] = p[2] / 255.f; out[1] = p[1] / 255.f; out[2] = p[0] / 255.f; out[3] = p[3] / 255.f;
        return true;
    case 8: for (int i = 0; i < 4; ++i) out[i] = p[i]; return true;          // UBYTE4
    case 9: case 0x13: s16(2, 1.f); return true;                               // SHORT2
    case 10: case 0x14: s16(4, 1.f); return true;                              // SHORT4
    case 0xB: for (int i = 0; i < 4; ++i) out[i] = p[i] / 255.f; return true;  // UBYTE4N
    case 0xC: s16(2, 1.f / 32767.f); return true;                             // SHORT2N
    case 0xD: s16(4, 1.f / 32767.f); return true;                             // SHORT4N
    case 0xE: u16n(2); return true;                                            // USHORT2N
    case 0xF: u16n(4); return true;                                            // USHORT4N
    default: return false;
    }
}

size_t attrSize(uint16_t fmt) {
    switch (fmt) {
    case 1: case 10: case 0xD: case 0xF: case 0x14: return 8;
    case 2: return 12;
    case 3: return 16;
    default: return 4;
    }
}

// Converts `count` vertices at `off` using `decl` into scene vertices.
std::vector<scene::Vertex> readVertices(const Reader& r, size_t off, uint32_t count, const Decl& decl,
                                        std::vector<std::string>& warnings, const std::string& where) {
    r.need(off, size_t(count) * decl.stride);
    for (const auto& e : decl.elems)
        if (size_t(e.offset) + attrSize(e.format) > decl.stride) throw FormatError("vertex element outside stride");
    const Elem *pos = nullptr, *nrm = nullptr, *uv = nullptr, *uv1 = nullptr, *col = nullptr, *tan = nullptr, *bin = nullptr;
    for (const auto& e : decl.elems) {
        if (e.hash == kPosition) pos = &e;
        else if (e.hash == kNormal || e.hash == kTerrainNormal) nrm = &e;
        else if (e.hash == kTexcoord1) uv = &e;
        else if (e.hash == kTexcoord2) uv1 = &e;
        else if (e.hash == kColor1) col = &e;
        else if (e.hash == kTangent) tan = &e;
        else if (e.hash == kBinormal) bin = &e;
    }
    if (!pos) throw FormatError("vertex declaration without position");
    float tmp[4];
    for (const Elem* e : {pos, nrm, uv, uv1, col, tan, bin})
        if (e && !readAttr(r.d.data() + off + e->offset, e->format, tmp)) {
            warnings.push_back(where + ": unsupported vertex format " + std::to_string(e->format));
            if (e == pos) return {};
        }
    std::vector<scene::Vertex> out(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* v = r.d.data() + off + size_t(i) * decl.stride;
        auto& o = out[i];
        readAttr(v + pos->offset, pos->format, tmp);
        std::copy_n(tmp, 3, o.position);
        o.normal[0] = o.normal[1] = 0;
        o.normal[2] = 1;
        if (nrm && readAttr(v + nrm->offset, nrm->format, tmp)) {
            if (nrm->format >= 4 && nrm->format <= 7) // D3DCOLOR normals: shader applies *2-1
                for (int k = 0; k < 3; ++k) tmp[k] = tmp[k] * 2 - 1;
            const float len = std::sqrt(tmp[0] * tmp[0] + tmp[1] * tmp[1] + tmp[2] * tmp[2]);
            if (len > 0) for (int k = 0; k < 3; ++k) o.normal[k] = tmp[k] / len;
        }
        o.uv[0] = o.uv[1] = 0;
        // SHORT2/SHORT4 texcoords are scaled by 1/2048 (constant at 0x84ba10 in tru.exe; the shaders' TexcoordScales c254)
        auto texcoord = [&](const Elem* e, float out2[2]) {
            if (!e || !readAttr(v + e->offset, e->format, tmp)) return;
            const float s = (e->format == 0x13 || e->format == 0x14) ? 1.f / 2048.f : 1.f;
            out2[0] = tmp[0] * s;
            out2[1] = tmp[1] * s;
        };
        texcoord(uv, o.uv);
        texcoord(uv1, o.uv1);
        if (col && readAttr(v + col->offset, col->format, tmp)) // as the vertex shader sees it, repacked R,G,B,A
            for (int k = 0; k < 4; ++k) o.color0 |= uint32_t(std::lround(std::clamp(tmp[k], 0.f, 1.f) * 255.f)) << (8 * k);
        // Tangent frame: same packing as the normal (NormalScaleOffset c21 applies to all three in the shaders).
        auto frame = [&](const Elem* e, float out3[3]) {
            if (!e || !readAttr(v + e->offset, e->format, tmp)) return;
            for (int k = 0; k < 3; ++k) out3[k] = (e->format >= 4 && e->format <= 7) ? tmp[k] * 2 - 1 : tmp[k];
        };
        frame(tan, o.tangent);
        frame(bin, o.binormal);
    }
    return out;
}

// Skin influences of `count` vertices; empty when the declaration has no skin indices. Indices without
// weights (rigid multi-bone objects: doors, switches) bind each vertex fully to the bone of index byte 0 [I].
std::vector<scene::SkinInfluence> readSkin(const Reader& r, size_t off, uint32_t count, const Decl& decl,
                                           const std::vector<uint32_t>& palette) {
    const Elem *w = nullptr, *ix = nullptr;
    for (const auto& e : decl.elems) {
        if (e.hash == kSkinWeights && e.format >= 4 && e.format <= 8) w = &e;
        else if (e.hash == kSkinIndices && e.format >= 4 && e.format <= 8) ix = &e;
    }
    if (!ix) return {};
    std::vector<scene::SkinInfluence> out(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* v = r.d.data() + off + size_t(i) * decl.stride;
        for (int k = 0; k < (w ? 4 : 1); ++k) {
            const uint8_t idx = v[ix->offset + k];
            if (!palette.empty() && idx >= palette.size()) {
                if (!w) return {}; // [I] not a rigid binding after all: leave the batch unskinned
                throw FormatError("skin index outside bone palette");
            }
            out[i].weight[k] = w ? v[w->offset + k] : 255;
            out[i].bone[k] = uint16_t(palette.empty() ? idx : palette[idx]);
        }
    }
    return out;
}

// {u32 count, u32 ids[count]} list addressed by a relocated pointer at `ptrOffset`.
std::vector<uint32_t> idList(const cdc::Section& s, uint32_t ptrOffset) {
    const auto p = cdc::internalPointer(s, ptrOffset);
    if (!p) return {};
    const Reader r{s.data};
    const uint32_t n = r.u32(*p);
    r.need(*p + 4, size_t(n) * 4);
    std::vector<uint32_t> ids(n);
    for (uint32_t i = 0; i < n; ++i) ids[i] = r.u32(*p + 4 + 4 * i);
    return ids;
}

struct Ctx {
    std::unordered_map<uint32_t, const cdc::Section*> materials;
    std::vector<std::string>& warnings;
    std::string origin;

    // Texture + pass state of material `index` of a model/terrain material list.
    scene::SubMesh material(const std::vector<uint32_t>& matIds, uint32_t index) {
        scene::SubMesh sm;
        if (index >= matIds.size()) return sm;
        const auto it = materials.find(matIds[index]);
        if (it == materials.end()) return sm;
        const MaterialState st = materialState(*it->second);
        sm.blend = st.blend, sm.alphaRef = st.alphaRef, sm.doubleSided = st.doubleSided;
        sm.materialId = matIds[index];
        // Category 1 = diffuse (verified visually on Lara's face/teeth textures); materials
        // without one (e.g. hair, all categories 0) use the slot-0 texture (heuristic). Of several
        // category-1 entries the lowest sampler slot wins: the co_natla / co_amanda skin material has its
        // albedo in s0 and a green detail normal map (texture 296) in s5, also category 1 [O: PS disassembly].
        const auto texs = materialTextures(*it->second);
        const MaterialTexture* best = nullptr;
        for (const auto& t : texs)
            if (t.category == 1 && (!best || t.slot < best->slot)) best = &t;
        if (best) { sm.textureId = best->textureId; return sm; }
        for (const auto& t : texs)
            if (t.category == 0 && t.slot == 0) { sm.textureId = t.textureId; return sm; }
        return sm;
    }
    // Four i32 indices into a resource's texture list (-1 = none) -> SubMesh::instanceTextures (texture ids).
    static void instanceTextures(scene::SubMesh& sm, const Reader& r, size_t at, const std::vector<uint32_t>& texIds) {
        for (int k = 0; k < 4; ++k) {
            const auto i = int32_t(r.u32(at + 4 * size_t(k)));
            if (i >= 0 && size_t(i) < texIds.size()) sm.instanceTextures[k] = texIds[size_t(i)];
        }
    }
    bool hasTextures(const std::vector<uint32_t>& matIds, uint32_t index) {
        if (index >= matIds.size()) return false;
        const auto it = materials.find(matIds[index]);
        return it != materials.end() && !materialTextures(*it->second).empty();
    }
};

void appendIndices(scene::MeshData& m, const Reader& r, size_t indexBase, uint32_t first, uint32_t count,
                   uint32_t vertexCount, scene::SubMesh sm) {
    r.need(indexBase + 2 * size_t(first), 2 * size_t(count));
    if (count % 3) throw FormatError("index count not a multiple of 3");
    sm.firstIndex = uint32_t(m.indices.size());
    sm.indexCount = count;
    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t v = r.u16(indexBase + 2 * (size_t(first) + i));
        if (v >= vertexCount) throw FormatError("index out of vertex range");
        m.indices.push_back(v);
    }
    auto& b = m.submeshes;
    if (!b.empty() && b.back().textureId == sm.textureId && b.back().blend == sm.blend &&
        b.back().alphaRef == sm.alphaRef && b.back().doubleSided == sm.doubleSided && b.back().materialId == sm.materialId &&
        b.back().drawGroup == sm.drawGroup &&
        std::equal(sm.instanceTextures, sm.instanceTextures + 4, b.back().instanceTextures) &&
        b.back().firstIndex + b.back().indexCount == sm.firstIndex)
        b.back().indexCount += count;
    else
        b.push_back(sm);
}

void decodeModel(Ctx& c, const cdc::Section& s, std::vector<scene::MeshData>& out) {
    const Reader r{s.data};
    // Resource header: {Mesh*, textures*, materials*, ?} (relocated pointers).
    const auto mesh = cdc::internalPointer(s, 0);
    if (!mesh || r.u32(*mesh) != kMeshMagic) throw FormatError("no Mesh header");
    const size_t M = *mesh;
    const auto matIds = idList(s, 8), texIds = idList(s, 4);
    const uint32_t numIndices = r.u32(M + 0x0C);
    const uint32_t pgOff = r.u32(M + 0x6C), batchOff = r.u32(M + 0x70), idxOff = r.u32(M + 0x78);
    const uint16_t numPg = r.u16(M + 0x7C), numBatches = r.u16(M + 0x7E);
    if (numIndices == 0 || numBatches == 0) return; // empty placeholder models (offsets are -1)
    const size_t indexBase = M + idxOff;
    r.need(indexBase, 2 * size_t(numIndices));
    uint32_t pg = 0;
    for (uint16_t b = 0; b < numBatches; ++b) {
        const size_t B = M + batchOff + 0x34 * size_t(b);
        const uint32_t batchPgs = r.u32(B + 0x08), vtxOff = r.u32(B + 0x14), declOff = r.u32(B + 0x24),
                       numVerts = r.u32(B + 0x28);
        const std::string where = c.origin + "#section " + std::to_string(s.id) + " batch " + std::to_string(b);
        if (pg + batchPgs > numPg) throw FormatError("primgroup count mismatch");
        const Decl decl = readDecl(r, M + declOff);
        // Batches without texcoords whose materials reference no texture are not drawn as
        // surfaces: in units they are level-enclosing boxes with depth/shadow/fog-only shaders
        // (material 0xd78; hypothesis: occlusion or fog volumes). Skipped, see MESHES.md.
        bool textured = std::any_of(decl.elems.begin(), decl.elems.end(), [](const Elem& e) { return e.hash == kTexcoord1; });
        for (uint32_t i = 0; i < batchPgs && !textured; ++i)
            textured = c.hasTextures(matIds, r.u32(M + pgOff + 0x40 * size_t(pg + i) + 0x28));
        if (!textured) {
            c.warnings.push_back(where + ": skipped (no texcoords, no material textures)");
            pg += batchPgs;
            continue;
        }
        scene::MeshData m;
        m.origin = where;
        m.vertices = readVertices(r, M + vtxOff, numVerts, decl, c.warnings, where);
        if (m.vertices.empty()) { pg += batchPgs; continue; }
        // Mesh flags bit 0: batch +0x0C/+0x10 = bone palette {count, offset of u32 bone indices}.
        std::vector<uint32_t> palette;
        if ((r.u32(M + 0x04) & 1) &&
            std::any_of(decl.elems.begin(), decl.elems.end(), [](const Elem& e) { return e.hash == kSkinIndices; })) {
            const uint32_t n = r.u32(B + 0x0C), po = r.u32(B + 0x10);
            r.need(M + po, 4 * size_t(n));
            for (uint32_t i = 0; i < n; ++i) palette.push_back(r.u32(M + po + 4 * size_t(i)));
        }
        m.skin = readSkin(r, M + vtxOff, numVerts, decl, palette);
        for (uint32_t i = 0; i < batchPgs; ++i, ++pg) {
            const size_t P = M + pgOff + 0x40 * size_t(pg);
            const uint32_t first = r.u32(P + 0x10), tris = r.u32(P + 0x14);
            if (first + 3ull * tris > numIndices) throw FormatError("primgroup outside index buffer");
            // Primgroup +0x2C..+0x38: default instance textures (PCRenderModel_Finalize 0x50EB30).
            scene::SubMesh sm = c.material(matIds, r.u32(P + 0x28));
            Ctx::instanceTextures(sm, r, P + 0x2C, texIds);
            sm.drawGroup = uint8_t(r.u32(P + 0x20)); // hide/unhide group (DXHR PrimGroup dword20)
            appendIndices(m, r, indexBase, first, 3 * tris, numVerts, sm);
        }
        out.push_back(std::move(m));
    }
}

void decodeTerrain(Ctx& c, const cdc::Section& s, std::vector<scene::MeshData>& out) {
    const Reader r{s.data};
    // Resource header: {?, Header*, textures*, materials*}.
    const auto hdr = cdc::internalPointer(s, 4);
    if (!hdr) throw FormatError("no terrain header pointer");
    const size_t H = *hdr;
    const auto matIds = idList(s, 12), texIds = idList(s, 8);
    const uint32_t nodeCount = r.u32(H + 0x24), groupOff = r.u32(H + 0x28), groupCount = r.u32(H + 0x2C),
                   declOff = r.u32(H + 0x30), declCount = r.u32(H + 0x34), vbOff = r.u32(H + 0x3C),
                   vbCount = r.u32(H + 0x40), geomOff = r.u32(H + 0x44), idxOff = r.u32(H + 0x50),
                   numIndices = r.u32(H + 0x54);
    std::vector<Decl> decls;
    for (size_t i = 0, p = H + declOff; i < declCount; ++i) {
        decls.push_back(readDecl(r, p));
        p += decls.back().bytes;
    }
    struct Group { uint32_t vb; scene::SubMesh material; };
    std::vector<Group> groups;
    for (uint32_t g = 0; g < groupCount; ++g) {
        const size_t G = H + groupOff + 0x20 * size_t(g);
        groups.push_back({r.u32(G + 4), c.material(matIds, r.u32(G))});
        // Group +0x10..+0x1C: instance textures (light map class 1, blend masks class 3; terrain construct 0x510780).
        Ctx::instanceTextures(groups.back().material, r, G + 0x10, texIds);
        if (groups.back().vb >= vbCount) throw FormatError("terrain group vertex buffer out of range");
    }
    // Collect chunks per (vertex buffer, group).
    std::map<std::pair<uint32_t, uint32_t>, std::vector<std::pair<uint32_t, uint32_t>>> chunks;
    for (uint32_t n = 0; n < nodeCount; ++n) {
        const uint32_t arr = r.u32(H + geomOff + 4 * size_t(n));
        if (!arr) continue;
        const uint16_t count = r.u16(H + arr);
        for (uint16_t k = 0; k < count; ++k) {
            const size_t C = H + arr + 4 + 8 * size_t(k);
            const uint16_t group = r.u16(C), num = r.u16(C + 2);
            const uint32_t first = r.u32(C + 4);
            if (group >= groupCount || first + uint64_t(num) > numIndices) throw FormatError("terrain chunk out of range");
            chunks[{groups[group].vb, group}].push_back({first, num});
        }
    }
    for (uint32_t v = 0; v < vbCount; ++v) {
        const size_t V = H + vbOff + 16 * size_t(v);
        const uint32_t dataOff = r.u32(V), numVerts = r.u32(V + 8), decl = r.u32(V + 12);
        if (decl >= decls.size()) throw FormatError("terrain vertex declaration out of range");
        const std::string where = c.origin + "#section " + std::to_string(s.id) + " terrain vb " + std::to_string(v);
        scene::MeshData m;
        m.origin = where;
        m.vertices = readVertices(r, H + dataOff, numVerts, decls[decl], c.warnings, where);
        if (m.vertices.empty()) continue;
        for (const auto& [key, list] : chunks)
            if (key.first == v)
                for (const auto& [first, num] : list)
                    appendIndices(m, r, H + idxOff, first, num, numVerts, groups[key.second].material);
        if (!m.indices.empty()) out.push_back(std::move(m));
    }
}

} // namespace

std::vector<MaterialTexture> materialTextures(const cdc::Section& material) {
    // Texture references are the type-5 ("Texture") resource relocations of the material;
    // each points at a 16-byte entry {u32 textureId, f32 lodBias?, u32 category, u8, u8 slot, u16}.
    std::vector<MaterialTexture> out;
    const Reader r{material.relocations};
    if (r.d.size() < 20) return out;
    uint32_t counts[5];
    for (int i = 0; i < 5; ++i) counts[i] = r.u32(4 * size_t(i));
    // Tables 3 and 5 share the encoding {offset/4 : 25, section type : 7}; observed texture
    // references are all in table 5 (resource pointer relocations), table 3 is scanned too.
    const size_t t3 = 20 + 8 * size_t(counts[0]) + 8 * size_t(counts[1]);
    const size_t t5 = t3 + 4 * size_t(counts[2]) + 4 * size_t(counts[3]);
    const Reader m{material.data};
    for (const auto [p0, n] : {std::pair{t3, counts[2]}, std::pair{t5, counts[4]}})
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t v = r.u32(p0 + 4 * size_t(k));
            if ((v >> 25) != uint32_t(cdc::SectionType::Texture)) continue;
            const size_t off = size_t(v & 0x1FFFFFF) * 4;
            out.push_back({m.u32(off), m.u32(off + 8), m.at<uint8_t>(off + 13)});
        }
    return out;
}

MaterialState materialState(const cdc::Section& material) {
    MaterialState st;
    const Reader r{material.data};
    if (r.d.size() < 0x50) return st;
    // Blend modes as PCStateManager_SetBlendMode (0x527A80): 0 opaque, 1 opaque + alpha test at
    // the material threshold (shader texkill against AlphaThreshold c2), 2-4 SRCALPHA/INVSRCALPHA,
    // 5 SRCALPHA/ONE, 8 BLENDFACTOR 0x80808080 both sides; 2-5 also alpha-test at ref 1.
    // 6 (reverse subtract), 7 (DESTCOLOR/SRCALPHA) and 9 (ONE/INVSRCALPHA) were not seen in the
    // 2248-material survey; they fall back to Alpha.
    switch (const uint32_t mode = r.u32(0x38)) {
    case 0: break;
    case 1: st.alphaRef = r.at<uint8_t>(0x4C); break;
    case 5: st.blend = scene::Blend::Additive, st.alphaRef = 1; break;
    case 8: st.blend = scene::Blend::Average; break;
    default: st.blend = scene::Blend::Alpha, st.alphaRef = mode <= 4 ? 1 : 0; break;
    }
    // Flags 0x80: cull none (PCMaterial cull selection 0x506040); 0x800 "render twice" (back then
    // front) is approximated as two-sided too.
    st.doubleSided = (r.u32(0x40) & 0x880) != 0;
    return st;
}

scene::Material decodeMaterial(const cdc::Section& material, MaterialShaderLibs* libs) {
    scene::Material m;
    m.id = material.id;
    const Reader r{material.data};
    if (r.d.size() < 0x74) return m;
    m.blendMode = uint8_t(r.u32(0x38));
    m.alphaThreshold = r.at<uint8_t>(0x4C);
    m.flags = r.u32(0x40);
    if (libs) *libs = {r.u32(0x34), r.u32(0x30)}; // resource ids (type-9 relocations), patched to PCShaderLib* at load
    // Constants: PS {u32 count +0x58, ptr +0x5C} -> c100, VS {u16 count +0x68, ptr +0x6C} -> c25 (0x505A90, 0x505C00).
    auto consts = [&](uint32_t count, uint32_t ptrOff, std::vector<float>& out) {
        const auto p = cdc::internalPointer(material, ptrOff);
        if (!count || !p) return;
        r.need(*p, 16 * size_t(count));
        out.resize(4 * size_t(count));
        std::memcpy(out.data(), r.d.data() + *p, 16 * size_t(count));
    };
    consts(r.u32(0x58), 0x5C, m.psParams);
    consts(r.u16(0x68), 0x6C, m.vsParams);
    // Texture entries {u32 id, f32 lodBias, u32 category, u8 class, u8 sampler, u16 filter} at *(+0x54): [0, n51)
    // material, [n51, n52) engine-global table by class, [n52, n52 + n50) per-instance by class (0x505A90).
    const uint8_t nInst = r.at<uint8_t>(0x50), nMat = r.at<uint8_t>(0x51), nGlobal = r.at<uint8_t>(0x52);
    if (const auto p = cdc::internalPointer(material, 0x54); p && nGlobal >= nMat) {
        const size_t n = size_t(nGlobal) + nInst;
        r.need(*p, 16 * n);
        for (size_t i = 0; i < n; ++i) {
            const size_t e = *p + 16 * i;
            scene::MaterialSampler t;
            t.textureId = r.u32(e);
            t.cls = r.at<uint8_t>(e + 12);
            t.category = r.u32(e + 8);
            t.reg = r.at<uint8_t>(e + 13);
            t.filter = r.u16(e + 14);
            t.source = i < nMat ? 0 : i < nGlobal ? 1 : 2;
            m.samplers.push_back(t);
        }
    }
    return m;
}

std::map<uint32_t, std::vector<scene::MeshData>> decodeRenderResources(const cdc::Drm& drm, std::vector<std::string>& warnings,
                                                                       std::string_view origin) {
    Ctx c{{}, warnings, std::string(origin)};
    for (const auto& s : drm.sections)
        if (s.type == cdc::SectionType::Material) c.materials[s.id] = &s;
    std::map<uint32_t, std::vector<scene::MeshData>> out;
    for (const auto& s : drm.sections) {
        if (s.type != cdc::SectionType::RenderResource) continue;
        auto& meshes = out[s.id];
        try {
            if (s.resourceType() == kResourceModel) decodeModel(c, s, meshes);
            else if (s.resourceType() == kResourceTerrain) decodeTerrain(c, s, meshes);
            else warnings.push_back(c.origin + "#section " + std::to_string(s.id) + ": unsupported render resource type " +
                                    std::to_string(s.resourceType()));
        } catch (const FormatError& e) {
            meshes.clear();
            warnings.push_back(c.origin + "#section " + std::to_string(s.id) + ": " + e.what());
        }
    }
    return out;
}

std::vector<scene::MeshData> decodeModels(const cdc::Drm& drm, std::vector<std::string>& warnings,
                                          std::string_view origin) {
    std::vector<scene::MeshData> out;
    for (auto& [id, meshes] : decodeRenderResources(drm, warnings, origin))
        for (auto& m : meshes) out.push_back(std::move(m));
    return out;
}

} // namespace ou::formats
