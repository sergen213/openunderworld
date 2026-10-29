// Progression: chapter table, level ends, pickups + save round trip (src/game/Progress), menus (src/ui).
#include "game/Game.hpp"
#include "game/Progress.hpp"
#include "save/Save.hpp"
#include "ui/Art.hpp"
#include "ui/Ui.hpp"

#include "cdc/Cdc.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>

#include <zlib.h>

using namespace ou;
using game::vec3;

namespace {

const char* gameDir() { return std::getenv("OU_GAME_DIR"); }

game::AnimLibrary idleAnims() {
    game::AnimLibrary a;
    a.skeleton.bones.resize(1);
    a.clips["ng_idle"].keyCount = 50;
    a.clips["ng_idle"].timePerKey = 100;
    a.clips["ng_idle"].tracks.resize(1);
    return a;
}

// "l3_main_bedroom": flat floor, Lara at the origin, a treasure 100 units ahead and a relic far away; a level-end
// cinematic (intro 900) naming l1_seatop + l3_caverns_start. Every other name: a plain floor.
game::UnitData unit(const std::string& name) {
    game::UnitData u;
    u.name = name;
    u.col.vertices = {{-5000, -5000, 0}, {5000, -5000, 0}, {5000, 5000, 0}, {-5000, 5000, 0}};
    u.col.faces = {{{0, 1, 2}}, {{0, 2, 3}}};
    u.spawn = game::Spawn{{0, 0, 0}, 0, true};
    if (name == "l3_main_bedroom") {
        u.pickups = {{501, game::PickupKind::Treasure, {100, 0, 0}}, {502, game::PickupKind::Relic, {3000, 0, 0}}};
        u.chapterLinks[900] = {"l1_seatop", "l3_caverns_start"};
    }
    return u;
}

game::Actions interact(bool on) {
    game::Actions a;
    a.interact = on;
    return a;
}

vec3 introPos(const game::UnitData& u, int32_t uid) {
    for (const auto& in : formats::parseUnit(*u.drm).intros)
        if (in.uniqueId == uid) return {in.position[0], in.position[1], in.position[2]};
    throw std::runtime_error("intro not found");
}

} // namespace

// ---- chapter sequence ---------------------------------------------------------------------------------

TEST(Chapters, SequenceTable) {
    ASSERT_EQ(game::kChapters.size(), 9u);
    EXPECT_EQ(game::kChapters[0].unit, "l3_main_bedroom"); // New Game (generalbank DTP "chapter0")
    EXPECT_EQ(game::kChapters[1].unit, "l1_seatop");
    EXPECT_EQ(game::kChapters[7].unit, "l7_thin_ice");
    for (int i = 1; i <= 7; ++i) EXPECT_EQ(game::kChapters[size_t(i)].nameId, i == 3 ? 54 : 51 + i); // 52..58
    EXPECT_EQ(game::chapterIndex("l4_trailstart"), 4);
    EXPECT_EQ(game::chapterIndex("l4_pool"), -1);
    EXPECT_EQ(game::chapterOfUnit("l4_pool"), 4);
    EXPECT_EQ(game::chapterOfUnit("l7_himalaya_end"), 8);
    EXPECT_EQ(game::chapterOfUnit("container1"), -1);
    // l3_main_hall's level end names chapter 1 and 3: only the prologue leaves through it.
    const std::vector<std::string> hall = {"l1_seatop", "l3_caverns_start"};
    EXPECT_EQ(game::nextChapter(0, hall), 1);
    EXPECT_EQ(game::nextChapter(3, hall), std::nullopt);
    EXPECT_EQ(game::nextChapter(1, {"l2_cliffclimb"}), 2);
}

TEST(Chapters, LevelEndStartsNextChapter) {
    game::World w(unit, "l3_main_bedroom", idleAnims());
    script::Object v;
    v.uid = 77, v.kind = script::Kind::Volume, v.unit = "l3_main_bedroom";
    v.shape.half = vec3(500);
    v.links[script::kVtOnFirstIn] = {900};
    w.script().addObject(v);
    w.tick({});
    EXPECT_EQ(w.chapter, 1);
    EXPECT_EQ(w.currentUnit().data.name, "l1_seatop");
    EXPECT_TRUE(w.warnings.empty());
}

// ---- pickups ------------------------------------------------------------------------------------------

TEST(Pickups, KindsFromObjectNames) {
    EXPECT_EQ(game::pickupKind("ca_rw_artifact"), game::PickupKind::Treasure);
    EXPECT_EQ(game::pickupKind("med_rw_treasure_a"), game::PickupKind::Relic);
    EXPECT_EQ(game::pickupKind("sa_rw_shard"), game::PickupKind::HealthPack);
    EXPECT_EQ(game::pickupKind("g_healthpack"), std::nullopt);
    EXPECT_EQ(game::pickupStringId(game::PickupKind::Treasure), 1251);
}

TEST(Pickups, InteractCollectsNearestOnceAndSaves) {
    game::World w(unit, "l3_main_bedroom", idleAnims());
    w.tick(interact(true)); // held since the start: no press edge
    EXPECT_TRUE(w.collected.empty());
    w.tick(interact(false));
    w.tick(interact(true));
    ASSERT_EQ(w.pickedUp().size(), 1u);
    EXPECT_EQ(w.pickedUp()[0].id, 501);
    EXPECT_EQ(w.collected, std::set<int32_t>{501}); // the relic is out of reach
    w.tick(interact(false));
    w.tick(interact(true));
    EXPECT_TRUE(w.pickedUp().empty()); // already collected

    w.chapter = 2;
    const auto bytes = save::serialize(save::capture(w));
    game::World w2(unit, "l1_seatop", idleAnims());
    save::apply(save::parse(bytes), w2);
    EXPECT_EQ(w2.collected, std::set<int32_t>{501});
    EXPECT_EQ(w2.chapter, 2);
    EXPECT_EQ(w2.currentUnit().data.name, "l3_main_bedroom");
    EXPECT_EQ(w2.found, (std::array<int, 3>{1, 0, 0}));
}

TEST(Pickups, OldSavesDeriveTheChapter) {
    save::SaveGame s;
    s.unit = "l4_pool";
    auto j = save::serialize(s);
    const auto body = j.substr(j.find('\n') + 1);
    // Strip the "chapter" key as a pre-chapter save would lack it, re-header it.
    auto json = nlohmann::json::parse(body);
    json.erase("chapter");
    const std::string b = json.dump();
    char h[64];
    std::snprintf(h, sizeof h, "OUSAVE 2 %08lx %zu\n", crc32(0, reinterpret_cast<const Bytef*>(b.data()), uInt(b.size())), b.size());
    EXPECT_EQ(save::parse(h + b).chapter, 4);
}

// ---- menus --------------------------------------------------------------------------------------------

TEST(Menus, MainMenuNavigation) {
    ui::Menus m;
    m.open(ui::Screen::Main);
    auto items = m.items();
    ASSERT_EQ(items.size(), 8u);
    EXPECT_EQ(items[0].stringId, 123); // PLAY GAME, the centre
    EXPECT_EQ(items[1].stringId, 124); // START NEW GAME, top of the ring
    EXPECT_EQ(items[7].stringId, 131); // QUIT GAME, bottom
    EXPECT_EQ(m.input(ui::Nav::Up), ui::Action::None);
    EXPECT_EQ(m.cursor(), 0); // Up/Down do nothing in the centre
    m.input(ui::Nav::Left);
    EXPECT_EQ(m.cursor(), 4); // first Left: the ring item level with the centre, SAVE GAME (as tru.exe)
    m.input(ui::Nav::Down), m.input(ui::Nav::Down), m.input(ui::Nav::Down);
    EXPECT_EQ(m.cursor(), 7);
    m.input(ui::Nav::Down);
    EXPECT_EQ(m.cursor(), 0); // off the bottom of the ring: back to the centre
    m.input(ui::Nav::Left);
    EXPECT_EQ(m.cursor(), 7); // Left returns to the last ring item
    m.input(ui::Nav::Right);
    EXPECT_EQ(m.cursor(), 0);
    m.input(ui::Nav::Left), m.input(ui::Nav::Up), m.input(ui::Nav::Up), m.input(ui::Nav::Up), m.input(ui::Nav::Up),
        m.input(ui::Nav::Up), m.input(ui::Nav::Up);
    EXPECT_EQ(m.cursor(), 1); // START NEW GAME
    m.input(ui::Nav::Up);
    EXPECT_EQ(m.cursor(), 0); // off the top: the centre
    m.input(ui::Nav::Left);
    EXPECT_EQ(m.cursor(), 1); // the last ring item
}

TEST(Menus, MainMenuActionsAndScreens) {
    ui::Menus m;
    m.open(ui::Screen::Main);
    m.input(ui::Nav::Left), m.input(ui::Nav::Up), m.input(ui::Nav::Up); // SAVE GAME -> LOAD GAME -> GAME OPTIONS
    EXPECT_EQ(m.items()[size_t(m.cursor())].stringId, 125);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None);
    ASSERT_EQ(m.screen(), ui::Screen::GameOptions);
    m.input(ui::Nav::Right); // COMBAT RETICLES: SHOWN -> HIDDEN
    EXPECT_FALSE(m.reticles);
    m.input(ui::Nav::Up); // wraps to BACK TO PREVIOUS MENU
    EXPECT_EQ(m.items()[size_t(m.cursor())].stringId, 469);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None);
    EXPECT_EQ(m.screen(), ui::Screen::Main);
    EXPECT_EQ(m.cursor(), 2); // back on GAME OPTIONS
    m.input(ui::Nav::Down), m.input(ui::Nav::Down), m.input(ui::Nav::Down); // AUDIO/VISUAL
    m.input(ui::Nav::Accept);
    ASSERT_EQ(m.screen(), ui::Screen::AudioVisual);
    m.input(ui::Nav::Down), m.input(ui::Nav::Accept); // SUBTITLES ON -> OFF
    EXPECT_FALSE(m.subtitles);
    m.input(ui::Nav::Down), m.input(ui::Nav::Accept); // VOLUME SETTINGS
    ASSERT_EQ(m.screen(), ui::Screen::Volume);
    m.input(ui::Nav::Down); // music volume
    m.input(ui::Nav::Right), m.input(ui::Nav::Right), m.input(ui::Nav::Right);
    EXPECT_EQ(m.musicVolume, 10); // clamped
    m.input(ui::Nav::Left);
    EXPECT_EQ(m.musicVolume, 9);
    m.input(ui::Nav::Back), m.input(ui::Nav::Back);
    EXPECT_EQ(m.screen(), ui::Screen::Main);
    m.input(ui::Nav::Up); // SAVE GAME: a message in the main menu
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None);
    EXPECT_EQ(m.screen(), ui::Screen::Message);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None); // OK
    EXPECT_EQ(m.screen(), ui::Screen::Main);
    EXPECT_EQ(m.input(ui::Nav::Back), ui::Action::None); // Main stays
    EXPECT_EQ(m.screen(), ui::Screen::Main);
    m.input(ui::Nav::Up), m.input(ui::Nav::Up), m.input(ui::Nav::Up); // START NEW GAME: PLAYER TAILORING first
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None);
    ASSERT_EQ(m.screen(), ui::Screen::Tailoring);
    EXPECT_EQ(m.cursor(), 1); // TOMB RAIDER
    m.input(ui::Nav::Down);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::NewGame);
    EXPECT_EQ(m.tailoring, 2); // MASTER SURVIVALIST
    EXPECT_EQ(m.screen(), ui::Screen::None);
    m.open(ui::Screen::Main);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::Continue); // PLAY GAME
    EXPECT_EQ(m.screen(), ui::Screen::None);
}

TEST(Menus, PauseSlotsAndPda) {
    ui::Menus m;
    m.open(ui::Screen::Pause);
    const auto ring = m.items();
    ASSERT_EQ(ring.size(), 8u); // the main menu's ring (no PDA item: key 1 opens it)
    EXPECT_EQ(ring[0].stringId, 122); // RESUME GAME, the centre
    EXPECT_EQ(ring[7].stringId, 131);
    m.saves = {"quick", "save-1"};
    m.input(ui::Nav::Left), m.input(ui::Nav::Up); // SAVE GAME -> LOAD GAME
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None);
    ASSERT_EQ(m.screen(), ui::Screen::Load);
    EXPECT_EQ(m.cursor(), -1); // focus on the ring item until Right
    const auto slotRing = m.items();
    ASSERT_EQ(slotRing.size(), 6u); // page up, four slots, page down
    EXPECT_EQ(slotRing[2].label, "save-1");
    EXPECT_TRUE(slotRing[3].label.empty());
    m.input(ui::Nav::Right), m.input(ui::Nav::Down);
    EXPECT_EQ(m.cursor(), 2);
    EXPECT_EQ(m.input(ui::Nav::Delete), ui::Action::Delete);
    EXPECT_EQ(m.selectedSave(), "save-1");
    m.input(ui::Nav::Down);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None); // empty slot
    m.input(ui::Nav::Up);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::Load);
    EXPECT_EQ(m.selectedSave(), "save-1");
    EXPECT_EQ(m.screen(), ui::Screen::None);
    m.open(ui::Screen::Pause);
    m.input(ui::Nav::Left), m.input(ui::Nav::Accept); // SAVE GAME
    ASSERT_EQ(m.screen(), ui::Screen::Save);
    m.input(ui::Nav::Accept), m.input(ui::Nav::Down), m.input(ui::Nav::Down); // first empty slot
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::Save);
    EXPECT_EQ(m.selectedSave(), ""); // a new save
    EXPECT_EQ(m.screen(), ui::Screen::Save); // stays for the result
    m.input(ui::Nav::Back);
    EXPECT_EQ(m.screen(), ui::Screen::Pause);
    EXPECT_EQ(m.input(ui::Nav::Back), ui::Action::Resume);
    m.open(ui::Screen::Pda);
    EXPECT_EQ(m.cursor(), 1); // AREA INFO
    m.input(ui::Nav::Down), m.input(ui::Nav::Down), m.input(ui::Nav::Down); // FIELD ASSISTANCE
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::Hint);
    EXPECT_EQ(m.input(ui::Nav::Alt), ui::Action::Answer);
    m.input(ui::Nav::Down); // JOURNAL / REVISIT LOCATION are greyed: RESUME GAME
    EXPECT_EQ(m.items()[size_t(m.cursor())].stringId, 122);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::Resume);
    EXPECT_EQ(m.screen(), ui::Screen::None);
    m.open(ui::Screen::Pda);
    EXPECT_EQ(m.input(ui::Nav::Back), ui::Action::Resume);
    EXPECT_EQ(m.screen(), ui::Screen::None);
}

TEST(Menus, TextAndCanvas) {
    EXPECT_EQ(ui::atlasText("\xE1\xA0\x83 RESUME"), " RESUME");       // U+1803 button glyph dropped
    EXPECT_EQ(ui::atlasText("RAGNAR\xC3\x96K"), "RAGNAROK");          // Latin-1 accent
    EXPECT_EQ(ui::atlasText("ONE WEEK EARLIER\xE2\x80\xA6"), "ONE WEEK EARLIER...");
    const auto lines = ui::wrap("aaa bbb ccc\\ndd", 7);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "aaa bbb");
    EXPECT_EQ(lines[2], "dd");
    ui::Canvas c(640, 480);
    c.text(0, 0, 8, "AB", ui::kWhite);
    EXPECT_EQ(c.vertices.size(), 12u);
    EXPECT_EQ(ui::Canvas::textWidth("AB", 8), 16);
}

// ---- real data ----------------------------------------------------------------------------------------

// Every chapter start unit loads with a spawn; each chapter's closing cinematic names the next chapter
// (intro ids from the level-end survey, docs/GAMEPLAY.md "Progression").
TEST(ChaptersReal, StartUnitsAndLevelEnds) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    for (const auto& c : game::kChapters) {
        const auto u = game::loadUnitData(bf, std::string(c.unit));
        EXPECT_TRUE(u.spawn && u.spawn->fromIntro) << c.unit;
    }
    const struct { const char* unit; int32_t intro; int from, to; } ends[] = {
        {"l3_main_hall", 4106, 0, 1},          {"l1_cine_ship_sinking", 1005, 1, 2}, {"l2_cliffclimb", 2565, 2, 3},
        {"l3_mansion_exterior_afterburn", 4070, 3, 4}, {"l4_ballcourt", 5483, 4, 5}, {"l5_valaskjalf", 6846, 5, 6},
        {"l6_ship_natla_room", 1387, 6, 7},
    };
    for (const auto& e : ends) {
        const auto u = game::loadUnitData(bf, e.unit);
        const auto it = u.chapterLinks.find(e.intro);
        ASSERT_NE(it, u.chapterLinks.end()) << e.unit;
        EXPECT_EQ(game::nextChapter(e.from, it->second), e.to) << e.unit;
    }
}

// generalbank.drm menu art: the font table and the hor+ 640x480 mapping reproduce tru.exe's measured layout
// (docs/GAMEPLAY.md "Menus": "PLAY GAME" ink from x 287.0 at 1920x1080, i.e. 66 wide centred on 320).
TEST(MenusReal, ArtAndFontMetrics) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    std::string why;
    const auto art = ui::loadArt(bf, &why);
    ASSERT_TRUE(art) << why;
    EXPECT_EQ(art->font.texture, 109u);
    EXPECT_EQ(art->font.cellHeight, 46);
    const ui::Glyph* a = art->font.find('A');
    ASSERT_TRUE(a);
    EXPECT_EQ(a->width, 29);
    EXPECT_EQ(a->x, 1);
    EXPECT_FALSE(a->second);
    EXPECT_TRUE(art->font.find(0x1800)->second);                 // controller glyphs: second texture
    EXPECT_EQ(art->font.find(0x2603), art->font.find(0x18AF));   // missing -> U+18AF
    for (const uint32_t id : {11u, 12u, 13u, 15u, 17u, 18u, 26u, 50u, 51u, 105u, 106u, 109u, 110u}) EXPECT_TRUE(art->find(id)) << id;
    EXPECT_EQ(art->find(17)->width, 1024);
    ui::Canvas c(1920, 1080, &*art);
    EXPECT_FLOAT_EQ(c.sx(0), 240);
    EXPECT_FLOAT_EQ(c.unit(), 2.25f);
    EXPECT_FLOAT_EQ(c.fontWidth("PLAY GAME", 0.625f), 66.0f);
}

// pc-w\image RAW backgrounds (loading screens, PDA frame) and the g_pda picture windows in the atlas.
TEST(MenusReal, RawImagesAndPdaArt) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto art = ui::loadArt(bf);
    ASSERT_TRUE(art);
    for (uint32_t id = 680; id <= 694; ++id) EXPECT_EQ(art->find(id) ? art->find(id)->width : 0, 512) << id;
    const std::pair<const char*, int> raws[] = {{"pda_bg_1280", 1280}, {"pda_bg_960", 960}, {"loading_bg_mansion", 640}};
    for (const auto& [n, w] : raws) {
        const auto img = ui::loadRaw(bf, n);
        ASSERT_TRUE(img) << n;
        EXPECT_EQ(img->width, w) << n;
        EXPECT_EQ(img->rgba.size(), size_t(img->width) * size_t(img->height) * 4) << n;
    }
    for (int c = 0; c < 9; ++c) EXPECT_TRUE(ui::loadRaw(bf, ui::loadingImage(c))) << c;
    const auto pda = ui::loadRaw(bf, "pda_bg_1280"); // the bezel is blue: B,G,R,A pixels
    const uint8_t* bar = &pda->rgba[(size_t(600) * 1280 + 640) * 4];
    EXPECT_GT(bar[2], bar[0]);
    EXPECT_FALSE(ui::loadRaw(bf, "no_such_image"));
}

TEST(ChaptersReal, MediterraneanEndingRunsThroughTriggerScript) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto unit = game::loadUnitData(bf, "l1_ship_sinking_hall");
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l1_ship_sinking_hall", idleAnims(), game::Spawn{introPos(unit, 1082), 0});
    w.script().setBigfile(&bf);
    std::vector<std::string> played;
    w.onCinematic = [&](const std::string& name, bool) { played.push_back(name); };
    for (int i = 0; i < 4; ++i) w.tick({});
    EXPECT_EQ(played, std::vector<std::string>{"070_med_dive"});
    EXPECT_EQ(w.chapter, 2);
}

TEST(ChaptersReal, CroftManorEndingRunsBothCinematics) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto unit = game::loadUnitData(bf, "l3_main_hall");
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l3_main_hall", idleAnims(), game::Spawn{introPos(unit, 4137), 0});
    w.script().setBigfile(&bf);
    std::vector<std::string> played;
    w.onCinematic = [&](const std::string& name, bool) { played.push_back(name); };
    w.tick({});
    auto state = w.player().state();
    state.pos = {1000000, 1000000, 1000000};
    w.restore(state, 0, 0);
    for (int i = 0; i < 30; ++i) w.tick({});
    EXPECT_EQ(played, (std::vector<std::string>{"160_man_shot", "170_man_exit"}));
    EXPECT_EQ(w.chapter, 4);
}

TEST(ChaptersReal, NatlaShipEndingPassesLoadedGuard) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto unit = game::loadUnitData(bf, "l6_ship_engine_hall");
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l6_ship_engine_hall", idleAnims(), game::Spawn{introPos(unit, 7566), 0});
    w.script().setBigfile(&bf);
    std::vector<std::string> played;
    w.onCinematic = [&](const std::string& name, bool) { played.push_back(name); };
    for (int i = 0; i < 4; ++i) w.tick({});
    EXPECT_EQ(played, std::vector<std::string>{"270_shp_escape"});
    EXPECT_EQ(w.chapter, 7);
}

TEST(ChaptersReal, PickupsAndMusic) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto pool = game::loadUnitData(bf, "l4_pool");
    int treasures = 0, relics = 0;
    for (const auto& p : pool.pickups) treasures += p.kind == game::PickupKind::Treasure, relics += p.kind == game::PickupKind::Relic;
    EXPECT_GT(treasures, 0);
    EXPECT_EQ(relics, 1); // ca_rw_treasure 5496
    EXPECT_EQ(pool.music, "l4_pool_mus_01");
    const auto seatop = game::loadUnitData(bf, "l1_seatop");
    EXPECT_EQ(seatop.music, "l1_seatop_mus_01");
    EXPECT_EQ(seatop.musicByIntro.count(997), 1u); // cinematic 997's script names l1_seatop_mus_01
}

// The PDA's per-chapter reward table matches the units' reward intros (survey in chapterRewards).
TEST(ChaptersReal, RewardTableMatchesUnits) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const std::pair<const char*, int> units[] = {{"l1_kraken_room", 1}, {"l2_maproom", 2}, {"l7_arctic_sea", 7}};
    for (const auto& [name, chapter] : units) {
        const auto& r = game::chapterRewards(chapter);
        for (const auto& p : game::loadUnitData(bf, name).pickups) {
            if (p.kind == game::PickupKind::Relic) {
                EXPECT_EQ(p.id, r.relic) << name;
            } else if (p.kind == game::PickupKind::Treasure) {
                EXPECT_NE(std::find(r.treasures.begin(), r.treasures.end(), p.id), r.treasures.end()) << name << " " << p.id;
            }
        }
    }
    EXPECT_EQ(game::chapterRewards(2).treasures.size(), 22u);
    EXPECT_EQ(game::chapterRewards(0).relic, 0);
    EXPECT_EQ(&game::chapterRewards(8), &game::chapterRewards(7));
}
