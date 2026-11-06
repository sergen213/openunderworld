// Progression: chapter table, level ends, pickups + save round trip (src/game/Progress), menus (src/ui).
#include "game/Game.hpp"
#include "game/Progress.hpp"
#include "config/Config.hpp"
#include "save/Save.hpp"
#include "locale/Locale.hpp"
#include "ui/Art.hpp"
#include "ui/Pda.hpp"
#include "ui/Ui.hpp"

#include "formats/Formats.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include "cdc/Cdc.hpp"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdlib>

#include <zlib.h>
#include <unistd.h>

#include <filesystem>

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

TEST(Menus, GraphicsOptions) {
    ui::Menus m;
    ui::GraphicsOptions& g = m.graphics;
    g.resolutions = {{1280, 720}, {1920, 1080}};
    g.resolution = 1;
    auto row = [&] { return m.items()[size_t(m.cursor())]; };
    m.open(ui::Screen::Pause);
    m.open(ui::Screen::AudioVisual);
    for (int i = 0; i < 4; ++i) m.input(ui::Nav::Down);
    EXPECT_EQ(row().stringId, 498); // DISPLAY MODE SELECTION
    m.input(ui::Nav::Accept);
    ASSERT_EQ(m.screen(), ui::Screen::GfxDisplay);
    EXPECT_EQ(row().label, "RENDERER");
    m.input(ui::Nav::Right), m.input(ui::Nav::Right), m.input(ui::Nav::Right);
    EXPECT_EQ(g.renderer, 2); // OPENGL, clamped
    m.input(ui::Nav::Accept);
    EXPECT_EQ(g.renderer, 0); // Accept wraps to AUTO
    m.input(ui::Nav::Down); // RESOLUTION
    EXPECT_EQ(row().choices, (std::vector<std::string>{"1280 X 720", "1920 X 1080"}));
    m.input(ui::Nav::Right);
    EXPECT_EQ(g.resolution, 1);
    m.input(ui::Nav::Left);
    EXPECT_EQ(g.resolution, 0);
    m.input(ui::Nav::Down), m.input(ui::Nav::Accept); // FULLSCREEN
    EXPECT_TRUE(g.fullscreen);
    m.input(ui::Nav::Down), m.input(ui::Nav::Down); // FIELD OF VIEW
    m.input(ui::Nav::Right);
    EXPECT_EQ(g.fov, 75);
    g.fov = 72, m.input(ui::Nav::Right);
    EXPECT_EQ(g.fov, 75); // snaps onto the 5-degree grid
    g.fov = 120, m.input(ui::Nav::Right);
    EXPECT_EQ(g.fov, 120);
    g.fov = 30, m.input(ui::Nav::Left);
    EXPECT_EQ(g.fov, 30);
    m.input(ui::Nav::Back);
    ASSERT_EQ(m.screen(), ui::Screen::AudioVisual);
    EXPECT_EQ(m.cursor(), 4);
    m.input(ui::Nav::Down), m.input(ui::Nav::Accept); // DISPLAY SETTINGS
    ASSERT_EQ(m.screen(), ui::Screen::GfxSettings);
    m.input(ui::Nav::Accept); // QUALITY
    ASSERT_EQ(m.screen(), ui::Screen::GfxQuality);
    g.rt = 3;
    m.input(ui::Nav::Right); // PRESET ORIGINAL: explicit RT is invalid there
    EXPECT_EQ(g.preset, 1);
    EXPECT_EQ(g.rt, 0);
    m.input(ui::Nav::Back), m.input(ui::Nav::Down), m.input(ui::Nav::Accept); // RAY TRACING
    ASSERT_EQ(m.screen(), ui::Screen::GfxRayTracing);
    EXPECT_EQ(m.cursor(), 6); // every RT row greyed with preset original: BACK
    m.input(ui::Nav::Back);
    EXPECT_EQ(m.cursor(), 1);
    g.preset = 0, g.rt = 1; // OFF: only the mode row stays selectable
    m.input(ui::Nav::Accept);
    EXPECT_EQ(m.cursor(), 0);
    m.input(ui::Nav::Down);
    EXPECT_EQ(m.cursor(), 6); // greyed rows skipped
    m.input(ui::Nav::Down);
    EXPECT_EQ(m.cursor(), 0);
    m.input(ui::Nav::Right), m.input(ui::Nav::Right);
    EXPECT_EQ(g.rt, 3); // FULL
    m.input(ui::Nav::Down);
    EXPECT_EQ(m.cursor(), 1); // RT QUALITY now selectable
    m.input(ui::Nav::Back), m.input(ui::Nav::Down), m.input(ui::Nav::Down), m.input(ui::Nav::Accept); // UPSCALING
    ASSERT_EQ(m.screen(), ui::Screen::GfxUpscaling);
    g.fsr = true, g.frameGeneration = true;
    m.input(ui::Nav::Accept); // FSR off: frame generation needs it
    EXPECT_FALSE(g.fsr);
    EXPECT_FALSE(g.frameGeneration);
    m.input(ui::Nav::Down);
    EXPECT_EQ(m.cursor(), 4); // FSR QUALITY / SHARPNESS / FRAME GENERATION greyed: AMD ANTI-LAG
    m.input(ui::Nav::Accept);
    EXPECT_TRUE(g.antiLag);
    m.input(ui::Nav::Back);
    g.fsrSupported = g.antiLagSupported = false;
    m.input(ui::Nav::Accept);
    EXPECT_EQ(m.cursor(), 5); // the upscaler and Anti-Lag rows greyed too
    m.input(ui::Nav::Accept); // BACK
    EXPECT_EQ(m.screen(), ui::Screen::GfxSettings);
    EXPECT_EQ(m.cursor(), 3);
}

TEST(Menus, PauseSlotsAndPda) {
    ui::Menus m;
    m.open(ui::Screen::Pause);
    const auto ring = m.items();
    ASSERT_EQ(ring.size(), 8u); // the main menu's ring (no PDA item: key 1 opens it)
    EXPECT_EQ(ring[0].stringId, 122); // RESUME GAME, the centre
    EXPECT_EQ(ring[7].stringId, 131);
    m.saves = {{.slot = "quick"}, {.slot = "save-1"}};
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
    EXPECT_EQ(m.input(ui::Nav::Delete), ui::Action::None); // asks 10 first
    ASSERT_EQ(m.screen(), ui::Screen::Confirm);
    m.input(ui::Nav::Left);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::Delete); // YES
    EXPECT_EQ(m.screen(), ui::Screen::Load);
    EXPECT_EQ(m.selectedSave(), "save-1");
    EXPECT_EQ(m.cursor(), 2);
    m.input(ui::Nav::Down);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None); // empty slot
    m.input(ui::Nav::Up);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None); // asks 12
    ASSERT_EQ(m.screen(), ui::Screen::Confirm);
    m.input(ui::Nav::Left);
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

// YES / NO questions [O dlg_*]: NO selected, Esc ignored, YES runs the item.
TEST(Menus, ConfirmDialogs) {
    ui::Menus m;
    m.open(ui::Screen::Main);
    m.input(ui::Nav::Left), m.input(ui::Nav::Down), m.input(ui::Nav::Down), m.input(ui::Nav::Down); // QUIT GAME
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None);
    ASSERT_EQ(m.screen(), ui::Screen::Confirm);
    EXPECT_EQ(m.cursor(), 1);
    EXPECT_EQ(m.input(ui::Nav::Back), ui::Action::None);
    EXPECT_EQ(m.screen(), ui::Screen::Confirm);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None); // NO
    EXPECT_EQ(m.screen(), ui::Screen::Main);
    EXPECT_EQ(m.cursor(), 7);
    m.input(ui::Nav::Accept), m.input(ui::Nav::Right);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::Quit); // YES
    m.close();
    m.open(ui::Screen::Pause);
    m.input(ui::Nav::Left), m.input(ui::Nav::Up), m.input(ui::Nav::Up), m.input(ui::Nav::Up); // START NEW GAME
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None);
    ASSERT_EQ(m.screen(), ui::Screen::Confirm); // 154
    m.input(ui::Nav::Up), m.input(ui::Nav::Accept);
    ASSERT_EQ(m.screen(), ui::Screen::Tailoring);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::NewGame);
    m.open(ui::Screen::Pause);
    m.saves = {{.slot = "quick"}};
    m.input(ui::Nav::Left), m.input(ui::Nav::Accept), m.input(ui::Nav::Accept); // SAVE GAME, the used slot
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None); // overwrite? (16)
    ASSERT_EQ(m.screen(), ui::Screen::Confirm);
    m.input(ui::Nav::Left);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::Save);
    EXPECT_EQ(m.selectedSave(), "quick");
    EXPECT_EQ(m.screen(), ui::Screen::Save);
}

// Filled slots [O load_slot_filled]: the autosave cannot be overwritten (17); level titles per unit (386..407).
TEST(Menus, SaveSlotsAndLevelTitles) {
    EXPECT_EQ(ui::levelTitle(2, "l2_cliffclimb"), 393); // [O] chapter starts
    EXPECT_EQ(ui::levelTitle(4, "l4_trailstart"), 398);
    EXPECT_EQ(ui::levelTitle(5, "l5_startsnow"), 402);
    EXPECT_EQ(ui::levelTitle(0, "l3_main_bedroom"), 386); // the prologue, not Croft Manor's 397
    EXPECT_EQ(ui::levelTitle(3, "l3_main_bedroom"), 397);
    EXPECT_EQ(ui::levelTitle(1, "l1_seatop"), 387);
    EXPECT_EQ(ui::levelTitle(7, "l7_yggdrasil_hall01"), 406);
    EXPECT_EQ(ui::levelTitle(8, "l7_himalaya_end"), 407);
    for (int c = 0; c <= 8; ++c) {
        const int t = ui::levelTitle(c, game::kChapters[size_t(c)].unit);
        EXPECT_GE(t, 386);
        EXPECT_LE(t, 407);
    }
    ui::Menus m;
    m.saves = {{.slot = "autosave", .area = 53, .title = 393, .autosave = true}, {.slot = "savegame-1", .name = "Savegame 1"}};
    m.open(ui::Screen::Pause);
    m.input(ui::Nav::Left), m.input(ui::Nav::Accept), m.input(ui::Nav::Accept); // SAVE GAME, the autosave slot
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None);
    EXPECT_EQ(m.screen(), ui::Screen::Message);
    m.input(ui::Nav::Accept);
    ASSERT_EQ(m.screen(), ui::Screen::Save);
    m.input(ui::Nav::Down);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None);
    EXPECT_EQ(m.screen(), ui::Screen::Confirm); // overwrite Savegame 1?
}

// PDA pages: SONAR MAP (only with a map; E resets, Escape back), WEAPON SELECTION (the secondaries + BACK, Return EQUIP
// names the row's weapon, its model in the panel), INVENTORY categories and their items.
TEST(Menus, PdaWeaponsInventoryAndSonar) {
    ui::Menus m;
    m.open(ui::Screen::Pda);
    m.input(ui::Nav::Up), m.input(ui::Nav::Accept);
    EXPECT_EQ(m.screen(), ui::Screen::Message); // no map: SONAR MAP IS CURRENTLY UNAVAILABLE
    m.close();
    m.pda.sonar = true;
    for (int w : {1, 2, 5, 6, 3}) m.pda.weapons.push_back({w, int16_t(75 + w), 10 * w, 20 * w, w == 6, "wp_" + std::to_string(w)});
    m.pda.inventory[0] = {{1274, " [2]", 1275, {}, 706, {}}, {1248, " [4]", 1249, {}, 0, "pr_grenade"}};
    m.open(ui::Screen::Pda);
    m.input(ui::Nav::Up), m.input(ui::Nav::Accept);
    ASSERT_EQ(m.screen(), ui::Screen::PdaSonar);
    EXPECT_EQ(m.input(ui::Nav::Up), ui::Action::None); // the app flies the camera with held keys
    EXPECT_EQ(m.input(ui::Nav::Alt), ui::Action::SonarReset);
    EXPECT_EQ(m.screen(), ui::Screen::PdaSonar);
    m.input(ui::Nav::Back);
    ASSERT_EQ(m.screen(), ui::Screen::Pda);
    m.input(ui::Nav::Down), m.input(ui::Nav::Down), m.input(ui::Nav::Accept); // WEAPON SELECTION
    ASSERT_EQ(m.screen(), ui::Screen::PdaWeapons);
    ASSERT_EQ(m.items().size(), 6u);
    EXPECT_EQ(m.items().back().stringId, 469); // BACK TO PREVIOUS MENU
    EXPECT_EQ(m.cursor(), 0);
    ASSERT_TRUE(m.pdaModel());
    EXPECT_EQ(m.pdaModel()->object, "wp_1");
    m.input(ui::Nav::Down), m.input(ui::Nav::Down);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::Equip);
    EXPECT_EQ(m.equipWeapon(), 5);
    EXPECT_EQ(m.screen(), ui::Screen::PdaWeapons); // stays, the app refreshes the rows
    for (int i = 0; i < 3; ++i) m.input(ui::Nav::Down);
    EXPECT_FALSE(m.pdaModel()); // BACK row
    m.input(ui::Nav::Accept);
    ASSERT_EQ(m.screen(), ui::Screen::Pda);
    m.input(ui::Nav::Down), m.input(ui::Nav::Accept); // INVENTORY
    ASSERT_EQ(m.screen(), ui::Screen::PdaInventory);
    EXPECT_EQ(m.items().size(), 5u);
    m.input(ui::Nav::Accept); // BACKPACK
    ASSERT_EQ(m.screen(), ui::Screen::PdaItems);
    ASSERT_EQ(m.items().size(), 3u);
    EXPECT_EQ(m.items()[0].label, " [2]");
    EXPECT_FALSE(m.pdaModel()); // a picture
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None);
    EXPECT_EQ(m.screen(), ui::Screen::PdaItems);
    m.input(ui::Nav::Down);
    ASSERT_TRUE(m.pdaModel());
    EXPECT_EQ(m.pdaModel()->object, "pr_grenade");
    m.input(ui::Nav::Back);
    EXPECT_EQ(m.screen(), ui::Screen::PdaInventory);
}

// The main menu's centre [O mainmenu, e4/e5]: PLAY GAME, RESUME GAME with an autosave, TREASURE HUNT after the ending;
// REVISIT LOCATION only in a Treasure Hunt.
TEST(Menus, MainCentreTreasureHuntAndRevisit) {
    ui::Menus m;
    m.open(ui::Screen::Main);
    EXPECT_EQ(m.items()[0].stringId, 123);
    EXPECT_EQ(m.items()[1].confirm, 0);
    m.saves = {{.slot = "autosave", .autosave = true}};
    EXPECT_EQ(m.items()[0].stringId, 122);
    EXPECT_EQ(m.items()[0].action, ui::Action::Continue);
    EXPECT_EQ(m.items()[1].confirm, 157); // new autosaves overwrite it
    m.finished = true;
    EXPECT_EQ(m.items()[0].stringId, 49);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::TreasureHunt);
    EXPECT_EQ(m.screen(), ui::Screen::None);
    m.open(ui::Screen::Pda);
    EXPECT_TRUE(m.items()[6].greyed);
    m.close();
    m.treasureHunt = true;
    m.open(ui::Screen::Pda);
    EXPECT_FALSE(m.items()[6].greyed);
    for (int i = 0; i < 4; ++i) m.input(ui::Nav::Down); // AREA INFO .. FIELD ASSISTANCE, JOURNAL greyed: REVISIT LOCATION
    EXPECT_EQ(m.cursor(), 6);
    m.input(ui::Nav::Accept);
    ASSERT_EQ(m.screen(), ui::Screen::Revisit);
    ASSERT_EQ(m.items().size(), 7u);
    EXPECT_EQ(m.items()[0].stringId, 52);
    for (int i = 0; i < 9; ++i) m.input(ui::Nav::Down);
    EXPECT_EQ(m.cursor(), 6); // stops at the end
    m.input(ui::Nav::Back);
    EXPECT_EQ(m.screen(), ui::Screen::Pda);
    m.input(ui::Nav::Accept), m.input(ui::Nav::Down);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::Revisit);
    EXPECT_EQ(m.revisitChapter(), 2);
    EXPECT_EQ(m.screen(), ui::Screen::None);
}

// EXTRAS [O extras captures]: the mirrored category ring, a category's rows (LOCKED until unlocked), the preview image,
// the full-screen view and the trailers' movies.
TEST(Menus, ExtrasRingCategoriesAndUnlocks) {
    EXPECT_TRUE(ui::unlocked(-1, {}));
    EXPECT_FALSE(ui::unlocked(3, {}));
    EXPECT_TRUE(ui::unlocked(3, {.chapters = 1u << 1}));  // Mediterranean Sea
    EXPECT_TRUE(ui::unlocked(4, {.chapters = 1u << 0}));  // the prologue
    EXPECT_TRUE(ui::unlocked(10, {.chapters = 1u << 7})); // Arctic Sea
    EXPECT_FALSE(ui::unlocked(5, {.chapters = 1u << 1}));
    EXPECT_TRUE(ui::unlocked(11, {.finished = true}));
    EXPECT_TRUE(ui::unlocked(12, {.treasures = true}));
    EXPECT_FALSE(ui::unlocked(13, {.treasures = true}));
    ui::Rewards r;
    r.environment = {{304, 3, "concept_medsea_ruin"}, {313, 5, "concept_thailand_temple"}};
    for (int16_t i = 0; i < 9; ++i) r.gear.push_back({int16_t(348 + i), 8, "concept_gear_" + std::to_string(i)});
    r.character = r.storyboards = r.environment;
    r.trailers = {{360, -1, "previously", "prevontr"}};
    ui::Menus m;
    m.open(ui::Screen::Main);
    EXPECT_EQ(m.items()[6].opens, ui::Screen::Message); // no rewards: "Unavailable"
    m.rewards = &r;
    m.unlocks.chapters = 1u << 1;
    ASSERT_EQ(m.items()[6].opens, ui::Screen::Extras);
    m.input(ui::Nav::Left);  // SAVE GAME (level with the centre)
    m.input(ui::Nav::Down), m.input(ui::Nav::Down);
    EXPECT_EQ(m.cursor(), 6);
    m.input(ui::Nav::Accept);
    ASSERT_EQ(m.screen(), ui::Screen::Extras);
    EXPECT_EQ(m.cursor(), -1); // focus on EXTRAS until Right
    const auto ring = m.items();
    ASSERT_EQ(ring.size(), 6u); // and OUTFITS (ours)
    EXPECT_EQ(ring[0].stringId, 146);
    EXPECT_EQ(ring[0].icon, 43u);
    EXPECT_EQ(ring[4].stringId, 151);
    EXPECT_EQ(ring[4].icon, 48u);
    m.input(ui::Nav::Right);
    EXPECT_EQ(m.cursor(), 0);
    m.input(ui::Nav::Accept); // ENVIRONMENT CONCEPTS
    ASSERT_EQ(m.screen(), ui::Screen::ExtrasCategory);
    EXPECT_EQ(m.items()[0].stringId, 304);
    EXPECT_EQ(m.items()[1].stringId, 470); // LOCKED
    EXPECT_EQ(m.extrasImage(), "concept_medsea_ruin");
    m.input(ui::Nav::Down);
    EXPECT_EQ(m.extrasImage(), "locked_concept");
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None); // locked: nothing
    EXPECT_EQ(m.screen(), ui::Screen::ExtrasCategory);
    m.input(ui::Nav::Up), m.input(ui::Nav::Accept);
    ASSERT_EQ(m.screen(), ui::Screen::ExtrasView);
    EXPECT_EQ(m.extrasImage(), "concept_medsea_ruin");
    m.input(ui::Nav::Back), m.input(ui::Nav::Back);
    EXPECT_EQ(m.screen(), ui::Screen::Extras);
    EXPECT_EQ(m.cursor(), 0);
    m.input(ui::Nav::Down), m.input(ui::Nav::Down), m.input(ui::Nav::Accept); // GEAR CONCEPTS: nine rows, five shown
    m.unlocks.chapters = 1u << 5; // Jan Mayen
    EXPECT_EQ(m.items()[0].stringId, 348);
    for (int i = 0; i < 8; ++i) m.input(ui::Nav::Down);
    EXPECT_EQ(m.cursor(), 8);
    m.input(ui::Nav::Back), m.input(ui::Nav::Down), m.input(ui::Nav::Down), m.input(ui::Nav::Accept); // TRAILERS & CREDITS
    EXPECT_EQ(m.extrasImage(), "previously");
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::PlayMovie);
    EXPECT_EQ(m.selectedSave(), "prevontr.bik");
    EXPECT_EQ(m.screen(), ui::Screen::ExtrasCategory); // the menu stays
    m.input(ui::Nav::Back), m.input(ui::Nav::Back);
    EXPECT_EQ(m.screen(), ui::Screen::Main);
    m.close(); // in play the movie first asks to leave the game (471)
    m.open(ui::Screen::Pause);
    m.input(ui::Nav::Left), m.input(ui::Nav::Down), m.input(ui::Nav::Down), m.input(ui::Nav::Accept), m.input(ui::Nav::Accept);
    for (int i = 0; i < 4; ++i) m.input(ui::Nav::Down);
    m.input(ui::Nav::Accept);
    ASSERT_EQ(m.screen(), ui::Screen::ExtrasCategory);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None);
    EXPECT_EQ(m.screen(), ui::Screen::Confirm);
    m.input(ui::Nav::Left);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::PlayMovie);
}

// EXTRAS OUTFITS (ui::kCostumes): DEFAULT and the outfits, LOCKED until their chapter ends; Accept wears an unlocked one
// (Action::Outfit, Menus::outfit); save::Profile keeps the choice; World::wear dresses Lara and level loads and saves keep
// it until DEFAULT gives the level's own back.
TEST(Menus, ExtrasOutfits) {
    ui::Rewards r;
    r.environment = r.gear = r.storyboards = r.trailers = {{304, -1, "x"}};
    r.character = {{301, -1, "concept_lara_outfits"}, {303, -1, "concept_lara_wetsuit"}};
    ui::Menus m;
    m.rewards = &r;
    m.unlocks.chapters = 1u << 1; // Mediterranean Sea: the wetsuit
    m.open(ui::Screen::Extras);
    m.input(ui::Nav::Right);
    for (int i = 0; i < 5; ++i) m.input(ui::Nav::Down);
    EXPECT_EQ(m.items()[size_t(m.cursor())].label, "OUTFITS");
    m.input(ui::Nav::Accept);
    ASSERT_EQ(m.screen(), ui::Screen::ExtrasCategory);
    const auto rows = m.items();
    ASSERT_EQ(rows.size(), 1 + ui::kCostumes.size());
    EXPECT_EQ(rows[0].label, "DEFAULT");
    EXPECT_EQ(rows[1].stringId, 470); // Casual: the prologue has not ended
    EXPECT_EQ(rows[2].stringId, 70);  // Wetsuit
    EXPECT_EQ(m.extrasImage(), "concept_lara_outfits");
    m.input(ui::Nav::Down);
    EXPECT_EQ(m.extrasImage(), "locked_concept");
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None);
    m.input(ui::Nav::Down);
    EXPECT_EQ(m.extrasImage(), "concept_lara_wetsuit");
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::Outfit);
    EXPECT_EQ(m.outfit, "lara_wetsuit_warm");
    EXPECT_EQ(m.screen(), ui::Screen::ExtrasCategory);

    const auto dir = std::filesystem::temp_directory_path() / ("ou_outfit_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    save::writeProfile({.chapters = 1u << 1, .outfit = m.outfit}, dir);
    const auto p = save::readProfile(dir);
    std::filesystem::remove_all(dir);
    EXPECT_EQ(p.outfit, "lara_wetsuit_warm");
    EXPECT_TRUE(ui::unlocked(ui::kCostumes[1].unlock, {.chapters = p.chapters}));

    const auto dressed = [](const std::string& n) { // "a" in casual, the others in jungle_a
        auto u = unit(n);
        u.playerObject = n == "a" ? "lara_casual" : "lara_jungle_a";
        return u;
    };
    game::World w(dressed, "a", idleAnims());
    EXPECT_EQ(w.outfit, "lara_casual");
    w.wear(p.outfit);
    EXPECT_EQ(w.outfit, "lara_wetsuit_warm");
    const auto saved = save::capture(w);
    EXPECT_EQ(saved.outfit, "lara_casual"); // the save keeps the level's own
    w.enterUnit("b", std::nullopt);
    EXPECT_EQ(w.outfit, "lara_wetsuit_warm");
    save::apply(saved, w);
    EXPECT_EQ(w.outfit, "lara_wetsuit_warm");
    w.wear(""); // DEFAULT
    EXPECT_EQ(w.outfit, "lara_casual");
}

// PDA JOURNAL [O j3..j5]: greyed without entries, a list page of the entries reached with each one's text; Tab opens the PDA.
TEST(Menus, PdaJournal) {
    EXPECT_EQ(config::InputConfig().bindings.at("pda"), (std::vector<std::string>{"Tab", "Pad back"}));
    EXPECT_EQ(ui::journalEntries(0), 0);
    EXPECT_EQ(ui::journalEntries(1), 7);  // MED SEA EXPEDITION .. NATLA
    EXPECT_EQ(ui::journalEntries(2), 14); // .. ODIN'S MAP ROOM
    EXPECT_EQ(ui::journalEntries(8), 45); // .. MOTHER
    ui::Menus m;
    m.open(ui::Screen::Pda);
    EXPECT_TRUE(m.items()[5].greyed);
    m.close();
    m.journal = ui::journalEntries(2);
    m.open(ui::Screen::Pda);
    ASSERT_FALSE(m.items()[5].greyed);
    for (int i = 0; i < 4; ++i) m.input(ui::Nav::Down); // AREA INFO .. FIELD ASSISTANCE, JOURNAL
    EXPECT_EQ(m.cursor(), 5);
    m.input(ui::Nav::Accept);
    ASSERT_EQ(m.screen(), ui::Screen::Journal);
    const auto list = m.items();
    ASSERT_EQ(list.size(), 14u);
    EXPECT_EQ(list[0].stringId, 1145);
    EXPECT_EQ(list[0].message, 1190); // its text
    EXPECT_EQ(list[13].stringId, 1158);
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None);
    EXPECT_EQ(m.screen(), ui::Screen::Journal); // Return does nothing
    for (int i = 0; i < 20; ++i) m.input(ui::Nav::Down);
    EXPECT_EQ(m.cursor(), 13);
    m.input(ui::Nav::Back);
    EXPECT_EQ(m.screen(), ui::Screen::Pda);
    EXPECT_EQ(m.cursor(), 5);
}

// GAME OPTIONS > PLAYER TAILORING [O tailor_options / tailor_changed*]: the four levels one by one, Left the lenient end;
// AMMUNITION CAPACITY and TIME FOR SAVING GRABS read EXTRA .. LESS.
TEST(Menus, PlayerTailoringOptions) {
    EXPECT_EQ(ui::tailoringValue(0, false), 376); // LESS ->
    EXPECT_EQ(ui::tailoringValue(1, false), 377); // <- NORMAL ->
    EXPECT_EQ(ui::tailoringValue(2, false), 378); // <- EXTRA
    EXPECT_EQ(ui::tailoringValue(0, true), 379);  // EXTRA ->
    EXPECT_EQ(ui::tailoringValue(1, true), 377);
    EXPECT_EQ(ui::tailoringValue(2, true), 380);  // <- LESS
    ui::Menus m;
    m.open(ui::Screen::GameOptions);
    for (int i = 0; i < 5; ++i) m.input(ui::Nav::Down);
    m.input(ui::Nav::Accept);
    ASSERT_EQ(m.screen(), ui::Screen::TailoringOptions);
    const auto list = m.items();
    ASSERT_EQ(list.size(), 5u);
    EXPECT_EQ(list[0].stringId, 368);
    EXPECT_FALSE(list[0].reversed);
    EXPECT_EQ(list[1].stringId, 370);
    EXPECT_TRUE(list[1].reversed);
    EXPECT_EQ(list[3].stringId, 372);
    EXPECT_TRUE(list[3].reversed);
    m.input(ui::Nav::Right), m.input(ui::Nav::Right); // ENEMY HEALTH: <- EXTRA, clamped
    m.input(ui::Nav::Down), m.input(ui::Nav::Left);   // AMMUNITION CAPACITY: EXTRA ->
    EXPECT_EQ(m.input(ui::Nav::Accept), ui::Action::None);
    EXPECT_EQ(m.screen(), ui::Screen::TailoringOptions); // Return on a level does not leave
    EXPECT_EQ(m.tailor, (std::array<int, 4>{2, 0, 1, 1}));
    EXPECT_EQ(m.tailoring, 1); // the preset is untouched
    m.input(ui::Nav::Down), m.input(ui::Nav::Down), m.input(ui::Nav::Down), m.input(ui::Nav::Accept); // BACK TO PREVIOUS MENU
    EXPECT_EQ(m.screen(), ui::Screen::GameOptions);
}

// save::Profile (chapters ended, the ending, all treasures / relics) and a Treasure Hunt's save flag.
TEST(Progress, ProfileAndTreasureHuntSave) {
    const auto dir = std::filesystem::temp_directory_path() / ("ou_profile_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    EXPECT_EQ(save::readProfile(dir), save::Profile{});
    save::writeProfile({0x7Fu, true, true, false}, dir);
    EXPECT_EQ(save::readProfile(dir), (save::Profile{0x7Fu, true, true, false}));
    std::filesystem::remove_all(dir);
    save::SaveGame g;
    g.unit = "l1_seatop", g.treasureHunt = true;
    EXPECT_TRUE(save::parse(save::serialize(g)).treasureHunt);
    g.treasureHunt = false;
    EXPECT_FALSE(save::parse(save::serialize(g)).treasureHunt);
}

TEST(Menus, WrapText) {
    const auto lines = ui::wrap("aaa bbb ccc\\ndd", 7);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "aaa bbb");
    EXPECT_EQ(lines[2], "dd");
}

// Button placeholders (tru.exe 0x5A7C20 / 0x8A9EF8) and how the font draws the result (0x476D80, 0x476540).
TEST(Menus, ButtonPrompts) {
    EXPECT_EQ(ui::promptControl(0x1800), 1);  // X: grapple
    EXPECT_EQ(ui::promptControl(0x1803), 3);  // A: jump
    EXPECT_EQ(ui::promptControl(0x1811), 11); // Back: PDA
    EXPECT_EQ(ui::promptControl(0x1814), 19); // up
    EXPECT_EQ(ui::promptControl(0x1829), 24); // vehicle reverse
    EXPECT_EQ(ui::promptControl(0x182A), 25); // Return
    EXPECT_EQ(ui::promptControl(0x182C), 27); // BackSpace
    EXPECT_EQ(ui::promptControl(0x180C), -1); // left stick icon: stays
    EXPECT_EQ(ui::promptControl(0x182D), -1);
    EXPECT_EQ(ui::promptControl('A'), -1);

    ui::PromptNames names;
    names[1] = "Q", names[3] = "\xE1\xA0\x83"; // jump on the pad: the A icon
    EXPECT_EQ(ui::expandPrompts("Press \xE1\xA0\x80 to fire", names), "Press \x1BQ\x1B to fire");
    EXPECT_EQ(ui::expandPrompts("\xE1\xA0\x83 RESUME", names), "\x1B\xE1\xA0\x83\x1B RESUME");
    EXPECT_EQ(ui::expandPrompts("\xE1\xA0\x89!", names), "\x1B<Unknown>\x1B!"); // d-pad down: unbound
    EXPECT_EQ(ui::expandPrompts("\xE1\xA0\x8D ROTATE \xC3\x96", names), "\xE1\xA0\x8D ROTATE \xC3\x96");

    ui::Art art;
    art.textures[1] = {0, 0, 1, 1, 64, 64};
    art.font.texture = art.font.texture2 = 1;
    art.font.cellHeight = 8;
    for (const uint32_t cp : {uint32_t('Q'), uint32_t('/'), 0x1803u}) art.font.glyphs[cp] = {4, 0, 0, cp >= 0x1800};
    ui::Canvas c(640, 480, &art);
    c.prompts = &names;
    const float w = c.fontText(0, 0, 1, "\x01Q\xE1\xA0\x80\\\xE1\xA0\x83", 0x80112233u); // ctrl, Q, grapple, '\', jump
    ASSERT_EQ(c.vertices.size(), 4u * 6u); // Q, Q (key name), '/', A icon
    EXPECT_EQ(c.vertices[0].color, 0x80112233u);  // plain
    EXPECT_EQ(c.vertices[6].color, 0x80F7D163u);  // key name: highlight, alpha kept
    EXPECT_EQ(c.vertices[12].color, 0x80112233u); // '\' as '/'
    EXPECT_EQ(c.vertices[18].color, 0x80FFFFFFu); // pad icon: white, alpha kept
    EXPECT_FLOAT_EQ(w, 4 * 3.0f);                 // (spacing 0 + width 4) - 1 per glyph
    EXPECT_FLOAT_EQ(c.fontWidth("\x01Q\xE1\xA0\x80\\\xE1\xA0\x83", 1, 2), w + 4 * 2);
}

// promptNames: the default controls as tru.exe names them.
TEST(ChaptersReal, PromptNames) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto strings = locale::loadStrings(gameDir(), locale::Language::English);
    ASSERT_TRUE(strings);
    const game::Controls controls;
    const auto kb = game::promptNames(controls, false, *strings), pad = game::promptNames(controls, true, *strings);
    EXPECT_EQ(kb[1], "Q");
    EXPECT_EQ(kb[3], "Space");
    EXPECT_EQ(kb[4], "G"); // a key before the mouse button
    EXPECT_EQ(kb[25], "Return");
    EXPECT_EQ(kb[13], "");  // field camera: not ours
    EXPECT_EQ(pad[1], "\xE1\xA0\x80"); // X
    EXPECT_EQ(pad[5], "\xE1\xA0\x87"); // right trigger
    EXPECT_EQ(pad[11], "Back Button");
    EXPECT_EQ(pad[19], "\xE1\xA0\x94"); // forward: no pad button, the placeholder's own icon
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

// Graphics rows fit the list: labels end before the slider's left arrow (307.6), values fit between its arrows.
TEST(MenusReal, GraphicsRowsFit) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto art = ui::loadArt(bf);
    ASSERT_TRUE(art);
    const auto strings = locale::loadStrings(gameDir(), locale::Language::English);
    ASSERT_TRUE(strings);
    ui::Canvas c(1920, 1080, &*art);
    ui::Menus m;
    m.graphics.resolutions = {{3840, 2160}};
    for (auto s : {ui::Screen::GfxDisplay, ui::Screen::GfxSettings, ui::Screen::GfxQuality, ui::Screen::GfxRayTracing,
                   ui::Screen::GfxWater, ui::Screen::GfxUpscaling}) {
        m.open(s);
        for (const auto& it : m.items()) {
            if (it.stringId == 469) continue;
            const std::string label = it.stringId ? std::string(strings->get(it.stringId)) : it.label;
            EXPECT_LT(152.4f + c.fontWidth(label, 0.6f), 303.0f) << label;
            for (const auto& v : it.choices) EXPECT_LT(c.fontWidth(v, 0.6f), 152.0f) << v; // 475.6 - 307.6 - two arrow widths
        }
        m.close();
    }
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
    const auto rewards = ui::loadRewards(bf); // EXTRAS: every reward's image, the trailers' movies
    ASSERT_TRUE(rewards);
    EXPECT_EQ(rewards->gear.size(), 9u);
    for (const auto* l : {&rewards->environment, &rewards->character, &rewards->gear, &rewards->storyboards, &rewards->trailers})
        for (const auto& w : *l) EXPECT_TRUE(bf.read("pc-w\\image\\" + w.image + ".raw")) << w.image;
    ASSERT_EQ(rewards->trailers.size(), 3u);
    EXPECT_EQ(rewards->trailers[0].movie, "prevontr");
    EXPECT_EQ(rewards->trailers[2].movie, "credits");
    for (const auto& c : ui::kCostumes) { // EXTRAS OUTFITS: each outfit's DRM and the concept shown beside it
        EXPECT_TRUE(bf.read("pc-w\\" + std::string(c.object) + ".drm")) << c.object;
        EXPECT_TRUE(std::any_of(rewards->character.begin(), rewards->character.end(), [&](const auto& w) { return w.name == c.art; }))
            << c.object;
    }
    EXPECT_TRUE(ui::loadRaw(bf, "locked_concept"));
    const auto pda = ui::loadRaw(bf, "pda_bg_1280"); // the bezel is blue: B,G,R,A pixels
    const uint8_t* bar = &pda->rgba[(size_t(600) * 1280 + 640) * 4];
    EXPECT_GT(bar[2], bar[0]);
    EXPECT_FALSE(ui::loadRaw(bf, "no_such_image"));
}

// Chapter ends that run through trigger scripts: the volume around the exit preloads the closing cinematic (vt 63, state 2:
// stream created, nothing plays), the plane beyond it plays it (vt 57); World then plays the stream(s) and starts the next
// chapter. Lara spawns in the preload volume and walks through the plane.
static void endingPlays(const char* unitName, int chapter, int32_t preload, int32_t plane, const std::vector<std::string>& streams,
                 int next) {
    const cdc::BigfileSet bf(gameDir());
    const auto unit = game::loadUnitData(bf, unitName);
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }), unitName,
                  idleAnims(), game::Spawn{introPos(unit, preload), 0});
    w.script().setBigfile(&bf);
    w.chapter = chapter;
    std::vector<std::string> played;
    w.onCinematic = [&](const std::string& name, bool) { played.push_back(name); };
    for (int i = 0; i < 4; ++i) w.tick({});
    EXPECT_TRUE(played.empty()) << unitName << ": the preload volume plays nothing";
    const auto* t = w.script().find(plane);
    ASSERT_TRUE(t && t->kind == script::Kind::Plane) << unitName;
    const glm::mat3 r = glm::mat3(glm::rotate(glm::mat4(1), t->rot.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), t->rot.y, {0, 1, 0}) *
                                  glm::rotate(glm::mat4(1), t->rot.z, {0, 0, 1}));
    const vec3 n = r * vec3(0, 1, 0), base = t->pos - vec3(0, 0, script::kTestPointHeight);
    for (const vec3 p : {base + n * 300.0f, base - n * 300.0f}) {
        auto s = w.player().state();
        s.pos = p;
        w.restore(s, 0, 0);
        w.tick({});
    }
    for (int i = 0; i < 120 && w.chapter == chapter; ++i) w.tick({});
    EXPECT_EQ(played, streams) << unitName;
    EXPECT_EQ(w.chapter, next) << unitName;
}

TEST(ChaptersReal, MediterraneanEndingRunsThroughTriggerScript) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    endingPlays("l1_ship_sinking_hall", 1, 1082, 1074, {"070_med_dive"}, 2);
}

TEST(ChaptersReal, CroftManorEndingRunsBothCinematics) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    endingPlays("l3_main_hall", 3, 4137, 4128, {"160_man_shot", "170_man_exit"}, 4);
}

TEST(ChaptersReal, NatlaShipEndingPassesLoadedGuard) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    endingPlays("l6_ship_engine_hall", 6, 7566, 7565, {"270_shp_escape"}, 7);
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

// PDA sonar map: l4_trailstart's map is in the unit frame (inside its collision bounds), shows only triangles near
// the reveal points, and renders the revealed part with the grid; the side-view model panel draws a weapon.
// OU_PDA_DUMP=<dir> writes the images as PPM for viewing.
TEST(PdaReal, SonarMapFrameRevealAndModels) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto map = ui::loadSonarMap(bf, "l4_trailstart");
    ASSERT_TRUE(map);
    const auto u = game::loadUnitData(bf, "l4_trailstart");
    vec3 lo(1e30f), hi(-1e30f);
    for (const auto& v : u.col.vertices) lo = glm::min(lo, v), hi = glm::max(hi, v);
    vec3 centre(0);
    for (const auto& v : map->vertices) centre += vec3(v[0], v[1], v[2]) / float(map->vertices.size());
    EXPECT_TRUE(glm::all(glm::greaterThan(centre, lo)) && glm::all(glm::lessThan(centre, hi))) << centre.x << " " << centre.y << " " << centre.z;
    const vec3 lara = u.spawn ? u.spawn->pos : centre;
    const std::vector<vec3> reveal{lara};
    const auto shown = ui::Sonar::revealTriangles(*map, reveal);
    const auto count = std::count(shown.begin(), shown.end(), 1);
    EXPECT_GT(count, 0);
    EXPECT_LT(size_t(count), shown.size());
    const auto none = ui::Sonar::revealTriangles(*map, {});
    EXPECT_EQ(std::count(none.begin(), none.end(), 1), 0);

    ui::SonarCamera cam;
    cam.reset(lara, u.spawn ? u.spawn->yaw : 0);
    ui::Sonar sonar;
    const ui::SonarUnit units[] = {{"l4_trailstart", &*map, vec3(0), &reveal}};
    const auto pawnMesh = ui::viewMesh(formats::loadDrmScene(bf, "pc-w\\ping_map_icon_lara.drm"));
    EXPECT_FALSE(pawnMesh.empty());
    const ui::SonarPawn pawns[] = {{&pawnMesh, glm::translate(glm::mat4(1), lara), {0.1f, 0.8f, 0.55f}}};
    const auto& img = sonar.render(960, 540, cam, units, pawns);
    size_t lit = 0;
    for (size_t i = 0; i < img.rgba.size(); i += 4) lit += img.rgba[i + 1] > 40;
    EXPECT_GT(lit, img.rgba.size() / 4 / 10); // the revealed map fills part of the view

    auto uzi = ui::viewMesh(formats::loadDrmScene(bf, "pc-w\\wp_uzi.drm"));
    EXPECT_FALSE(uzi.empty());
    EXPECT_FALSE(uzi.textures.empty());
    ui::Image panel{std::vector<uint8_t>(426 * 164 * 4, 0), 426, 164};
    ui::drawModelPanel(panel, 0, 0, 426, 164, &uzi);
    if (const char* dir = std::getenv("OU_PDA_DUMP")) {
        auto dump = [&](const ui::Image& im, const std::string& name) {
            std::FILE* f = std::fopen((std::string(dir) + "/" + name).c_str(), "wb");
            std::fprintf(f, "P6\n%d %d\n255\n", im.width, im.height);
            for (size_t i = 0; i < im.rgba.size(); i += 4) std::fwrite(&im.rgba[i], 1, 3, f);
            std::fclose(f);
        };
        dump(img, "sonar.ppm"), dump(panel, "panel.ppm");
    }
}
