#include "cdc/Cdc.hpp"
#include "importer/Importer.hpp"

#include <gtest/gtest.h>
#include <zlib.h>

#include <cstring>
#include <fstream>

using namespace ou::importer;

namespace {
using Bytes = std::vector<uint8_t>;
constexpr uint64_t kAlign = 0x2000; // tiny part alignment so a few records spill into .001/.002

void put32(Bytes& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
Bytes str(std::string_view s) { return {s.begin(), s.end()}; }

Bytes makeDrm(std::string_view deps) { // one 8-byte texture section, no relocations
    Bytes v;
    put32(v, 19); put32(v, uint32_t(deps.size())); put32(v, 0); put32(v, 0); put32(v, 0); put32(v, 1);
    put32(v, 8); v.push_back(5); v.push_back(0); v.push_back(0); v.push_back(0);
    put32(v, 0); put32(v, 0x1234); put32(v, 0xFFFFFFFF);
    v.insert(v.end(), deps.begin(), deps.end());
    put32(v, 0xCAFEBABE); put32(v, 0x12345678);
    return v;
}
Bytes makeCdrm(const Bytes& a) { // single zlib block
    Bytes z(compressBound(uLong(a.size())));
    uLongf zl = z.size();
    compress(z.data(), &zl, a.data(), uLong(a.size()));
    z.resize(zl);
    Bytes v = str("CDRM");
    put32(v, 0); put32(v, 1); put32(v, 0);
    put32(v, 2 | uint32_t(a.size()) << 8); put32(v, uint32_t(z.size()));
    while (v.size() % 16) v.push_back(0);
    v.insert(v.end(), z.begin(), z.end());
    while (v.size() % 16) v.push_back(0);
    return v;
}

struct Rec { std::string name; Bytes data; uint32_t spec = 0xFFFFFFFF; uint32_t sizeOverride = 0; };

// Writes <dir>/<base>.000.. with records laid out sector-aligned, never straddling a part.
void writeBigfile(const fs::path& dir, const std::string& base, std::vector<Rec> recs) {
    std::ranges::sort(recs, {}, [](const Rec& r) { return ou::cdc::pathHash(r.name); });
    const size_t n = recs.size();
    uint64_t pos = ((4 + n * 20) + 2047) & ~uint64_t(2047);
    Bytes index;
    put32(index, uint32_t(n));
    for (auto& r : recs) put32(index, ou::cdc::pathHash(r.name));
    std::vector<Bytes> parts(1);
    std::vector<std::pair<uint64_t, const Rec*>> placed;
    for (auto& r : recs) {
        if (pos / kAlign != (pos + r.data.size() - 1) / kAlign) pos = (pos / kAlign + 1) * kAlign;
        placed.emplace_back(pos, &r);
        pos += (r.data.size() + 2047) & ~size_t(2047);
    }
    for (auto& [p, r] : placed) {
        put32(index, r->sizeOverride ? r->sizeOverride : uint32_t(r->data.size()));
        put32(index, uint32_t(p >> 11)); put32(index, r->spec); put32(index, 0);
        const size_t part = p / kAlign, off = p % kAlign;
        if (parts.size() <= part) parts.resize(part + 1);
        if (parts[part].size() < off + r->data.size()) parts[part].resize(off + r->data.size());
        std::memcpy(parts[part].data() + off, r->data.data(), r->data.size());
    }
    if (parts[0].size() < index.size()) parts[0].resize(index.size());
    std::memcpy(parts[0].data(), index.data(), index.size());
    for (size_t i = 0; i < parts.size(); ++i) {
        char ext[24];
        std::snprintf(ext, sizeof ext, ".%03zu", i);
        std::ofstream(dir / (base + ext), std::ios::binary).write(reinterpret_cast<const char*>(parts[i].data()), std::streamsize(parts[i].size()));
    }
}

struct TempDir {
    fs::path p;
    TempDir() {
        p = fs::temp_directory_path() / ("ou_importer_" + std::to_string(::getpid()) + "_" +
                                         ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(p);
        fs::create_directories(p);
    }
    ~TempDir() { fs::remove_all(p); }
};

Bytes pad(Bytes b, size_t n) { b.resize(n, 0x11); return b; }

std::vector<Rec> sampleRecords() {
    Bytes mus = str("Mus!....\\tr8\\audio\\music\\l9_test\\l9_testctl.txt");
    Bytes mul;
    put32(mul, 44100); put32(mul, 0xFFFFFFFF); put32(mul, 0); put32(mul, 2);
    return {
        {"\\tr8\\pc-w\\objectlist.txt", str("2\r\n1,lara\r\n2,big_object\r\n")},
        {"pc-w\\lara.drm", makeCdrm(makeDrm(std::string_view("dep_obj\0", 8)))},
        {"pc-w\\dep_obj.drm", makeCdrm(makeDrm({}))},
        {"pc-w\\big_object.drm", pad(makeCdrm(makeDrm({})), 5000)}, // pushed into a later part
        {"pc-w\\music\\l9_test\\l9_test.mus", mus},
        {"pc-w\\music\\l9_test\\l9_test.sam", pad(str("FSB4"), 3000)},
        {"pc-w\\music\\l9_test\\l9_test.mul", pad(mul, 7000)},
        {"pc-w\\image\\nameless.raw", pad(str("!WAR"), 100)},
        {"mystery.bin", str("ZZZZ1234")},
    };
}

json kindsOf(const json& arc) { return arc["kinds"]; }
} // namespace

TEST(Importer, SyntheticRoundTripWithSplitParts) {
    TempDir t;
    writeBigfile(t.p, "bigfile", sampleRecords());
    writeBigfile(t.p, "patch", {{"pc-w\\dep_obj.drm", makeCdrm(makeDrm({}))}});
    ASSERT_TRUE(fs::exists(t.p / "bigfile.001"));

    for (bool deep : {false, true}) {
        ScanOptions o;
        o.partAlignment = kAlign;
        o.deep = deep;
        o.threads = 3;
        auto m = scan(t.p, o);
        const auto& big = m["archives"][0];
        EXPECT_EQ(big["record_count"], 9);
        EXPECT_EQ(big["record_errors"], 0);
        EXPECT_TRUE(big["missing_parts"].empty());
        EXPECT_GE(big["parts"].size(), 2u);
        auto k = kindsOf(big);
        EXPECT_EQ(k["CDRM"], 3);
        EXPECT_EQ(k["FSB4"], 1);
        EXPECT_EQ(k["Mus!"], 1);
        EXPECT_EQ(k["MUL?"], 1);
        EXPECT_EQ(k["!WAR"], 1);
        EXPECT_EQ(k["unknown"], 2); // objectlist.txt + mystery.bin
        // objectlist -> lara/big_object, lara's dependency -> dep_obj, Mus! script -> music trio,
        // fixed exe template -> objectlist.txt. nameless.raw/mystery.bin stay unknown.
        EXPECT_EQ(big["named_unique_hashes"], 7);
        std::map<std::string, std::string> src;
        for (auto& r : big["records"])
            if (r.contains("name")) src[r["name"]] = r["name_source"];
        EXPECT_EQ(src["pc-w\\dep_obj.drm"], "drm-dependency");
        EXPECT_EQ(src["pc-w\\lara.drm"], "objectlist.txt");
        EXPECT_EQ(src["pc-w\\music\\l9_test\\l9_test.sam"], "mus-script");
        for (auto& r : big["records"])
            if (r.value("name", "") == "pc-w\\lara.drm") {
                EXPECT_EQ(r["drm"]["sections"], 1);
                EXPECT_EQ(r["drm"]["section_types"]["texture"], 1);
                EXPECT_EQ(r["drm"]["object_deps"][0], "dep_obj");
            }
        ASSERT_EQ(m["patch_overrides"].size(), 1u);
        EXPECT_TRUE(m["patch_overrides"][0]["same_spec_in_bigfile"].get<bool>());
        EXPECT_EQ(m["patch_overrides"][0]["name"], "pc-w\\dep_obj.drm");
    }

    auto x = extract(t.p, "pc-w\\big_object.drm", std::nullopt, false, kAlign);
    EXPECT_EQ(x.archive, "bigfile");
    EXPECT_EQ(x.bytes, makeDrm({}));
    EXPECT_EQ(extract(t.p, "pc-w\\dep_obj.drm", std::nullopt, true, kAlign).archive, "patch");
    char h[16];
    std::snprintf(h, sizeof h, "0x%08x", ou::cdc::pathHash("mystery.bin"));
    EXPECT_EQ(extract(t.p, h, std::nullopt, false, kAlign).bytes, str("ZZZZ1234"));
    EXPECT_THROW(extract(t.p, "nope.drm", std::nullopt, false, kAlign), std::runtime_error);
}

TEST(Importer, ExternalNameListFillsGapsAndCountsAgreement) {
    TempDir t;
    writeBigfile(t.p, "bigfile", sampleRecords());
    std::ofstream(t.p / "names.txt") << "; header\npc-w\\image\\nameless.raw\npc-w\\lara.drm\n";
    ScanOptions o;
    o.partAlignment = kAlign;
    o.nameLists = {t.p / "names.txt"};
    auto m = scan(t.p, o);
    EXPECT_EQ(m["names"]["resolved_install_derived"], 7);
    EXPECT_EQ(m["names"]["resolved_total"], 8);
    EXPECT_EQ(m["names"]["external_lists"][0]["agree_with_derived"], 1);
}

TEST(Importer, TruncatedIndexIsArchiveError) {
    TempDir t;
    Bytes b;
    put32(b, 50); // claims 50 records, provides none
    put32(b, 1);
    std::ofstream(t.p / "bigfile.000", std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    ScanOptions o;
    o.partAlignment = kAlign;
    auto m = scan(t.p, o);
    EXPECT_NE(m["archives"][0]["error"].get<std::string>().find("truncated"), std::string::npos);
    EXPECT_EQ(m["archives"][1]["error"], "patch.000 missing");
}

TEST(Importer, RecordPastEofAndMissingPart) {
    TempDir t;
    auto recs = sampleRecords();
    recs[7].sizeOverride = 1 << 20; // !WAR record claims 1 MiB
    writeBigfile(t.p, "bigfile", recs);
    ScanOptions o;
    o.partAlignment = kAlign;
    auto errorsMatching = [](const json& arc, std::string_view what) {
        int n = 0;
        for (auto& r : arc["records"]) n += r.value("error", "").find(what) != std::string::npos;
        return n;
    };
    {
        auto m = scan(t.p, o);
        EXPECT_EQ(errorsMatching(m["archives"][0], "past end"), 1);
        EXPECT_EQ(m["archives"][0]["record_errors"], 1);
    }
    fs::path last;
    for (int i = 1; fs::exists(t.p / ("bigfile.00" + std::to_string(i))); ++i) last = t.p / ("bigfile.00" + std::to_string(i));
    ASSERT_FALSE(last.empty());
    fs::remove(last);
    auto m = scan(t.p, o);
    const auto& big = m["archives"][0];
    ASSERT_EQ(big["missing_parts"].size(), 1u);
    EXPECT_EQ(big["missing_parts"][0], last.filename().string());
    EXPECT_GE(errorsMatching(big, " missing"), 1);
}

TEST(Importer, DiffDetectsFileAndRecordChanges) {
    TempDir t;
    writeBigfile(t.p, "bigfile", sampleRecords());
    std::ofstream(t.p / "readme.txt") << "a";
    ScanOptions o;
    o.partAlignment = kAlign;
    auto a = scan(t.p, o);
    EXPECT_TRUE(diffManifests(a, a)["identical"].get<bool>());

    auto recs = sampleRecords();
    recs.pop_back();                                 // drop mystery.bin
    recs.push_back({"extra.bin", str("NEW!")});      // add a record
    recs[7].data = pad(str("!WAR"), 200);            // resize nameless.raw
    writeBigfile(t.p, "bigfile", recs);
    std::ofstream(t.p / "readme.txt") << "b";
    std::ofstream(t.p / "new.txt") << "x";
    auto b = scan(t.p, o);
    auto d = diffManifests(a, b);
    EXPECT_FALSE(d["identical"].get<bool>());
    EXPECT_EQ(d["files"]["added"], json::array({"new.txt"}));
    bool readmeChanged = false;
    for (auto& c : d["files"]["changed"]) readmeChanged |= c["path"] == "readme.txt";
    EXPECT_TRUE(readmeChanged);
    EXPECT_EQ(d["records"]["bigfile"]["added"].size(), 1u);
    EXPECT_EQ(d["records"]["bigfile"]["removed"].size(), 1u);
    bool sizeChanged = false;
    for (auto& c : d["records"]["bigfile"]["changed"]) sizeChanged |= std::ranges::find(c["fields"], "size") != c["fields"].end();
    EXPECT_TRUE(sizeChanged);
}

TEST(Importer, GogHashdbStoredZip) {
    TempDir t;
    Bytes table;
    put32(table, 12); put32(table, 1); put32(table, 3);
    for (auto [name, md5] : {std::pair{"tru.exe", "76B4DA2FD9178299065BB83412F324EF"}, std::pair{"L\xE9" "eme.rtf", "00000000000000000000000000000000"},
                             std::pair{"L\xC3\xA9" "eme.rtf", "00000000000000000000000000000000"}}) {
        Bytes e(1056, 0);
        std::memcpy(e.data(), name, std::strlen(name));
        std::memcpy(e.data() + 1024, md5, 32);
        table.insert(table.end(), e.begin(), e.end());
    }
    const std::string fn = "goggame-1.hashdb";
    Bytes z;
    auto put16 = [&](uint16_t x) { z.push_back(uint8_t(x)); z.push_back(uint8_t(x >> 8)); };
    put32(z, 0x04034b50); put16(20); put16(0); put16(0); put16(0); put16(0);
    put32(z, 0); put32(z, uint32_t(table.size())); put32(z, uint32_t(table.size())); put16(uint16_t(fn.size())); put16(0);
    z.insert(z.end(), fn.begin(), fn.end());
    z.insert(z.end(), table.begin(), table.end());
    const auto cd = uint32_t(z.size());
    put32(z, 0x02014b50); put16(20); put16(20); put16(0); put16(0); put16(0); put16(0);
    put32(z, 0); put32(z, uint32_t(table.size())); put32(z, uint32_t(table.size()));
    put16(uint16_t(fn.size())); put16(0); put16(0); put16(0); put16(0); put32(z, 0); put32(z, 0);
    z.insert(z.end(), fn.begin(), fn.end());
    const auto cdSize = uint32_t(z.size()) - cd;
    put32(z, 0x06054b50); put16(0); put16(0); put16(1); put16(1); put32(z, cdSize); put32(z, cd); put16(0);
    std::ofstream(t.p / fn, std::ios::binary).write(reinterpret_cast<const char*>(z.data()), std::streamsize(z.size()));

    auto e = parseGogHashdb(t.p / fn);
    ASSERT_EQ(e.size(), 3u);
    EXPECT_EQ(e[0].path, "tru.exe");
    EXPECT_EQ(e[0].md5, "76b4da2fd9178299065bb83412f324ef");
    EXPECT_EQ(e[1].path, "L\xC3\xA9" "eme.rtf"); // Latin-1 -> UTF-8
    EXPECT_EQ(e[2].path, "L\xC3\xA9" "eme.rtf"); // UTF-8 kept (what GOG actually stores)
}

TEST(Importer, PathContainment) {
    EXPECT_TRUE(isInside("/a/b/c.json", "/a/b"));
    EXPECT_TRUE(isInside("/a/b/../b/x", "/a/b"));
    EXPECT_TRUE(isInside("/a/b", "/a/b"));
    EXPECT_FALSE(isInside("/a/bc/x", "/a/b"));
    EXPECT_FALSE(isInside("/a/x", "/a/b"));
}

TEST(Importer, DefaultManifestPathHonoursXdg) {
    ::setenv("XDG_DATA_HOME", "/tmp/xdgtest", 1);
    EXPECT_EQ(defaultManifestPath("1fdbd34560ffe6b5ffff"), fs::path("/tmp/xdgtest/openunderworld/manifests/1fdbd34560ffe6b5.json"));
    ::unsetenv("XDG_DATA_HOME");
}

TEST(Importer, RealInstall) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir || !fs::is_directory(dir)) GTEST_SKIP() << "OU_GAME_DIR not set";
    ScanOptions o;
    o.hashFiles = false; // full hashing is covered by the CLI run documented in INVENTORY.md
    auto m = scan(dir, o);
    const auto& big = m["archives"][0];
    EXPECT_FALSE(big.contains("error"));
    EXPECT_TRUE(big["missing_parts"].empty());
    EXPECT_EQ(big["record_errors"], 0);
    EXPECT_GT(m["names"]["resolved_install_derived"].get<int>(), 0);
    if (m["exe"].value("sha256", "") == "1fdbd34560ffe6b5fa959666e023e405d6f3166ce3b63b0ae16a6aa4121d1011") {
        EXPECT_EQ(big["record_count"], 7339);
        EXPECT_EQ(big["kinds"]["CDRM"], 1056);
        EXPECT_EQ(m["exe"]["pe"]["machine"], "i386");
        ASSERT_EQ(m["gog_hashdb"].size(), 1u);
        EXPECT_FALSE(m["gog_hashdb"][0].contains("error"));
    }
}
