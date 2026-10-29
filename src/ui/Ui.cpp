// Menus, PDA and loading screen as quads, drawn with the original art and font (ui::Art) at the layout measured
// from tru.exe captures. Strings and ids, measured layout: docs/GAMEPLAY.md "Menus".
#include "ui/Ui.hpp"

#include "locale/Locale.hpp"
#include "render/RenderCommon.hpp"
#include "ui/Art.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <glm/glm.hpp>

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
            // Half-texel inset: bilinear taps at large scales would pick up the neighbouring glyphs' edges.
            image(0, sx(x), sy(y), w * unit(), h * unit(), u0 + du / 2, v0 + dv / 2, u0 + (float(g->width) - 0.5f) * du,
                  v0 + (float(f.cellHeight) - 0.5f) * dv, c);
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
                   kBlackDisc = 51, kDialog = 106, kLine = 105, kEmblem = 5, kArrowLeft = 113, kArrowRight = 114;
constexpr uint32_t kIconPlay = 18, kIconNew = 19, kIconOptions = 20, kIconLoad = 21, kIconSave = 22, kIconAv = 23,
                   kIconExtras = 24, kIconQuit = 26, kIconPageUp = 27, kIconPageDown = 28, kIconSlotEmpty = 29,
                   kIconSlotUsed = 30, kIconVolume = 32;
constexpr uint32_t kPdaPicture = 680, kPdaSonar = 688, kPdaWeapons = 689, kPdaInventory = 690; // pc-w\g_pda.drm
constexpr uint32_t kPdaDoc = 698, kPdaButtons = 699, kPdaBackpack = 702; // title icon, window buttons, 702..705 categories

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
constexpr float kCapTop = 4.52f, kCapHeight = 14.0f;  // per unit text scale: cell top -> cap top, cap height
constexpr float kLabelGap = 53.6f;                    // ring label: right ink edge to the item centre
constexpr float kFooterY = 434.1f, kFooterLine0 = 432.0f, kFooterLine1 = 450.5f; // prompt cell top, lines
// Message box: texture 106 (its box covers texels 202..822 x 5..250), text at 0.74 with 16-pixel lines.
constexpr float kDialogY = 214.7f, kDialogW = 672, kDialogH = 210, kDialogText = 0.74f, kDialogTop = 179.8f;
// YES / NO question in the same box [O dlg_quit / dlg_load(_yes) / dlg_newgame / dlg_autosave_*, 1080p]: text caps from 164
// every 16, wrapped at 356; labels 384 / 385 at the text's 0.74 with their ink ends (YES right, NO left) at 302.2 / 339.6,
// a white "/" at 320. The quit question 494 (the main menu's own dialog [I]) differs: caps from 168.9, ink ends 310.7 / 325,
// "/" at 319. The selected label sits on bar 107 (YES, right end 322.2) or 108 (NO, left end 318.5): 128x32 texels at
// 0.6 x 0.83, white at 0.5, centred at y 291.1.
constexpr uint32_t kYesBar = 107, kNoBar = 108;
constexpr float kConfirmCap = 164, kConfirmCapQuit = 168.9f, kConfirmW = 356, kConfirmMid = 291.1f, kConfirmBarW = 0.6f,
                kConfirmBarH = 0.83f;
// Load/Save slot ring: the main ring mirrored about x = kRingX (items 2..7: page up, four slots, page down).
constexpr float kOpenedRing = 29.8f, kOpenedRingW = 1.3f; // white circle around the ring item that opened it
constexpr float kSlotFooterL = 193.6f, kSlotFooterR = 433.3f; // "Return SELECT" / "BackSpace DELETE" centres
// The selected slot's panel, relative to its disc centre [O load_slot_filled, 1080p]: box to x 585.3, 27 above / 26.3 below;
// name (89, 168, 239) at +31 cap -24.3 scale 0.57; separators -14.1 / +14.6; area / title caps -7.4 / +2.8 and time / date
// at +29.7 / +82.6 cap +18.3, scale 0.45; the slot number centred at (+4.8, +5) scale 0.6 in the selected icon.
constexpr float kSlotBoxRight = 585.3f, kSlotBoxTop = 27, kSlotBoxBottom = 26.3f, kSlotTextX = 31, kSlotNameCap = -24.3f,
                kSlotNameScale = 0.57f, kSlotLine0 = -14.1f, kSlotLine1 = 14.6f, kSlotAreaCap = -7.4f, kSlotTitleCap = 2.8f,
                kSlotSmall = 0.45f, kSlotTimeX = 29.7f, kSlotDateX = 82.6f, kSlotTimeCap = 18.3f, kSlotBoxLeft = 23.6f;
constexpr float kSlotNameTrack = 0.94f, kSlotTrack = 0.72f; // letter-spaced [O widths]
constexpr float kSlotNumberX = 4.8f / kSelIcon, kSlotNumberY = 5.0f / kSelIcon, kSlotNumberScale = 0.6f / kSelIcon;
// List screens (GAME OPTIONS, AUDIO/VISUAL, VOLUME SETTINGS).
constexpr float kListIconX = 191, kListIconY = 88, kListDivX = 218, kListDivTop = 73.3f, kListDivBottom = 104;
constexpr float kTitle1X = 226.7f, kTitle1Cap = 75.1f, kTitle1Scale = 0.95f;
constexpr float kTitle2X = 262.2f, kTitle2Cap = 91.6f, kTitle2Scale = 0.67f;
constexpr float kListLineTop = 110, kListLineW = 409, kListPanelSolid = 125, kListPanelFade = 181;
constexpr float kRowCap = 127.6f, kRowStep = 24, kRowScale = 0.6f, kRowX = 152.4f, kRowXVolume = 164.4f;
constexpr float kRowsBelow = 25.7f, kListMinBottom = 246.7f, kFewRowsDrop = 8.4f; // < 5 rows start lower
constexpr float kSelBarH = 15.5f, kSelBarSolid = 107.5f, kSelBarFade = 217.5f;
constexpr float kSliderX0 = 322.7f, kSliderX1 = 461.3f, kSliderH = 7, kSliderArrowL = 307.6f, kSliderArrowR = 475.6f;
constexpr float kSliderArrow = 7.8f; // textures 113/114 (16x32, ink 9x18): a ~4.4 square triangle
constexpr float kListFooterL = 271.6f, kListFooterR = 379.0f;
// PLAYER TAILORING panel (texture 106 stretched).
constexpr float kTailorY = 216, kTailorH = 237, kTailorTitleCap = 117.8f, kTailorTitleScale = 0.8f, kTailorLineY = 139.6f;
constexpr float kTailorRow0 = 176.9f, kTailorStep = 28.9f, kTailorScale = 0.66f, kTailorBarSolid = 100, kTailorBarFade = 182;
constexpr float kTailorFoot = 0.76f, kTailorFootLine = 287, kTailorFootY = 306.7f, kTailorFootL = 266.7f, kTailorFootR = 373.3f;
// Loading screen.
constexpr float kLoadBand0 = 88.9f, kLoadBand1 = 390.2f, kLoadTitleCap = 220.4f, kLoadTitleScale = 1.17f, kLoadLine = 242.4f;
constexpr float kLoadTextCap = 256.9f, kLoadTextScale = 0.66f, kLoadTextStep = 13.15f, kLoadTextW = 540;

constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) { return r | g << 8 | b << 16 | a << 24; }
constexpr uint32_t kIconBlue = rgba(50, 115, 185), kLabel = rgba(203, 203, 203), kKeyBlue = rgba(99, 209, 247),
                   kLineBlue = rgba(43, 105, 140), kButtonBlue = rgba(25, 73, 103), kBlack = rgba(0, 0, 0),
                   kSlotIcon = rgba(114, 124, 165), kTitleBlue = rgba(110, 165, 200), kRowGrey = rgba(129, 130, 131),
                   kSelBar = rgba(23, 69, 97), kSliderFrame = rgba(5, 50, 80), kSliderFill = rgba(185, 230, 255),
                   kTailorBar = rgba(30, 90, 127), kPdaSel = rgba(76, 139, 193), kPdaGrey = rgba(91, 91, 91),
                   kPdaLine = rgba(46, 86, 121), kPdaSelLine = rgba(68, 130, 184), kPdaBoxLine = rgba(16, 60, 66),
                   kSlotName = rgba(89, 168, 239), kSlotLine = rgba(72, 72, 72), kSlotRim = rgba(28, 92, 150),
                   kPdaHead = rgba(100, 170, 235), kPdaEquipped = rgba(245, 135, 25), kPdaTitleRule = rgba(40, 95, 120),
                   kPdaValue = rgba(200, 200, 200), kPdaYes = rgba(40, 220, 40), kPdaNo = rgba(230, 25, 25);

uint32_t withAlpha(uint32_t c, float a) { return (c & 0x00FFFFFFu) | uint32_t(std::clamp(a, 0.0f, 1.0f) * float(c >> 24)) << 24; }
uint32_t mix(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8)
        out |= uint32_t(std::lround(float((a >> s) & 0xFF) * (1 - t) + float((b >> s) & 0xFF) * t)) << s;
    return out;
}

// Ring position of radial item `i` (0 = centre); `mirror`: the Load/Save slot ring.
std::pair<float, float> radialPos(size_t i, bool mirror = false) {
    if (i == 0) return {kRingX, kRingY};
    const float a = (-90.0f - float(i - 1) * kRingStep) * 3.14159265359f / 180.0f;
    const float x = std::cos(a) * kRingRadius;
    return {kRingX + (mirror ? -x : x), kRingY + std::sin(a) * kRingRadius};
}

// The ring item level with the centre: where Left goes first (tru.exe: SAVE GAME of the main menu).
int levelRingItem(size_t count) {
    int best = 1;
    for (size_t i = 1; i < count; ++i)
        if (std::abs(radialPos(i).second - kRingY) < std::abs(radialPos(size_t(best)).second - kRingY)) best = int(i);
    return best;
}

// Solid quad between screen-pixel corners with per-corner colours (left/right): horizontal gradients.
void hquad(Canvas& c, float x0, float y0, float x1, float y1, uint32_t left, uint32_t right) {
    c.vertices.insert(c.vertices.end(), {{x0, y0, -1, -1, left}, {x1, y0, -1, -1, right}, {x1, y1, -1, -1, right},
                                         {x0, y0, -1, -1, left}, {x1, y1, -1, -1, right}, {x0, y1, -1, -1, left}});
}
// Horizontal bar centred on virtual x 320: solid over +-solid, fading out to +-fade.
void fadeBar(Canvas& c, float y, float h, float solid, float fade, uint32_t color, float cx = 320) {
    const float y0 = c.sy(y), y1 = c.sy(y + h);
    const uint32_t clear = withAlpha(color, 0);
    hquad(c, c.sx(cx - fade), y0, c.sx(cx - solid), y1, clear, color);
    hquad(c, c.sx(cx - solid), y0, c.sx(cx + solid), y1, color, color);
    hquad(c, c.sx(cx + solid), y0, c.sx(cx + fade), y1, color, clear);
}
void vrect(Canvas& c, float x, float y, float w, float h, uint32_t color) { c.rect(c.sx(x), c.sy(y), w * c.unit(), h * c.unit(), color); }
// Annulus (screen-space triangles) at virtual (cx, cy).
void ring(Canvas& c, float cx, float cy, float r, float width, uint32_t color) {
    constexpr int kSegments = 64;
    for (int i = 0; i < kSegments; ++i) {
        const float a0 = float(i) * 6.2831853f / kSegments, a1 = float(i + 1) * 6.2831853f / kSegments;
        auto p = [&](float a, float rr) { return std::pair{c.sx(cx + std::cos(a) * rr), c.sy(cy + std::sin(a) * rr)}; };
        const auto [ax, ay] = p(a0, r - width);
        const auto [bx, by] = p(a0, r);
        const auto [cx1, cy1] = p(a1, r);
        const auto [dx, dy] = p(a1, r - width);
        c.vertices.insert(c.vertices.end(), {{ax, ay, -1, -1, color}, {bx, by, -1, -1, color}, {cx1, cy1, -1, -1, color},
                                             {ax, ay, -1, -1, color}, {cx1, cy1, -1, -1, color}, {dx, dy, -1, -1, color}});
    }
}

// Text placed by its cap top (the measured ink top of capitals).
// `tracking`: extra advance per glyph (the PDA prompts are letter-spaced [O captures]).
float capText(Canvas& c, float x, float capTop, float scale, std::string_view t, uint32_t color, float tracking = 0) {
    if (tracking == 0) return c.fontText(x, capTop - kCapTop * scale, scale, t, color);
    const float x0 = x;
    for (size_t p = 0; p < t.size();) {
        const uint8_t b = uint8_t(t[p]);
        const size_t n = std::min(t.size() - p, size_t(b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4));
        x += c.fontText(x, capTop - kCapTop * scale, scale, t.substr(p, n), color) + tracking;
        p += n;
    }
    return x - x0;
}
float trackedWidth(const Canvas& c, std::string_view t, float scale, float tracking) {
    return c.fontWidth(t, scale) + tracking * float(decodeUtf8(t).size());
}

// locals.bin prompts carry control placeholders (U+1800..U+18FF) that the game replaces with the bound control's
// name, drawn in blue: the keyboard defaults as tru.exe shows them [O captures].
std::string_view placeholderKey(char32_t cp, std::string_view select) {
    switch (cp) {
    case 0x182A: return select;      // SELECT
    case 0x182B: return "Escape";    // BACK
    case 0x182C: return "BackSpace"; // DELETE
    case 0x1803: return "space";     // jump (PDA "PRESS ... TO")
    case 0x1801: return "E";         // interact
    case 0x1805: return "G";         // sonar map DOWN
    case 0x1807: return "H";         // sonar map UP
    default: return {};
    }
}
struct Run { std::string text; bool key; };
std::vector<Run> keyRuns(std::string_view utf8, std::string_view select) {
    std::vector<Run> runs;
    size_t p = 0;
    while (p < utf8.size()) {
        const uint8_t b = uint8_t(utf8[p]);
        const size_t n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
        const auto cps = decodeUtf8(utf8.substr(p, n));
        const char32_t cp = cps.empty() ? 0 : cps[0];
        if (cp >= 0x1800 && cp < 0x1900) {
            if (const auto k = placeholderKey(cp, select); !k.empty()) runs.push_back({std::string(k), true});
        } else {
            if (runs.empty() || runs.back().key) runs.push_back({{}, false});
            runs.back().text.append(utf8.substr(p, n));
        }
        p += n;
    }
    return runs;
}
float keyedWidth(const Canvas& c, std::string_view t, float scale, std::string_view select, float tracking = 0) {
    float w = 0;
    for (const auto& r : keyRuns(t, select)) w += trackedWidth(c, r.text, scale, tracking);
    return w;
}
// Prompt text centred on `cx` (cap top `capTop`): placeholders as key names in blue, the rest in `color`.
void keyed(Canvas& c, float cx, float capTop, float scale, std::string_view t, std::string_view select, float tracking = 0) {
    float x = cx - keyedWidth(c, t, scale, select, tracking) / 2;
    for (const auto& r : keyRuns(t, select)) x += capText(c, x, capTop, scale, r.text, r.key ? kKeyBlue : kWhite, tracking);
}

// Footer lines and prompts (centres in virtual x).
void footer(Canvas& c, const locale::Strings& s, std::string_view key, std::initializer_list<std::pair<float, int16_t>> prompts) {
    c.sprite(kLine, 320, kFooterLine0, 1024 * kTexel, 1.8f, kLineBlue);
    c.sprite(kLine, 320, kFooterLine1, 1024 * kTexel, 1.8f, kLineBlue);
    for (const auto& [cx, id] : prompts) keyed(c, cx, kFooterY + kCapTop * kTextScale, kTextScale, s.get(id), key);
}

// Breaks `text` (locals.bin "\n" escapes too) into lines no wider than `width` virtual pixels.
std::vector<std::string> fontWrap(const Canvas& c, std::string_view text, float width, float scale, float tracking = 0) {
    std::vector<std::string> lines;
    for (const auto& para : wrap(text, 1u << 20)) {
        std::string line;
        for (size_t p = 0; p <= para.size();) {
            const size_t q = std::min(para.find(' ', p), para.size());
            const std::string word = para.substr(p, q - p);
            const std::string next = line.empty() ? word : line + ' ' + word;
            if (!line.empty() && trackedWidth(c, next, scale, tracking) > width) lines.push_back(line), line = word;
            else line = next;
            p = q + 1;
        }
        lines.push_back(line);
    }
    return lines;
}

bool radial(Screen s) { return s == Screen::Main || s == Screen::Pause; }
bool slots(Screen s) { return s == Screen::Load || s == Screen::Save; }

// PDA pages (left list): SONAR MAP, AREA INFO, WEAPON SELECTION, INVENTORY, FIELD ASSISTANCE, JOURNAL and REVISIT
// LOCATION (greyed: not ported, as on PC without them), RESUME GAME.
enum PdaPage { kSonar, kAreaInfo, kWeapons, kInventory, kField, kJournal, kRevisit, kResume };
// PDA geometry is in pda_bg_1280 pixels (720 high, fitted to the window height and centred; pda_bg_960 for narrow
// windows has the same content 160 pixels further left): bg pixel -> virtual.
constexpr float pdaX(float x) { return x * 2 / 3 - 320.0f / 3; }
constexpr float pdaY(float y) { return y * 2 / 3; }
constexpr float kPdaItemRight = 502.7f, kPdaItemCap = 173.3f, kPdaItemStep = 43.3f, kPdaItemScale = 0.605f;
constexpr float kPdaLineX0 = 251.3f, kPdaLineBelow = 17.4f, kPdaArrowW = 10;
constexpr float kPdaPicX = 544.7f, kPdaPicY = 166, kPdaPicRight = 1035, kPdaTabRight = 994, kPdaTabCap = 360.7f, kPdaSmall = 0.57f, kPdaTracking = 0.55f;
constexpr float kPdaBoxTop = 405.3f, kPdaBoxBottom = 525, kPdaRowCap0 = 442.7f, kPdaRowCap1 = 478.7f, kPdaRowScale = 0.8f;
constexpr float kPdaRowX = 602.7f, kPdaRowRight = 963.3f, kPdaPromptMid = 465, kPdaPromptStep = 19.3f, kPdaPromptW = 330;
constexpr float kPdaFieldBox0 = 155, kPdaFieldBox1 = 344, kPdaFieldBox2 = 357, kPdaFieldBox3 = 544;
constexpr float kPdaFootCap = 600, kPdaFootBack = 289.8f, kPdaFootSelect = 621.3f;
// WEAPON SELECTION / INVENTORY pages [V captures at 1920x1080]: the list's header row, the right pane's title bar,
// model panel, ammunition rows and description box.
constexpr float kPdaHeadX = 248;
constexpr float kPdaTitleX = 583.3f, kPdaTitleCap = 163.3f, kPdaTitleScale = 0.54f, kPdaTitleLine = 184;
constexpr float kPdaAmmoScale = 0.55f, kPdaAmmoTracking = 1.0f;
constexpr float kPdaDescTop = 370, kPdaDescBottom = 544, kPdaDescX = 558, kPdaDescCap = 445, kPdaDescStep = 16.7f,
                kPdaDescW = 462, kPdaDescScale = 0.58f; // [V inventory_view]
constexpr int kWeaponPanel[4] = {573, 214, 422, 163}, kItemPanel[4] = {580, 193, 433, 167}; // x, y, w, h

} // namespace

int16_t levelTitle(int chapter, std::string_view unit) {
    auto has = [&](std::string_view p) { return unit.find(p) != std::string_view::npos; };
    switch (chapter) {
    case 0: return 386; // Prologue
    case 1: return has("seafloor") ? 388 : has("kraken") || has("norse") ? 389 : has("baldrs_tomb") || has("baldrs_sideroom") ? 391
                 : has("baldrs") ? 390 : has("ship") ? 392 : 387;
    case 2: return has("norsehall") ? 396 : has("norsehub") ? 395 : has("cliff") || has("junglehub") ? 393 : 394;
    case 3: return 397;
    case 4: return has("mictlan") ? 401 : has("ragnarok") || has("pool") ? 400
                 : has("xibalba") || has("staircase_upper") || has("ballcourt") || has("traprooms") ? 399 : 398;
    case 5: return has("valaskjalf") ? 403 : 402;
    case 6: return 404;
    case 7: return has("yggdrasil") ? 406 : has("monastery") ? 407 : 405;
    case 8: return 407;
    default: return 0;
    }
}

std::string_view loadingImage(int chapter) {
    static constexpr std::string_view kImages[] = {"loading_bg_mansion", "loading_bg_medsea", "loading_bg_thailand",
                                                   "loading_bg_dungeon", "loading_bg_mexico", "loading_bg_snow",
                                                   "loading_bg_ship", "loading_bg_arcticsea", "loading_bg_arcticsea"};
    return kImages[size_t(std::clamp(chapter, 0, 8))];
}

void drawLoading(Canvas& c, const locale::Strings& s, int chapter) {
    const float w = float(c.width), h = float(c.height);
    const float k = std::max(w / 640, h / 360); // 640x360 image, covering the window
    c.image(1, (w - 640 * k) / 2, (h - 360 * k) / 2, 640 * k, 360 * k, 0, 0, 1, 1, kWhite);
    const float b0 = c.sy(kLoadBand0), b1 = c.sy(kLoadBand1);
    c.rect(0, 0, w, b0, withAlpha(kBlack, 0.69f)); // ~0.31 outside the band, ~0.92 inside [O captures]
    c.rect(0, b1, w, h - b1, withAlpha(kBlack, 0.69f));
    c.rect(0, b0, w, b1 - b0, withAlpha(kBlack, 0.08f));
    c.rect(0, b0, w, 2.2f * c.unit(), withAlpha(kWhite, 0.3f));
    c.rect(0, b1 - 2.2f * c.unit(), w, 2.2f * c.unit(), withAlpha(kWhite, 0.3f));
    c.sprite(kEmblem, 320, 240, 345, 345, withAlpha(kWhite, 0.35f));
    const int ch = std::clamp(chapter, 0, 7);
    const std::string title(s.get(int16_t(51 + ch)));
    capText(c, 320 - c.fontWidth(title, kLoadTitleScale) / 2, kLoadTitleCap, kLoadTitleScale, title, kWhite);
    c.sprite(kLine, 320, kLoadLine, kListLineW, 1.8f, kWhite);
    float y = kLoadTextCap;
    for (const auto& l : fontWrap(c, s.get(int16_t(59 + ch)), kLoadTextW, kLoadTextScale))
        capText(c, 320 - c.fontWidth(l, kLoadTextScale) / 2, y, kLoadTextScale, l, kWhite), y += kLoadTextStep;
}

void Menus::open(Screen s) {
    cursors_.push_back(cursor_);
    stack_.push_back(s);
    cursor_ = 0, ring_ = -1, label_ = 0, page_ = 0;
    grow_.fill(0);
    if (radial(s)) grow_[0] = 1;
    if (slots(s)) cursor_ = -1;          // focus stays on the ring item until Right / Accept
    if (s == Screen::Pda) cursor_ = kAreaInfo;
    if (s == Screen::Tailoring) cursor_ = tailoring;
    if (s == Screen::PdaItems) category_ = std::clamp(cursors_.back(), 0, 3); // the INVENTORY row that opened it
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
    const bool animated = radial(screen()) || slots(screen());
    for (size_t i = 0; i < grow_.size(); ++i) { // ~0.15 s to grow / shrink
        const float target = animated && int(i) == cursor_ ? 1.0f : 0.0f;
        const float step = float(seconds) / 0.15f;
        grow_[i] = grow_[i] < target ? std::min(target, grow_[i] + step) : std::max(target, grow_[i] - step);
    }
    label_ = std::min(1.0f, label_ + float(seconds) / 0.5f); // label fades in over ~0.5 s
}

std::vector<Item> Menus::items() { return itemsOf(screen()); }

std::vector<Item> Menus::itemsOf(Screen screen) {
    const int16_t unavailable = 456; // "Unavailable": not ported (the launcher configures display and controls)
    switch (screen) {
    case Screen::Main:
    case Screen::Pause: {
        const bool pause = screen == Screen::Pause;
        return {pause ? Item{.stringId = 122, .action = Action::Resume, .icon = kIconPlay}
                      : Item{.stringId = 123, .action = Action::Continue, .icon = kIconPlay},
                {.stringId = 124, .opens = Screen::Tailoring, .icon = kIconNew, .confirm = int16_t(pause ? 154 : 0)},
                {.stringId = 125, .opens = Screen::GameOptions, .icon = kIconOptions},
                {.stringId = 126, .opens = Screen::Load, .icon = kIconLoad},
                pause ? Item{.stringId = 127, .opens = Screen::Save, .icon = kIconSave}
                      : Item{.stringId = 127, .opens = Screen::Message, .icon = kIconSave, .message = 161}, // "only ... in gameplay mode"
                {.stringId = 128, .opens = Screen::AudioVisual, .icon = kIconAv},
                {.stringId = 129, .opens = Screen::Message, .icon = kIconExtras, .message = unavailable},
                {.stringId = 131, .action = pause ? Action::QuitToMenu : Action::Quit, .icon = kIconQuit, .confirm = 494}};
    }
    case Screen::Load:
    case Screen::Save: {
        std::vector<Item> v{{.icon = kIconPageUp}};
        for (int k = 0; k < 4; ++k) {
            const size_t i = size_t(page_ * 4 + k);
            if (i >= saves.size()) v.push_back({.action = screen == Screen::Load ? Action::None : Action::Save, .icon = kIconSlotEmpty});
            else if (screen == Screen::Save && saves[i].autosave) // "not allowed to overwrite auto saved games"
                v.push_back({.label = saves[i].slot, .opens = Screen::Message, .icon = kIconSlotUsed, .message = 17});
            else
                v.push_back({.label = saves[i].slot, .action = screen == Screen::Load ? Action::Load : Action::Save,
                             .icon = kIconSlotUsed, .confirm = int16_t(screen == Screen::Load ? 12 : 16)});
        }
        v.push_back({.icon = kIconPageDown});
        return v;
    }
    case Screen::GameOptions:
        return {{.stringId = 132, .toggle = &reticles, .onId = 133, .offId = 134},
                {.stringId = 135, .toggle = &helperButtons, .onId = 133, .offId = 134},
                {.stringId = 136, .toggle = &trainingText, .onId = 133, .offId = 134},
                {.stringId = 492, .opens = Screen::Message, .message = unavailable},
                {.stringId = 138, .toggle = &invertY, .onId = 45, .offId = 46, .pair = true},
                {.stringId = 140, .opens = Screen::Tailoring},
                {.stringId = 493, .opens = Screen::Message, .message = unavailable},
                {.stringId = 469}};
    case Screen::AudioVisual:
        return {{.stringId = 145, .onId = 110},
                {.stringId = 144, .toggle = &subtitles, .onId = 45, .offId = 46, .pair = true},
                {.stringId = 142, .opens = Screen::Volume},
                {.stringId = 143, .value = &brightness},
                {.stringId = 498, .opens = Screen::Message, .message = unavailable},
                {.stringId = 499, .opens = Screen::Message, .message = unavailable},
                {.stringId = 469}};
    case Screen::Volume:
        return {{.stringId = 487, .value = &sfxVolume}, {.stringId = 488, .value = &musicVolume},
                {.stringId = 489, .value = &cinematicVolume}, {.stringId = 469}};
    case Screen::Tailoring: { // from START NEW GAME it starts the game; from GAME OPTIONS it only sets the preset
        const bool newGame = stack_.size() >= 2 && radial(stack_[stack_.size() - 2]);
        const Action a = newGame ? Action::NewGame : Action::None;
        return {{.stringId = 481, .action = a}, {.stringId = 482, .action = a}, {.stringId = 483, .action = a}};
    }
    case Screen::Pda:
        return {pda.sonar ? Item{.stringId = 1074, .opens = Screen::PdaSonar}
                          : Item{.stringId = 1074, .opens = Screen::Message, .message = 1093}, // "SONAR MAP IS CURRENTLY UNAVAILABLE"
                {.stringId = 1075},
                {.stringId = 1076, .opens = Screen::PdaWeapons},
                {.stringId = 1077, .opens = Screen::PdaInventory},
                {.stringId = 1078, .action = Action::Hint},
                {.stringId = 1081, .greyed = true},
                {.stringId = 1082, .greyed = true},
                {.stringId = 122, .action = Action::Resume}};
    case Screen::Message: return {{.stringId = 383}}; // OK
    case Screen::Confirm: return {{.stringId = 384}, {.stringId = 385}}; // YES / NO
    case Screen::PdaWeapons: { // the secondaries, BACK TO PREVIOUS MENU
        std::vector<Item> v;
        for (const auto& w : pda.weapons) v.push_back({.stringId = w.name, .action = Action::Equip});
        v.push_back({.stringId = 469});
        return v;
    }
    case Screen::PdaInventory: // BACKPACK, GEAR, WEAPONS, TREASURES
        return {{.stringId = 1101, .opens = Screen::PdaItems}, {.stringId = 1235, .opens = Screen::PdaItems},
                {.stringId = 1100, .opens = Screen::PdaItems}, {.stringId = 1251, .opens = Screen::PdaItems}, {.stringId = 469}};
    case Screen::PdaItems: {
        std::vector<Item> v;
        for (const auto& it : pda.inventory[size_t(category_)]) v.push_back({.stringId = it.name, .label = it.suffix});
        v.push_back({.stringId = 469});
        return v;
    }
    case Screen::PdaSonar: break;
    case Screen::None: break;
    }
    return {};
}

Action Menus::input(Nav n) {
    if (screen() == Screen::PdaSonar) { // Escape BACK, E RESET VIEW; the app flies the camera with the held keys
        if (n == Nav::Back) back();
        return n == Nav::Alt ? Action::SonarReset : Action::None;
    }
    auto list = items();
    if (list.empty()) return Action::None;
    const int count = int(list.size());
    const Screen scr = screen();
    if (n == Nav::Alt) {
        if (scr == Screen::Pda && cursor_ == kField) return Action::Answer;
        n = Nav::Accept;
    }
    if (scr == Screen::Confirm) { // Esc does nothing [O]
        if (n == Nav::Up || n == Nav::Down || n == Nav::Left || n == Nav::Right) cursor_ = 1 - cursor_;
        if (n != Nav::Accept) return Action::None;
        const bool yes = cursor_ == 0;
        const Item it = pending_;
        back();
        return yes ? activate(it) : Action::None;
    }
    if (slots(scr)) { // the slot ring: -1 = focus on the ring item that opened the screen
        auto page = [&](int d) {
            const int pages = std::max(1, int(saves.size() + (scr == Screen::Save ? 4 : 3)) / 4);
            page_ = std::clamp(page_ + d, 0, pages - 1);
        };
        switch (n) {
        case Nav::Up: case Nav::Down:
            if (cursor_ >= 0) select(std::clamp(cursor_ + (n == Nav::Up ? -1 : 1), 0, count - 1));
            return Action::None;
        case Nav::Right: if (cursor_ < 0) select(1); return Action::None;
        case Nav::Left: if (cursor_ >= 0) cursor_ = -1, label_ = 0; return Action::None;
        case Nav::Back: back(); return Action::None;
        case Nav::Delete:
            if (cursor_ < 1 || cursor_ > 4 || list[size_t(cursor_)].label.empty()) return Action::None;
            selected_ = list[size_t(cursor_)].label;
            return activate({.action = Action::Delete, .confirm = 10});
        case Nav::Accept: {
            if (cursor_ < 0) { select(1); return Action::None; }
            if (cursor_ == 0 || cursor_ == count - 1) { page(cursor_ == 0 ? -1 : 1); return Action::None; }
            const Item& it = list[size_t(cursor_)];
            if (it.action == Action::None && it.opens == Screen::None) return Action::None; // empty slot on Load
            selected_ = it.label;
            return activate(it);
        }
        default: return Action::None;
        }
    }
    auto skip = [&](int c, int d) { // next selectable item
        for (int i = 0; i < count; ++i) {
            c = (c + d + count) % count;
            if (!list[size_t(c)].greyed) return c;
        }
        return c;
    };
    switch (n) {
    case Nav::Up:
    case Nav::Down:
        if (!radial(scr)) select(skip(cursor_, n == Nav::Up ? -1 : 1));
        else if (cursor_ > 0) select((cursor_ + (n == Nav::Up ? -1 : 1) + count) % count); // ring ends lead to the centre
        return Action::None;
    case Nav::Left:
    case Nav::Right:
        if (radial(scr)) {
            if (n == Nav::Left && cursor_ == 0) select(ring_ > 0 ? ring_ : levelRingItem(list.size()));
            else if (n == Nav::Right) select(0);
        } else if (int* v = list[size_t(cursor_)].value) *v = std::clamp(*v + (n == Nav::Right ? 1 : -1), 0, 10);
        else if (bool* t = list[size_t(cursor_)].toggle) *t = !*t;
        return Action::None;
    case Nav::Back:
        if (scr == Screen::Main) return Action::None;
        if (stack_.size() == 1) { close(); return Action::Resume; } // Pause, PDA
        back();
        return Action::None;
    case Nav::Delete: return Action::None;
    case Nav::Alt:
    case Nav::Accept: return activate(list[size_t(cursor_)]);
    }
    return Action::None;
}

Action Menus::activate(Item it) {
    if (it.confirm) {
        pending_ = it, pending_.confirm = 0, message_ = it.confirm;
        open(Screen::Confirm);
        cursor_ = 1; // NO [O]
        return Action::None;
    }
    const Screen scr = screen();
    if (it.opens != Screen::None) {
        if (it.opens == Screen::Message) message_ = it.message;
        open(it.opens);
        return Action::None;
    }
    if (it.toggle) { *it.toggle = !*it.toggle; return Action::None; }
    if (scr == Screen::Tailoring) {
        tailoring = cursor_;
        if (it.action == Action::None) { back(); return Action::None; }
    }
    if (scr == Screen::PdaItems && cursor_ + 1 < int(items().size())) return Action::None; // an item: only shown
    if (it.action == Action::None) { // BACK / OK / sliders / PDA AREA INFO
        if (!it.value && scr != Screen::Pda) return input(Nav::Back);
        return Action::None;
    }
    if (it.action == Action::Hint) return Action::Hint;
    if (it.action == Action::Equip) equip_ = pda.weapons[size_t(cursor_)].weapon;
    if (it.action == Action::Resume || it.action == Action::NewGame || it.action == Action::Continue || it.action == Action::Load)
        close();
    return it.action;
}

void Menus::drawRadial(Canvas& c, const locale::Strings& s, Screen screen, int cursor, bool opened) {
    const auto list = itemsOf(screen);
    // Unselected first, the growing/selected ones on top; the opened item (Load/Save beneath) keeps its ring.
    std::vector<size_t> order(list.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    auto g = [&](size_t i) { return opened ? (i == 0 ? 1.0f : 0.0f) : grow_[i]; };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return g(a) < g(b); });
    for (const size_t i : order) {
        const auto [x, y] = radialPos(i);
        const float gi = g(i);
        const float disc = 128 * kTexel, icon = 64 * kTexel;
        const float sDisc = 1 + (i ? kSelDisc - 1 : 0) * gi, sIcon = 1 + (i ? kSelIcon - 1 : 0) * gi;
        const float sGlow = i ? kGlowRing : kGlowCentre;
        if (gi > 0) c.sprite(kGlow, x, y, disc * sGlow, disc * sGlow, withAlpha(kWhite, gi * kGlowAlpha));
        const bool isOpened = opened && int(i) == cursor;
        c.sprite(kDisc, x, y, disc * (isOpened ? kSelDisc : sDisc), disc * (isOpened ? kSelDisc : sDisc), kWhite);
        if (gi > 0 || isOpened) c.sprite(kBlackDisc, x, y, disc * kSelDisc, disc * kSelDisc, withAlpha(kBlack, isOpened ? 1 : gi));
        if (isOpened) ring(c, x, y, kOpenedRing, kOpenedRingW, kWhite);
        c.sprite(list[i].icon, x, y, icon * sIcon, icon * sIcon, mix(kIconBlue, kWhite, gi));
    }
    if (cursor < 0 || size_t(cursor) >= list.size()) return;
    const int labelled = opened ? 0 : cursor; // Load/Save: the centre label stays
    const float alpha = opened ? 1 : label_;
    const std::string label(s.get(list[size_t(labelled)].stringId));
    const auto [x, y] = radialPos(size_t(labelled));
    const uint32_t text = withAlpha(kLabel, alpha);
    if (labelled == 0) {
        c.sprite(kBarCentre, 320, 199.1f, kBarW, kBarH, withAlpha(kWhite, alpha));
        c.fontText(320 - c.fontWidth(label, kTextScale) / 2, 192.4f, kTextScale, label, text);
    } else {
        c.sprite(kBarLeft, x - kSideBarEnd - kSideBarW / 2, y, kSideBarW, kBarH, withAlpha(kWhite, alpha));
        c.fontText(x - kLabelGap - c.fontWidth(label, kTextScale), y - kCapMid, kTextScale, label, text);
    }
}

void Menus::drawSlots(Canvas& c, const locale::Strings& s) {
    const auto list = items();
    // The selected slot's panel [O load_slot_filled, load_slots]: a black box from the disc to x 585.3, y -27 .. +26.3, with
    // a blue rim; a used slot shows its name (blue, cut with "..."), "<AREA>:" / title, then time and date. The box goes
    // under the discs, its text over them (the selected disc's glow).
    size_t text0 = c.vertices.size(), text1 = text0;
    if (cursor_ >= 1 && cursor_ <= 4) {
        const auto [x, y] = radialPos(size_t(cursor_) + 2, true);
        const float a = label_;
        const float x0 = x + kSlotBoxLeft, w = kSlotBoxRight - x0, h = kSlotBoxTop + kSlotBoxBottom;
        for (const auto [d, rim] : {std::pair{2.6f, 0.12f}, {1.6f, 0.25f}, {0.7f, 0.6f}}) // the rim's soft glow
            vrect(c, x0 - d, y - kSlotBoxTop - d, w + 2 * d, h + 2 * d, withAlpha(kSlotRim, a * rim));
        vrect(c, x0, y - kSlotBoxTop, w, h, withAlpha(kBlack, a));
        text0 = c.vertices.size();
        const size_t i = size_t(page_ * 4 + cursor_ - 1);
        if (i < saves.size()) {
            const SaveSlot& sv = saves[i];
            const float tx = x + kSlotTextX, right = kSlotBoxRight - 4;
            std::string name = sv.name.empty() ? std::string(s.get(sv.area)) + " - " + std::string(s.get(sv.title)) : sv.name;
            if (trackedWidth(c, name, kSlotNameScale, kSlotNameTrack) > right - tx) { // cut at a word to fit with "..." [O]
                while (!name.empty() && trackedWidth(c, name + "...", kSlotNameScale, kSlotNameTrack) > right - tx) {
                    const size_t sp = name.find_last_of(' ');
                    name.resize(sp == std::string::npos ? name.size() - 1 : sp);
                    while (!name.empty() && (name.back() == ' ' || name.back() == '-')) name.pop_back();
                }
                name += "...";
            }
            capText(c, tx, y + kSlotNameCap, kSlotNameScale, name, withAlpha(kSlotName, a), kSlotNameTrack);
            for (const float ly : {kSlotLine0, kSlotLine1}) vrect(c, tx - 1.5f, y + ly, right - tx, 0.9f, withAlpha(kSlotLine, a));
            if (sv.area) capText(c, tx, y + kSlotAreaCap, kSlotSmall, std::string(s.get(sv.area)) + ":", withAlpha(kWhite, a), kSlotTrack);
            if (sv.title) capText(c, tx, y + kSlotTitleCap, kSlotSmall, s.get(sv.title), withAlpha(kWhite, a), kSlotTrack);
            capText(c, x + kSlotTimeX, y + kSlotTimeCap, kSlotSmall, sv.time, withAlpha(kWhite, a), kSlotTrack);
            capText(c, x + kSlotDateX, y + kSlotTimeCap, kSlotSmall, sv.date, withAlpha(kWhite, a), kSlotTrack);
        }
        text1 = c.vertices.size();
    }
    std::vector<size_t> order(list.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return grow_[a] < grow_[b]; });
    for (const size_t i : order) {
        const auto [x, y] = radialPos(i + 2, true);
        const float g = grow_[i], disc = 128 * kTexel, icon = 64 * kTexel;
        const float sDisc = 1 + (kSelDisc - 1) * g, sIcon = 1 + (kSelIcon - 1) * g;
        if (g > 0) c.sprite(kGlow, x, y, disc * kGlowRing, disc * kGlowRing, withAlpha(kWhite, g * kGlowAlpha));
        c.sprite(kDisc, x, y, disc * sDisc, disc * sDisc, kWhite);
        if (g > 0) c.sprite(kBlackDisc, x, y, disc * sDisc, disc * sDisc, withAlpha(kBlack, g));
        const uint32_t tint = mix(kSlotIcon, kWhite, g);
        c.sprite(list[i].icon, x, y, icon * sIcon, icon * sIcon, tint);
        if (list[i].icon == kIconSlotUsed) { // its number in the icon's circle
            const std::string n = std::to_string(page_ * 4 + int(i));
            const float sc = kSlotNumberScale * sIcon;
            capText(c, x + kSlotNumberX * sIcon - c.fontWidth(n, sc) / 2, y + kSlotNumberY * sIcon - kCapHeight * sc / 2, sc, n, tint);
        }
    }
    std::rotate(c.vertices.begin() + ptrdiff_t(text0), c.vertices.begin() + ptrdiff_t(text1), c.vertices.end());
}

void Menus::drawList(Canvas& c, const locale::Strings& s) {
    const Screen scr = screen();
    const auto list = items();
    const int n = int(list.size());
    // Title: icon | white line 1 / blue line 2 (GAME OPTIONS, AUDIO/VISUAL OPTIONS, VOLUME SETTINGS AUDIO/VISUAL).
    const uint32_t icon = scr == Screen::GameOptions ? kIconOptions : scr == Screen::AudioVisual ? kIconAv : kIconVolume;
    const int16_t t1 = scr == Screen::GameOptions ? 490 : scr == Screen::AudioVisual ? 497 : 486;
    const int16_t t2 = scr == Screen::Volume ? 497 : 491;
    c.sprite(icon, kListIconX, kListIconY, 64 * kTexel, 64 * kTexel, kWhite);
    vrect(c, kListDivX, kListDivTop, 0.9f, kListDivBottom - kListDivTop, kWhite);
    capText(c, kTitle1X, kTitle1Cap, kTitle1Scale, s.get(t1), kWhite);
    capText(c, kTitle2X, kTitle2Cap, kTitle2Scale, s.get(t2), kTitleBlue);
    // Panel between the two lines; rows.
    const float row0 = kRowCap + (n < 5 ? kFewRowsDrop : 0);
    const float bottom = std::max(row0 + float(n - 1) * kRowStep + kRowsBelow, kListMinBottom);
    const float top = c.sy(kListLineTop), bot = c.sy(bottom);
    hquad(c, c.sx(320 - kListPanelFade), top, c.sx(320 - kListPanelSolid), bot, withAlpha(kBlack, 0), kBlack);
    hquad(c, c.sx(320 - kListPanelSolid), top, c.sx(320 + kListPanelSolid), bot, kBlack, kBlack);
    hquad(c, c.sx(320 + kListPanelSolid), top, c.sx(320 + kListPanelFade), bot, kBlack, withAlpha(kBlack, 0));
    c.sprite(kLine, 320, kListLineTop + 0.65f, kListLineW, 1.8f, kWhite);
    c.sprite(kLine, 320, bottom + 0.65f, kListLineW, 1.8f, kWhite);
    const float labelX = scr == Screen::Volume ? kRowXVolume : kRowX;
    const float valueRight = scr == Screen::GameOptions ? 498.7f : 492.6f;
    const float capMid = kCapHeight * kRowScale / 2;
    for (int i = 0; i < n; ++i) {
        const Item& it = list[size_t(i)];
        const float y = row0 + float(i) * kRowStep;
        const bool sel = i == cursor_;
        if (sel) fadeBar(c, y + capMid - kSelBarH / 2, kSelBarH, kSelBarSolid, kSelBarFade, kSelBar);
        capText(c, labelX, y, kRowScale, s.get(it.stringId), sel ? kWhite : kRowGrey);
        if (it.value) {
            const float sy0 = y + capMid - kSliderH / 2;
            vrect(c, kSliderX0, sy0, kSliderX1 - kSliderX0, kSliderH, kSliderFrame);
            vrect(c, kSliderX0 + 1, sy0 + 1, (kSliderX1 - kSliderX0 - 2) * float(*it.value) / 10, kSliderH - 2, kSliderFill);
            if (sel) {
                c.sprite(kArrowLeft, kSliderArrowL, y + capMid, kSliderArrow, kSliderArrow, kWhite);
                c.sprite(kArrowRight, kSliderArrowR, y + capMid, kSliderArrow, kSliderArrow, kWhite);
            }
        } else if (it.toggle && it.pair) { // "ON / OFF": the current one white
            const std::string on(s.get(it.onId)), off(s.get(it.offId)), slash("   /   ");
            float x = valueRight - c.fontWidth(on + slash + off, kRowScale);
            x += capText(c, x, y, kRowScale, on, *it.toggle ? kWhite : kRowGrey);
            x += capText(c, x, y, kRowScale, slash, kRowGrey);
            capText(c, x, y, kRowScale, off, *it.toggle ? kRowGrey : kWhite);
        } else if (it.onId) {
            const std::string v(s.get(!it.toggle || *it.toggle ? it.onId : it.offId));
            capText(c, valueRight - c.fontWidth(v, kRowScale), y, kRowScale, v, kWhite);
        }
    }
}

void Menus::drawTailoring(Canvas& c, const locale::Strings& s) {
    c.sprite(kDialog, 320, kTailorY, kDialogW, kTailorH, kWhite);
    const std::string title(s.get(484));
    capText(c, 320 - c.fontWidth(title, kTailorTitleScale) / 2, kTailorTitleCap, kTailorTitleScale, title, kWhite);
    c.sprite(kLine, 320, kTailorLineY, 190, 1.8f, kLineBlue);
    const auto list = items();
    for (size_t i = 0; i < list.size(); ++i) {
        const float mid = kTailorRow0 + float(i) * kTailorStep;
        const bool sel = int(i) == cursor_;
        if (sel) fadeBar(c, mid - kSelBarH / 2, kSelBarH, kTailorBarSolid, kTailorBarFade, kTailorBar);
        const std::string t(s.get(list[i].stringId));
        capText(c, 320 - c.fontWidth(t, kTailorScale) / 2, mid - kCapHeight * kTailorScale / 2, kTailorScale, t, sel ? kWhite : kLabel);
    }
    c.sprite(kLine, 320, kTailorFootLine, 190, 1.8f, kLineBlue);
    const float cap = kTailorFootY - kCapHeight * kTailorFoot / 2;
    keyed(c, kTailorFootL, cap, kTailorFoot, s.get(120), "Return");
    keyed(c, kTailorFootR, cap, kTailorFoot, s.get(375), "Return");
}

void Menus::drawPda(Canvas& c, const locale::Strings& s) {
    // Background pc-w\image\pda_bg_1280 / _960 (uiImages[1], set by the app) fitted to the window height.
    const float k = float(c.height) / 720, bw = (float(c.width) / float(c.height) < 1.556f ? 960 : 1280) * k;
    c.image(1, (float(c.width) - bw) / 2, 0, bw, float(c.height), 0, 0, 1, 1, kWhite);
    const auto list = items();
    const Screen scr = screen();
    const bool sub = scr != Screen::Pda; // WEAPON SELECTION / INVENTORY pages: a header row above the list
    if (sub) {
        static constexpr int16_t kCategory[] = {1101, 1235, 1100, 1251};
        const std::string t(s.get(scr == Screen::PdaWeapons ? 1100 : scr == Screen::PdaInventory ? 1077 : kCategory[category_]));
        capText(c, pdaX(kPdaHeadX), pdaY(kPdaItemCap), kPdaItemScale, t, kPdaHead);
        vrect(c, pdaX(kPdaHeadX), pdaY(kPdaItemCap + kPdaLineBelow), (kPdaItemRight - kPdaHeadX) * 2 / 3, pdaY(1.5f), kPdaSelLine);
    }
    for (size_t i = 0; i < list.size(); ++i) {
        const float cap = kPdaItemCap + float(i + sub) * kPdaItemStep;
        const bool sel = int(i) == cursor_;
        const bool equipped = scr == Screen::PdaWeapons && i < pda.weapons.size() && pda.weapons[i].equipped;
        const uint32_t col = list[i].greyed ? kPdaGrey : sel ? kPdaSel : equipped ? kPdaEquipped : kWhite;
        const std::string t = std::string(s.get(list[i].stringId)) + list[i].label; // INVENTORY items: " [count]"
        const float right = sel ? kPdaItemRight - kPdaArrowW - 3 : kPdaItemRight;
        capText(c, pdaX(right) - c.fontWidth(t, kPdaItemScale), pdaY(cap), kPdaItemScale, t, col);
        if (sel) // texture 114's ink (9x18 of 16x32 texels) drawn 10x12, tinted to the label's blue
            c.sprite(kArrowRight, pdaX(kPdaItemRight - kPdaArrowW / 2), pdaY(cap + 6.3f), pdaY(17.8f), pdaY(21.3f), rgba(118, 168, 193));
        vrect(c, pdaX(kPdaLineX0), pdaY(cap + kPdaLineBelow), (kPdaItemRight - kPdaLineX0) * 2 / 3, pdaY(sel ? 2 : 1),
              sel ? kPdaSelLine : kPdaLine);
    }
    auto box = [&](float y0, float y1) {
        vrect(c, pdaX(kPdaPicX), pdaY(y0), (kPdaPicRight - kPdaPicX) * 2 / 3, pdaY(1.4f), kPdaBoxLine);
        vrect(c, pdaX(kPdaPicX), pdaY(y0 + 2), (kPdaPicRight - kPdaPicX) * 2 / 3, pdaY(y1 - y0 - 2), kBlack);
    };
    // Prompt lines ("PRESS <key> TO ...") centred on the right pane around `mid`.
    auto prompt = [&](int16_t id, float mid) {
        const auto lines = fontWrap(c, s.get(id), pdaX(kPdaPromptW) - pdaX(0), kPdaSmall, kPdaTracking);
        float cap = mid - (float(lines.size()) * kPdaPromptStep - (kPdaPromptStep - 12)) / 2;
        for (const auto& l : lines) keyed(c, pdaX((kPdaPicX + kPdaPicRight) / 2), pdaY(cap), kPdaSmall, l, "Return", kPdaTracking), cap += kPdaPromptStep;
    };
    auto picture = [&](uint32_t tex, int16_t tab) {
        c.sprite(tex, pdaX(kPdaPicX + 256), pdaY(kPdaPicY + 128), pdaX(512) - pdaX(0), pdaY(256), kWhite);
        const std::string t(s.get(tab));
        capText(c, pdaX(kPdaTabRight) - c.fontWidth(t, kPdaSmall), pdaY(kPdaTabCap), kPdaSmall, t, kWhite);
    };
    // Title bar of a WEAPON SELECTION / INVENTORY item: document icon, blue title, rule, window buttons.
    auto titleBar = [&](std::string_view t) {
        c.sprite(kPdaDoc, pdaX(562.5f), pdaY(166), pdaY(24), pdaY(24), kWhite);
        capText(c, pdaX(kPdaTitleX), pdaY(kPdaTitleCap), kPdaTitleScale, t, kPdaHead);
        vrect(c, pdaX(kPdaPicX + 4), pdaY(kPdaTitleLine), (kPdaPicRight - kPdaPicX - 4) * 2 / 3, pdaY(1.3f), kPdaTitleRule);
        c.sprite(kPdaButtons, pdaX(1016.7f), pdaY(170), pdaY(36), pdaY(18), kWhite);
    };
    const bool onItem = cursor_ >= 0 && cursor_ + 1 < int(list.size()); // not on BACK TO PREVIOUS MENU
    if (scr == Screen::PdaInventory) {
        box(kPdaBoxTop, kPdaBoxBottom);
        if (onItem) picture(kPdaBackpack + uint32_t(cursor_), list[size_t(cursor_)].stringId), prompt(int16_t(1236 + cursor_), kPdaPromptMid);
    } else if (scr == Screen::PdaWeapons && onItem) {
        const PdaWeapon& w = pda.weapons[size_t(cursor_)];
        titleBar(s.get(w.name));
        box(kPdaBoxTop, kPdaBoxBottom);
        // "AVAILABLE AMMUNITION : n/max" and "CURRENTLY EQUIPPED : YES / NO", centred on the pane.
        const std::pair<std::string, std::pair<std::string, uint32_t>> rows[] = {
            {std::string(s.get(1102)), {std::to_string(w.ammo) + "/" + std::to_string(w.max), kPdaValue}},
            {std::string(s.get(1103)), {std::string(s.get(w.equipped ? 384 : 385)), w.equipped ? kPdaYes : kPdaNo}}};
        float cap = kPdaRowCap0;
        for (const auto& [label, value] : rows) {
            const float lw = trackedWidth(c, label, kPdaAmmoScale, kPdaAmmoTracking), vw = trackedWidth(c, value.first, kPdaAmmoScale, kPdaAmmoTracking);
            const float x = pdaX((kPdaPicX + kPdaPicRight) / 2) - (lw + vw) / 2;
            capText(c, x, pdaY(cap), kPdaAmmoScale, label, kPdaHead, kPdaAmmoTracking);
            capText(c, x + lw, pdaY(cap), kPdaAmmoScale, value.first, value.second, kPdaAmmoTracking);
            cap = kPdaRowCap1;
        }
    } else if (scr == Screen::PdaItems && onItem) {
        const PdaItem& it = pda.inventory[size_t(category_)][size_t(cursor_)];
        titleBar(s.get(it.name));
        if (it.picture) // 706: the first-aid kit on the grid, its lower quarter transparent
            c.sprite(it.picture, pdaX(kItemPanel[0] + kItemPanel[2] / 2.0f), pdaY(kItemPanel[1] + kItemPanel[2] / 4.0f),
                     pdaX(float(kItemPanel[2])) - pdaX(0), pdaY(kItemPanel[2] / 2.0f), kWhite);
        box(kPdaDescTop, kPdaDescBottom);
        float cap = kPdaDescCap;
        for (const auto& l : fontWrap(c, it.text ? s.get(it.text) : std::string_view(it.body), pdaX(kPdaDescW) - pdaX(0), kPdaDescScale))
            capText(c, pdaX(kPdaDescX), pdaY(cap), kPdaDescScale, l, kWhite), cap += kPdaDescStep;
    }
    if (!sub) switch (cursor_) {
    case kSonar: picture(kPdaSonar, 1074), prompt(1085, kPdaPromptMid); break;
    case kAreaInfo: {
        picture(kPdaPicture + uint32_t(std::clamp(pda.chapter, 0, 7)), 1075);
        box(kPdaBoxTop, kPdaBoxBottom);
        const std::pair<int16_t, std::string> rows[] = {
            {1072, std::to_string(pda.relics) + "/" + std::to_string(pda.relicsTotal)},
            {1073, std::to_string(pda.treasures) + "/" + std::to_string(pda.treasuresTotal)}};
        float cap = kPdaRowCap0;
        for (const auto& [id, v] : rows) {
            capText(c, pdaX(kPdaRowX), pdaY(cap), kPdaRowScale, s.get(id), kWhite);
            capText(c, pdaX(kPdaRowRight) - c.fontWidth(v, kPdaRowScale), pdaY(cap), kPdaRowScale, v, kWhite);
            cap = kPdaRowCap1;
        }
        break;
    }
    case kWeapons: picture(kPdaWeapons, 1076), box(kPdaBoxTop, kPdaBoxBottom), prompt(1086, kPdaPromptMid); break;
    case kInventory: picture(kPdaInventory, 1077), box(kPdaBoxTop, kPdaBoxBottom), prompt(1087, kPdaPromptMid); break;
    case kField:
        box(kPdaFieldBox0, kPdaFieldBox1), box(kPdaFieldBox2, kPdaFieldBox3);
        prompt(1091, (kPdaFieldBox0 + kPdaFieldBox1) / 2), prompt(1092, (kPdaFieldBox2 + kPdaFieldBox3) / 2);
        break;
    default: break;
    }
    {
        const float cap = pdaY(kPdaFootCap);
        auto left = [&](float x, int16_t id) { // left-aligned prompt
            float px = pdaX(x);
            for (const auto& r : keyRuns(s.get(id), "Return")) px += capText(c, px, cap, kPdaSmall, r.text, r.key ? kKeyBlue : kWhite, kPdaTracking);
        };
        left(kPdaFootBack, 375);
        const int16_t select = !sub ? (cursor_ != kAreaInfo ? 120 : 0) : scr == Screen::PdaWeapons ? 1104 : scr == Screen::PdaInventory ? 120 : 0;
        if (select) left(kPdaFootSelect, select); // "Return SELECT" / "Return EQUIP"
    }
}

// SONAR MAP view overlay [V captures at 1920x1080, positions in their pixels scaled by the window height; x from the
// centre]: frame lines and rulers top and bottom (black beyond them), "G DOWN" / "H UP", side brackets, the curved
// bottom panel with "E RESET VIEW" / "Escape BACK". The map image itself (uiImages[1]) fills the window.
void Menus::drawSonar(Canvas& c, const locale::Strings& s) {
    const float w = float(c.width), h = float(c.height), f = h / 1080, cx = w / 2;
    c.image(1, 0, 0, w, h, 0, 0, 1, 1, kWhite);
    const uint32_t blue = rgba(90, 190, 230), dim = rgba(55, 60, 62), grey = rgba(105, 108, 110), black = rgba(0, 0, 0, 235);
    auto tri = [&](glm::vec2 a, glm::vec2 b, glm::vec2 d, uint32_t col) {
        c.vertices.insert(c.vertices.end(), {{a.x, a.y, -1, -1, col}, {b.x, b.y, -1, -1, col}, {d.x, d.y, -1, -1, col}});
    };
    auto line = [&](glm::vec2 a, glm::vec2 b, float width, uint32_t col) { // thick segment
        const glm::vec2 d = b - a, n = glm::vec2(-d.y, d.x) / std::max(glm::length(d), 1e-3f) * (width / 2);
        tri(a - n, b - n, b + n, col), tri(a - n, b + n, a + n, col);
    };
    auto ruler = [&](float y) { // texture 697: ticks, faded towards the ends
        constexpr int kSeg = 8;
        for (int i = 0; i < kSeg; ++i) {
            const float t0 = float(i) / kSeg, t1 = float(i + 1) / kSeg, a = 1 - std::abs((t0 + t1) - 1);
            if (const AtlasRect* r = c.art ? c.art->find(697) : nullptr)
                c.image(0, cx - 390 * f + 780 * f * t0, y - 8 * f, 780 * f / kSeg, 16 * f, r->u0 + (r->u1 - r->u0) * t0, r->v0,
                        r->u0 + (r->u1 - r->u0) * t1, r->v1, withAlpha(kWhite, 0.8f * a));
        }
    };
    // Top: black band, a dim rule, the ruler, the bright rule.
    c.rect(0, 0, w, 65 * f, black);
    c.rect(0, 44 * f, w, std::max(1.0f, f), dim);
    ruler(53 * f);
    c.rect(0, 64 * f, w, 2 * f, blue);
    // Bottom panel: the bright rule rises in two quarter curves around the prompts; black below it.
    std::vector<glm::vec2> path{{0, 1017 * f}, {cx - 705 * f, 1017 * f}};
    for (int i = 1; i <= 12; ++i) { // left curve, centre (cx - 620, 1017), radii 85 x 70
        const float a = float(i) / 12 * 1.5707963f;
        path.push_back({cx - 620 * f - 85 * f * std::cos(a), 1017 * f - 70 * f * std::sin(a)});
    }
    for (int i = 0; i <= 12; ++i) {
        const float a = float(i) / 12 * 1.5707963f;
        path.push_back({cx + 620 * f + 85 * f * std::sin(a), 1017 * f - 70 * f * std::cos(a)});
    }
    path.push_back({w, 1017 * f});
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        const glm::vec2 a = path[i], b = path[i + 1];
        tri(a, b, {b.x, h}, black), tri(a, {b.x, h}, {a.x, h}, black);
    }
    for (size_t i = 0; i + 1 < path.size(); ++i) line(path[i], path[i + 1], 2 * f, blue);
    ruler(1034 * f);
    c.rect(0, 1046 * f, w, std::max(1.0f, f), dim);
    // Side brackets with their middle tabs.
    for (const float side : {-1.0f, 1.0f}) {
        const float x = cx + side * 663 * f, in = -side; // `in`: towards the centre
        line({x, 139 * f}, {x, 880 * f}, 2 * f, grey);
        line({x, 139 * f}, {x + in * 7 * f, 139 * f}, 2 * f, grey), line({x, 880 * f}, {x + in * 7 * f, 880 * f}, 2 * f, grey);
        const glm::vec2 t0{x, 466 * f}, t1{x + in * 18 * f, 478 * f}, t2{x + in * 18 * f, 544 * f}, t3{x, 556 * f};
        tri(t0, t1, t2, grey), tri(t0, t2, t3, grey);
    }
    const float top = 88 / 2.25f, bottom = 974 / 2.25f, scale = 0.57f;
    keyed(c, 320 - 240 / 2.25f, top, scale, s.get(1141), "Return"), keyed(c, 320 + 237 / 2.25f, top, scale, s.get(1142), "Return");
    keyed(c, 320 - 462 / 2.25f, bottom, scale, s.get(1140), "Return"), keyed(c, 320 + 450 / 2.25f, bottom, scale, s.get(375), "Return");
}

std::optional<PdaModel> Menus::pdaModel() const {
    const Screen scr = screen();
    if (scr == Screen::PdaWeapons && cursor_ >= 0 && size_t(cursor_) < pda.weapons.size() && !pda.weapons[size_t(cursor_)].model.empty())
        return PdaModel{pda.weapons[size_t(cursor_)].model, kWeaponPanel[0], kWeaponPanel[1], kWeaponPanel[2], kWeaponPanel[3]};
    if (scr == Screen::PdaItems && cursor_ >= 0 && size_t(cursor_) < pda.inventory[size_t(category_)].size()) {
        const PdaItem& it = pda.inventory[size_t(category_)][size_t(cursor_)];
        if (!it.picture && !it.model.empty()) return PdaModel{it.model, kItemPanel[0], kItemPanel[1], kItemPanel[2], kItemPanel[3]};
    }
    return std::nullopt;
}

void Menus::drawMessage(Canvas& c, const locale::Strings& s) {
    c.sprite(kDialog, 320, kDialogY, kDialogW, kDialogH, kWhite);
    float y = kDialogTop;
    for (const auto& l : fontWrap(c, s.get(message_), 320, kDialogText))
        c.fontText(320 - c.fontWidth(l, kDialogText) / 2, y, kDialogText, l, kWhite), y += 16;
    // OK button: a blue bar fading out at both ends.
    fadeBar(c, 282.2f, 16, 34, 56, kButtonBlue);
    const std::string ok(s.get(383));
    c.fontText(320 - c.fontWidth(ok, kDialogText) / 2, 290.2f - kCapMid * kDialogText / kTextScale, kDialogText, ok, kWhite);
}

void Menus::drawConfirm(Canvas& c, const locale::Strings& s) {
    c.sprite(kDialog, 320, kDialogY, kDialogW, kDialogH, kWhite);
    float cap = message_ == 494 ? kConfirmCapQuit : kConfirmCap;
    for (const auto& l : fontWrap(c, s.get(message_), kConfirmW, kDialogText))
        capText(c, 320 - c.fontWidth(l, kDialogText) / 2, cap, kDialogText, l, kWhite), cap += 16;
    const float barW = 128 * kConfirmBarW, barH = 32 * kConfirmBarH, kConfirmScale = kDialogText;
    const bool yes = cursor_ == 0;
    c.sprite(yes ? kYesBar : kNoBar, yes ? 322.2f - barW / 2 : 318.5f + barW / 2, kConfirmMid, barW, barH, withAlpha(kWhite, 0.5f));
    const float labelCap = kConfirmMid - kCapHeight * kConfirmScale / 2;
    const std::string y(s.get(384)), n(s.get(385)), slash("/");
    const uint32_t on = rgba(181, 217, 255), off = rgba(76, 76, 76);
    const bool quit = message_ == 494;
    capText(c, (quit ? 310.7f : 302.2f) - c.fontWidth(y, kConfirmScale), labelCap, kConfirmScale, y, yes ? on : off);
    capText(c, quit ? 325 : 339.6f, labelCap, kConfirmScale, n, yes ? off : on);
    capText(c, (quit ? 319 : 320) - c.fontWidth(slash, kConfirmScale) / 2, labelCap, kConfirmScale, slash, kWhite);
}

void Menus::draw(Canvas& c, const locale::Strings& s, std::string_view key) {
    const Screen scr = screen();
    if (scr == Screen::None) return;
    if (scr == Screen::Pda || scr == Screen::PdaWeapons || scr == Screen::PdaInventory || scr == Screen::PdaItems) return drawPda(c, s);
    if (scr == Screen::PdaSonar) return drawSonar(c, s);
    if (!mainMenu()) c.rect(0, 0, float(c.width), float(c.height), 0x70000000u); // the paused game, dimmed
    if (scr == Screen::Tailoring) return drawTailoring(c, s); // these two over the backdrop alone [O captures]
    if (scr == Screen::Message) return drawMessage(c, s), footer(c, s, key, {{320, 120}});
    if (scr == Screen::Confirm) return drawConfirm(c, s), footer(c, s, key, {{320, 120}});
    // Backdrop of the ring and list screens: the glyph art and the ring's shadow.
    c.sprite(kFrontArt, kArtX, kArtY, kArtSize, kArtSize, withAlpha(kWhite, kArtAlpha));
    for (int q = 0; q < 4; ++q) // one quarter texture, mirrored
        c.sprite(kRingShade, kRingX + (q & 1 ? 0.5f : -0.5f) * kShadeQuarter, kRingY + (q & 2 ? 0.5f : -0.5f) * kShadeQuarter,
                 kShadeQuarter, kShadeQuarter, kWhite, q & 1, q & 2);
    if (radial(scr)) {
        drawRadial(c, s, scr, cursor_, false);
        footer(c, s, key, {{320, 120}});
    } else if (slots(scr)) {
        drawRadial(c, s, stack_[stack_.size() - 2], cursors_.back(), true);
        drawSlots(c, s);
        footer(c, s, key, {});
        const std::string both(s.get(121)); // "<select> SELECT      <delete> DELETE"
        const size_t gap = both.find("  "), right = both.find_first_not_of(' ', gap);
        const float cap = kFooterY + kCapTop * kTextScale;
        keyed(c, kSlotFooterL, cap, kTextScale, std::string_view(both).substr(0, gap), key);
        if (right != std::string::npos) keyed(c, kSlotFooterR, cap, kTextScale, std::string_view(both).substr(right), key);
    } else {
        drawList(c, s);
        footer(c, s, key, {{kListFooterL, 120}, {kListFooterR, 375}});
    }
}

} // namespace ou::ui
