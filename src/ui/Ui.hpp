// Front-end and in-game UI: main/pause/load/options menus, the PDA page, pickup notifications and chapter
// title cards. Renderer-agnostic: draws into render::TextVertex quads (FrameOptions::ui). Main and pause menus
// are the original radial menu drawn with the game's own art and font (ui::Art, generalbank.drm) at the layout
// measured from tru.exe captures; submenus, HUD and subtitles use the renderer's 8x8 atlas font. Text is the
// original locals.bin strings. The original UI scripts are not executed. docs/GAMEPLAY.md "Menus".
#pragma once

#include "render/Renderer.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ou::locale { class Strings; }

namespace ou::ui {

struct Art;
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
enum class Nav : uint8_t { Up, Down, Left, Right, Accept, Back };
enum class Screen : uint8_t { None, Main, Pause, Load, Options, Pda, Message };
// Continue: load the newest save (the app starts a new game when there is none).
enum class Action : uint8_t { None, Continue, NewGame, Resume, Save, Load, QuitToMenu, Quit };

struct Item {
    int16_t stringId = 0;  // locals.bin label; 0 = `label`
    std::string label;     // save slot names
    Action action = Action::None;
    Screen opens = Screen::None;
    int* value = nullptr;  // Options sliders 0..10 (Left/Right)
    uint32_t icon = 0;     // radial menus: generalbank texture id
    int16_t message = 0;   // opens Screen::Message with this locals.bin string
};

// What the PDA page shows.
struct PdaInfo {
    int16_t chapterNameId = 0;
    int treasures = 0, relics = 0, healthPacks = 0; // collected so far (whole game)
};

// Stack of screens; Back pops back to the item that opened the screen (from Pause it resumes; from Main it does
// nothing). Accept on an item runs its action or opens its screen; the app executes actions. Main and Pause are
// radial: item 0 sits in the centre, the others on the ring. As tru.exe: Left from the centre goes to the last
// ring item (first time: the one level with the centre), Right returns to the centre, Up/Down step along the
// ring and leave it at either end to the centre; Up/Down do nothing in the centre.
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
    const std::string& selectedSave() const { return selected_; } // after Action::Load

    std::vector<std::string> saves; // Load screen entries (slot names, newest first); set before opening it
    int musicVolume = 8, sfxVolume = 8; // 0..10, Options
    PdaInfo pda;

    // `keyName`: the key shown in the footer prompt (tru.exe shows the bound key's name, "Return").
    void draw(Canvas& c, const locale::Strings& s, std::string_view keyName = "Return");

private:
    void drawRadial(Canvas& c, const locale::Strings& s);
    void drawMessage(Canvas& c, const locale::Strings& s);
    void back(); // pop, restoring the cursor of the screen below
    void select(int item);

    std::vector<Screen> stack_;
    std::vector<int> cursors_; // cursor of each screen below the top
    int cursor_ = 0, ring_ = -1; // ring_: last ring item (radial screens), -1 = none yet
    int16_t message_ = 0;
    std::string selected_;
    std::array<float, 8> grow_{}; // per radial item: 0 = idle .. 1 = selected (eased)
    float label_ = 0;             // label fade-in of the selected radial item, 0..1
};

// ---- HUD: pickup notifications and chapter title cards ------------------------------------------------
class Hud {
public:
    void notify(std::string text, double seconds = 3.0) { toast_ = std::move(text), toastLeft_ = seconds; }
    void title(std::string text, std::string subtitle = {}, double seconds = 4.0) {
        title_ = std::move(text), subtitle_ = std::move(subtitle), titleLeft_ = seconds;
    }
    void tick(double dt) { toastLeft_ -= dt, titleLeft_ -= dt; }
    bool showing() const { return toastLeft_ > 0 || titleLeft_ > 0; }
    void draw(Canvas& c) const;

private:
    std::string toast_, title_, subtitle_;
    double toastLeft_ = 0, titleLeft_ = 0;
};

} // namespace ou::ui
