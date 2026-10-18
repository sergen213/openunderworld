// Menus, PDA page and HUD drawn as atlas-font quads. Layout [I]; strings and their ids: docs/GAMEPLAY.md "Menus".
#include "ui/Ui.hpp"

#include "locale/Locale.hpp"
#include "render/RenderCommon.hpp"

#include <algorithm>
#include <cmath>

namespace ou::ui {

std::string atlasText(std::string_view s) {
    // Latin-1 U+00C0..U+00FF without accents ('?' for the few with no ASCII base).
    static constexpr std::string_view kLatin1 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const auto c = uint8_t(s[i]);
        uint32_t cp = c;
        size_t n = 1;
        if (c >= 0xF0) n = 4, cp = c & 0x07;
        else if (c >= 0xE0) n = 3, cp = c & 0x0F;
        else if (c >= 0xC0) n = 2, cp = c & 0x1F;
        for (size_t k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (uint8_t(s[i + k]) & 0x3F);
        i += n;
        if (cp >= 32 && cp < 127) out += char(cp);
        else if (cp >= 0xC0 && cp <= 0xFF) out += kLatin1[cp - 0xC0];
        else if (cp == 0x2026) out += "...";
        else if (cp == 0x2018 || cp == 0x2019) out += '\'';
        else if (cp == 0x201C || cp == 0x201D) out += '"';
        else if ((cp >= 0x1800 && cp <= 0x18FF) || cp < 32) continue; // button glyphs, controls
        else out += '?';
    }
    return out;
}

std::vector<std::string> wrap(std::string_view text, size_t cols) {
    std::string t;
    for (size_t i = 0; i < text.size(); ++i) // locals.bin stores line breaks as the two characters "\n"
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') t += '\n', ++i;
        else t += text[i];
    std::vector<std::string> lines;
    for (size_t start = 0; start <= t.size();) {
        size_t end = t.find('\n', start);
        if (end == std::string::npos) end = t.size();
        std::string_view para(t.data() + start, end - start);
        std::string line;
        for (size_t p = 0; p < para.size();) {
            const size_t q = std::min(para.find(' ', p), para.size());
            const std::string_view w = para.substr(p, q - p);
            if (!line.empty() && line.size() + 1 + w.size() > cols) lines.push_back(std::move(line)), line.clear();
            if (!line.empty()) line += ' ';
            line += w;
            p = q + 1;
        }
        lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
}

void Canvas::rect(float x, float y, float w, float h, uint32_t c) {
    vertices.insert(vertices.end(), {{x, y, -1, -1, c}, {x + w, y, -1, -1, c}, {x + w, y + h, -1, -1, c},
                                     {x, y, -1, -1, c}, {x + w, y + h, -1, -1, c}, {x, y + h, -1, -1, c}});
}

void Canvas::text(float x, float y, float g, std::string_view utf8, uint32_t c) {
    using namespace render;
    for (const char ch : atlasText(utf8)) {
        const int i = ch - 32;
        const float u0 = float(i % kAtlasCols * kGlyph) / kAtlasW, v0 = float(i / kAtlasCols * kGlyph) / kAtlasH;
        const float u1 = u0 + float(kGlyph) / kAtlasW, v1 = v0 + float(kGlyph) / kAtlasH;
        vertices.insert(vertices.end(), {{x, y, u0, v0, c}, {x + g, y, u1, v0, c}, {x + g, y + g, u1, v1, c},
                                         {x, y, u0, v0, c}, {x + g, y + g, u1, v1, c}, {x, y + g, u0, v1, c}});
        x += g;
    }
}

float Canvas::textWidth(std::string_view utf8, float g) { return float(atlasText(utf8).size()) * g; }

void Canvas::textCentered(float cx, float y, float g, std::string_view utf8, uint32_t c) {
    text(std::round(cx - textWidth(utf8, g) / 2), y, g, utf8, c);
}

// ---- menus ------------------------------------------------------------------------------------------

void Menus::open(Screen s) {
    stack_.push_back(s);
    cursor_ = 0;
}

std::vector<Item> Menus::items() {
    switch (screen()) {
    case Screen::Main:
        return {{124, {}, Action::NewGame}, {126, {}, Action::None, Screen::Load}, {125, {}, Action::None, Screen::Options},
                {131, {}, Action::Quit}};
    case Screen::Pause:
        return {{122, {}, Action::Resume}, {127, {}, Action::Save}, {126, {}, Action::None, Screen::Load},
                {1050, {}, Action::None, Screen::Pda}, {125, {}, Action::None, Screen::Options}, {1062, {}, Action::QuitToMenu}};
    case Screen::Load: {
        std::vector<Item> v;
        for (const auto& s : saves) v.push_back({0, s, Action::Load});
        v.push_back({469, {}});
        return v;
    }
    case Screen::Options: return {{487, {}, Action::None, Screen::None, &sfxVolume}, {488, {}, Action::None, Screen::None, &musicVolume}, {469, {}}};
    case Screen::Pda: return {{469, {}}};
    case Screen::None: break;
    }
    return {};
}

Action Menus::input(Nav n) {
    auto list = items();
    if (list.empty()) return Action::None;
    const int count = int(list.size());
    switch (n) {
    case Nav::Up: cursor_ = (cursor_ + count - 1) % count; return Action::None;
    case Nav::Down: cursor_ = (cursor_ + 1) % count; return Action::None;
    case Nav::Left:
    case Nav::Right:
        if (int* v = list[size_t(cursor_)].value) *v = std::clamp(*v + (n == Nav::Right ? 1 : -1), 0, 10);
        return Action::None;
    case Nav::Back:
        if (screen() == Screen::Main) return Action::None;
        if (screen() == Screen::Pause) { close(); return Action::Resume; }
        stack_.pop_back(), cursor_ = 0;
        return Action::None;
    case Nav::Accept: {
        const Item& it = list[size_t(cursor_)];
        if (it.opens != Screen::None) { open(it.opens); return Action::None; }
        if (it.action == Action::None) { // BACK / sliders
            if (!it.value) return input(Nav::Back);
            return Action::None;
        }
        if (it.action == Action::Load) selected_ = it.label;
        if (it.action == Action::Resume || it.action == Action::NewGame || it.action == Action::Load) close();
        return it.action;
    }
    }
    return Action::None;
}

void Menus::draw(Canvas& c, const locale::Strings& s) {
    if (screen() == Screen::None) return;
    const float u = c.unit(), cx = float(c.width) / 2;
    const float big = std::round(16 * u), mid = std::round(10 * u), small = std::round(8 * u);
    c.rect(0, 0, float(c.width), float(c.height), screen() == Screen::Main ? 0xE0000000u : kShade);
    int16_t titleId = 0;
    switch (screen()) {
    case Screen::Main: titleId = 51; break;   // TOMB RAIDER: UNDERWORLD
    case Screen::Pause: titleId = 1049; break; // PAUSE
    case Screen::Load: titleId = 126; break;   // LOAD GAME
    case Screen::Options: titleId = 491; break; // OPTIONS
    case Screen::Pda: titleId = pda.chapterNameId; break;
    case Screen::None: break;
    }
    c.textCentered(cx, 96 * u, big, s.get(titleId), kGold);
    float y = 170 * u;
    if (screen() == Screen::Pda) {
        c.textCentered(cx, 140 * u, small, s.get(1075), kGrey); // AREA INFO
        const std::pair<int16_t, int> rows[] = {{1073, pda.treasures}, {1072, pda.relics}, {1274, pda.healthPacks}};
        for (const auto& [id, n] : rows) {
            c.text(cx - 150 * u, y, mid, s.get(id), kWhite);
            const std::string v = std::to_string(n);
            c.text(cx + 150 * u - Canvas::textWidth(v, mid), y, mid, v, kWhite);
            y += 24 * u;
        }
        y += 24 * u;
    }
    const auto list = items();
    if (screen() == Screen::Load && saves.empty()) {
        c.textCentered(cx, y, mid, s.get(456), kGrey), y += 24 * u; // Unavailable
    }
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& it = list[i];
        std::string label = it.stringId ? std::string(s.get(it.stringId)) : it.label;
        if (it.value) label += "  " + std::string(size_t(*it.value), '|') + std::string(size_t(10 - *it.value), '.');
        const bool sel = int(i) == cursor_;
        if (sel) c.rect(cx - 170 * u, y - 4 * u, 340 * u, mid + 8 * u, 0x60FFFFFFu);
        c.textCentered(cx, y, mid, label, sel ? kGold : kWhite);
        y += 22 * u;
    }
}

void Hud::draw(Canvas& c) const {
    const float u = c.unit(), cx = float(c.width) / 2;
    auto fade = [](double left) { return float(std::clamp(left / 0.5, 0.0, 1.0)); }; // last half second fades out
    auto alpha = [](uint32_t col, float a) { return (col & 0x00FFFFFFu) | (uint32_t(float(col >> 24) * a) << 24); };
    if (titleLeft_ > 0) {
        const float a = fade(titleLeft_), g = std::round(18 * u);
        c.rect(0, 200 * u, float(c.width), 80 * u, alpha(kShade, a));
        c.textCentered(cx, 214 * u, g, title_, alpha(kGold, a));
        if (!subtitle_.empty()) c.textCentered(cx, 250 * u, std::round(9 * u), subtitle_, alpha(kWhite, a));
    }
    if (toastLeft_ > 0) {
        const float a = fade(toastLeft_), g = std::round(10 * u);
        const float w = Canvas::textWidth(toast_, g) + 24 * u;
        c.rect(cx - w / 2, 400 * u, w, g + 16 * u, alpha(kShade, a));
        c.textCentered(cx, 408 * u, g, toast_, alpha(kWhite, a));
    }
}

} // namespace ou::ui
