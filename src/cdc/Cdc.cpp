#include "cdc/Cdc.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

namespace ou::cdc {
namespace {

template <class T>
T rd(std::span<const uint8_t> d, size_t off) {
    if (off > d.size() || d.size() - off < sizeof(T))
        throw FormatError("truncated read at offset " + std::to_string(off));
    T v;
    std::memcpy(&v, d.data() + off, sizeof(T)); // little-endian host (x86-64/arm64 Linux)
    return v;
}

constexpr std::array<uint32_t, 256> makeCrcTable() {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i << 24;
        for (int k = 0; k < 8; ++k) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
        t[i] = c;
    }
    return t;
}
constexpr auto kCrc = makeCrcTable();

std::vector<std::string> splitNames(std::span<const uint8_t> block) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i < block.size(); ++i) {
        if (block[i] == 0) {
            if (i > start) out.emplace_back(reinterpret_cast<const char*>(block.data() + start), i - start);
            start = i + 1;
        }
    }
    return out;
}

} // namespace

uint32_t pathHash(std::string_view path) {
    uint32_t c = 0xFFFFFFFFu;
    for (char ch : path) {
        auto b = static_cast<uint8_t>(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
        c = (c << 8) ^ kCrc[(c >> 24) ^ b];
    }
    return ~c;
}

// ---- Bigfile ---------------------------------------------------------------------------
Bigfile::Bigfile(std::filesystem::path firstPart, uint64_t partAlignment)
    : first_(std::move(firstPart)), alignment_(partAlignment) {
    std::ifstream f(first_, std::ios::binary);
    if (!f) throw FormatError("cannot open " + first_.string());
    uint32_t n = 0;
    if (!f.read(reinterpret_cast<char*>(&n), 4)) throw FormatError("bigfile header truncated");
    if (n == 0 || n > 1'000'000) throw FormatError("implausible bigfile record count " + std::to_string(n));
    std::vector<uint32_t> raw(size_t(n) * 5);
    if (!f.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size() * 4)))
        throw FormatError("bigfile index truncated");
    records_.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t* r = &raw[n + size_t(i) * 4];
        records_.push_back({raw[i], r[0], r[1], r[2], r[3]});
    }
    if (!std::ranges::is_sorted(records_, {}, &BigfileRecord::hash))
        throw FormatError("bigfile hash table not sorted");
}

std::span<const BigfileRecord> Bigfile::find(uint32_t hash) const {
    auto [lo, hi] = std::ranges::equal_range(records_, hash, {}, &BigfileRecord::hash);
    return {lo, hi};
}

std::optional<BigfileRecord> Bigfile::find(std::string_view path, uint32_t specMask) const {
    for (const auto& r : find(pathHash(path)))
        if ((r.specMask & specMask) == specMask) return r;
    return std::nullopt;
}

BigfileSet::BigfileSet(const std::filesystem::path& gameDir, uint32_t specMask) : specMask_(specMask) {
    if (std::filesystem::exists(gameDir / "patch.000")) archives_.emplace_back(gameDir / "patch.000");
    archives_.emplace_back(gameDir / "bigfile.000");
}

std::optional<std::vector<uint8_t>> BigfileSet::read(std::string_view path) const {
    for (const auto& a : archives_)
        if (const auto r = a.find(path, specMask_)) return a.read(*r);
    return std::nullopt;
}

std::filesystem::path Bigfile::partPath(uint32_t part) const {
    auto p = first_;
    char ext[8];
    std::snprintf(ext, sizeof ext, ".%03u", part % 1000);
    return p.replace_extension(ext);
}

std::vector<uint8_t> Bigfile::read(const BigfileRecord& r) const {
    if (r.compressedSize != 0) throw FormatError("bigfile-level compression is not supported (never observed in TRU PC)");
    const uint64_t pos = uint64_t(r.sector) << 11;
    const auto part = uint32_t(pos / alignment_);
    const uint64_t off = pos % alignment_;
    std::ifstream f(partPath(part), std::ios::binary);
    if (!f) throw FormatError("cannot open " + partPath(part).string());
    std::vector<uint8_t> out(r.size);
    f.seekg(std::streamoff(off));
    if (!f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())))
        throw FormatError("record beyond end of " + partPath(part).string());
    return out;
}

// ---- CDRM ------------------------------------------------------------------------------
bool isCdrm(std::span<const uint8_t> d) { return d.size() >= 16 && std::memcmp(d.data(), "CDRM", 4) == 0; }

std::vector<uint8_t> decompressCdrm(std::span<const uint8_t> d) {
    if (!isCdrm(d)) throw FormatError("not a CDRM container");
    const auto numBlocks = rd<uint32_t>(d, 8);
    if (numBlocks > (d.size() - 16) / 8) throw FormatError("CDRM block table truncated");
    size_t total = 0;
    for (uint32_t i = 0; i < numBlocks; ++i) total += rd<uint32_t>(d, 16 + 8 * i) >> 8;
    std::vector<uint8_t> out(total);
    size_t pos = (16 + 8 * size_t(numBlocks) + 15) & ~size_t(15), dst = 0;
    for (uint32_t i = 0; i < numBlocks; ++i) {
        const auto a = rd<uint32_t>(d, 16 + 8 * i);
        const auto csize = rd<uint32_t>(d, 20 + 8 * i);
        const uint32_t type = a & 0xFF, usize = a >> 8;
        if (pos > d.size() || d.size() - pos < csize) throw FormatError("CDRM block " + std::to_string(i) + " truncated");
        if (type == 1) {
            if (csize < usize) throw FormatError("CDRM stored block smaller than declared");
            std::memcpy(out.data() + dst, d.data() + pos, usize);
        } else if (type == 2) {
            uLongf len = usize;
            if (uncompress(out.data() + dst, &len, d.data() + pos, csize) != Z_OK || len != usize)
                throw FormatError("CDRM block " + std::to_string(i) + " zlib error");
        } else if (type != 0 || usize != 0) {
            throw FormatError("CDRM block type " + std::to_string(type) + " unsupported");
        }
        dst += usize;
        pos = (pos + csize + 15) & ~size_t(15);
    }
    return out;
}

// ---- DRM -------------------------------------------------------------------------------
std::string_view sectionTypeName(SectionType t) {
    static constexpr std::string_view names[] = {"general", "empty", "animation", "pushbuffer_wc", "pushbuffer",
        "texture", "wave", "dtpdata", "script", "shaderlib", "material", "object", "renderresource", "pdamap"};
    auto i = size_t(t);
    return i < std::size(names) ? names[i] : "unknown";
}

Drm parseDrm(std::vector<uint8_t> bytes) {
    Drm drm;
    drm.bytes = isCdrm(bytes) ? decompressCdrm(bytes) : std::move(bytes);
    std::span<const uint8_t> b = drm.bytes;
    drm.version = rd<uint32_t>(b, 0);
    if (drm.version != kDrmVersion) throw FormatError("DRM version " + std::to_string(drm.version) + " (expected 19)");
    const auto objDeps = rd<uint32_t>(b, 4), unitDeps = rd<uint32_t>(b, 8);
    const auto n = rd<uint32_t>(b, 20);
    if (n > (b.size() - 24) / 20) throw FormatError("DRM section table truncated");
    size_t pos = 24 + size_t(n) * 20;
    if (b.size() - pos < size_t(objDeps) + unitDeps) throw FormatError("DRM dependency table truncated");
    drm.dependencies = splitNames(b.subspan(pos, objDeps));
    for (auto& s : splitNames(b.subspan(pos + objDeps, unitDeps))) drm.dependencies.push_back(std::move(s));
    pos += size_t(objDeps) + unitDeps;
    drm.sections.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        const size_t h = 24 + size_t(i) * 20;
        Section s{};
        const auto size = rd<uint32_t>(b, h);
        s.type = SectionType(rd<uint8_t>(b, h + 4));
        s.flags = rd<uint8_t>(b, h + 5);
        s.versionId = rd<uint16_t>(b, h + 6);
        s.packed = rd<uint32_t>(b, h + 8);
        s.id = rd<uint32_t>(b, h + 12);
        s.specMask = rd<uint32_t>(b, h + 16);
        const size_t rel = s.relocBytes();
        if (b.size() - pos < rel || b.size() - pos - rel < size)
            throw FormatError("DRM section " + std::to_string(i) + " exceeds file");
        s.relocations = b.subspan(pos, rel);
        s.data = b.subspan(pos + rel, size);
        pos += rel + size;
        drm.sections.push_back(s);
    }
    return drm;
}

std::optional<uint32_t> internalPointer(const Section& s, uint32_t offset) {
    if (s.relocations.size() < 20) return std::nullopt;
    const auto count = rd<uint32_t>(s.relocations, 0);
    for (uint32_t i = 0; i < count; ++i) {
        if (rd<uint32_t>(s.relocations, 20 + 8 * size_t(i)) == offset)
            return rd<uint32_t>(s.relocations, 24 + 8 * size_t(i));
    }
    return std::nullopt;
}

int resourceRefType(const Section& s, uint32_t offset) {
    if (s.relocations.size() < 20) return -1;
    uint32_t c[5];
    for (int i = 0; i < 5; ++i) c[i] = rd<uint32_t>(s.relocations, 4 * size_t(i));
    const size_t t5 = 20 + 8 * size_t(c[0]) + 8 * size_t(c[1]) + 4 * size_t(c[2]) + 4 * size_t(c[3]);
    for (uint32_t k = 0; k < c[4]; ++k) {
        const uint32_t v = rd<uint32_t>(s.relocations, t5 + 4 * size_t(k));
        if ((v & 0x1FFFFFF) * 4 == offset) return int(v >> 25);
    }
    return -1;
}

std::unordered_map<uint32_t, Pointer> sectionPointers(const Drm& drm, uint32_t index) {
    std::unordered_map<uint32_t, Pointer> out;
    const auto r = drm.sections.at(index).relocations;
    if (r.size() < 20) return out;
    const auto internal = rd<uint32_t>(r, 0), external = rd<uint32_t>(r, 4);
    size_t p = 20;
    for (uint32_t i = 0; i < internal; ++i, p += 8) out[rd<uint32_t>(r, p)] = {index, rd<uint32_t>(r, p + 4)};
    // External: {lo: section:14 | patch/4 low 18}, {hi: patch/4 high 6 | target:26} (0x54EC60).
    for (uint32_t i = 0; i < external; ++i, p += 8) {
        const auto lo = rd<uint32_t>(r, p), hi = rd<uint32_t>(r, p + 4);
        const uint32_t section = lo & 0x3FFF;
        if (section >= drm.sections.size()) throw FormatError("external relocation to section " + std::to_string(section));
        out[((lo >> 14) | (hi & 0x3F) << 18) * 4] = {section, hi >> 6};
    }
    return out;
}

// ---- Writers ---------------------------------------------------------------------------
namespace {
void put32(std::vector<uint8_t>& v, uint32_t x) {
    const size_t n = v.size();
    v.resize(n + 4);
    std::memcpy(v.data() + n, &x, 4); // little-endian host, as rd()
}
void padTo(std::vector<uint8_t>& v, size_t align) { v.resize((v.size() + align - 1) / align * align, 0); }
} // namespace

std::vector<uint8_t> encodeBigfileIndex(std::span<const BigfileRecord> records) {
    std::vector<uint8_t> v;
    v.reserve(4 + 20 * records.size() + 2048);
    put32(v, uint32_t(records.size()));
    for (const auto& r : records) put32(v, r.hash);
    for (const auto& r : records) {
        put32(v, r.size); put32(v, r.sector); put32(v, r.specMask); put32(v, r.compressedSize);
    }
    padTo(v, 2048);
    return v;
}

std::vector<uint32_t> layoutBigfileSectors(std::span<const uint32_t> sizes, uint32_t firstSector, uint64_t partAlignment) {
    std::vector<uint32_t> out;
    out.reserve(sizes.size());
    uint64_t pos = uint64_t(firstSector) << 11;
    for (const uint32_t size : sizes) {
        if (pos % partAlignment + size > partAlignment) pos = (pos / partAlignment + 1) * partAlignment;
        if (pos % 2048) throw FormatError("bigfile part alignment is not a whole number of sectors");
        out.push_back(uint32_t(pos >> 11));
        pos = (pos + size + 2047) & ~uint64_t(2047);
    }
    return out;
}

std::vector<CdrmBlock> readCdrmBlocks(std::span<const uint8_t> d) {
    if (!isCdrm(d)) throw FormatError("not a CDRM container");
    if (rd<uint32_t>(d, 4) != 0 || rd<uint32_t>(d, 12) != 0)
        throw FormatError("CDRM version/padding word not 0 (writeCdrm could not reproduce it)");
    const auto numBlocks = rd<uint32_t>(d, 8);
    if (numBlocks > (d.size() - 16) / 8) throw FormatError("CDRM block table truncated");
    std::vector<CdrmBlock> out(numBlocks);
    size_t pos = (16 + 8 * size_t(numBlocks) + 15) & ~size_t(15);
    for (uint32_t i = 0; i < numBlocks; ++i) {
        const auto a = rd<uint32_t>(d, 16 + 8 * i);
        const auto csize = rd<uint32_t>(d, 20 + 8 * i);
        if (pos > d.size() || d.size() - pos < csize) throw FormatError("CDRM block " + std::to_string(i) + " truncated");
        out[i].type = uint8_t(a & 0xFF);
        out[i].uncompressedSize = a >> 8;
        out[i].payload.assign(d.begin() + pos, d.begin() + pos + csize);
        pos = (pos + csize + 15) & ~size_t(15);
    }
    return out;
}

std::vector<uint8_t> writeCdrm(std::span<const CdrmBlock> blocks) {
    std::vector<uint8_t> v{'C', 'D', 'R', 'M'};
    put32(v, 0); put32(v, uint32_t(blocks.size())); put32(v, 0);
    for (const auto& b : blocks) {
        if (b.uncompressedSize > 0xFFFFFF) throw FormatError("CDRM block larger than 24 bits");
        put32(v, b.type | b.uncompressedSize << 8);
        put32(v, uint32_t(b.payload.size()));
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
        padTo(v, 16);
        v.insert(v.end(), blocks[i].payload.begin(), blocks[i].payload.end());
    }
    if (blocks.empty()) padTo(v, 16);
    return v;
}

std::vector<uint8_t> deflateLevel9(std::span<const uint8_t> raw) {
    uLongf len = compressBound(uLong(raw.size()));
    std::vector<uint8_t> out(len);
    if (compress2(out.data(), &len, raw.data(), uLong(raw.size()), kCdrmZlibLevel) != Z_OK)
        throw FormatError("zlib compress2 failed");
    out.resize(len);
    return out;
}

std::vector<uint8_t> compressCdrm(std::span<const uint8_t> raw, Deflate deflate) {
    std::vector<CdrmBlock> blocks;
    for (size_t off = 0; off < raw.size(); off += kCdrmBlockBytes) {
        const auto chunk = raw.subspan(off, std::min(kCdrmBlockBytes, raw.size() - off));
        blocks.push_back({2, uint32_t(chunk.size()), deflate(chunk)});
    }
    return writeCdrm(blocks);
}

Relocations decodeRelocations(std::span<const uint8_t> t) {
    if (t.size() < 20) throw FormatError("relocation table shorter than its 20-byte header");
    uint32_t c[5];
    for (int i = 0; i < 5; ++i) c[i] = rd<uint32_t>(t, 4 * size_t(i));
    const uint64_t want = 20 + 8 * uint64_t(c[0]) + 8 * uint64_t(c[1]) + 4 * (uint64_t(c[2]) + c[3] + c[4]);
    if (want != t.size())
        throw FormatError("relocation table is " + std::to_string(t.size()) + " bytes, counts imply " + std::to_string(want));
    Relocations r;
    size_t p = 20;
    r.internal.reserve(c[0]);
    for (uint32_t i = 0; i < c[0]; ++i, p += 8) r.internal.push_back({rd<uint32_t>(t, p), rd<uint32_t>(t, p + 4)});
    r.external.reserve(c[1]);
    for (uint32_t i = 0; i < c[1]; ++i, p += 8) {
        const auto lo = rd<uint32_t>(t, p), hi = rd<uint32_t>(t, p + 4);
        r.external.push_back({lo & 0x3FFF, ((lo >> 14) | (hi & 0x3F) << 18) * 4, hi >> 6});
    }
    for (auto* list : {&r.kind2, &r.resource32, &r.resource16}) {
        const uint32_t n = list == &r.kind2 ? c[2] : list == &r.resource32 ? c[3] : c[4];
        list->reserve(n);
        for (uint32_t i = 0; i < n; ++i, p += 4) {
            const auto v = rd<uint32_t>(t, p);
            list->push_back({(v & 0x1FFFFFF) * 4, uint8_t(v >> 25)});
        }
    }
    return r;
}

std::vector<uint8_t> encodeRelocations(const Relocations& r) {
    std::vector<uint8_t> v;
    v.reserve(20 + 8 * (r.internal.size() + r.external.size()) + 4 * (r.kind2.size() + r.resource32.size() + r.resource16.size()));
    for (const size_t n : {r.internal.size(), r.external.size(), r.kind2.size(), r.resource32.size(), r.resource16.size()})
        put32(v, uint32_t(n));
    for (const auto& e : r.internal) { put32(v, e.offset); put32(v, e.target); }
    for (const auto& e : r.external) {
        if (e.section > 0x3FFF || e.offset % 4 || e.offset >= (1u << 26) || e.target >= (1u << 26))
            throw FormatError("external relocation field out of range");
        const uint32_t words = e.offset / 4;
        put32(v, e.section | (words & 0x3FFFF) << 14);
        put32(v, (words >> 18) | e.target << 6);
    }
    for (const auto* list : {&r.kind2, &r.resource32, &r.resource16})
        for (const auto& e : *list) {
            if (e.offset % 4 || e.offset >= (1u << 27) || e.sectionType > 0x7F)
                throw FormatError("typed relocation field out of range");
            put32(v, e.offset / 4 | uint32_t(e.sectionType) << 25);
        }
    return v;
}

DrmImage toDrmImage(const Drm& drm) {
    std::span<const uint8_t> b = drm.bytes;
    DrmImage img;
    img.version = drm.version;
    const auto block1 = rd<uint32_t>(b, 4), block2 = rd<uint32_t>(b, 8);
    img.header3 = rd<uint32_t>(b, 12);
    img.header4 = rd<uint32_t>(b, 16);
    const size_t names = 24 + 20 * drm.sections.size();
    const auto nameBlock = b.subspan(names, block1);
    for (size_t start = 0; start < nameBlock.size();) {
        const auto* nul = static_cast<const uint8_t*>(std::memchr(nameBlock.data() + start, 0, nameBlock.size() - start));
        if (!nul || nul == nameBlock.data() + start)
            throw FormatError("DRM name block is not a list of NUL-terminated names");
        const size_t end = size_t(nul - nameBlock.data());
        img.names.emplace_back(reinterpret_cast<const char*>(nameBlock.data() + start), end - start);
        start = end + 1;
    }
    const auto raw2 = b.subspan(names + block1, block2);
    img.block2.assign(raw2.begin(), raw2.end());
    img.sections.reserve(drm.sections.size());
    for (const auto& s : drm.sections) {
        DrmImage::Sec o;
        o.type = s.type;
        o.flags = s.flags;
        o.versionId = s.versionId;
        o.packedLow = uint8_t(s.packed & 0xFF);
        o.id = s.id;
        o.specMask = s.specMask;
        if (!s.relocations.empty()) o.relocations = decodeRelocations(s.relocations);
        o.data.assign(s.data.begin(), s.data.end());
        img.sections.push_back(std::move(o));
    }
    return img;
}

std::vector<uint8_t> serializeDrm(const DrmImage& img) {
    std::vector<uint8_t> nameBlock;
    for (const auto& n : img.names) {
        if (n.empty() || n.find('\0') != std::string::npos) throw FormatError("DRM name empty or containing NUL");
        nameBlock.insert(nameBlock.end(), n.begin(), n.end());
        nameBlock.push_back(0);
    }
    std::vector<std::vector<uint8_t>> tables(img.sections.size());
    size_t total = 24 + 20 * img.sections.size() + nameBlock.size() + img.block2.size();
    for (size_t i = 0; i < img.sections.size(); ++i) {
        if (img.sections[i].relocations) tables[i] = encodeRelocations(*img.sections[i].relocations);
        if (tables[i].size() > 0xFFFFFF) throw FormatError("relocation table larger than 24 bits");
        total += tables[i].size() + img.sections[i].data.size();
    }
    std::vector<uint8_t> v;
    v.reserve(total);
    put32(v, img.version); put32(v, uint32_t(nameBlock.size())); put32(v, uint32_t(img.block2.size()));
    put32(v, img.header3); put32(v, img.header4); put32(v, uint32_t(img.sections.size()));
    for (size_t i = 0; i < img.sections.size(); ++i) {
        const auto& s = img.sections[i];
        put32(v, uint32_t(s.data.size()));
        v.push_back(uint8_t(s.type));
        v.push_back(s.flags);
        v.push_back(uint8_t(s.versionId));
        v.push_back(uint8_t(s.versionId >> 8));
        put32(v, s.packedLow | uint32_t(tables[i].size()) << 8);
        put32(v, s.id);
        put32(v, s.specMask);
    }
    v.insert(v.end(), nameBlock.begin(), nameBlock.end());
    v.insert(v.end(), img.block2.begin(), img.block2.end());
    for (size_t i = 0; i < img.sections.size(); ++i) {
        v.insert(v.end(), tables[i].begin(), tables[i].end());
        v.insert(v.end(), img.sections[i].data.begin(), img.sections[i].data.end());
    }
    return v;
}

} // namespace ou::cdc
