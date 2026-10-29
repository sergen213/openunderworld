// Front-end and in-game UI: the menus (main/pause ring, save slots, option lists, player tailoring, message box),
// the PDA, loading screens, the HUD, notifications and chapter title cards. Renderer-agnostic: draws into
// render::TextVertex quads (FrameOptions::ui), all with the game's own art and font (ui::Art) at layouts measured from
// tru.exe captures (the HUD in src/ui/Hud.cpp). Text is the original locals.bin strings. The original UI scripts are not
// executed. docs/GAMEPLAY.md "Menus".
#pragma once

#include "render/Renderer.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ou::locale { class Strings; }

namespace ou::ui {

struct Art;
struct Reward;
struct Rewards;
using render::TextVertex;

// Colours, 0xAABBGGRR as TextVertex::color.
inline constexpr uint32_t kWhite = 0xFFFFFFFFu, kGold = 0xFF40C0FFu, kGrey = 0xFFA0A0A0u, kShade = 0xB0000000u;

// Atlas text: UTF-8 -> ASCII 32..126. Controller-button placeholders U+1800..U+18FF (TEXT.md §1) and
// control characters are dropped, Latin-1 letters lose their accent, anything else becomes '?'.
std::string atlasText(std::string_view utf8);
// Greedy word wrap of `text` to lines of at most `cols` characters; "\n" sequences (as locals.bin
// escapes them) and '\n' break lines.
std::vector<std::string> wrap(std::string_view text, size_t cols);

class Canvas {
public:
    Canvas(int width, int height, const Art* art = nullptr) : width(width), height(height), art(art) {}
    const int width, height;
    const Art* const art; // original art; required by the radial menus and fontText

    // The original UI space is 640x480 at uniform height scale, centred horizontally (hor+; tru.exe at 16:9
    // keeps the 4:3 layout in the middle): one virtual pixel in screen pixels, and virtual -> screen.
    float unit() const { return float(height) / 480.0f; }
    float sx(float x) const { return (float(width) - 640.0f * unit()) / 2 + x * unit(); }
    float sy(float y) const { return y * unit(); }

    void rect(float x, float y, float w, float h, uint32_t color); // screen pixels
    // `size`: 8x8 atlas glyph cell in pixels; text is atlasText()-converted.
    void text(float x, float y, float size, std::string_view utf8, uint32_t color);
    void textCentered(float cx, float y, float size, std::string_view utf8, uint32_t color);
    static float textWidth(std::string_view utf8, float size);

    // Screen-pixel quad sampling FrameOptions::uiImages[image] at uv [u0, u1] x [v0, v1].
    void image(int image, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t color);
    // Art texture `id` centred at virtual (cx, cy), `w` x `h` virtual pixels; flipX/flipY mirror it.
    void sprite(uint32_t id, float cx, float cy, float w, float h, uint32_t color, bool flipX = false, bool flipY = false);
    // Original font at virtual (x, y) = top-left of the glyph cells; `scale` multiplies the font's own scale.
    // Returns the advance (tru.exe Font_GetTextWidth): sum of (spacing + width) * scale - 1 per glyph.
    float fontText(float x, float y, float scale, std::string_view utf8, uint32_t color);
    float fontWidth(std::string_view utf8, float scale) const;

    std::vector<TextVertex> vertices;
};

// ---- menus ------------------------------------------------------------------------------------------
// Delete: Backspace on the Save/Load slot ring. Alt: the second PDA prompt key (E: Field Assistance TASK); Accept
// elsewhere.
enum class Nav : uint8_t { Up, Down, Left, Right, Accept, Back, Delete, Alt };
// Confirm: a YES / NO question (Item::confirm) over the backdrop alone, NO selected; YES runs the item. Revisit / Journal:
// the PDA's REVISIT LOCATION list (Treasure Hunt) and JOURNAL ENTRIES. Extras: the EXTRAS categories on a mirrored ring; ExtrasCategory: one's rewards
// (Menus::rewards) with a preview; ExtrasView: an unlocked image full screen. TailoringOptions: GAME OPTIONS > PLAYER
// TAILORING, its four levels one by one (Menus::tailor).
enum class Screen : uint8_t { None, Main, Pause, Load, Save, GameOptions, AudioVisual, Volume, Tailoring, Pda, Message, Confirm,
                              PdaSonar, PdaWeapons, PdaInventory, PdaItems, Revisit, Extras, ExtrasCategory, ExtrasView, Journal,
                              TailoringOptions };
// Continue: load the autosave, else the newest save (the app starts a new game when there is none). Save/Load/Delete act
// on selectedSave() (Save: empty = a new slot). Hint/Answer: PDA Field Assistance.
// Equip: PDA WEAPON SELECTION, the weapon in equipWeapon(). SonarReset: the sonar view's E (RESET VIEW). TreasureHunt: the
// main menu's centre after the ending. Revisit: start revisitChapter(). PlayMovie: EXTRAS TRAILERS & CREDITS, the
// Movies\ file in selectedSave().
enum class Action : uint8_t { None, Continue, NewGame, Resume, Save, Load, Delete, QuitToMenu, Quit, Hint, Answer, Equip, SonarReset,
                              TreasureHunt, Revisit, PlayMovie };

struct Item {
    int16_t stringId = 0;  // locals.bin label; 0 = `label`
    std::string label{};   // save slot names
    Action action = Action::None;
    Screen opens = Screen::None;
    int* value = nullptr;  // slider 0..10 (Left/Right)
    int* level = nullptr;  // PLAYER TAILORING level 0..2 (Left/Right; 0 = the lenient end, tailoringValue)
    bool reversed = false;
    uint32_t icon = 0;     // radial menus and save slots: generalbank texture id
    int16_t message = 0;   // opens Screen::Message with this locals.bin string
    bool* toggle = nullptr; // Left/Right/Accept flip it; shows onId / offId (both, "ON / OFF", when `pair`)
    int16_t onId = 0, offId = 0;
    bool pair = false;
    bool greyed = false;   // shown, never selected (PDA JOURNAL / REVISIT LOCATION)
    int16_t confirm = 0;   // Accept first asks this locals.bin YES / NO question (Screen::Confirm)
};

// A Load/Save slot: its file (save::slotPath), the name shown (empty: "<AREA> - <TITLE>", as tru.exe 0x789770 names
// autosaves), where it was saved (`area` = the chapter name, locals.bin 52..58; `title` = levelTitle) and when
// ("HH:MM:SS", "DD-MM-YYYY"). The autosave cannot be overwritten (message 17).
struct SaveSlot {
    std::string slot{}, name{};
    int16_t area = 0, title = 0;
    std::string time{}, date{};
    bool autosave = false;
};
inline constexpr std::string_view kAutosaveSlot = "autosave";
// Level title (locals.bin 386..407) of `unit` in chapter `chapter` (kChapters index): tru.exe keeps it in the scene
// object (+0x1B0, set per area; no exe table found). [O] chapter starts l2_cliffclimb 393, l4_trailstart 398,
// l5_startsnow 402; [I] the other units by name (docs/GAMEPLAY.md "Menus"). 0 = none.
int16_t levelTitle(int chapter, std::string_view unit);

// What the EXTRAS rewards unlock against (save::Profile): chapters ended (bit n = kChapters index n), the game finished,
// every treasure / relic found.
struct Unlocks {
    uint32_t chapters = 0;
    bool finished = false, treasures = false, relics = false;
};
// Reward unlock codes (ui::Reward::unlock) [O values per list; I meanings from what they show]: -1 always; 3..10 a
// chapter ended: 3 Mediterranean Sea, 4 the prologue, 5..10 Coastal Thailand .. Arctic Sea; 11 the game finished
// (storyboards); 12 every treasure (creatures); 13 every relic (Lara's outfits).
bool unlocked(int code, const Unlocks& u);
// JOURNAL entries (locals.bin 1145.., text id + 45) available in chapter `chapter` (kChapters index) [I: the entries by
// their topics up to that chapter's: 1145 Mediterranean Sea .. 1184..1189 Arctic Sea; tru.exe unlocks them by script].
int journalEntries(int chapter);
// PLAYER TAILORING value shown for `level` (0..2, Left = 0 the lenient end) [O tailor_* captures]: LESS -> / <- NORMAL -> /
// <- EXTRA (376 / 377 / 378); `reversed` (AMMUNITION CAPACITY, TIME FOR SAVING GRABS): EXTRA -> / NORMAL / <- LESS (379 /
// 377 / 380). The arrows are the strings' U+180A / U+180B font glyphs.
int16_t tailoringValue(int level, bool reversed);

// What the PDA shows: the chapter (kChapters index) and this chapter's relics/treasures found / in it; WEAPON
// SELECTION rows (the secondaries, game::Weapon index in `weapon`); INVENTORY items per category (BACKPACK, GEAR,
// WEAPONS, TREASURES). An item shows a g_pda picture (`picture`) or the side view of `model` (pc-w\<model>.drm,
// drawn by the app into the PDA background: Menus::pdaModel).
struct PdaWeapon {
    int weapon = 0;
    int16_t name = 0;
    int ammo = 0, max = 0;
    bool equipped = false;
    std::string model{}; // pc-w\<model>.drm
};
struct PdaItem {
    int16_t name = 0;     // locals.bin label
    std::string suffix{}; // after the label (" [3]")
    int16_t text = 0;     // description; 0 = `body`
    std::string body{};
    uint32_t picture = 0; // g_pda texture, 0 = `model`
    std::string model{};
};
struct PdaInfo {
    int chapter = 0;
    int relics = 0, relicsTotal = 0, treasures = 0, treasuresTotal = 0;
    bool sonar = false; // SONAR MAP: the area has a map (else "SONAR MAP IS CURRENTLY UNAVAILABLE")
    std::vector<PdaWeapon> weapons;
    std::array<std::vector<PdaItem>, 4> inventory;
};
// The model panel of WEAPON SELECTION / INVENTORY items: pc-w\<object>.drm drawn into the PDA background
// (pda_bg_1280 pixels; pda_bg_960 is the same 160 pixels further left).
struct PdaModel {
    std::string object;
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const PdaModel&) const = default;
};

// Loading screen background of a chapter (kChapters index): pc-w\image\<name>.raw [I mapping by area].
std::string_view loadingImage(int chapter);
// tru.exe loading screen: `image` = uiImages[1] (loadingImage), chapter title 51.. and story 59.. (docs "Menus").
void drawLoading(Canvas& c, const locale::Strings& s, int chapter);

// Stack of screens; Back pops back to the item that opened the screen (from Pause or the PDA it resumes; from Main
// it does nothing). Accept on an item runs its action or opens its screen; the app executes actions. Main and Pause
// are radial: item 0 sits in the centre, the others on the ring. As tru.exe: Left from the centre goes to the last
// ring item (first time: the one level with the centre), Right returns to the centre, Up/Down step along the
// ring and leave it at either end to the centre; Up/Down do nothing in the centre. Load/Save keep the ring and add
// a mirrored slot ring (page up, four slots, page down): Right/Accept enter it, Left leaves it.
class Menus {
public:
    Screen screen() const { return stack_.empty() ? Screen::None : stack_.back(); }
    bool mainMenu() const { return !stack_.empty() && stack_.front() == Screen::Main; }
    void open(Screen s);   // push
    void close();
    Action input(Nav n);
    void tick(double seconds); // selection animation
    int cursor() const { return cursor_; }
    std::vector<Item> items(); // of the current screen
    const std::string& selectedSave() const { return selected_; } // after Action::Load / Save / Delete
    int equipWeapon() const { return equip_; }                     // after Action::Equip: PdaWeapon::weapon
    std::optional<PdaModel> pdaModel() const;                      // the model panel the current PDA page shows
    int revisitChapter() const { return revisit_; }                 // after Action::Revisit: kChapters index 1..7
    // The pc-w\image raw the EXTRAS screens show in uiImages[2] (the selected reward, "locked_concept" while locked);
    // empty elsewhere.
    std::string extrasImage() const;

    std::vector<SaveSlot> saves;    // Load/Save slot entries (newest first); set before opening them
    int musicVolume = 8, sfxVolume = 8, cinematicVolume = 8; // 0..10, VOLUME SETTINGS
    int brightness = 5;             // 0..10 (5 = neutral; the app maps it to graphics brightness)
    bool reticles = true, helperButtons = true, trainingText = true, invertY = false, subtitles = true;
    int tailoring = 1;              // PLAYER TAILORING preset: 0 casual explorer, 1 tomb raider, 2 master survivalist
    // GAME OPTIONS > PLAYER TAILORING levels 0..2 (game::Tailoring): enemy health, ammunition capacity, damage to Lara,
    // time for saving grabs.
    std::array<int, 4> tailor{1, 1, 1, 1};
    PdaInfo pda;
    int journal = 0;                // PDA JOURNAL entries available (journalEntries); 0 = greyed
    // The main menu's centre [O]: TREASURE HUNT (49) once the game is `finished`, else RESUME GAME (122) with an autosave
    // in `saves`, else PLAY GAME (123). `treasureHunt`: the game being played is one (PDA REVISIT LOCATION).
    bool finished = false, treasureHunt = false;
    const Rewards* rewards = nullptr; // EXTRAS (ui::loadRewards); null = EXTRAS shows "Unavailable"
    Unlocks unlocks;

    // `keyName`: the key shown in the footer prompt (tru.exe shows the bound key's name, "Return").
    void draw(Canvas& c, const locale::Strings& s, std::string_view keyName = "Return");

private:
    // `opened`: under the Load/Save / EXTRAS ring, the centre still selected and labelled unless `centre` is false.
    void drawRadial(Canvas& c, const locale::Strings& s, Screen ring, int cursor, bool opened, bool centre = true);
    void drawSlots(Canvas& c, const locale::Strings& s);
    void drawList(Canvas& c, const locale::Strings& s);
    void drawTailoring(Canvas& c, const locale::Strings& s);
    void drawPda(Canvas& c, const locale::Strings& s);
    void drawSonar(Canvas& c, const locale::Strings& s);
    void drawMessage(Canvas& c, const locale::Strings& s);
    void drawConfirm(Canvas& c, const locale::Strings& s);
    void drawPdaList(Canvas& c, const locale::Strings& s);
    void drawExtras(Canvas& c, const locale::Strings& s);
    void drawExtrasCategory(Canvas& c, const locale::Strings& s);
    void drawExtrasView(Canvas& c);
    const std::vector<Reward>* rewardList() const; // the open EXTRAS category's
    std::vector<Item> itemsOf(Screen s);
    Action activate(Item it); // Accept on `it` of the current screen
    void back(); // pop, restoring the cursor of the screen below
    void select(int item);

    std::vector<Screen> stack_;
    std::vector<int> cursors_; // cursor of each screen below the top
    int cursor_ = 0, ring_ = -1; // ring_: last ring item (radial screens), -1 = none yet
    int page_ = 0;               // Load/Save slot page (four slots each)
    int16_t message_ = 0;
    Item pending_;         // Screen::Confirm: the item YES runs
    int category_ = 0; // PdaItems: the INVENTORY category it shows
    int equip_ = 0;
    int revisit_ = 1;
    int top_ = 0;          // PDA list pages, EXTRAS lists: first row shown
    int extras_ = 0;       // EXTRAS category (environment, character, gear, storyboards, trailers & credits)
    std::string selected_;
    std::array<float, 8> grow_{}; // per radial / slot item: 0 = idle .. 1 = selected (eased)
    float label_ = 0;             // label fade-in of the selected radial item, 0..1
};

// ---- HUD: the original display groups, reticles, notifications, title cards, subtitles ---------------
// Game state the HUD shows, filled by the app each tick (no ou_game dependency).
struct HudState {
    float health = 1000, healthMax = 1000; // absolute: the HUD script's colour bands are health values
    float adrenaline = 0;                  // 0..1
    float breath = 1;                      // 0..1; below 1 the adrenaline bar shows it (breath bar)
    int weapon = 0;                        // game::Weapon order: pistols, uzi, shotgun, speargun
    bool drawn = false, dual = true, dead = false;
    int clip[2] = {}, clipSize = 12;       // rounds in the right / left gun
    int nextHand = 0;                      // gun that fires next (its last round is highlighted)
    int healthPacks = 0;
    int gearItem = -1;                     // d-pad item pressed: 0 health pack, 1 flashlight, 2 weapon (draw/switch), 3 grenade
    bool flashlight = false;               // toggling it lights the d-pad's flashlight item
    int grenades = 0, grenadesMax = 0;     // grenade row under the clip pips; throwing lights the grenade item
};

// Combat reticle (screen pixels), drawn while a weapon is out.
struct Reticle {
    enum class Kind : uint8_t { None, Free, Target, Locked };
    Kind kind = Kind::None;
    float x = 0, y = 0;
    bool slowMo = false; // adrenaline head-shot window: ring + pip
    float pipX = 0, pipY = 0; // pip offset in reticle radii (+y up)
};

class Hud {
public:
    enum Group { Health, Weapons, Gear, kGroups };
    // HUD space (tru.exe captures): origin (5 % W, 5 % H) for the top groups, (5 % W, 95 % H) for the d-pad,
    // 1 virtual unit = H / 480, art 2/3 virtual unit per texel.
    static float unit(int height) { return float(height) / 480.0f; }
    static float originX(int width) { return 0.05f * float(width); }
    static float originY(int height, bool bottom) { return (bottom ? 0.95f : 0.05f) * float(height); }
    // Lara silhouette tint for a health value (HUD script bands 2000/1500/1000/500), 0xAABBGGRR.
    static uint32_t healthColor(float health);
    // In-game subtitle lines in the game font (TEXT.md §4: scale 0.6 at virtual y 352 / 374).
    static void subtitles(Canvas& c, std::string_view top, std::string_view bottom);

    void notify(std::string text, double seconds = 3.0) { toast_ = std::move(text), toastLeft_ = seconds; }
    void title(std::string text, std::string subtitle = {}, double seconds = 4.0) {
        title_ = std::move(text), subtitle_ = std::move(subtitle), titleLeft_ = seconds;
    }
    // Display groups appear on their triggers (damage, weapon drawn, d-pad...) and fade out after a hold.
    void update(const HudState& s, double dt);
    void tick(double dt) { toastLeft_ -= dt, titleLeft_ -= dt; }
    float alpha(Group g) const { return alpha_[g]; }
    void draw(Canvas& c) const;

    Reticle reticle;                       // set each frame
    std::optional<std::array<float, 2>> grapple; // grapple target marker, screen pixels; set each frame

private:
    HudState s_, prev_;
    bool first_ = true;
    int lit_ = -1; // d-pad item drawn bright while the GEAR group shows
    std::array<float, kGroups> alpha_{}, hold_{};
    std::string toast_, title_, subtitle_;
    double toastLeft_ = 0, titleLeft_ = 0;
};

} // namespace ou::ui
