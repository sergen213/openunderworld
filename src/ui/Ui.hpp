// Front-end and in-game UI: main/pause/load/options menus, the PDA page, pickup notifications and chapter
// title cards. Renderer-agnostic: draws into render::TextVertex quads (FrameOptions::ui) with the renderer's
// 8x8 atlas font. Text is the original locals.bin strings; the original screens are Scaleform/UI scripts
// (cdc::UIPropertyLocalStr, TEXT.md §1), so layouts here are approximations [I]. docs/GAMEPLAY.md "Menus".
#pragma once

#include "render/Renderer.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ou::locale { class Strings; }

namespace ou::ui {

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
    Canvas(int width, int height) : width(width), height(height) {}
    const int width, height;
    // Original UI space is 640x480 (TEXT.md §4): one virtual pixel in screen pixels.
    float unit() const { return float(height) / 480.0f; }
    void rect(float x, float y, float w, float h, uint32_t color);
    // `size`: glyph cell in pixels; text is atlasText()-converted.
    void text(float x, float y, float size, std::string_view utf8, uint32_t color);
    void textCentered(float cx, float y, float size, std::string_view utf8, uint32_t color);
    static float textWidth(std::string_view utf8, float size);
    std::vector<TextVertex> vertices;
};

// ---- menus ------------------------------------------------------------------------------------------
enum class Nav : uint8_t { Up, Down, Left, Right, Accept, Back };
enum class Screen : uint8_t { None, Main, Pause, Load, Options, Pda };
enum class Action : uint8_t { None, NewGame, Resume, Save, Load, QuitToMenu, Quit };

struct Item {
    int16_t stringId = 0;  // locals.bin label; 0 = `label`
    std::string label;     // save slot names
    Action action = Action::None;
    Screen opens = Screen::None;
    int* value = nullptr;  // Options sliders 0..10 (Left/Right)
};

// What the PDA page shows.
struct PdaInfo {
    int16_t chapterNameId = 0;
    int treasures = 0, relics = 0, healthPacks = 0; // collected so far (whole game)
};

// Stack of screens; Back pops (from Pause it resumes; from Main it does nothing). Accept on an item runs
// its action or opens its screen; the app executes actions.
class Menus {
public:
    Screen screen() const { return stack_.empty() ? Screen::None : stack_.back(); }
    void open(Screen s);   // push
    void close() { stack_.clear(); }
    Action input(Nav n);
    int cursor() const { return cursor_; }
    std::vector<Item> items(); // of the current screen
    const std::string& selectedSave() const { return selected_; } // after Action::Load

    std::vector<std::string> saves; // Load screen entries (slot names, newest first); set before opening it
    int musicVolume = 8, sfxVolume = 8; // 0..10, Options
    PdaInfo pda;

    void draw(Canvas& c, const locale::Strings& s);

private:
    std::vector<Screen> stack_;
    int cursor_ = 0;
    std::string selected_;
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
