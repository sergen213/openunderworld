// Menus, PDA page and HUD as quads: the original radial menu from ui::Art, the rest in the 8x8 atlas font.
// Strings and ids, measured layout: docs/GAMEPLAY.md "Menus".
#include "ui/Ui.hpp"

#include "locale/Locale.hpp"
#include "render/RenderCommon.hpp"
#include "ui/Art.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

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

void Canvas::image(int img, float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t c) {
    const float o = float(2 * (img + 1)); // TextVertex: uiImages[img] at u - 2 (img + 1)
    vertices.insert(vertices.end(), {{x, y, u0 + o, v0, c}, {x + w, y, u1 + o, v0, c}, {x + w, y + h, u1 + o, v1, c},
                                     {x, y, u0 + o, v0, c}, {x + w, y + h, u1 + o, v1, c}, {x, y + h, u0 + o, v1, c}});
}

void Canvas::sprite(uint32_t id, float cx, float cy, float w, float h, uint32_t c, bool flipX, bool flipY) {
    const AtlasRect* r = art ? art->find(id) : nullptr;
    if (!r) return;
    float u0 = r->u0, u1 = r->u1, v0 = r->v0, v1 = r->v1;
    if (flipX) std::swap(u0, u1);
    if (flipY) std::swap(v0, v1);
    image(0, sx(cx - w / 2), sy(cy - h / 2), w * unit(), h * unit(), u0, v0, u1, v1, c);
}

float Canvas::fontWidth(std::string_view utf8, float scale) const {
    if (!art) return 0;
    const Font& f = art->font;
    float w = 0;
    for (const char32_t cp : decodeUtf8(utf8))
        if (const Glyph* g = f.find(uint32_t(cp))) w += float(f.spacing + g->width) * f.scaleX * scale - 1;
    return w;
}

float Canvas::fontText(float x, float y, float scale, std::string_view utf8, uint32_t c) {
    if (!art) return 0;
    const Font& f = art->font;
    const AtlasRect* pages[2] = {art->find(f.texture), art->find(f.texture2)};
    const float x0 = x, h = float(f.cellHeight) * f.scaleY * scale;
    for (const char32_t cp : decodeUtf8(utf8)) {
        const Glyph* g = f.find(uint32_t(cp));
        if (!g) continue;
        if (const AtlasRect* p = pages[g->second ? 1 : 0]) {
            const float du = (p->u1 - p->u0) / float(p->width), dv = (p->v1 - p->v0) / float(p->height);
            const float u0 = p->u0 + float(g->x) * du, v0 = p->v0 + float(g->y) * dv;
            const float w = float(g->width) * f.scaleX * scale;
            image(0, sx(x), sy(y), w * unit(), h * unit(), u0, v0, u0 + float(g->width) * du, v0 + float(f.cellHeight) * dv, c);
        }
        x += float(f.spacing + g->width) * f.scaleX * scale - 1;
    }
    return x - x0;
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

namespace {

// generalbank.drm texture section ids (ui::Art).
constexpr uint32_t kGlow = 11, kBarCentre = 12, kBarLeft = 13, kDisc = 15, kFrontArt = 17, kRingShade = 50,
                   kBlackDisc = 51, kDialog = 106, kLine = 105;
constexpr uint32_t kIconPlay = 18, kIconNew = 19, kIconOptions = 20, kIconLoad = 21, kIconSave = 22, kIconAv = 23,
                   kIconExtras = 24, kIconQuit = 26, kIconPda = 44;

// Layout in the 640x480 virtual space, measured from tru.exe at 1024x768 (4:3) and 1920x1080 (16:9) with the
// art textures fitted to the captures (docs/GAMEPLAY.md "Menus").
constexpr float kTexel = 0.67f;                       // virtual pixels per texel: discs, icons, glow
constexpr float kRingX = 320.3f, kRingY = 240.3f;     // ring centre = centre item
constexpr float kRingRadius = 136.8f, kRingStep = 180.0f / 7; // ring item k at -90 - (k - 1) * step degrees
constexpr float kArtX = 317.5f, kArtY = 242.25f, kArtSize = 672.0f; // texture 17, the glyph art
constexpr float kArtAlpha = 0.75f;                    // its lines are ~3/4 as bright as the texture over black
constexpr float kShadeQuarter = 208.0f;               // texture 50 quarter ring (inner edge at radius ~119)
constexpr float kSelDisc = 1.17f, kSelIcon = 1.125f;  // selected ring item; the selected centre item stays 1
// Glow (texture 11) behind the selected item: scale and opacity fitted to its fall-off in the captures.
constexpr float kGlowCentre = 1.17f, kGlowRing = 1.22f, kGlowAlpha = 0.7f;
constexpr float kBarW = 189.4f, kBarH = 23.7f;        // centre label bar (texture 12)
constexpr float kSideBarW = 240.0f, kSideBarEnd = 16; // ring label bar (texture 13): width, right end under the disc
constexpr float kTextScale = 0.625f;                  // x the font's 0.5: cap height 8.75
constexpr float kCapMid = 7.2f;                       // cell top -> middle of the capitals at kTextScale
constexpr float kLabelGap = 53.6f;                    // ring label: right ink edge to the item centre
constexpr float kFooterY = 434.1f, kFooterLine0 = 432.0f, kFooterLine1 = 450.5f; // prompt cell top, lines
// Message box: texture 106 (its box covers texels 202..822 x 5..250), text at 0.74 with 16-pixel lines.
constexpr float kDialogY = 214.7f, kDialogW = 672, kDialogH = 210, kDialogText = 0.74f, kDialogTop = 179.8f;

constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) { return r | g << 8 | b << 16 | a << 24; }
constexpr uint32_t kIconBlue = rgba(50, 115, 185), kLabel = rgba(203, 203, 203), kKeyBlue = rgba(99, 209, 247),
                   kLineBlue = rgba(43, 105, 140), kButtonBlue = rgba(25, 73, 103), kBlack = rgba(0, 0, 0);

uint32_t withAlpha(uint32_t c, float a) { return (c & 0x00FFFFFFu) | uint32_t(std::clamp(a, 0.0f, 1.0f) * float(c >> 24)) << 24; }
uint32_t mix(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8)
        out |= uint32_t(std::lround(float((a >> s) & 0xFF) * (1 - t) + float((b >> s) & 0xFF) * t)) << s;
    return out;
}

// Ring position of radial item `i` (0 = centre).
std::pair<float, float> radialPos(size_t i) {
    if (i == 0) return {kRingX, kRingY};
    const float a = (-90.0f - float(i - 1) * kRingStep) * 3.14159265359f / 180.0f;
    return {kRingX + std::cos(a) * kRingRadius, kRingY + std::sin(a) * kRingRadius};
}

// The ring item level with the centre: where Left goes first (tru.exe: SAVE GAME of the main menu).
int levelRingItem(size_t count) {
    int best = 1;
    for (size_t i = 1; i < count; ++i)
        if (std::abs(radialPos(i).second - kRingY) < std::abs(radialPos(size_t(best)).second - kRingY)) best = int(i);
    return best;
}

// locals.bin prompts start with an action placeholder (U+1800..U+18FF) that the game replaces with the bound
// control: here the key name in blue, then the rest in white. Draws centred on `cx`; returns nothing.
void prompt(Canvas& c, float cx, float y, std::string_view key, std::string_view text) {
    std::string rest(text);
    if (rest.size() >= 3 && uint8_t(rest[0]) == 0xE1 && (uint8_t(rest[1]) == 0xA0 || uint8_t(rest[1]) == 0xA1)) rest.erase(0, 3);
    const float w = c.fontWidth(key, kTextScale) + c.fontWidth(rest, kTextScale);
    const float x = cx - w / 2;
    c.fontText(x + c.fontText(x, y, kTextScale, key, kKeyBlue), y, kTextScale, rest, kWhite);
}

void footer(Canvas& c, const locale::Strings& s, std::string_view key, bool back) {
    c.sprite(kLine, 320, kFooterLine0, 1024 * kTexel, 1.8f, kLineBlue);
    c.sprite(kLine, 320, kFooterLine1, 1024 * kTexel, 1.8f, kLineBlue);
    if (!back) return prompt(c, 320, kFooterY, key, s.get(120)); // "<select> SELECT"
    prompt(c, 270, kFooterY, key, s.get(120));
    prompt(c, 370, kFooterY, "Escape", s.get(375)); // "<back> BACK"
}

// Breaks `text` (locals.bin "\n" escapes too) into lines no wider than `width` virtual pixels.
std::vector<std::string> fontWrap(const Canvas& c, std::string_view text, float width, float scale) {
    std::vector<std::string> lines;
    for (const auto& para : wrap(text, 1u << 20)) {
        std::string line;
        for (size_t p = 0; p <= para.size();) {
            const size_t q = std::min(para.find(' ', p), para.size());
            const std::string word = para.substr(p, q - p);
            const std::string next = line.empty() ? word : line + ' ' + word;
            if (!line.empty() && c.fontWidth(next, scale) > width) lines.push_back(line), line = word;
            else line = next;
            p = q + 1;
        }
        lines.push_back(line);
    }
    return lines;
}

} // namespace

void Menus::open(Screen s) {
    cursors_.push_back(cursor_);
    stack_.push_back(s);
    cursor_ = 0, ring_ = -1, label_ = 0;
    grow_.fill(0);
    if (s == Screen::Main || s == Screen::Pause) grow_[0] = 1;
}

void Menus::close() {
    stack_.clear(), cursors_.clear();
}

void Menus::back() {
    stack_.pop_back();
    cursor_ = cursors_.empty() ? 0 : cursors_.back();
    if (!cursors_.empty()) cursors_.pop_back();
    label_ = 1;
}

void Menus::select(int item) {
    if (item == cursor_) return;
    cursor_ = item, label_ = 0;
    if (item > 0) ring_ = item;
}

void Menus::tick(double seconds) {
    const bool radial = screen() == Screen::Main || screen() == Screen::Pause;
    for (size_t i = 0; i < grow_.size(); ++i) { // ~0.15 s to grow / shrink
        const float target = radial && int(i) == cursor_ ? 1.0f : 0.0f;
        const float step = float(seconds) / 0.15f;
        grow_[i] = grow_[i] < target ? std::min(target, grow_[i] + step) : std::max(target, grow_[i] - step);
    }
    label_ = std::min(1.0f, label_ + float(seconds) / 0.5f); // label fades in over ~0.5 s
}

std::vector<Item> Menus::items() {
    switch (screen()) {
    case Screen::Main:
        return {{123, {}, Action::Continue, Screen::None, nullptr, kIconPlay},
                {124, {}, Action::NewGame, Screen::None, nullptr, kIconNew},
                {125, {}, Action::None, Screen::Options, nullptr, kIconOptions},
                {126, {}, Action::None, Screen::Load, nullptr, kIconLoad},
                {127, {}, Action::None, Screen::Message, nullptr, kIconSave, 161}, // "only ... in gameplay mode"
                {128, {}, Action::None, Screen::Options, nullptr, kIconAv},
                {129, {}, Action::None, Screen::Message, nullptr, kIconExtras, 456}, // "Unavailable" (not ported)
                {131, {}, Action::Quit, Screen::None, nullptr, kIconQuit}};
    case Screen::Pause:
        return {{122, {}, Action::Resume, Screen::None, nullptr, kIconPlay},
                {127, {}, Action::Save, Screen::None, nullptr, kIconSave},
                {126, {}, Action::None, Screen::Load, nullptr, kIconLoad},
                {1050, {}, Action::None, Screen::Pda, nullptr, kIconPda},
                {125, {}, Action::None, Screen::Options, nullptr, kIconOptions},
                {1062, {}, Action::QuitToMenu, Screen::None, nullptr, kIconQuit}};
    case Screen::Load: {
        std::vector<Item> v;
        for (const auto& s : saves) v.push_back({0, s, Action::Load});
        v.push_back({469, {}});
        return v;
    }
    case Screen::Options: return {{487, {}, Action::None, Screen::None, &sfxVolume}, {488, {}, Action::None, Screen::None, &musicVolume}, {469, {}}};
    case Screen::Pda: return {{469, {}}};
    case Screen::Message: return {{383, {}}}; // OK
    case Screen::None: break;
    }
    return {};
}

Action Menus::input(Nav n) {
    auto list = items();
    if (list.empty()) return Action::None;
    const int count = int(list.size());
    const bool radial = screen() == Screen::Main || screen() == Screen::Pause;
    switch (n) {
    case Nav::Up:
    case Nav::Down:
        if (!radial) select((cursor_ + (n == Nav::Up ? count - 1 : 1)) % count);
        else if (cursor_ > 0) select((cursor_ + (n == Nav::Up ? -1 : 1) + count) % count); // ring ends lead to the centre
        return Action::None;
    case Nav::Left:
    case Nav::Right:
        if (radial) {
            if (n == Nav::Left && cursor_ == 0) select(ring_ > 0 ? ring_ : levelRingItem(list.size()));
            else if (n == Nav::Right) select(0);
        } else if (int* v = list[size_t(cursor_)].value) *v = std::clamp(*v + (n == Nav::Right ? 1 : -1), 0, 10);
        return Action::None;
    case Nav::Back:
        if (screen() == Screen::Main) return Action::None;
        if (screen() == Screen::Pause) { close(); return Action::Resume; }
        back();
        return Action::None;
    case Nav::Accept: {
        const Item& it = list[size_t(cursor_)];
        if (it.opens != Screen::None) {
            if (it.opens == Screen::Message) message_ = it.message;
            open(it.opens);
            return Action::None;
        }
        if (it.action == Action::None) { // BACK / OK / sliders
            if (!it.value) return input(Nav::Back);
            return Action::None;
        }
        if (it.action == Action::Load) selected_ = it.label;
        if (it.action == Action::Resume || it.action == Action::NewGame || it.action == Action::Load || it.action == Action::Continue)
            close();
        return it.action;
    }
    }
    return Action::None;
}

void Menus::drawRadial(Canvas& c, const locale::Strings& s) {
    const auto list = items();
    c.sprite(kFrontArt, kArtX, kArtY, kArtSize, kArtSize, withAlpha(kWhite, kArtAlpha));
    for (int q = 0; q < 4; ++q) // shadow ring behind the items: one quarter texture, mirrored
        c.sprite(kRingShade, kRingX + (q & 1 ? 0.5f : -0.5f) * kShadeQuarter, kRingY + (q & 2 ? 0.5f : -0.5f) * kShadeQuarter,
                 kShadeQuarter, kShadeQuarter, kWhite, q & 1, q & 2);
    // Unselected first, the growing/selected ones on top.
    std::vector<size_t> order(list.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return grow_[a] < grow_[b]; });
    for (const size_t i : order) {
        const auto [x, y] = radialPos(i);
        const float g = grow_[i];
        const float disc = 128 * kTexel, icon = 64 * kTexel;
        const float sDisc = 1 + (i ? kSelDisc - 1 : 0) * g, sIcon = 1 + (i ? kSelIcon - 1 : 0) * g;
        const float sGlow = i ? kGlowRing : kGlowCentre;
        if (g > 0) c.sprite(kGlow, x, y, disc * sGlow, disc * sGlow, withAlpha(kWhite, g * kGlowAlpha));
        c.sprite(kDisc, x, y, disc * sDisc, disc * sDisc, kWhite);
        if (g > 0) c.sprite(kBlackDisc, x, y, disc * sDisc, disc * sDisc, withAlpha(kBlack, g));
        c.sprite(list[i].icon, x, y, icon * sIcon, icon * sIcon, mix(kIconBlue, kWhite, g));
    }
    if (cursor_ < 0 || size_t(cursor_) >= list.size()) return;
    const std::string label(s.get(list[size_t(cursor_)].stringId));
    const auto [x, y] = radialPos(size_t(cursor_));
    const uint32_t text = withAlpha(kLabel, label_);
    if (cursor_ == 0) {
        c.sprite(kBarCentre, 320, 199.1f, kBarW, kBarH, withAlpha(kWhite, label_));
        c.fontText(320 - c.fontWidth(label, kTextScale) / 2, 192.4f, kTextScale, label, text);
    } else {
        c.sprite(kBarLeft, x - kSideBarEnd - kSideBarW / 2, y, kSideBarW, kBarH, withAlpha(kWhite, label_));
        const float w = c.fontWidth(label, kTextScale);
        c.fontText(x - kLabelGap - w, y - kCapMid, kTextScale, label, text);
    }
}

void Menus::drawMessage(Canvas& c, const locale::Strings& s) {
    c.sprite(kDialog, 320, kDialogY, kDialogW, kDialogH, kWhite);
    float y = kDialogTop;
    for (const auto& l : fontWrap(c, s.get(message_), 320, kDialogText))
        c.fontText(320 - c.fontWidth(l, kDialogText) / 2, y, kDialogText, l, kWhite), y += 16;
    // OK button: a blue bar fading out at both ends.
    const float x0 = c.sx(264), x1 = c.sx(286), x2 = c.sx(354), x3 = c.sx(376), y0 = c.sy(282.2f), y1 = c.sy(298.2f);
    const uint32_t solid = kButtonBlue, clear = withAlpha(kButtonBlue, 0);
    c.vertices.insert(c.vertices.end(), {{x0, y0, -1, -1, clear}, {x1, y0, -1, -1, solid}, {x1, y1, -1, -1, solid},
                                         {x0, y0, -1, -1, clear}, {x1, y1, -1, -1, solid}, {x0, y1, -1, -1, clear}});
    c.rect(x1, y0, x2 - x1, y1 - y0, solid);
    c.vertices.insert(c.vertices.end(), {{x2, y0, -1, -1, solid}, {x3, y0, -1, -1, clear}, {x3, y1, -1, -1, clear},
                                         {x2, y0, -1, -1, solid}, {x3, y1, -1, -1, clear}, {x2, y1, -1, -1, solid}});
    const std::string ok(s.get(383));
    c.fontText(320 - c.fontWidth(ok, kDialogText) / 2, 290.2f - kCapMid * kDialogText / kTextScale, kDialogText, ok, kWhite);
}

void Menus::draw(Canvas& c, const locale::Strings& s, std::string_view key) {
    if (screen() == Screen::None) return;
    const float u = c.unit(), cx = float(c.width) / 2;
    const float mid = std::round(10 * u), small = std::round(8 * u);
    if (screen() == Screen::Main || screen() == Screen::Pause) {
        if (screen() == Screen::Pause) c.rect(0, 0, float(c.width), float(c.height), 0x70000000u);
        drawRadial(c, s);
        footer(c, s, key, false);
        return;
    }
    if (screen() == Screen::Message) {
        drawMessage(c, s);
        footer(c, s, key, false);
        return;
    }

    const auto list = items();
    const float panelX = cx - 205 * u, panelW = 410 * u;
    c.rect(panelX, 66 * u, panelW, 320 * u, 0xD0000000u);
    c.rect(panelX, 66 * u, panelW, 2 * u, kIconBlue);
    c.rect(panelX, 384 * u, panelW, 2 * u, kIconBlue);
    int16_t titleId = 0;
    switch (screen()) {
    case Screen::Load: titleId = 126; break;
    case Screen::Options: titleId = 491; break;
    case Screen::Pda: titleId = pda.chapterNameId; break;
    default: break;
    }
    c.textCentered(cx, 86 * u, std::round(16 * u), s.get(titleId), kWhite);
    c.rect(cx - 150 * u, 112 * u, 300 * u, std::max(1.0f, u), kIconBlue);
    float y = 142 * u;
    if (screen() == Screen::Pda) {
        c.textCentered(cx, 124 * u, small, s.get(1075), kGrey); // AREA INFO
        const std::pair<int16_t, int> rows[] = {{1073, pda.treasures}, {1072, pda.relics}, {1274, pda.healthPacks}};
        for (const auto& [id, n] : rows) {
            c.text(panelX + 35 * u, y, mid, s.get(id), kWhite);
            const std::string v = std::to_string(n);
            c.text(panelX + panelW - 35 * u - Canvas::textWidth(v, mid), y, mid, v, kWhite);
            y += 28 * u;
        }
        y += 20 * u;
    }
    if (screen() == Screen::Load && saves.empty()) {
        c.textCentered(cx, y, mid, s.get(456), kGrey), y += 28 * u; // Unavailable
    }
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& it = list[i];
        std::string label = it.stringId ? std::string(s.get(it.stringId)) : it.label;
        if (it.value) label += "  " + std::string(size_t(*it.value), '|') + std::string(size_t(10 - *it.value), '.');
        const bool sel = int(i) == cursor_;
        if (sel) c.rect(panelX + 12 * u, y - 5 * u, panelW - 24 * u, mid + 10 * u, 0xB0704020u);
        c.text(panelX + 30 * u, y, mid, label, sel ? kWhite : kGrey);
        y += 28 * u;
    }
    footer(c, s, key, true);
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
