#include "cdc/Cdc.hpp"

#include <gtest/gtest.h>
#include <zlib.h>

#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace ou::cdc;

namespace {
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void pad16(std::vector<uint8_t>& v) { while (v.size() % 16) v.push_back(0); }

// One zlib block + one stored block.
std::vector<uint8_t> makeCdrm(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    std::vector<uint8_t> z(compressBound(uLong(a.size())));
    uLongf zl = z.size();
    compress(z.data(), &zl, a.data(), uLong(a.size()));
    z.resize(zl);
    std::vector<uint8_t> v{'C', 'D', 'R', 'M'};
    put32(v, 0); put32(v, 2); put32(v, 0);
    put32(v, 2 | uint32_t(a.size()) << 8); put32(v, uint32_t(z.size()));
    put32(v, 1 | uint32_t(b.size()) << 8); put32(v, uint32_t(b.size()));
    pad16(v);
    v.insert(v.end(), z.begin(), z.end()); pad16(v);
    v.insert(v.end(), b.begin(), b.end()); pad16(v);
    return v;
}

std::vector<uint8_t> makeDrm() {
    std::vector<uint8_t> v;
    const char deps[] = "obj_a\0obj_b\0";
    put32(v, 19); put32(v, sizeof deps - 1); put32(v, 0); put32(v, 0); put32(v, 0); put32(v, 1);
    // section: 8 data bytes, texture type, 1 internal reloc (20 + 8 bytes)
    put32(v, 8); v.push_back(5); v.push_back(0); v.push_back(0); v.push_back(0);
    put32(v, 28u << 8); put32(v, 0x1234); put32(v, 0xFFFFFFFF);
    v.insert(v.end(), deps, deps + sizeof deps - 1);
    put32(v, 1); put32(v, 0); put32(v, 0); put32(v, 0); put32(v, 0);
    put32(v, 0); put32(v, 4); // pointer at 0 -> offset 4
    put32(v, 0); put32(v, 0xCAFEBABE);
    return v;
}
} // namespace

TEST(PathHash, KnownTruPaths) {
    // Both names exist in the retail GOG bigfile.000 index.
    EXPECT_EQ(pathHash("pc-w\\lara.drm"), pathHash("PC-W\\LARA.DRM"));
    EXPECT_EQ(pathHash("123456789"), 0xFC891918u); // CRC-32/BZIP2 check value
}

TEST(Cdrm, RoundTripMixedBlocks) {
    std::vector<uint8_t> a(5000, 'x'), b{1, 2, 3};
    auto out = decompressCdrm(makeCdrm(a, b));
    ASSERT_EQ(out.size(), a.size() + b.size());
    EXPECT_EQ(out[4999], 'x');
    EXPECT_EQ(out[5002], 3);
}

TEST(Cdrm, TruncationThrows) {
    auto c = makeCdrm(std::vector<uint8_t>(5000, 'x'), {1, 2, 3});
    for (size_t cut : {size_t(10), size_t(20), size_t(40), c.size() - 20}) {
        std::vector<uint8_t> t(c.begin(), c.begin() + cut);
        EXPECT_THROW(decompressCdrm(t), FormatError) << cut;
    }
}

TEST(Cdrm, CorruptZlibThrows) {
    auto c = makeCdrm(std::vector<uint8_t>(5000, 'x'), {1});
    c[34] ^= 0xFF;
    c[35] ^= 0xFF;
    EXPECT_THROW(decompressCdrm(c), FormatError);
}

TEST(Drm, ParsesSectionsDepsAndRelocs) {
    auto d = parseDrm(makeDrm());
    ASSERT_EQ(d.sections.size(), 1u);
    EXPECT_EQ(d.dependencies, (std::vector<std::string>{"obj_a", "obj_b"}));
    const auto& s = d.sections[0];
    EXPECT_EQ(s.type, SectionType::Texture);
    EXPECT_EQ(s.id, 0x1234u);
    EXPECT_EQ(s.data.size(), 8u);
    EXPECT_EQ(internalPointer(s, 0), 4u);
    EXPECT_FALSE(internalPointer(s, 4));
}

TEST(Drm, WrappedInCdrm) {
    auto raw = makeDrm();
    auto d = parseDrm(makeCdrm(raw, {}));
    EXPECT_EQ(d.sections.size(), 1u);
}

TEST(Drm, RejectsBadVersionAndTruncation) {
    auto raw = makeDrm();
    auto bad = raw; bad[0] = 18;
    EXPECT_THROW(parseDrm(bad), FormatError);
    for (size_t cut = 0; cut < raw.size(); cut += 7)
        EXPECT_THROW(parseDrm(std::vector<uint8_t>(raw.begin(), raw.begin() + cut)), FormatError) << cut;
}

namespace {
struct Rec { std::string path; uint32_t specMask; std::string payload; };
// Single-part bigfile; records sorted by hash, stable for equal hashes (stored order kept).
void writeBigfile(const std::filesystem::path& file, std::vector<Rec> recs) {
    std::ranges::stable_sort(recs, {}, [](const Rec& r) { return pathHash(r.path); });
    std::vector<uint8_t> v;
    put32(v, uint32_t(recs.size()));
    for (const auto& r : recs) put32(v, pathHash(r.path));
    uint32_t sector = uint32_t((4 + 20 * recs.size() + 2047) / 2048);
    for (const auto& r : recs) {
        put32(v, uint32_t(r.payload.size())); put32(v, sector); put32(v, r.specMask); put32(v, 0);
        sector += uint32_t((r.payload.size() + 2047) / 2048);
    }
    for (const auto& r : recs) {
        v.resize((v.size() + 2047) / 2048 * 2048);
        v.insert(v.end(), r.payload.begin(), r.payload.end());
    }
    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(v.data()), std::streamsize(v.size()));
}
std::string str(const std::optional<std::vector<uint8_t>>& b) { return b ? std::string(b->begin(), b->end()) : "<none>"; }
} // namespace

TEST(Bigfile, SpecMaskNeedsAllBitsLikeTheExe) {
    const auto dir = std::filesystem::temp_directory_path() / ("ou_cdc_spec_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    // Stored order: French first, as for many real voice files; then Dutch-style (no bit 0), then English.
    writeBigfile(dir / "bigfile.000", {{"v.mul", 0xFFFFE002, "fr"}, {"v.mul", 0xFFFFFFE0, "nl"},
                                       {"v.mul", 0xFFFFFFE1, "en"}, {"all.drm", 0xFFFFFFFF, "all"}});
    const Bigfile bf(dir / "bigfile.000");
    EXPECT_EQ(str(bf.read(*bf.find("v.mul"))), "en"); // default = kSpecEnglish
    EXPECT_EQ(str(bf.read(*bf.find("v.mul", kSpecFrench))), "fr");
    EXPECT_EQ(str(bf.read(*bf.find("v.mul", 1u << 11))), "nl"); // only 0xFFFFFFE0 has bit 11
    EXPECT_FALSE(bf.find("v.mul", kSpecEnglish | kSpecFrench)); // no record has both bits
    EXPECT_EQ(str(bf.read(*bf.find("all.drm", kSpecSpanish))), "all");
    EXPECT_FALSE(bf.find("missing.drm"));

    // patch.000 is looked up first; a path only in bigfile still resolves; language applies to both.
    BigfileSet noPatch(dir);
    EXPECT_FALSE(noPatch.hasPatch());
    EXPECT_EQ(str(noPatch.read("all.drm")), "all");
    writeBigfile(dir / "patch.000", {{"all.drm", 0xFFFFFFFF, "patched"}, {"v.mul", 0xFFFFE002, "fr2"}});
    const BigfileSet en(dir), fr(dir, kSpecFrench);
    EXPECT_TRUE(en.hasPatch());
    EXPECT_EQ(str(en.read("all.drm")), "patched");
    EXPECT_EQ(str(en.read("v.mul")), "en"); // patch has no English variant
    EXPECT_EQ(str(fr.read("v.mul")), "fr2");
    EXPECT_EQ(str(en.read("missing.drm")), "<none>");
    std::filesystem::remove_all(dir);
}

TEST(Drm, SectionPointersInternalAndExternal) {
    // Two general sections; section 1 has one internal (0 -> 8) and one external pointer
    // (patch 4 -> section 0 offset 12).
    std::vector<uint8_t> v;
    put32(v, 19); put32(v, 0); put32(v, 0); put32(v, 0); put32(v, 0); put32(v, 2);
    put32(v, 16); put32(v, 0); put32(v, 0); put32(v, 0); put32(v, 0);
    put32(v, 16); put32(v, 0); put32(v, 36u << 8); put32(v, 0); put32(v, 0);
    v.resize(v.size() + 16); // section 0 data
    put32(v, 1); put32(v, 1); put32(v, 0); put32(v, 0); put32(v, 0);
    put32(v, 0); put32(v, 8);
    const uint32_t patchWords = 4 / 4;
    put32(v, 0 | (patchWords & 0x3FFFF) << 14); put32(v, (patchWords >> 18) | 12u << 6);
    v.resize(v.size() + 16); // section 1 data
    const auto d = parseDrm(v);
    const auto p = sectionPointers(d, 1);
    ASSERT_EQ(p.size(), 2u);
    EXPECT_EQ(p.at(0).section, 1u); EXPECT_EQ(p.at(0).offset, 8u);
    EXPECT_EQ(p.at(4).section, 0u); EXPECT_EQ(p.at(4).offset, 12u);
    EXPECT_TRUE(sectionPointers(d, 0).empty());
}
