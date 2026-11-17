#include "cdc/Cdc.hpp"

#include <gtest/gtest.h>
#include <zlib.h>

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>
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

// ---- Writers / round trip (W2-4, W2-5; CONTAINERS.md §8) ---------------------------------

TEST(RoundTrip, SyntheticDrmRebuildsAndParserSeesEdits) {
    const auto raw = makeDrm();
    auto img = toDrmImage(parseDrm(raw));
    EXPECT_EQ(serializeDrm(img), raw);
    ASSERT_EQ(img.names, (std::vector<std::string>{"obj_a", "obj_b"}));
    ASSERT_TRUE(img.sections[0].relocations);
    // Edits go through the writer and come back through the reference parser.
    img.names.push_back("obj_c");
    img.sections[0].relocations->internal.push_back({4, 0});
    img.sections[0].data.resize(16, 0xEE);
    img.sections[0].packedLow = 5 << 1 | 1; // resourceType 5 + debug bit: kept beside the recomputed table size
    img.sections[0].versionId = 0xBEEF;
    const auto edited = parseDrm(serializeDrm(img));
    EXPECT_EQ(edited.dependencies, (std::vector<std::string>{"obj_a", "obj_b", "obj_c"}));
    ASSERT_EQ(edited.sections.size(), 1u);
    EXPECT_EQ(edited.sections[0].data.size(), 16u);
    EXPECT_EQ(edited.sections[0].relocBytes(), 36u);
    EXPECT_EQ(internalPointer(edited.sections[0], 4), 0u);
    EXPECT_EQ(edited.sections[0].id, 0x1234u);
    EXPECT_EQ(edited.sections[0].resourceType(), 5u);
    EXPECT_EQ(edited.sections[0].packed & 1, 1u);
    EXPECT_EQ(edited.sections[0].versionId, 0xBEEF);
    // A table whose counts disagree with its size is rejected rather than silently rewritten.
    auto bad = raw;
    bad[24 + 20 + 12] = 2; // section 0 internal count 1 -> 2 (deps block is 12 bytes)
    EXPECT_THROW(toDrmImage(parseDrm(bad)), FormatError);
}

TEST(RoundTrip, RelocationFieldsBitExact) {
    Relocations r;
    r.internal = {{0, 4}, {0xFFFFFFFC, 0x12345678}};
    r.external = {{0x3FFF, (1u << 26) - 4, (1u << 26) - 1}, {7, 4 << 18, 9}}; // patch words cross the lo/hi split
    r.kind2 = {{(1u << 27) - 4, 0x7F}};
    r.resource16 = {{8, 5}, {0, 0}};
    const auto t = encodeRelocations(r);
    EXPECT_EQ(t.size(), 20u + 16 + 16 + 4 + 8);
    const auto back = decodeRelocations(t);
    ASSERT_EQ(back.external.size(), 2u);
    EXPECT_EQ(back.external[0].section, 0x3FFFu);
    EXPECT_EQ(back.external[0].offset, (1u << 26) - 4);
    EXPECT_EQ(back.external[0].target, (1u << 26) - 1);
    EXPECT_EQ(back.external[1].offset, 4u << 18);
    EXPECT_EQ(back.kind2[0].offset, (1u << 27) - 4);
    EXPECT_EQ(back.kind2[0].sectionType, 0x7F);
    EXPECT_EQ(back.resource16[0].sectionType, 5);
    EXPECT_TRUE(back.resource32.empty());
    EXPECT_EQ(encodeRelocations(back), t);
    // The writer agrees with the reference resolvers (sectionPointers / resourceRefType) on the same table.
    Drm d;
    d.sections.resize(0x4000);
    d.sections[1].relocations = t;
    const auto p = sectionPointers(d, 1);
    EXPECT_EQ(p.at((1u << 26) - 4).section, 0x3FFFu);
    EXPECT_EQ(p.at(4u << 18).offset, 9u);
    EXPECT_EQ(resourceRefType(d.sections[1], 8), 5);
    // Out-of-range fields throw instead of being truncated.
    for (auto e : {Relocations::External{0x4000, 0, 0}, {0, 2, 0}, {0, 1u << 26, 0}, {0, 0, 1u << 26}}) {
        Relocations x;
        x.external = {e};
        EXPECT_THROW(encodeRelocations(x), FormatError);
    }
    EXPECT_THROW(decodeRelocations(std::vector<uint8_t>(t.begin(), t.end() - 4)), FormatError);
}

TEST(RoundTrip, CdrmWriterSplitsBlocksAndRewraps) {
    std::vector<uint8_t> raw(kCdrmBlockBytes * 2 + 1234);
    for (size_t i = 0; i < raw.size(); ++i) raw[i] = uint8_t(i * 2654435761u >> 13);
    const auto c = compressCdrm(raw);
    const auto blocks = readCdrmBlocks(c);
    ASSERT_EQ(blocks.size(), 3u);
    EXPECT_EQ(blocks[0].uncompressedSize, kCdrmBlockBytes);
    EXPECT_EQ(blocks[2].uncompressedSize, 1234u);
    EXPECT_EQ(blocks[0].payload[0], 0x78);
    EXPECT_EQ(blocks[0].payload[1], 0xDA); // zlib header for level 9, as every install block
    EXPECT_EQ(writeCdrm(blocks), c);
    EXPECT_EQ(decompressCdrm(c), raw);
    EXPECT_EQ(c.size(), (16 + 3 * 8 + 15) / 16 * 16 + (blocks[0].payload.size() + 15) / 16 * 16 +
                            (blocks[1].payload.size() + 15) / 16 * 16 + blocks[2].payload.size()); // no pad after the last
}

TEST(RoundTrip, BigfileLayoutSkipsToNextPart) {
    // Parts of 8 sectors: the 3-sector payload would start at sector 7 and cross into part 1, so it starts at 8; an
    // empty payload takes a sector number but no space; the last one skips from sector 11 to part 2.
    const std::vector<uint32_t> sizes{2048 * 5, 100, 2049 * 2, 0, 2048 * 8};
    EXPECT_EQ(layoutBigfileSectors(sizes, 1, 8 * 2048), (std::vector<uint32_t>{1, 6, 8, 11, 16}));
    const std::vector<BigfileRecord> recs{{1, 10, 1, 0xFFFFFFFF, 0}, {2, 20, 2, 3, 0}};
    const auto idx = encodeBigfileIndex(recs);
    ASSERT_EQ(idx.size(), 2048u);
    EXPECT_EQ(idx[0], 2);
    EXPECT_EQ(idx[4 + 8 + 16 + 4], 2); // record 1 sector
    EXPECT_TRUE(std::all_of(idx.begin() + 4 + 40, idx.end(), [](uint8_t b) { return b == 0; }));
}

// ---- Real data: whole-install round trip (skipped without OU_GAME_DIR) -------------------
namespace {
const char* gameDir() { return std::getenv("OU_GAME_DIR"); }

std::vector<uint8_t> readPrefix(const std::filesystem::path& p, size_t n) {
    std::vector<uint8_t> v(n);
    std::ifstream f(p, std::ios::binary);
    f.read(reinterpret_cast<char*>(v.data()), std::streamsize(n));
    v.resize(size_t(f.gcount()));
    return v;
}

std::vector<std::filesystem::path> archives() {
    std::vector<std::filesystem::path> out{std::filesystem::path(gameDir()) / "bigfile.000"};
    if (std::filesystem::exists(std::filesystem::path(gameDir()) / "patch.000"))
        out.push_back(std::filesystem::path(gameDir()) / "patch.000");
    return out;
}

// Calls fn(record index, payload) for every DRM record (CDRM or raw version 19) of `bf` on up to 8 threads. Reads are
// serialised and go in sector order, so a spinning disk sees one forward stream; other payloads are skipped after a
// 4-byte peek. fn must be thread-safe.
template <class Fn> void forEachDrm(const Bigfile& bf, Fn fn) {
    const auto& recs = bf.records();
    std::vector<size_t> order(recs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::ranges::stable_sort(order, {}, [&](size_t i) { return recs[i].sector; });
    std::atomic<size_t> next{0};
    std::mutex io;
    std::vector<std::thread> pool;
    const unsigned threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (;;) {
                size_t i;
                std::vector<uint8_t> bytes;
                {
                    std::scoped_lock l(io);
                    for (;;) {
                        const size_t k = next.fetch_add(1);
                        if (k >= order.size()) return;
                        i = order[k];
                        const uint64_t pos = uint64_t(recs[i].sector) << 11;
                        uint8_t head[4] = {};
                        std::ifstream f(bf.partPath(uint32_t(pos / Bigfile::kPartAlignment)), std::ios::binary);
                        f.seekg(std::streamoff(pos % Bigfile::kPartAlignment));
                        f.read(reinterpret_cast<char*>(head), 4);
                        uint32_t version;
                        std::memcpy(&version, head, 4);
                        if (recs[i].size >= 24 && (std::memcmp(head, "CDRM", 4) == 0 || version == kDrmVersion)) break;
                    }
                    bytes = bf.read(recs[i]);
                }
                fn(i, std::move(bytes));
            }
        });
    for (auto& t : pool) t.join();
}
} // namespace

TEST(RealData, RoundTripBigfileIndexAndLayout) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    for (const auto& path : archives()) {
        SCOPED_TRACE(path.filename().string());
        const Bigfile bf(path);
        const auto& recs = bf.records();
        const auto idx = encodeBigfileIndex(recs);
        EXPECT_EQ(idx, readPrefix(path, idx.size()));
        // Payload slots in sector order (records sharing a sector share one payload) rebuild every sector.
        std::vector<size_t> order(recs.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::ranges::stable_sort(order, {}, [&](size_t i) { return recs[i].sector; });
        std::vector<uint32_t> sizes, sectors;
        size_t shared = 0;
        for (size_t k = 0; k < order.size(); ++k) {
            const auto& r = recs[order[k]];
            if (k && r.sector == recs[order[k - 1]].sector) {
                EXPECT_EQ(r.size, recs[order[k - 1]].size) << "records sharing a sector differ in size";
                ++shared;
                continue;
            }
            sizes.push_back(r.size);
            sectors.push_back(r.sector);
        }
        EXPECT_EQ(sectors.front(), idx.size() / 2048) << "first payload directly after the padded index";
        EXPECT_EQ(layoutBigfileSectors(sizes, sectors.front()), sectors);
        // Each part file ends at its last payload, rounded up to a sector.
        std::map<uint32_t, uint64_t> partEnd;
        for (size_t k = 0; k < sizes.size(); ++k) {
            const uint64_t pos = uint64_t(sectors[k]) << 11;
            auto& e = partEnd[uint32_t(pos / Bigfile::kPartAlignment)];
            e = std::max(e, (pos % Bigfile::kPartAlignment + sizes[k] + 2047) & ~uint64_t(2047));
        }
        for (const auto& [part, end] : partEnd) EXPECT_EQ(std::filesystem::file_size(bf.partPath(part)), end) << part;
        std::printf("[ round trip ] %s: %zu records, %zu payloads (%zu shared), %zu parts: index and layout identical\n",
                    path.filename().string().c_str(), recs.size(), sizes.size(), shared, partEnd.size());
    }
}

TEST(RealData, RoundTripEveryDrm) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    for (const auto& path : archives()) {
        SCOPED_TRACE(path.filename().string());
        const Bigfile bf(path);
        const auto& recs = bf.records();
        std::atomic<size_t> cdrm{0}, raw{0}, cdrmSame{0}, drmSame{0}, sections{0}, blocks{0};
        std::mutex m;
        std::vector<std::string> mismatches;
        forEachDrm(bf, [&](size_t i, std::vector<uint8_t> bytes) {
            const bool wrapped = isCdrm(bytes);
            auto fail = [&](const std::string& what) {
                std::scoped_lock l(m);
                char h[16];
                std::snprintf(h, sizeof h, "%08x", recs[i].hash);
                mismatches.push_back(std::string(h) + ": " + what);
            };
            try {
                if (wrapped) {
                    ++cdrm;
                    const auto b = readCdrmBlocks(bytes);
                    blocks += b.size();
                    if (writeCdrm(b) == bytes) ++cdrmSame;
                    else fail("CDRM re-wrap differs");
                } else {
                    ++raw;
                }
                const Drm drm = parseDrm(bytes);
                sections += drm.sections.size();
                if (serializeDrm(toDrmImage(drm)) == drm.bytes) ++drmSame;
                else fail("DRM serialise differs");
            } catch (const std::exception& e) {
                fail(e.what());
            }
        });
        for (const auto& s : mismatches) ADD_FAILURE() << s;
        EXPECT_TRUE(mismatches.empty());
        std::printf("[ round trip ] %s: %zu DRMs (%zu CDRM, %zu raw), %zu sections; DRM serialise identical %zu, "
                    "CDRM re-wrap identical %zu/%zu (%zu blocks), mismatches %zu\n",
                    path.filename().string().c_str(), cdrm + raw, size_t(cdrm), size_t(raw), size_t(sections),
                    size_t(drmSame), size_t(cdrmSame), size_t(cdrm), size_t(blocks), mismatches.size());
        if (path.filename() == "bigfile.000") {
            EXPECT_EQ(cdrm, 1056u); // CONTAINERS.md §4
            EXPECT_EQ(raw, 1u);
        }
    }
}

namespace {
// compress2 of a classic (non zlib-ng) zlib, dlopen'ed for the recompression survey.
using Compress2 = int (*)(uint8_t*, unsigned long*, const uint8_t*, unsigned long, int);
using CompressBound = unsigned long (*)(unsigned long);
Compress2 gCompress2 = nullptr;
CompressBound gCompressBound = nullptr;
std::vector<uint8_t> classicDeflate9(std::span<const uint8_t> raw) {
    unsigned long len = gCompressBound(raw.size());
    std::vector<uint8_t> out(len);
    if (gCompress2(out.data(), &len, raw.data(), raw.size(), kCdrmZlibLevel) != 0) throw FormatError("compress2 failed");
    out.resize(len);
    return out;
}
} // namespace

// Opt-in (minutes): recompress every CDRM at level 9 and compare with the install. Deflate output depends on the
// implementation: the install was made with classic zlib, so this needs one. OU_CLASSIC_ZLIB=<path to libz.so.1>
// (classic, e.g. a flatpak runtime's), or the linked zlib when it is not zlib-ng.
TEST(RealData, CdrmRecompressSurvey) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const char* lib = std::getenv("OU_CLASSIC_ZLIB");
    Deflate deflate = deflateLevel9;
    if (lib) {
        // DEEPBIND: compress2's own calls (deflateInit2_, deflate) must stay inside the classic library instead of
        // binding to the zlib(-ng) this test already links (without it every stream differs).
        void* h = dlopen(lib, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
        ASSERT_TRUE(h) << dlerror();
        using Version = const char* (*)();
        const auto version = reinterpret_cast<Version>(dlsym(h, "zlibVersion"));
        gCompress2 = reinterpret_cast<Compress2>(dlsym(h, "compress2"));
        gCompressBound = reinterpret_cast<CompressBound>(dlsym(h, "compressBound"));
        ASSERT_TRUE(version && gCompress2 && gCompressBound);
        ASSERT_EQ(std::strstr(version(), "zlib-ng"), nullptr) << version() << " is zlib-ng, not classic zlib";
        deflate = classicDeflate9;
    } else if (std::strstr(zlibVersion(), "zlib-ng")) {
        GTEST_SKIP() << "linked zlib is " << zlibVersion() << "; set OU_CLASSIC_ZLIB to a classic libz.so.1";
    }
    for (const auto& path : archives()) {
        SCOPED_TRACE(path.filename().string());
        const Bigfile bf(path);
        const auto& recs = bf.records();
        std::atomic<size_t> files{0}, same{0}, blocks{0}, blocksSame{0};
        std::mutex m;
        std::vector<std::string> mismatches;
        forEachDrm(bf, [&](size_t i, std::vector<uint8_t> bytes) {
            if (!isCdrm(bytes)) return;
            ++files;
            const auto orig = readCdrmBlocks(bytes);
            const auto rebuilt = compressCdrm(decompressCdrm(bytes), deflate);
            if (rebuilt == bytes) ++same;
            const auto nb = readCdrmBlocks(rebuilt);
            blocks += orig.size();
            size_t ok = 0;
            for (size_t k = 0; k < std::min(orig.size(), nb.size()); ++k) ok += orig[k].payload == nb[k].payload;
            blocksSame += ok;
            if (rebuilt != bytes) {
                std::scoped_lock l(m);
                char hx[16];
                std::snprintf(hx, sizeof hx, "%08x", recs[i].hash);
                mismatches.push_back(std::string(hx) + ": " + std::to_string(orig.size() - ok) + " of " +
                                     std::to_string(orig.size()) + " blocks differ");
            }
        });
        for (const auto& s : mismatches) ADD_FAILURE() << s;
        std::printf("[ round trip ] %s: recompressed %zu CDRMs, identical %zu; blocks identical %zu/%zu (%s)\n",
                    path.filename().string().c_str(), size_t(files), size_t(same), size_t(blocksSame), size_t(blocks),
                    lib ? lib : zlibVersion());
    }
}
