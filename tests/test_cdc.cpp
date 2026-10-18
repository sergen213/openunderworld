#include "cdc/Cdc.hpp"

#include <gtest/gtest.h>
#include <zlib.h>

#include <cstring>

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
