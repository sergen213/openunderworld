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

} // namespace ou::cdc
