#include "save/Save.hpp"

#include "cdc/Cdc.hpp"

#include <gtest/gtest.h>

#include <set>
#include <zlib.h>

#include <cstdlib>
#include <fstream>
#include <memory>

using namespace ou;
using game::vec3;

namespace {

namespace fs = std::filesystem;

// As test_game.cpp: root moves forward (model -Y) by `perKey` units per key, 1/30 s keys.
anim::Clip rootClip(int keys, float perKey) {
    anim::Clip c;
    c.keyCount = uint16_t(keys);
    c.timePerKey = 100;
    c.tracks.resize(1);
    if (perKey != 0)
        for (int i = 0; i < keys; ++i) c.tracks[0][anim::TransY].push_back(-perKey * float(i + 1));
    return c;
}

game::AnimLibrary syntheticAnims() {
    game::AnimLibrary a;
    a.skeleton.bones.resize(1);
    a.clips["ng_idle"] = rootClip(50, 0);
    a.clips["ng_walk"] = rootClip(38, 11.6f);
    a.clips["ng_run"] = rootClip(20, 42.47f);
    return a;
}

void addFloor(collision::CollisionMesh& m, float x0, float y0, float x1, float y1, float z) {
    const auto b = uint32_t(m.vertices.size());
    m.vertices.insert(m.vertices.end(), {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}});
    m.faces.push_back({{b, b + 1, b + 2}});
    m.faces.push_back({{b, b + 2, b + 3}});
}

// Portal plane x = `x` spanning y [y0, y0 + 2000], z [z0, z0 + 2000], in its unit's frame.
formats::Portal portal(const std::string& to, int16_t signal, int16_t toSignal, float x, float y0, float z0) {
    formats::Portal p{};
    p.toUnit = to, p.signal = signal, p.toSignal = toSignal;
    const float lo[3] = {x, y0, z0}, hi[3] = {x, y0 + 2000, z0 + 2000};
    for (int i = 0; i < 3; ++i) p.min[i] = lo[i], p.max[i] = hi[i], p.normal[i] = i == 0;
    const float q[4][3] = {{x, y0, z0}, {x, y0 + 2000, z0}, {x, y0 + 2000, z0 + 2000}, {x, y0, z0 + 2000}};
    std::copy(&q[0][0], &q[0][0] + 12, &p.quad[0][0]);
    return p;
}

// Three units in a row, each in its own frame: A [-2000, 2000] at z 0; B [10000, 14000] at z 500 (world
// offset -8000, -6000, -500 from the A|B portal pair); C beyond B. One checkpoint in B, one ledge in C.
int loads = 0;
game::UnitData syntheticUnit(const std::string& name) {
    ++loads;
    game::UnitData u;
    u.name = name;
    if (name == "a") {
        addFloor(u.col, -2000, -1000, 2000, 1000, 0);
        u.portals = {portal("b", 11, 21, 2000, -1000, 0)};
        u.spawn = game::Spawn{{0, 0, 0}, 0, true};
    } else if (name == "b") {
        addFloor(u.col, 10000, 5000, 14000, 7000, 500);
        u.portals = {portal("a", 21, 11, 10000, 5000, 500), portal("c", 22, 31, 14000, 5000, 500)};
        u.checkpoints = {{77, {11500, 6000, 500}, 0.5f}};
        u.spawn = game::Spawn{{12000, 6000, 500}, 0, true};
    } else if (name == "c") {
        addFloor(u.col, 0, 0, 4000, 2000, 0);
        u.col.markup.push_back({1, 5, {{100, 100, 0}, {200, 100, 0}}});
        u.portals = {portal("b", 31, 22, 0, 0, 0)};
        u.spawn = game::Spawn{{2000, 1000, 0}, 0, true};
    } else if (name == "l1_cine_ship_sinking" || name == "l3_mansion_exterior_afterburn" || name == "l6_ship_natla_room") {
        // Cinematic-only registration needs the unit reference, not a gameplay spawn or collision.
    } else {
        throw cdc::FormatError("pc-w\\" + name + ".drm not in bigfile");
    }
    return u;
}

game::Actions forward() {
    game::Actions a;
    a.move.y = 1;
    return a;
}

std::string withHeader(int version, const std::string& body) {
    char h[64];
    std::snprintf(h, sizeof h, "OUSAVE %d %08lx %zu\n", version, crc32(0, reinterpret_cast<const Bytef*>(body.data()), uInt(body.size())),
                  body.size());
    return h + body;
}

save::SaveGame sample() {
    save::SaveGame s;
    s.unit = "l4_ragnarok_room";
    s.player.state = game::State::Shimmy;
    s.player.pos = {-1234.5678f, 9876.54321f, 3.0e-3f};
    s.player.yaw = 2.2f;
    s.player.vel = {1, -2, 3.5f};
    s.player.clip = "ledgeb_right";
    s.player.clipTime = 12.3456789012345;
    s.player.clipRate = -1;
    s.player.loop = true;
    s.player.ledgeSeg = 3, s.player.ledgeT = 17.25f, s.player.ledgeSide = -1;
    s.player.runJump = true;
    s.player.regrabCooldown = 0.125f;
    s.ledgeUnit = "l4_pool", s.ledgeIndex = 42;
    s.cameraYaw = 1.5f, s.cameraPitch = -0.4f;
    s.checkpoint = game::CheckpointRef{"l4_pool", 5502};
    s.collected = {7, 9, 5501};
    s.script = {{"5497", {{"state", 2}, {"fired", true}}}};
    s.ticks = 4321;
    return s;
}

void expectSame(const game::PlayerState& a, const game::PlayerState& b) {
    EXPECT_EQ(a.state, b.state);
    EXPECT_EQ(a.pos, b.pos);
    EXPECT_EQ(a.yaw, b.yaw);
    EXPECT_EQ(a.vel, b.vel);
    EXPECT_EQ(a.clip, b.clip);
    EXPECT_EQ(a.clipTime, b.clipTime);
    EXPECT_EQ(a.clipRate, b.clipRate);
    EXPECT_EQ(a.loop, b.loop);
    EXPECT_EQ(a.ledge, b.ledge);
    EXPECT_EQ(a.ledgeSeg, b.ledgeSeg);
    EXPECT_EQ(a.ledgeT, b.ledgeT);
    EXPECT_EQ(a.ledgeSide, b.ledgeSide);
    EXPECT_EQ(a.runJump, b.runJump);
    EXPECT_EQ(a.regrabCooldown, b.regrabCooldown);
}

fs::path tempDir(const char* name) {
    const fs::path d = fs::temp_directory_path() / ("ou_save_test_" + std::to_string(::getpid())) / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
void spit(const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }

} // namespace

// ---- format ------------------------------------------------------------------------------------

TEST(Save, RoundTripIsExact) {
    auto s = sample();
    s.outfit = "lara_casual";
    const auto r = save::parse(save::serialize(s));
    EXPECT_EQ(r.unit, s.unit);
    expectSame(r.player, s.player);
    EXPECT_EQ(r.ledgeUnit, s.ledgeUnit);
    EXPECT_EQ(r.ledgeIndex, s.ledgeIndex);
    EXPECT_EQ(r.cameraYaw, s.cameraYaw);
    EXPECT_EQ(r.cameraPitch, s.cameraPitch);
    EXPECT_EQ(r.checkpoint, s.checkpoint);
    EXPECT_EQ(r.collected, s.collected);
    EXPECT_EQ(r.script, s.script);
    EXPECT_EQ(r.ticks, s.ticks);
    EXPECT_EQ(r.outfit, "lara_casual");
    EXPECT_EQ(save::serialize(r), save::serialize(s));
}

TEST(Save, RejectsCorruptAndTruncated) {
    const std::string good = save::serialize(sample());
    EXPECT_NO_THROW(save::parse(good));
    EXPECT_THROW(save::parse(""), save::SaveError);
    EXPECT_THROW(save::parse(good.substr(0, good.size() - 1)), save::SaveError);   // truncated body
    EXPECT_THROW(save::parse(good.substr(0, good.find('\n'))), save::SaveError);   // header only
    std::string flipped = good;
    flipped[flipped.size() / 2] ^= 0x04;
    EXPECT_THROW(save::parse(flipped), save::SaveError);                           // checksum
    EXPECT_THROW(save::parse("XXSAVE" + good.substr(6)), save::SaveError);         // magic
    EXPECT_THROW(save::parse(good + "x"), save::SaveError);                        // trailing bytes
    EXPECT_THROW(save::parse(withHeader(99, "{}")), save::SaveError);              // future version
    EXPECT_THROW(save::parse(withHeader(2, "[1, 2]")), save::SaveError);           // not an object
    EXPECT_THROW(save::parse(withHeader(2, "{\"unit\": \"a\"}")), save::SaveError); // no player
    EXPECT_THROW(save::parse(withHeader(2, R"({"unit":"a","player":{"state":"idle","pos":[1,2],"yaw":0}})")), save::SaveError);
    EXPECT_THROW(save::parse(withHeader(2, R"({"unit":"a","player":{"state":"teleport","pos":[1,2,3],"yaw":0}})")), save::SaveError);
    EXPECT_THROW(save::parse(withHeader(2, R"({"unit":"a","player":{"state":"idle","pos":[1,2,3],"yaw":0},"outfit":"..\\x"})")), save::SaveError);
}

TEST(Save, MigratesVersion1) {
    const auto s = save::parse(withHeader(1, R"({"unit":"l4_pool","player":{"state":"run","pos":[1.5,-2,3],"yaw":0.75},"checkpoint":5502})"));
    EXPECT_EQ(s.unit, "l4_pool");
    EXPECT_EQ(s.player.state, game::State::Run);
    EXPECT_EQ(s.player.pos, vec3(1.5f, -2, 3));
    EXPECT_EQ(s.cameraYaw, 0.75f); // v1 had no camera: behind Lara
    ASSERT_TRUE(s.checkpoint);
    EXPECT_EQ(*s.checkpoint, (game::CheckpointRef{"l4_pool", 5502}));
    EXPECT_TRUE(s.collected.empty());
    EXPECT_TRUE(s.script.empty());
    // and it is written back as the current version
    EXPECT_EQ(save::serialize(s).rfind("OUSAVE 2 ", 0), 0u);
}

TEST(Save, SlotNames) {
    EXPECT_EQ(save::slotPath("quick", "/x"), fs::path("/x/quick.ousave"));
    EXPECT_THROW(save::slotPath("../etc/passwd", "/x"), save::SaveError);
    EXPECT_THROW(save::slotPath("", "/x"), save::SaveError);
    ::setenv("XDG_DATA_HOME", "/tmp/xdg-test", 1);
    EXPECT_EQ(save::saveDir(), fs::path("/tmp/xdg-test/openunderworld/saves"));
}

TEST(Save, AtomicWriteKeepsBackup) {
    const fs::path dir = tempDir("atomic") / "saves"; // created by writeFile
    const fs::path p = save::slotPath("quick", dir);
    auto s1 = sample(), s2 = sample();
    s2.unit = "l4_pool";
    save::writeFile(p, save::serialize(s1));
    EXPECT_FALSE(fs::exists(p.string() + ".bak"));
    save::writeFile(p, save::serialize(s2));
    EXPECT_EQ(save::readFile(p).unit, "l4_pool");
    EXPECT_EQ(slurp(p.string() + ".bak"), save::serialize(s1));
    EXPECT_FALSE(fs::exists(p.string() + ".tmp"));
    EXPECT_FALSE(fs::exists(p.string() + ".bak.tmp"));

    // A crash mid-write leaves a partial .tmp: the slot is untouched and the next write succeeds.
    spit(p.string() + ".tmp", save::serialize(s1).substr(0, 20));
    EXPECT_EQ(save::readFile(p).unit, "l4_pool");
    save::writeFile(p, save::serialize(s1));
    EXPECT_EQ(save::readFile(p).unit, s1.unit);

    // Corrupt slot: readFile falls back to the backup and says so; both corrupt: error.
    std::string bad = slurp(p);
    bad.resize(bad.size() / 2);
    spit(p, bad);
    bool usedBackup = false;
    EXPECT_EQ(save::readFile(p, &usedBackup).unit, "l4_pool");
    EXPECT_TRUE(usedBackup);
    spit(p.string() + ".bak", "garbage");
    EXPECT_THROW(save::readFile(p), save::SaveError);
    fs::remove_all(dir.parent_path());
}

// ---- streaming world (synthetic units) ---------------------------------------------------------

TEST(Stream, CrossesPortalLoadsNeighboursUnloadsOld) {
    loads = 0;
    game::World w(syntheticUnit, "a", syntheticAnims(), game::Spawn{{1500, 0, 0}, 0});
    ASSERT_EQ(w.units().size(), 2u); // a + its neighbour b
    EXPECT_EQ(w.currentUnit().data.name, "a");
    EXPECT_EQ(w.units()[1].offset, vec3(-8000, -6000, -500));
    EXPECT_TRUE(w.collision().groundProbe({3000, 0, 100}, 200)); // b's floor, relocated next to a's
    float minZ = 0;
    std::string seen;
    for (int i = 0; i < 300 && w.currentUnit().data.name != "c"; ++i) {
        w.tick(forward());
        minZ = std::min(minZ, w.player().state().pos.z);
        if (w.currentUnit().data.name == "b" && seen.empty()) {
            seen = "b";
            EXPECT_EQ(w.units().size(), 3u); // b, a, c
        }
    }
    EXPECT_EQ(seen, "b");
    ASSERT_EQ(w.currentUnit().data.name, "c");
    EXPECT_GT(minZ, -1.0f); // continuous floor: never fell
    EXPECT_NE(w.player().state().state, game::State::Fall);
    ASSERT_EQ(w.units().size(), 2u); // a unloaded
    for (const auto& u : w.units()) EXPECT_NE(u.data.name, "a");
    EXPECT_EQ(w.currentUnit().offset, vec3(14000 - 8000, 5000 - 6000, 500 - 500));
    EXPECT_EQ(w.markupIndex("c", 0), int(w.currentUnit().markupBase));
    EXPECT_EQ(w.markupOwner(int(w.currentUnit().markupBase)), (std::pair<std::string, int>{"c", 0}));
    EXPECT_TRUE(w.warnings.empty());
}

TEST(Stream, CheckpointAndRespawn) {
    game::World w(syntheticUnit, "a", syntheticAnims(), game::Spawn{{1500, 0, 0}, 0});
    EXPECT_FALSE(w.checkpoint());
    for (int i = 0; i < 200 && !w.checkpoint(); ++i) w.tick(forward()); // b's checkpoint is at world x 3500
    ASSERT_TRUE(w.checkpoint());
    EXPECT_EQ(*w.checkpoint(), (game::CheckpointRef{"b", 77}));
    for (int i = 0; i < 30; ++i) w.tick(forward());
    w.respawn();
    EXPECT_EQ(w.currentUnit().data.name, "b");
    EXPECT_EQ(w.currentUnit().offset, vec3(0));
    EXPECT_EQ(w.player().state().pos, vec3(11500, 6000, 500));
    EXPECT_EQ(w.player().state().yaw, 0.5f);
    w.setCheckpoint(std::nullopt);
    w.respawn(); // no checkpoint: start unit + start spawn
    EXPECT_EQ(w.currentUnit().data.name, "a");
    EXPECT_EQ(w.player().state().pos, vec3(1500, 0, 0));
}

TEST(Stream, MissingNeighbourIsAWarning) {
    game::World w([](const std::string& n) { return syntheticUnit(n == "b" ? "missing" : n); }, "a", syntheticAnims());
    EXPECT_EQ(w.units().size(), 1u);
    ASSERT_EQ(w.warnings.size(), 1u);
    EXPECT_NE(w.warnings[0].find("missing"), std::string::npos);
    EXPECT_THROW(game::World(syntheticUnit, "nope", syntheticAnims()), cdc::FormatError);
}

TEST(Stream, SaveMoveLoadLandsOnSameState) {
    game::World w(syntheticUnit, "a", syntheticAnims(), game::Spawn{{1500, 0, 0}, 0});
    for (int i = 0; i < 70; ++i) w.tick(forward()); // into b, mid-run
    ASSERT_EQ(w.currentUnit().data.name, "b");
    w.collected = {3, 4};
    w.setCheckpoint(game::CheckpointRef{"a", 1});
    const auto saved = w.player().state();
    const auto savedOffset = w.currentUnit().offset;
    const auto first = save::capture(w);
    const auto bytes = save::serialize(first);
    for (int i = 0; i < 60; ++i) w.tick(forward());
    w.setCheckpoint(std::nullopt);
    w.collected.clear();
    save::apply(save::parse(bytes), w);
    EXPECT_EQ(w.currentUnit().data.name, "b");
    game::PlayerState expect = saved;
    expect.pos -= savedOffset; // b is now the base unit (offset 0)
    expectSame(w.player().state(), expect);
    EXPECT_EQ(w.collected, (std::set<int32_t>{3, 4}));
    EXPECT_EQ(w.checkpoint(), (game::CheckpointRef{"a", 1}));
    auto again = save::capture(w);
    again.ticks = first.ticks;
    EXPECT_EQ(save::serialize(again), bytes); // the same document
    w.tick(forward());
    EXPECT_GT(w.player().state().pos.x, expect.pos.x); // and it keeps running
}

// Health survives save -> load (it used to load at full); saves without it load at full; bad values are rejected.
TEST(Save, HealthRoundTrip) {
    game::World w(syntheticUnit, "a", syntheticAnims(), game::Spawn{{1500, 0, 0}, 0});
    w.damageLara(337.25f, vec3(0), game::kDamageAbsolute);
    w.tick({});
    const float hp = w.health().value();
    ASSERT_LT(hp, 1000.0f);
    const auto bytes = save::serialize(save::capture(w));
    EXPECT_EQ(*save::parse(bytes).health, hp);
    w.damageLara(100, vec3(0), game::kDamageAbsolute);
    save::apply(save::parse(bytes), w);
    EXPECT_EQ(w.health().value(), hp);
    auto old = save::parse(bytes);
    old.health.reset();
    save::apply(save::parse(save::serialize(old)), w);
    EXPECT_EQ(w.health().value(), w.health().max());
    EXPECT_THROW(save::parse(withHeader(2, R"({"unit":"a","player":{"state":"idle","pos":[0,0,0],"yaw":0},"health":-5})")),
                 save::SaveError);
}

// ---- real data (OU_GAME_DIR) -------------------------------------------------------------------
namespace {
const char* gameDir() { return std::getenv("OU_GAME_DIR"); }
} // namespace

TEST(StreamReal, PoolPortalPairs) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto pool = game::loadUnitData(bf, "l4_pool"), rag = game::loadUnitData(bf, "l4_ragnarok_room");
    ASSERT_EQ(pool.portals.size(), 1u);
    EXPECT_EQ(pool.portals[0].toUnit, "l4_ragnarok_room");
    ASSERT_EQ(rag.portals.size(), 2u);
    EXPECT_EQ(rag.portals[1].toUnit, "l4_pool");
    EXPECT_EQ(rag.portals[1].toSignal, pool.portals[0].signal);
    EXPECT_EQ(pool.portals[0].toSignal, rag.portals[1].signal);
    EXPECT_EQ(pool.checkpoints.size(), 12u);
    ASSERT_TRUE(pool.spawn);
    EXPECT_TRUE(pool.spawn->fromIntro);
    // Portal pair (Level+0x80 applied): pool's doorway floor is where its player intro stands.
    EXPECT_NEAR(pool.portals[0].min[2], pool.spawn->pos.z, 1);
    EXPECT_NEAR(pool.portals[0].max[1], -pool.portals[0].min[1], 1);
}

// Lara runs back out of l4_pool through its doorway into l4_ragnarok_room on continuous floor; then a
// save in the new unit, more running, and a load land on the saved state.
TEST(StreamReal, PoolToRagnarokOnFootThenSaveLoad) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w([&](const std::string& n) { return game::loadUnitData(bf, n); }, "l4_pool", game::loadLaraAnims(bf));
    ASSERT_EQ(w.units().size(), 2u);
    EXPECT_TRUE(w.warnings.empty());
    const float startZ = w.player().state().pos.z;
    game::Actions back;
    back.move.y = -1; // away from the camera: Lara turns and runs west
    int crossed = -1;
    for (int i = 0; i < 300 && crossed < 0; ++i) {
        w.tick(back);
        const auto& s = w.player().state();
        ASSERT_NE(s.state, game::State::Fall) << "tick " << i;
        ASSERT_NEAR(s.pos.z, startZ, 80) << "tick " << i;
        if (w.currentUnit().data.name == "l4_ragnarok_room") crossed = i;
    }
    ASSERT_GE(crossed, 0);
    std::vector<std::string> names;
    for (const auto& u : w.units()) names.push_back(u.data.name);
    EXPECT_EQ(names, (std::vector<std::string>{"l4_pool", "l4_ragnarok_room", "l4_staircase_mictlan"}));
    for (int i = 0; i < 30; ++i) w.tick(back);
    EXPECT_EQ(w.currentUnit().data.name, "l4_ragnarok_room");
    ASSERT_TRUE(w.collision().groundProbe(w.player().state().pos + vec3(0, 0, 50), 200)); // ragnarok floor under her

    const auto saved = w.player().state();
    const vec3 offset = w.currentUnit().offset;
    const auto bytes = save::serialize(save::capture(w));
    std::vector<game::PlayerState> after; // the 40 ticks following the save, in ragnarok's frame
    for (int i = 0; i < 40; ++i) {
        w.tick(back);
        after.push_back(w.player().state());
        after.back().pos -= offset;
    }
    save::apply(save::parse(bytes), w);
    EXPECT_EQ(w.currentUnit().data.name, "l4_ragnarok_room");
    EXPECT_EQ(w.currentUnit().offset, vec3(0));
    game::PlayerState expect = saved;
    expect.pos -= offset;
    expectSame(w.player().state(), expect);
    // Same inputs after the load replay the same 40 ticks (up to float rounding of the other frame).
    for (int i = 0; i < 40; ++i) {
        w.tick(back);
        const auto& s = w.player().state();
        ASSERT_EQ(s.state, after[size_t(i)].state) << "tick " << i;
        ASSERT_EQ(s.clip, after[size_t(i)].clip) << "tick " << i;
        ASSERT_LT(glm::length(s.pos - after[size_t(i)].pos), 0.5f) << "tick " << i;
    }
}

// Level loads dress Lara in the unit's outfit object (GAMEPLAY.md "Outfits"); streaming keeps it; a load restores the
// saved one (tru.exe 0x5DE2B0).
// Thor's gear per chapter start (GAMEPLAY.md "Thor's gear", checked in the original under Wine): none before Jan
// Mayen, both gauntlets and the belt from it on; the gear draw groups 10-12 stay hidden in every chapter (the
// original's mask +0x23C keeps 0xE00 while she carries them); saves keep both.
TEST(StreamReal, ChapterThorGear) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w([&](const std::string& n) { return game::loadUnitData(bf, n); }, "l3_main_bedroom", game::loadLaraAnims(bf));
    w.script().setBigfile(&bf);
    const std::set<std::string> none, gear{"g_belt", "g_gauntletleft", "g_gauntletright"};
    for (int c = 0; c < int(game::kChapters.size()); ++c) {
        w.startChapter(c);
        EXPECT_EQ(w.script().playerObjects(), c >= 5 ? gear : none) << c;
        EXPECT_EQ(w.script().playerHiddenGroups(), 0x7u << 9) << c; // carried gear is still put away
    }
    w.startChapter(6);
    auto saved = save::capture(w);
    w.startChapter(2);
    save::apply(saved, w);
    EXPECT_EQ(w.script().playerObjects(), gear);
    saved.script.erase("player"); // a chapter-start slot written before gear state was saved
    saved.chapter = 1;
    save::apply(saved, w);
    EXPECT_TRUE(w.script().playerObjects().empty());
}

TEST(StreamReal, ChapterOutfits) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w([&](const std::string& n) { return game::loadUnitData(bf, n); }, "l3_main_bedroom", game::loadLaraAnims(bf));
    EXPECT_EQ(w.outfit, "lara_casual");
    const char* expect[] = {"lara_casual", "lara_wetsuit_warm", "lara_jungle_a", "lara_casual", "lara_jungle_heavy",
                            "lara_snow_heavy", "lara_jungle_b", "lara_wetsuit_cold", "lara_wetsuit_cold"};
    for (int c = 0; c < int(game::kChapters.size()); ++c) {
        w.startChapter(c);
        EXPECT_EQ(w.outfit, expect[c]) << c;
    }
    w.startChapter(4);
    const auto saved = save::capture(w);
    EXPECT_EQ(saved.outfit, "lara_jungle_heavy");
    auto other = saved;
    other.outfit = "lara_bikini"; // a saved outfit wins over the unit's
    save::apply(other, w);
    EXPECT_EQ(w.outfit, "lara_bikini");
    // The outfit's skeleton re-rigs the pose (same bone count, its own pivots).
    const auto drm = cdc::parseDrm(*bf.read("pc-w\\lara_wetsuit_warm.drm"));
    w.setOutfitRig(drm);
    EXPECT_EQ(w.anims().skeleton.bones.size(), 153u);
    EXPECT_EQ(w.pose(1).size(), 153u);
}
