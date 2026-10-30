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

// Weapons (secondary, clips, reserves) and the sonar reveal points survive save -> load; saves without combat state
// load the chapter's loadout (every secondary full, the chapter's equipped); PDA EQUIP keeps the rounds.
TEST(Save, CombatAndRevealRoundTrip) {
    game::World w(syntheticUnit, "a", syntheticAnims(), game::Spawn{{1500, 0, 0}, 0});
    w.chapterLoadout(4);
    const auto rifle = size_t(game::Weapon::AssaultRifle), tranq = size_t(game::Weapon::Tranquilizer);
    EXPECT_EQ(w.combat().secondary, game::Weapon::Tranquilizer);
    EXPECT_EQ(w.combat().reserve[rifle], w.maxReserve(game::Weapon::AssaultRifle));
    w.giveWeapon(game::Weapon::AssaultRifle, 17); // 17 spare, full clip
    w.equipWeapon(game::Weapon::Tranquilizer);
    EXPECT_EQ(w.combat().secondary, game::Weapon::Tranquilizer);
    EXPECT_EQ(w.combat().reserve[rifle], 17); // equip does not refill
    EXPECT_EQ(w.combat().reserve[tranq], w.maxReserve(game::Weapon::Tranquilizer));
    for (int i = 0; i < 70; ++i) w.tick(forward()); // into b: a reveal point in each unit
    ASSERT_FALSE(w.mapReveal["a"].empty());
    ASSERT_FALSE(w.mapReveal["b"].empty());
    const auto reveal = w.mapReveal;
    const auto bytes = save::serialize(save::capture(w));
    w.chapterLoadout(6), w.mapReveal.clear();
    save::apply(save::parse(bytes), w);
    EXPECT_EQ(w.combat().secondary, game::Weapon::Tranquilizer);
    EXPECT_EQ(w.combat().reserve[rifle], 17);
    EXPECT_EQ(w.combat().clip[rifle][0], 30);
    EXPECT_EQ(w.mapReveal.size(), reveal.size());
    EXPECT_EQ(w.mapReveal.at("b").size(), reveal.at("b").size());
    auto old = save::parse(bytes);
    old.combat = nullptr, old.chapter = 6;
    save::apply(save::parse(save::serialize(old)), w);
    EXPECT_EQ(w.combat().secondary, game::Weapon::AssaultRifle);
    EXPECT_EQ(w.combat().reserve[rifle], w.maxReserve(game::Weapon::AssaultRifle));
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
// Mayen, both gauntlets and the belt from it on, Mjolnir from the Andaman Sea on (GAMEPLAY.md "Mjolnir"); the gear draw
// groups 10-12 stay hidden in every chapter (the original's mask +0x23C keeps 0xE00 while she carries them); saves
// keep both.
TEST(StreamReal, ChapterThorGear) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w([&](const std::string& n) { return game::loadUnitData(bf, n); }, "l3_main_bedroom", game::loadLaraAnims(bf));
    w.script().setBigfile(&bf);
    const std::set<std::string> none, gear{"g_belt", "g_gauntletleft", "g_gauntletright"},
        hammer{"g_belt", "g_gauntletleft", "g_gauntletright", "wp_thorhammer"};
    for (int c = 0; c < int(game::kChapters.size()); ++c) {
        w.startChapter(c);
        EXPECT_EQ(w.script().playerObjects(), c >= 6 ? hammer : c == 5 ? gear : none) << c;
        EXPECT_EQ(w.hasHammer(), c >= 6) << c;
        EXPECT_EQ(w.script().playerHiddenGroups(), 0x7u << 9) << c; // carried gear is still put away
    }
    w.startChapter(6);
    auto saved = save::capture(w);
    w.startChapter(2);
    save::apply(saved, w);
    EXPECT_EQ(w.script().playerObjects(), hammer);
    saved.script.erase("player"); // a chapter-start slot written before gear state was saved
    saved.chapter = 1;
    save::apply(saved, w);
    EXPECT_TRUE(w.script().playerObjects().empty());
}

// Every chapter start: after 5 s standing still no enemy is out of its medium. Sharks (aquatic) sense Lara only
// while she swims, so l2_cliffclimb's three placed sharks stay under the start boat (they used to chase her onto the
// deck); none is hunting at any start and every one stays below a Lara who is not in the water.
TEST(StreamReal, ChapterStartEnemiesStayInTheirMedium) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w([&](const std::string& n) { return game::loadUnitData(bf, n); }, "l3_main_bedroom", game::loadLaraAnims(bf));
    w.script().setBigfile(&bf);
    w.enemies().setBigfile(&bf);
    int sharks = 0;
    for (int c = 0; c < int(game::kChapters.size()); ++c) {
        w.startChapter(c);
        for (int i = 0; i < 300; ++i) w.tick({});
        const auto& lara = w.player().state();
        for (const auto& e : w.enemies().all()) {
            if (!e->type().aquatic()) continue;
            if (c == 2) ++sharks;
            const auto mode = e->state().mode;
            EXPECT_TRUE(mode != game::ai::Mode::Chase && mode != game::ai::Mode::Attack) << c << " " << e->uid();
            if (lara.water < 0) EXPECT_LT(e->state().pos.z + e->type().tuning.height, lara.pos.z) << c << " " << e->uid();
        }
    }
    EXPECT_GE(sharks, 3); // l2_cliffclimb 4021-4023 are loaded at the start
}

// Open sea: the watervolume intros' own boxes (intro data +0x54 half extents, World.cpp findWater). Lara starts on
// l2_cliffclimb's yacht deck (below the sea top at z 108, the deck collider exists before her first tick) and swims
// once she is put into the sea beside it.
TEST(StreamReal, OpenSeaWater) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto cliff = game::loadUnitData(bf, "l2_cliffclimb");
    int boxes = 0;
    for (const auto& m : cliff.col.markup)
        if (m.type == 10 && m.id == 2563) { ++boxes; EXPECT_NEAR(m.points[1].z, 108.0f, 1.0f); }
    EXPECT_EQ(boxes, 1);
    size_t arctic = 0;
    for (const auto& m : game::loadUnitData(bf, "l7_arctic_sea").col.markup) arctic += m.type == 10;
    EXPECT_EQ(arctic, 11u);
    game::World w([&](const std::string& n) { return game::loadUnitData(bf, n); }, "l3_main_bedroom", game::loadLaraAnims(bf));
    w.script().setBigfile(&bf);
    w.enemies().setBigfile(&bf);
    w.startChapter(2);
    for (int i = 0; i < 60; ++i) w.tick({});
    EXPECT_LT(w.player().state().water, 0) << "on the deck";
    EXPECT_GT(w.player().state().pos.z, 40.0f);
    auto s = w.player().state();
    s.pos = glm::vec3(-31000, -10000, -500); // open sea, 600 below the surface
    w.restore(s, w.camera().yaw, w.camera().pitch);
    for (int i = 0; i < 120; ++i) w.tick({});
    EXPECT_GE(w.player().state().water, 0);
    EXPECT_GT(w.player().state().pos.z, -3000.0f) << "does not sink through the sea";
}

// The original's chapter starts (GAMEPLAY.md "Chapter starts"): startChapter Sends messagechannel 2 id = chapter, the
// start unit's script enters state 1 and activates its intro cinematic; after the stream (skipped here) World places
// Lara / the bikes at the header tails and sends cinematic event 1, whose handler mounts her (larabase 52).
TEST(StreamReal, ChapterStartIntros) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w([&](const std::string& n) { return game::loadUnitData(bf, n); }, "l3_main_bedroom", game::loadLaraAnims(bf));
    w.script().setBigfile(&bf);
    w.enemies().setBigfile(&bf);
    std::vector<std::string> played;
    w.onCinematic = [&](const std::string& n, bool) { played.push_back(n); };
    const auto start = [&](int c, int ticks) {
        played.clear();
        w.startChapter(c);
        for (int i = 0; i < ticks; ++i) w.tick({});
        for (const auto& p : played) std::fprintf(stderr, "ch%d played %s\n", c, p.c_str());
        const auto& s = w.player().state();
        std::fprintf(stderr, "ch%d: unit %s pos %.1f %.1f %.1f state %s clip %s water %d played %zu\n", c, w.currentUnit().data.name.c_str(),
                     s.pos.x, s.pos.y, s.pos.z, std::string(game::stateName(s.state)).c_str(), s.clip.c_str(), s.water, played.size());
    };
    start(4, 60); // l4_trailstart: 180_mex_intro, then riding bike 5482
    EXPECT_EQ(played, std::vector<std::string>{"180_mex_intro"});
    ASSERT_TRUE(w.ridden());
    EXPECT_EQ(w.ridden()->uid, 5482);
    EXPECT_EQ(w.player().state().state, game::State::Vehicle);
    start(5, 90); // l5_startsnow: 230_val_intro, 0.6 s later riding l5_valgrind_tunnel's 7229
    EXPECT_EQ(played, std::vector<std::string>{"230_val_intro"});
    ASSERT_TRUE(w.ridden());
    EXPECT_EQ(w.ridden()->uid, 7229);
    EXPECT_EQ(w.currentUnit().data.name, "l5_valgrind_tunnel");
    start(7, 120); // l7_thin_ice: 275_hel_explosion leaves her swimming under the ice in l7_arctic_sea
    EXPECT_EQ(played, std::vector<std::string>{"275_hel_explosion"});
    EXPECT_FALSE(w.ridden());
    EXPECT_EQ(w.currentUnit().data.name, "l7_arctic_sea");
    EXPECT_GE(w.player().state().water, 0);
    EXPECT_EQ(w.player().state().state, game::State::SwimUnder);
    start(2, 60); // l2_cliffclimb: 080_tha_yacht leaves her standing on the yacht
    EXPECT_EQ(played, std::vector<std::string>{"080_tha_yacht"});
    EXPECT_EQ(w.player().state().water, -1);
    EXPECT_EQ(w.player().state().state, game::State::Idle);
    start(3, 60); // l3_caverns_start: 120_man_vault's tail is over no loaded floor; she stays on the cavern stairs
    EXPECT_EQ(played, std::vector<std::string>{"120_man_vault"});
    EXPECT_LT(glm::distance(w.player().state().pos, w.currentUnit().data.spawn->pos), 200.0f);
    // l6_ship_exterior: 260_ship_intro ends in the boat, cinematic object 7652 (+140) warps her onto the deck, standing;
    // its end handler requests the autosave (uisavemenu 5) and fades in.
    const uint32_t saves = w.autosaveRequests();
    start(6, 60);
    EXPECT_EQ(played, std::vector<std::string>{"260_ship_intro"});
    EXPECT_LT(glm::distance(w.player().state().pos, vec3(5006.0f, 21948.7f, 5386.2f)), 50.0f);
    EXPECT_EQ(w.player().state().water, -1);
    EXPECT_EQ(w.player().state().clip, "ng_idle");
    EXPECT_EQ(w.autosaveRequests(), saves + 1);
    EXPECT_EQ(w.script().screenFade(), 0.0f);
    // l1_seatop: 030_med_yacht ends in the sea; 997 fn 5 then puts her on ms_uo_yacht's stern (uberobjectbase 2 anchor
    // at HInfo marker 13 -> instance 26), standing on the deck.
    start(1, 30);
    EXPECT_EQ(played, std::vector<std::string>{"030_med_yacht"});
    EXPECT_LT(glm::distance(w.player().state().pos, vec3(-18829.5f, -3621.6f, 2025.6f)), 50.0f);
    EXPECT_EQ(w.player().state().water, -1);
    EXPECT_EQ(w.player().state().state, game::State::Idle);
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
        // On dry land the wetsuits hide the face mask and the stowed gear and keep the harness (original: 0xE03 | 2).
        if (w.outfit.starts_with("lara_wetsuit") && w.player().state().water < 0) EXPECT_EQ(w.laraHiddenGroups() & 7u, 3u) << c;
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

// Uber section state machines (the l3_chapel shadow pillars 4837) save and load mid-puzzle: the same states, no
// default-state events re-run, timers running on alike after the load.
TEST(StreamReal, UberStateSaveLoad) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    const game::UnitLoader load([&](const std::string& n) { return game::loadUnitData(bf, n); });
    game::World w(load, "l3_chapel", anims);
    w.script().setBigfile(&bf);
    w.onCinematic = [](const std::string&, bool) {};
    for (int i = 0; i < 300; ++i) w.tick({});
    const auto* sw = w.script().find(4819);
    ASSERT_TRUE(sw);
    game::PlayerState s;
    s.pos = sw->pos;
    w.restore(s, 0, -0.25f);
    game::Actions use;
    use.interact = true;
    for (int i = 0; i < 10; ++i) w.tick(use);
    for (int i = 0; i < 300; ++i) w.tick({});
    ASSERT_TRUE(w.uberState(4837));
    const auto moved = *w.uberState(4837);
    EXPECT_NE(moved, (std::vector<uint32_t>{0, 2, 1, 0, 0, 1})); // section 1 turned
    const auto bytes = save::serialize(save::capture(w));
    for (int i = 0; i < 20; ++i) w.tick({});
    const auto expect = w.serializeObjects();

    game::World fresh(load, "l3_chapel", anims);
    fresh.script().setBigfile(&bf);
    fresh.onCinematic = [](const std::string&, bool) {};
    save::apply(save::parse(bytes), fresh);
    for (int i = 0; i < 20; ++i) fresh.tick({});
    ASSERT_TRUE(fresh.uberState(4837));
    EXPECT_EQ(*fresh.uberState(4837), moved);
    EXPECT_EQ(fresh.serializeObjects(), expect); // timers, once-events and travel ran on alike
}

// Uber joints save and load mid-motion: l4_staircase_upper ramp 6045's section 0 falls on its prismatic joint (command
// 1 switches the motor off); a save during the fall restores the joint's position, velocity, motor and range watches, so
// trigger 14 at the bottom fires alike after the load.
TEST(StreamReal, UberJointSaveLoad) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    const game::UnitLoader load([&](const std::string& n) { return game::loadUnitData(bf, n); });
    game::World w(load, "l4_staircase_upper", anims);
    w.script().setBigfile(&bf);
    w.onCinematic = [](const std::string&, bool) {};
    for (int i = 0; i < 60; ++i) w.tick({});
    ASSERT_TRUE(w.sendUberCommand(6045, 1));
    for (int i = 0; i < 20; ++i) w.tick({});
    const auto* falling = w.uberJoint(6045, 0);
    ASSERT_TRUE(falling);
    EXPECT_LT(falling->x, -100.0f);
    EXPECT_GT(falling->x, -900.0f) << "mid-fall";
    const auto bytes = save::serialize(save::capture(w));
    for (int i = 0; i < 60; ++i) w.tick({});
    EXPECT_EQ((*w.uberState(6045))[0], 2u) << "down: trigger 14 at the bottom";
    const auto expect = w.serializeObjects();

    game::World fresh(load, "l4_staircase_upper", anims);
    fresh.script().setBigfile(&bf);
    fresh.onCinematic = [](const std::string&, bool) {};
    save::apply(save::parse(bytes), fresh);
    for (int i = 0; i < 60; ++i) fresh.tick({});
    EXPECT_EQ(fresh.serializeObjects(), expect);
}
